/* Headless CPU benchmarks (terrain generation, meshing, physics). */
#ifndef MC_BENCH_H
#define MC_BENCH_H

#include <stdint.h>

/* Prints timings to stdout; returns the process exit code (0). */
int bench_run(uint32_t seed);

#endif
