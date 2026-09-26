#ifndef MC_LOG_H
#define MC_LOG_H

#include <stdio.h>
#include <stdlib.h>

#define log_info(...)  do { fprintf(stderr, "[info] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define log_warn(...)  do { fprintf(stderr, "[warn] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define log_error(...) do { fprintf(stderr, "[error] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define log_fatal(...)                                                   \
    do {                                                                 \
        fprintf(stderr, "[fatal] " __VA_ARGS__);                         \
        fputc('\n', stderr);                                             \
        log_run_fatal_hook();                                            \
        abort();                                                         \
    } while (0)

/* Registers a function to run before a fatal error aborts, such as saving
 * the world after the GPU device is lost. It runs only when the error is
 * raised on the thread that registered it (the one owning the state it
 * touches), and at most once, so a fatal error inside it still aborts. */
void log_set_fatal_hook(void (*fn)(void *user), void *user);
void log_run_fatal_hook(void);

#endif
