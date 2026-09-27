/* Logging to stderr: one line per message with a [info], [warn], [error]
 * or [fatal] prefix, safe to call from worker threads. log_fatal runs the
 * fatal hook (saving the world) and aborts; it is for states the game
 * cannot continue from, such as a lost GPU device, never for bad input. */
#ifndef MC_LOG_H
#define MC_LOG_H

#include <stdio.h>
#include <stdlib.h>

/* flockfile keeps the message and its newline together when worker threads
 * log at the same time as the main thread. */
/* clang-format off */
#define MC_LOG_LINE(...)                        \
    do {                                        \
        flockfile(stderr);                      \
        (void)fprintf(stderr, __VA_ARGS__);     \
        (void)fputc('\n', stderr);              \
        funlockfile(stderr);                    \
    } while (0)
#define log_info(...)  MC_LOG_LINE("[info] " __VA_ARGS__)
#define log_warn(...)  MC_LOG_LINE("[warn] " __VA_ARGS__)
#define log_error(...) MC_LOG_LINE("[error] " __VA_ARGS__)
#define log_fatal(...) do { MC_LOG_LINE("[fatal] " __VA_ARGS__); log_run_fatal_hook(); abort(); } while (0)
/* clang-format on */

/* Registers a function to run before a fatal error aborts, such as saving
 * the world after the GPU device is lost. It runs only when the error is
 * raised on the thread that registered it (the one owning the state it
 * touches), and at most once, so a fatal error inside it still aborts. */
void log_set_fatal_hook(void (*fn)(void *user), void *user);
void log_run_fatal_hook(void);

#endif
