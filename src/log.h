#ifndef MC_LOG_H
#define MC_LOG_H

#include <stdio.h>
#include <stdlib.h>

#define log_info(...)  do { fprintf(stderr, "[info] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define log_warn(...)  do { fprintf(stderr, "[warn] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define log_error(...) do { fprintf(stderr, "[error] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define log_fatal(...) do { fprintf(stderr, "[fatal] " __VA_ARGS__); fputc('\n', stderr); abort(); } while (0)

#endif
