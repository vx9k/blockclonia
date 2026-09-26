#include "jobs.h"
#include "mem.h"
#include "log.h"

#include <pthread.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

struct jobs {
    pthread_mutex_t lock;
    pthread_cond_t work_cv;
    pthread_cond_t idle_cv;
    job *queue_head, *queue_tail;
    job *done_head, *done_tail;
    int running;       /* jobs taken by workers but not yet finished */
    int quit;
    int worker_count;
    pthread_t *threads;
};

static _Thread_local void *t_scratch;
static _Thread_local size_t t_scratch_size;

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

static void *worker_main(void *arg)
{
    jobs *js = arg;
#ifdef __linux__
    /* Background work yields to the render thread on small CPUs. Linux
     * applies nice values per thread; failure is harmless. */
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
#endif
    pthread_mutex_lock(&js->lock);
    for (;;) {
        while (!js->queue_head && !js->quit)
            pthread_cond_wait(&js->work_cv, &js->lock);
        if (js->quit && !js->queue_head) break;
        job *j = pop(&js->queue_head, &js->queue_tail);
        js->running++;
        pthread_mutex_unlock(&js->lock);

        j->run(j);

        pthread_mutex_lock(&js->lock);
        push(&js->done_head, &js->done_tail, j);
        js->running--;
        if (!js->queue_head && js->running == 0)
            pthread_cond_broadcast(&js->idle_cv);
    }
    pthread_mutex_unlock(&js->lock);
    free_scratch();
    return NULL;
}

jobs *jobs_create(int workers)
{
    jobs *js = mem_calloc(1, sizeof *js);
    pthread_mutex_init(&js->lock, NULL);
    pthread_cond_init(&js->work_cv, NULL);
    pthread_cond_init(&js->idle_cv, NULL);
    if (workers < 0) workers = 0;
    js->threads = workers ? mem_calloc((size_t)workers, sizeof(pthread_t)) : NULL;
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&js->threads[i], NULL, worker_main, js) != 0) {
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
    pthread_mutex_lock(&js->lock);
    js->quit = 1;
    pthread_cond_broadcast(&js->work_cv);
    pthread_mutex_unlock(&js->lock);
    for (int i = 0; i < js->worker_count; i++) pthread_join(js->threads[i], NULL);
    jobs_poll(js, -1);
    free_scratch(); /* inline (zero-worker) jobs used the caller's scratch */
    pthread_cond_destroy(&js->idle_cv);
    pthread_cond_destroy(&js->work_cv);
    pthread_mutex_destroy(&js->lock);
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
    pthread_mutex_lock(&js->lock);
    push(&js->queue_head, &js->queue_tail, j);
    pthread_cond_signal(&js->work_cv);
    pthread_mutex_unlock(&js->lock);
}

void jobs_submit_front(jobs *js, job *j)
{
    if (js->worker_count == 0) {
        jobs_submit(js, j);
        return;
    }
    pthread_mutex_lock(&js->lock);
    j->next = js->queue_head;
    js->queue_head = j;
    if (!js->queue_tail) js->queue_tail = j;
    pthread_cond_signal(&js->work_cv);
    pthread_mutex_unlock(&js->lock);
}

int jobs_poll(jobs *js, int budget)
{
    int n = 0;
    while (budget < 0 || n < budget) {
        pthread_mutex_lock(&js->lock);
        job *j = pop(&js->done_head, &js->done_tail);
        pthread_mutex_unlock(&js->lock);
        if (!j) break;
        j->done(j);
        n++;
    }
    return n;
}

void jobs_wait_idle(jobs *js)
{
    pthread_mutex_lock(&js->lock);
    while (js->queue_head || js->running > 0)
        pthread_cond_wait(&js->idle_cv, &js->lock);
    pthread_mutex_unlock(&js->lock);
}

int jobs_worker_count(const jobs *js) { return js->worker_count; }

int cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
