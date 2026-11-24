/* Runs the unit test suites, each in a child process of its own, spread
 * over the CPU's cores. Most of the time goes to long body and physics
 * simulations that each keep one core busy, so a serial run left the
 * other cores idle. A child shares no state with the other suites, and a
 * crash takes down one suite instead of the run. "-j 1" runs everything
 * in this process, in table order, the way a debugger wants it. */
#include "os.h"
#include "test_util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define RUN_MAX_JOBS 256
#define RUN_MARK "@@mc_tests"

typedef struct {
    pid_t pid;
    FILE *out; /* the child's stdout and stderr, read back when it ends */
    int suite, part;
    double start;
} run_slot;

typedef struct {
    int jobs, verbose, list;
    int names; /* suite names asked for; none means every suite */
    const char *name[RUN_MAX_JOBS];
} run_opts;

static void usage(void) { fprintf(stderr, "usage: mc_tests [-j N] [-v] [--list] [suite...]\n"); }

/* A whole number from s, or -1 when s is not one. */
static int number(const char *s, const char **end)
{
    char *e = NULL;
    long v = strtol(s, &e, 10);
    if (end) *end = e;
    return e == s || v < 0 || v > 1000000000L ? -1 : (int)v;
}

static int parse(run_opts *o, int argc, char **argv)
{
    o->jobs = os_cpu_count();
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) o->verbose = 1;
        else if (strcmp(argv[i], "--list") == 0) o->list = 1;
        else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) o->jobs = number(argv[++i], NULL);
        else if (strncmp(argv[i], "-j", 2) == 0 && argv[i][2]) o->jobs = number(argv[i] + 2, NULL);
        else if (argv[i][0] == '-' || o->names == RUN_MAX_JOBS) return 0;
        else o->name[o->names++] = argv[i];
    }
    if (o->jobs < 1 || o->jobs > RUN_MAX_JOBS) o->jobs = o->jobs < 1 ? 1 : RUN_MAX_JOBS;
    return 1;
}

/* A suite is named by its function, and "test_" in front and "_all" at the
 * end may be left out: test_png_all is also png_all and png. */
static int name_is(const char *name, const char *arg)
{
    if (strncmp(name, "test_", 5) == 0 && strncmp(arg, "test_", 5) != 0) name += 5;
    size_t n = strlen(name), a = strlen(arg);
    if (a == n) return strcmp(name, arg) == 0;
    return a + 4 == n && strncmp(name, arg, a) == 0 && strcmp(name + a, "_all") == 0;
}

static int wanted(const run_opts *o, const char *name)
{
    if (o->names == 0) return 1;
    for (int i = 0; i < o->names; i++)
        if (name_is(name, o->name[i])) return 1;
    return 0;
}

static int unknown_names(const run_opts *o, const test_suite *s, int n)
{
    int bad = 0;
    for (int i = 0; i < o->names; i++) {
        int found = 0;
        for (int k = 0; k < n && !found; k++) found = name_is(s[k].name, o->name[i]);
        if (!found) {
            fprintf(stderr, "mc_tests: no suite named %s (--list shows them)\n", o->name[i]);
            bad = 1;
        }
    }
    return bad;
}

static int g_part_next;

int test_part_take(void) { return g_part_next++ % g_parts == g_part; }

static run_slot start_suite(const test_suite *s, int idx, int part)
{
    run_slot r = {0};
    r.suite = idx;
    r.part = part;
    r.start = os_now();
    r.out = tmpfile();
    if (!r.out) {
        perror("mc_tests: tmpfile");
        exit(2);
    }
    /* Anything still buffered would be written again by the child. */
    if (fflush(stdout) != 0 || fflush(stderr) != 0) perror("mc_tests: fflush");
    r.pid = fork();
    if (r.pid < 0) {
        perror("mc_tests: fork");
        exit(2);
    }
    if (r.pid == 0) {
        if (dup2(fileno(r.out), STDOUT_FILENO) < 0 || dup2(fileno(r.out), STDERR_FILENO) < 0) _exit(2);
        g_checks = 0;
        g_failed = 0;
        g_part = part;
        g_parts = s->parts > 1 ? s->parts : 1;
        s->run();
        printf(RUN_MARK " %d %d\n", g_checks, g_failed);
        exit(0); /* through exit, not _exit: the leak checker runs at exit */
    }
    return r;
}

