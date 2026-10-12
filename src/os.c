#include "os.h"
#include "mem.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

double os_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void os_sleep(double seconds)
{
    if (!(seconds > 0.0)) return;
    struct timespec ts = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

int os_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

void os_mutex_init(os_mutex *m) { pthread_mutex_init(m, NULL); }
void os_mutex_destroy(os_mutex *m) { pthread_mutex_destroy(m); }
void os_mutex_lock(os_mutex *m) { pthread_mutex_lock(m); }
void os_mutex_unlock(os_mutex *m) { pthread_mutex_unlock(m); }
/* cppcheck 2.22's posix.cfg wrongly forbids a NULL attr and takes the
 * condition variable to pthread_cond_destroy as read-only. */
/* cppcheck-suppress nullPointer */
void os_cond_init(os_cond *c) { pthread_cond_init(c, NULL); }
/* cppcheck-suppress constParameterPointer */
void os_cond_destroy(os_cond *c) { pthread_cond_destroy(c); }
void os_cond_wait(os_cond *c, os_mutex *m) { pthread_cond_wait(c, m); }
void os_cond_signal(os_cond *c) { pthread_cond_signal(c); }
void os_cond_broadcast(os_cond *c) { pthread_cond_broadcast(c); }

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} thread_start;

static void *thread_main(void *p)
{
    thread_start s = *(thread_start *)p;
    mem_free(p);
    s.fn(s.arg);
    return NULL;
}

int os_thread_start(os_thread *t, void (*fn)(void *arg), void *arg)
{
    thread_start *s = mem_alloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(t, NULL, thread_main, s) != 0) {
        mem_free(s);
        return -1;
    }
    return 0;
}

void os_thread_join(os_thread t) { pthread_join(t, NULL); }

void os_thread_lower_priority(void)
{
#ifdef __linux__
    /* Linux applies nice values per thread. */
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
#endif
}

int os_path_kind(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? OS_MISSING : OS_ERROR;
    if (S_ISREG(st.st_mode)) return OS_FILE;
    return S_ISDIR(st.st_mode) ? OS_DIR : OS_OTHER;
}

int os_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

long os_read_regular(const char *path, uint8_t *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? -2 : -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    if ((unsigned long long)st.st_size > cap) {
        close(fd);
        return (long)cap + 1;
    }
    size_t len = 0;
    while (len < cap) {
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    return (long)len;
}

/* Makes a rename in path's folder survive a power cut. Best effort: the
 * file is already in place, and only the folder entry could be lost. */
static void sync_parent(const char *path)
{
    char dir[PATH_MAX];
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (slash == path) n = 1; /* "/name" lives in "/" */
    if (n >= sizeof dir) return;
    if (n) memcpy(dir, path, n);
    else dir[n++] = '.';
    dir[n] = '\0';
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    (void)fsync(fd);
    close(fd);
}

int os_write_atomic(const char *path, const char *tmp, const void *data, size_t len)
{
    unlink(tmp); /* a stale or planted temp entry; unlink never follows */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    /* Data on disk before the rename: without it a power cut can leave the
     * new name on an empty file, the old contents already gone. */
    int ok = done == len && fsync(fd) == 0;
    ok &= close(fd) == 0;
    if (ok && rename(tmp, path) == 0) {
        sync_parent(path);
        return 0;
    }
    unlink(tmp);
    return -1;
}

int os_mkdir(const char *dir, unsigned mode) { return mkdir(dir, (mode_t)mode); }

int os_full_path(const char *dir, char *out, size_t cap)
{
    char real[PATH_MAX];
    if (!realpath(dir, real)) return -1;
    size_t n = strlen(real);
    if (n >= cap) return -1;
    memcpy(out, real, n + 1);
    return 0;
}

struct os_dir {
    DIR *d;
};

os_dir *os_dir_open(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return NULL;
    os_dir *od = mem_alloc(sizeof *od);
    od->d = d;
    return od;
}

const char *os_dir_next(os_dir *d)
{
    /* Each os_dir has its own DIR, so readdir is safe across threads here;
     * readdir_r is deprecated in POSIX.1-2008. */
    /* cppcheck-suppress readdirCalled */
    const struct dirent *e = readdir(d->d);
    return e ? e->d_name : NULL;
}

void os_dir_close(os_dir *d)
{
    if (!d) return;
    closedir(d->d);
    mem_free(d);
}
