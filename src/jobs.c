#include "jobs.h"
#include "log.h"
#include "mem.h"
#include "os.h"

struct jobs {
    os_mutex lock;
    os_cond work_cv;
    os_cond idle_cv;
    job *queue_head, *queue_tail;
    job *done_head, *done_tail;
    int running;       /* jobs taken by workers but not yet finished */
    int quit;
    int worker_count;
    os_thread *threads;
};

static OS_THREAD_LOCAL void *t_scratch;
static OS_THREAD_LOCAL size_t t_scratch_size;

void *jobs_scratch(size_t size)
{
    if (t_scratch_size < size) {
        mem_free(t_scratch);
        t_scratch = mem_alloc(size);
        t_scratch_size = size;
    }
    return t_scratch;
}

static void free_scratch(void)
{
    mem_free(t_scratch);
    t_scratch = NULL;
    t_scratch_size = 0;
}

static void push(job **head, job **tail, job *j)
{
    j->next = NULL;
    if (*tail) (*tail)->next = j; else *head = j;
    *tail = j;
}

static job *pop(job **head, job **tail)
{
    job *j = *head;
    if (j) {
        *head = j->next;
        if (!*head) *tail = NULL;
        j->next = NULL;
    }
    return j;
}

static void worker_main(void *arg)
{
    jobs *js = arg;
    os_thread_lower_priority();
    os_mutex_lock(&js->lock);
    for (;;) {
        while (!js->queue_head && !js->quit) os_cond_wait(&js->work_cv, &js->lock);
        if (js->quit && !js->queue_head) break;
        job *j = pop(&js->queue_head, &js->queue_tail);
        js->running++;
        os_mutex_unlock(&js->lock);

        j->run(j);

        os_mutex_lock(&js->lock);
        push(&js->done_head, &js->done_tail, j);
        js->running--;
        if (!js->queue_head && js->running == 0) os_cond_broadcast(&js->idle_cv);
    }
    os_mutex_unlock(&js->lock);
    free_scratch();
}

jobs *jobs_create(int workers)
{
    jobs *js = mem_calloc(1, sizeof *js);
    os_mutex_init(&js->lock);
    os_cond_init(&js->work_cv);
    os_cond_init(&js->idle_cv);
    if (workers < 0) workers = 0;
    js->threads = workers ? mem_calloc((size_t)workers, sizeof(os_thread)) : NULL;
    for (int i = 0; i < workers; i++) {
        if (os_thread_start(&js->threads[i], worker_main, js) != 0) {
            log_warn("could only start %d worker threads", i);
            break;
        }
        js->worker_count++;
    }
    return js;
}

void jobs_destroy(jobs *js)
{
    if (!js) return;
    os_mutex_lock(&js->lock);
    js->quit = 1;
    os_cond_broadcast(&js->work_cv);
    os_mutex_unlock(&js->lock);
    for (int i = 0; i < js->worker_count; i++) os_thread_join(js->threads[i]);
    jobs_poll(js, -1);
    free_scratch(); /* inline (zero-worker) jobs used the caller's scratch */
    os_cond_destroy(&js->idle_cv);
    os_cond_destroy(&js->work_cv);
    os_mutex_destroy(&js->lock);
    mem_free(js->threads);
    mem_free(js);
}

void jobs_submit(jobs *js, job *j)
{
    if (js->worker_count == 0) {
        j->run(j);
        push(&js->done_head, &js->done_tail, j);
        return;
    }
    os_mutex_lock(&js->lock);
    push(&js->queue_head, &js->queue_tail, j);
    os_cond_signal(&js->work_cv);
    os_mutex_unlock(&js->lock);
}

void jobs_submit_front(jobs *js, job *j)
{
    if (js->worker_count == 0) {
        jobs_submit(js, j);
        return;
    }
    os_mutex_lock(&js->lock);
    j->next = js->queue_head;
    js->queue_head = j;
    if (!js->queue_tail) js->queue_tail = j;
    os_cond_signal(&js->work_cv);
    os_mutex_unlock(&js->lock);
}

int jobs_poll(jobs *js, int budget)
{
    int n = 0;
    while (budget < 0 || n < budget) {
        os_mutex_lock(&js->lock);
        job *j = pop(&js->done_head, &js->done_tail);
        os_mutex_unlock(&js->lock);
        if (!j) break;
        j->done(j);
        n++;
    }
    return n;
}

void jobs_wait_idle(jobs *js)
{
    os_mutex_lock(&js->lock);
    while (js->queue_head || js->running > 0) os_cond_wait(&js->idle_cv, &js->lock);
    os_mutex_unlock(&js->lock);
}

int jobs_worker_count(const jobs *js) { return js->worker_count; }

int cpu_count(void) { return os_cpu_count(); }
