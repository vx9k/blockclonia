/* The few operating-system services the game needs that POSIX and Windows
 * spell differently: a monotonic clock, the CPU count, threads, and the
 * checked file access that save.c and save_db.c build on. Each function
 * has one POSIX and one Win32 body; the Win32 one serves MinGW and MSVC
 * alike, so neither needs winpthreads or a POSIX emulation layer. Paths
 * are UTF-8 on every platform. It does not decide where files go
 * (paths.c) or what is in them (save.c). */
#ifndef MC_OS_H
#define MC_OS_H

#include <stddef.h>
#include <stdint.h>

#ifndef _WIN32
#include <pthread.h>
#endif

/* MSVC only accepts its own spelling of thread-local storage in C. */
#if defined(_MSC_VER) && !defined(__clang__)
#define OS_THREAD_LOCAL __declspec(thread)
#else
#define OS_THREAD_LOCAL _Thread_local
#endif

/* Seconds on a monotonic clock with an arbitrary start. Lock-free and
 * allocation-free, so the audio callback may call it. */
double os_now(void);

/* Online CPU cores, at least 1. */
int os_cpu_count(void);

/* ------------------------------------------------------------ threads */

#ifdef _WIN32
/* SRWLOCK, CONDITION_VARIABLE and HANDLE are each one pointer; holding them
 * as void * keeps <windows.h> out of every file that includes this one. */
typedef struct {
    void *p;
} os_mutex;
typedef struct {
    void *p;
} os_cond;
typedef struct {
    void *p;
} os_thread;
#else
typedef pthread_mutex_t os_mutex;
typedef pthread_cond_t os_cond;
typedef pthread_t os_thread;
#endif

void os_mutex_init(os_mutex *m);
void os_mutex_destroy(os_mutex *m);
void os_mutex_lock(os_mutex *m);
void os_mutex_unlock(os_mutex *m);
void os_cond_init(os_cond *c);
void os_cond_destroy(os_cond *c);
void os_cond_wait(os_cond *c, os_mutex *m);
void os_cond_signal(os_cond *c);
void os_cond_broadcast(os_cond *c);
/* Starts fn(arg) on a new thread. Returns 0, or -1 if it could not. */
int os_thread_start(os_thread *t, void (*fn)(void *arg), void *arg);
void os_thread_join(os_thread t);
/* Lowers the calling thread's priority below the render thread's, so
 * background work yields on small CPUs. Failure is harmless and ignored. */
void os_thread_lower_priority(void);

/* ------------------------------------------------------------ files */

/* World folders come from anyone, so these never follow a symlink (or, on
 * Windows, any reparse point such as a junction) and only touch regular
 * files. Otherwise a shared world could point a save file at, say,
 * ~/.bashrc and have the game overwrite it, or at a FIFO or device and
 * hang a loader thread. */

/* What sits at a path, without following a final symlink. */
enum { OS_MISSING, OS_FILE, OS_DIR, OS_OTHER, OS_ERROR };
int os_path_kind(const char *path);
/* 1 if path is a directory, following symlinks (a home or world folder
 * reached through one is fine; only the files inside are checked). */
int os_is_dir(const char *path);

/* Reads up to cap bytes of a regular file. Returns the length read, cap + 1
 * if the file is larger, -1 if it is unusable, or -2 if it is missing. */
long os_read_regular(const char *path, uint8_t *buf, size_t cap);

/* Writes a whole file through a fresh temp file and a rename, so a crash
 * mid-save never leaves a truncated file, and whatever sat at either name
 * before (a symlink, a hard link to another file) is replaced, never
 * written through. Returns 0 or -1. */
int os_write_atomic(const char *path, const char *tmp, const void *data, size_t len);

/* Creates one directory (mode is ignored on Windows, where the user's
 * profile already restricts access). Returns 0, or -1 if it was not
 * created, including because something already exists there. */
int os_mkdir(const char *dir, unsigned mode);

/* An absolute form of dir in out. POSIX resolves every symlink; Windows
 * only makes the path absolute. Returns 0, or -1 if dir does not exist or
 * the result does not fit. */
int os_full_path(const char *dir, char *out, size_t cap);

/* Lists the names in a directory, "." and ".." included, in no order. */
typedef struct os_dir os_dir;
os_dir *os_dir_open(const char *dir); /* NULL if it cannot be read */
const char *os_dir_next(os_dir *d);   /* NULL at the end; valid until the next call */
void os_dir_close(os_dir *d);

#endif
