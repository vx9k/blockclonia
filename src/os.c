#include "os.h"
#include "mem.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <limits.h>
#include <process.h>
#include <string.h>
#include <windows.h>

_Static_assert(sizeof(SRWLOCK) == sizeof(os_mutex), "os_mutex holds an SRWLOCK");
_Static_assert(sizeof(CONDITION_VARIABLE) == sizeof(os_cond), "os_cond holds a CONDITION_VARIABLE");
_Static_assert(sizeof(HANDLE) == sizeof(os_thread), "os_thread holds a HANDLE");

/* Longest path accepted, in UTF-16 units. Without the \\?\ prefix Win32
 * stops at MAX_PATH anyway on systems without long paths enabled. */
#define WPATH_MAX 1024

double os_now(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

int os_cpu_count(void)
{
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 && n < 4096 ? (int)n : 1;
}

void os_mutex_init(os_mutex *m) { InitializeSRWLock((SRWLOCK *)(void *)m); }
void os_mutex_destroy(os_mutex *m) { (void)m; } /* an SRWLOCK holds no resources */
void os_mutex_lock(os_mutex *m) { AcquireSRWLockExclusive((SRWLOCK *)(void *)m); }
void os_mutex_unlock(os_mutex *m) { ReleaseSRWLockExclusive((SRWLOCK *)(void *)m); }
void os_cond_init(os_cond *c) { InitializeConditionVariable((CONDITION_VARIABLE *)(void *)c); }
void os_cond_destroy(os_cond *c) { (void)c; }
void os_cond_signal(os_cond *c) { WakeConditionVariable((CONDITION_VARIABLE *)(void *)c); }
void os_cond_broadcast(os_cond *c) { WakeAllConditionVariable((CONDITION_VARIABLE *)(void *)c); }

void os_cond_wait(os_cond *c, os_mutex *m)
{
    SleepConditionVariableSRW((CONDITION_VARIABLE *)(void *)c, (SRWLOCK *)(void *)m, INFINITE, 0);
}

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} thread_start;

/* _beginthreadex rather than CreateThread: it sets up the C runtime's
 * per-thread state (errno, stdio locks) for the new thread. */
static unsigned __stdcall thread_main(void *p)
{
    thread_start s = *(thread_start *)p;
    mem_free(p);
    s.fn(s.arg);
    return 0;
}

int os_thread_start(os_thread *t, void (*fn)(void *arg), void *arg)
{
    thread_start *s = mem_alloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, thread_main, s, 0, NULL);
    if (h == 0) {
        mem_free(s);
        return -1;
    }
    t->p = (void *)h;
    return 0;
}

void os_thread_join(os_thread t)
{
    WaitForSingleObject(t.p, INFINITE);
    CloseHandle(t.p);
}

void os_thread_lower_priority(void) { (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

/* UTF-8 to the UTF-16 Win32 expects. The narrow (A) functions would read
 * paths in the ANSI code page and mangle a non-ASCII user name. */
static int widen(const char *s, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap) > 0;
}

static int narrow(const wchar_t *s, char *out, size_t cap)
{
    int c = cap > INT_MAX ? INT_MAX : (int)cap;
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, out, c, NULL, NULL) > 0;
}

static int is_missing(DWORD err) { return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND; }

/* Asks the open handle rather than GetFileAttributesW, which some
 * implementations (Wine) answer for a symlink's target. */
int os_path_kind(const char *path)
{
    wchar_t w[WPATH_MAX];
    if (!widen(path, w, WPATH_MAX)) return OS_ERROR;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return is_missing(GetLastError()) ? OS_MISSING : OS_ERROR;
    BY_HANDLE_FILE_INFORMATION fi;
    int kind = OS_ERROR;
    if (GetFileInformationByHandle(h, &fi)) {
        if (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) kind = OS_OTHER;
        else if (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) kind = OS_DIR;
        else kind = GetFileType(h) == FILE_TYPE_DISK ? OS_FILE : OS_OTHER;
    }
    CloseHandle(h);
    return kind;
}

int os_is_dir(const char *path)
{
    wchar_t w[WPATH_MAX];
    if (!widen(path, w, WPATH_MAX)) return 0;
    /* A junction or directory symlink carries the directory attribute of
     * the link; asking for the target's tells a dangling one apart. */
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION fi;
    int dir = GetFileInformationByHandle(h, &fi) && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    CloseHandle(h);
    return dir;
}

/* FILE_FLAG_OPEN_REPARSE_POINT opens a symlink or junction itself instead of
 * its target, and the attribute check then refuses it: the O_NOFOLLOW of
 * Win32. FILE_TYPE_DISK rules out pipes and devices such as CON. */