/* Prints what the child wrote and adds its counts. A child that crashed,
 * leaked under the sanitizers or never reported counts as one failure. */
static void finish_suite(const test_suite *s, run_slot *r, int status, int verbose, int *checks, int *failed)
{
    int c = 0, f = 0, counted = 0;
    char line[2048] = {0};
    if (fseek(r->out, 0, SEEK_SET) != 0) perror("mc_tests: fseek");
    while (fgets(line, sizeof line, r->out)) {
        const char *e = NULL;
        if (strncmp(line, RUN_MARK " ", sizeof RUN_MARK) == 0) {
            c = number(line + sizeof RUN_MARK, &e);
            f = number(e, NULL);
            counted = c >= 0 && f >= 0;
            if (!counted) c = f = 0;
        } else fputs(line, strncmp(line, "FAIL ", 5) == 0 ? stderr : stdout);
    }
    if (fclose(r->out) != 0) perror("mc_tests: fclose");
    *checks += c;
    *failed += f;
    char name[160] = {0};
    if (s->parts > 1) snprintf(name, sizeof name, "%s part %d/%d", s->name, r->part + 1, s->parts);
    else snprintf(name, sizeof name, "%s", s->name);
    if (!counted || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) fprintf(stderr, "FAIL %s: killed by signal %d\n", name, WTERMSIG(status));
        else fprintf(stderr, "FAIL %s: exited with status %d\n", name, WEXITSTATUS(status));
        *checks += 1;
        *failed += 1;
    }
    if (verbose) printf("%7.2f s  %s\n", os_now() - r->start, name);
}

static void run_serial(const run_opts *o, const test_suite *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (!wanted(o, s[i].name)) continue;
        double t0 = os_now();
        s[i].run();
        if (o->verbose) printf("%7.2f s  %s\n", os_now() - t0, s[i].name);
    }
}

static int run_forked(const run_opts *o, const test_suite *s, int n)
{
    run_slot slot[RUN_MAX_JOBS] = {0};
    int busy = 0, next = 0, part = 0;
    for (;;) {
        while (busy < o->jobs && next < n) {
            if (wanted(o, s[next].name)) slot[busy++] = start_suite(&s[next], next, part);
            if (++part >= s[next].parts) {
                part = 0;
                next++;
            }
        }
        if (busy == 0) return 0;
        int status = 0;
        pid_t p = waitpid(-1, &status, 0);
        if (p < 0 && errno == EINTR) continue;
        if (p < 0) {
            perror("mc_tests: waitpid");
            for (int i = 0; i < busy; i++)
                if (fclose(slot[i].out) != 0) perror("mc_tests: fclose");
            return 2;
        }
        for (int i = 0; i < busy; i++) {
            if (slot[i].pid != p) continue;
            finish_suite(&s[slot[i].suite], &slot[i], status, o->verbose, &g_checks, &g_failed);
            slot[i] = slot[--busy];
            break;
        }
    }
}

int test_run(const test_suite *suites, int n, int argc, char **argv)
{
    run_opts o = {0};
    if (!parse(&o, argc, argv)) {
        usage();
        return 2;
    }
    if (o.list) {
        for (int i = 0; i < n; i++) printf("%s\n", suites[i].name);
        return 0;
    }
    if (unknown_names(&o, suites, n)) return 2;
    double t0 = os_now();
    int err = 0;
    if (o.jobs == 1) run_serial(&o, suites, n);
    else err = run_forked(&o, suites, n);
    printf("%d/%d checks passed in %.1f s on %d process%s\n", g_checks - g_failed, g_checks, os_now() - t0, o.jobs,
           o.jobs == 1 ? "" : "es");
    return err ? err : (g_failed ? 1 : 0);
}
