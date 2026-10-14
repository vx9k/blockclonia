#include "upnp.h"
#include "log.h"
#include "mem.h"
#include "os.h"

#include <miniupnpc.h>
#include <stdio.h>
#include <string.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#define DISCOVER_MS 2000

struct upnp {
    os_thread thread;
    os_mutex lock;
    uint16_t port;
    int state;       /* 0 asking, 1 forwarded, -1 failed: under lock */
    char text[96];   /* the address, or why not: under lock */
    int have_urls;   /* the thread's own until it ends */
    struct UPNPUrls urls;
    struct IGDdatas data;
};

static void finish(upnp *u, int state, const char *text)
{
    os_mutex_lock(&u->lock);
    u->state = state;
    snprintf(u->text, sizeof u->text, "%s", text);
    os_mutex_unlock(&u->lock);
}

static int add_mapping(const upnp *u, const char *port, const char *lan)
{
    /* Some routers take only permanent forwards (lease 0), others only
     * timed ones: a day outlasts any session. */
    int r = UPNP_AddPortMapping(u->urls.controlURL, u->data.first.servicetype, port, port, lan, "blockclonia", "UDP",
                                NULL, "0");
    if (r != UPNPCOMMAND_SUCCESS)
        r = UPNP_AddPortMapping(u->urls.controlURL, u->data.first.servicetype, port, port, lan, "blockclonia", "UDP",
                                NULL, "86400");
    return r;
}

static void run(void *arg)
{
    upnp *u = arg;
    int err = 0;
    struct UPNPDev *devs = upnpDiscover(DISCOVER_MS, NULL, NULL, UPNP_LOCAL_PORT_ANY, 0, 2, &err);
    char lan[64] = {0}, ext[40] = {0}, port[8] = {0}, text[96] = {0};
#if MINIUPNPC_API_VERSION >= 18
    int igd = devs ? UPNP_GetValidIGD(devs, &u->urls, &u->data, lan, sizeof lan, NULL, 0) : 0;
#else
    int igd = devs ? UPNP_GetValidIGD(devs, &u->urls, &u->data, lan, sizeof lan) : 0;
#endif
    freeUPNPDevlist(devs);
    u->have_urls = igd != 0;
    /* 1: a router connected to the internet; 2 means its own address is
     * private (another NAT beyond it), where a forward would not help. */
    if (igd != 1) {
        finish(u, -1, igd == 2 ? "the router is behind another router" : "no router answered UPnP (it may be off)");
        return;
    }
    snprintf(port, sizeof port, "%u", (unsigned)u->port);
    int r = add_mapping(u, port, lan);
    if (r != UPNPCOMMAND_SUCCESS) {
        snprintf(text, sizeof text, "the router refused to open the port (%s)", strupnperror(r));
        finish(u, -1, text);
        return;
    }
    if (UPNP_GetExternalIPAddress(u->urls.controlURL, u->data.first.servicetype, ext) != UPNPCOMMAND_SUCCESS ||
        !ext[0])
        snprintf(ext, sizeof ext, "?");
    snprintf(text, sizeof text, "%s:%s", ext, port);
    log_info("upnp: the router forwards UDP %s to %s; the internet reaches this game at %s", port, lan, text);
    finish(u, 1, text);
}

upnp *upnp_open(uint16_t port)
{
    upnp *u = mem_calloc(1, sizeof *u);
    u->port = port;
    os_mutex_init(&u->lock);
    if (os_thread_start(&u->thread, run, u) != 0) {
        os_mutex_destroy(&u->lock);
        mem_free(u);
        return NULL;
    }
    return u;
}

int upnp_state(upnp *u, char *out, size_t cap)
{
    os_mutex_lock(&u->lock);
    int s = u->state;
    if (s && out && cap) snprintf(out, cap, "%s", u->text);
    os_mutex_unlock(&u->lock);
    return s;
}

void upnp_close(upnp *u)
{
    if (!u) return;
    os_thread_join(u->thread);
    if (u->state == 1) {
        char port[8] = {0};
        snprintf(port, sizeof port, "%u", (unsigned)u->port);
        UPNP_DeletePortMapping(u->urls.controlURL, u->data.first.servicetype, port, "UDP", NULL);
    }
    if (u->have_urls) FreeUPNPUrls(&u->urls);
    os_mutex_destroy(&u->lock);
    mem_free(u);
}
