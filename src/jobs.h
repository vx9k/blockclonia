/* Minimal thread pool. Jobs run on workers; their completion callbacks run
 * on the main thread inside jobs_poll(), so completion code never races with
 * game state. With zero workers, jobs run inline at submit time (tests). */
#ifndef MC_JOBS_H
#define MC_JOBS_H

#include <stddef.h>

typedef struct job {
    void (*run)(struct job *j);   /* worker thread */
    void (*done)(struct job *j);  /* main thread; must free the job */
    struct job *next;
} job;

typedef struct jobs jobs;

/* Starts `workers` threads; with 0, jobs_submit runs each job and its
 * completion inline. */
jobs *jobs_create(int workers);
void jobs_destroy(jobs *js);    /* drains queued jobs, then joins workers */
void jobs_submit(jobs *js, job *j);
/* Queues ahead of everything else (edits the player is waiting to see). */
void jobs_submit_front(jobs *js, job *j);
/* Runs up to `budget` completion callbacks (budget < 0: all). */
int jobs_poll(jobs *js, int budget);
int jobs_worker_count(const jobs *js);
/* Blocks until nothing is queued or running. Completions still need
 * jobs_poll(). */
void jobs_wait_idle(jobs *js);

/* Per-thread scratch buffer of at least `size` bytes, owned by the calling
 * thread (worker or main) and freed when the pool is destroyed. */
void *jobs_scratch(size_t size);

/* Online CPU cores, at least 1. */
int cpu_count(void);

#endif