long os_read_regular(const char *path, uint8_t *buf, size_t cap)
{
    wchar_t w[WPATH_MAX];
    if (!widen(path, w, WPATH_MAX)) return -1;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return is_missing(GetLastError()) ? -2 : -1;
    BY_HANDLE_FILE_INFORMATION fi;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &fi) ||
        (fi.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(h);
        return -1;
    }
    unsigned long long size = (unsigned long long)fi.nFileSizeHigh << 32 | fi.nFileSizeLow;
    if (size > cap) {
        CloseHandle(h);
        return (long)cap + 1;
    }
    size_t len = 0;
    while (len < cap) {
        size_t want = cap - len;
        DWORD got = 0;
        if (!ReadFile(h, buf + len, want > 0x40000000u ? 0x40000000u : (DWORD)want, &got, NULL)) {
            CloseHandle(h);
            return -1;
        }
        if (got == 0) break;
        len += got;
    }
    CloseHandle(h);
    return (long)len;
}

int os_write_atomic(const char *path, const char *tmp, const void *data, size_t len)
{
    wchar_t wp[WPATH_MAX], wt[WPATH_MAX];
    if (!widen(path, wp, WPATH_MAX) || !widen(tmp, wt, WPATH_MAX)) return -1;
    DeleteFileW(wt); /* a stale or planted temp entry; deleting a link never touches its target */
    HANDLE h = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        DWORD put = 0;
        if (!WriteFile(h, p + done, want > 0x40000000u ? 0x40000000u : (DWORD)want, &put, NULL) || put == 0) break;
        done += put;
    }
    int ok = done == len;
    ok &= CloseHandle(h) != 0;
    /* CRT rename() refuses an existing target; MoveFileExW replaces it (or
     * the link at that name, never its target). Virus scanners and search
     * indexers briefly open new files, which shows up as a sharing or
     * access error, so a few retries ride that out. */
    for (int i = 0; ok && i < 5; i++) {
        if (MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
        DWORD err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION) break;
        Sleep(10);
    }
    DeleteFileW(wt);
    return -1;
}

int os_mkdir(const char *dir, unsigned mode)
{
    (void)mode;
    wchar_t w[WPATH_MAX];
    return widen(dir, w, WPATH_MAX) && CreateDirectoryW(w, NULL) ? 0 : -1;
}

int os_full_path(const char *dir, char *out, size_t cap)
{
    wchar_t w[WPATH_MAX], full[WPATH_MAX];
    if (!widen(dir, w, WPATH_MAX)) return -1;
    DWORD n = GetFullPathNameW(w, WPATH_MAX, full, NULL);
    if (n == 0 || n >= WPATH_MAX || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return -1;
    return narrow(full, out, cap) ? 0 : -1;
}

struct os_dir {
    HANDLE h;
    int first; /* fd holds an entry FindFirstFileW returned but nobody has read */
    WIN32_FIND_DATAW fd;
    char name[MAX_PATH * 3]; /* worst case of UTF-16 to UTF-8 */
};

os_dir *os_dir_open(const char *dir)
{
    wchar_t w[WPATH_MAX];
    if (!widen(dir, w, WPATH_MAX - 2)) return NULL;
    size_t n = wcslen(w);
    if (n > 0 && w[n - 1] != L'/' && w[n - 1] != L'\\') w[n++] = L'\\';
    w[n++] = L'*';
    w[n] = 0;
    os_dir *d = mem_alloc(sizeof *d);
    d->h = FindFirstFileExW(w, FindExInfoBasic, &d->fd, FindExSearchNameMatch, NULL, 0);
    if (d->h == INVALID_HANDLE_VALUE) {
        mem_free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

const char *os_dir_next(os_dir *d)
{
    for (;;) {
        if (!d->first && !FindNextFileW(d->h, &d->fd)) return NULL;
        d->first = 0;
        if (narrow(d->fd.cFileName, d->name, sizeof d->name)) return d->name;
        /* A name that is not valid UTF-16 cannot be one the game wrote. */
    }
}

void os_dir_close(os_dir *d)
{
    if (!d) return;
    FindClose(d->h);
    mem_free(d);
}

#else /* POSIX */

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

int os_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

void os_mutex_init(os_mutex *m) { pthread_mutex_init(m, NULL); }
void os_mutex_destroy(os_mutex *m) { pthread_mutex_destroy(m); }
void os_mutex_lock(os_mutex *m) { pthread_mutex_lock(m); }
void os_mutex_unlock(os_mutex *m) { pthread_mutex_unlock(m); }
void os_cond_init(os_cond *c) { pthread_cond_init(c, NULL); }
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
    int ok = done == len;
    ok &= close(fd) == 0;
    if (ok && rename(tmp, path) == 0) return 0;
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
    const struct dirent *e = readdir(d->d);
    return e ? e->d_name : NULL;
}

void os_dir_close(os_dir *d)
{
    if (!d) return;
    closedir(d->d);
    mem_free(d);
}

#endif
