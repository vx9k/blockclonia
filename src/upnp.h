/* Opening a port on the home router with UPnP, so friends on the internet
 * can join a host by its address alone. The router is found, asked to
 * forward the game's UDP port to this machine and asked for its public
 * address on a thread of its own (discovery takes a couple of seconds);
 * closing removes the forward again.
 *
 * Everything the router says is untrusted (miniupnpc parses it, and only
 * the address is kept, as text for the chat). Not here: what to do when
 * the router refuses (net.c's hole punching), IPv6, NAT-PMP/PCP. */
#ifndef MC_UPNP_H
#define MC_UPNP_H

#include <stddef.h>
#include <stdint.h>

typedef struct upnp upnp;

/* Starts asking the router to forward UDP port to this machine. */
upnp *upnp_open(uint16_t port);
/* 0 while asking, 1 once forwarded (out: "a.b.c.d:port" as the internet
 * sees it), -1 if there is no router that does UPnP or it refused (out:
 * why). */
int upnp_state(upnp *u, char *out, size_t cap);
/* Removes the forward and frees everything. Waits for the asking to
 * finish. NULL is fine. */
void upnp_close(upnp *u);

#endif
