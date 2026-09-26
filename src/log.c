#include "log.h"

static void (*g_hook)(void *user);
static void *g_hook_user;
static _Thread_local int g_hook_thread;
static _Thread_local int g_hook_running;

void log_set_fatal_hook(void (*fn)(void *user), void *user)
{
    g_hook = fn;
    g_hook_user = user;
    g_hook_thread = 1;
}

void log_run_fatal_hook(void)
{
    if (!g_hook || !g_hook_thread || g_hook_running) return;
    g_hook_running = 1;
    fputs("[fatal] running shutdown hook\n", stderr);
    g_hook(g_hook_user);
}
