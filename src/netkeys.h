/* Multiplayer keys on disk: this player's identity (a secret only this
 * user can read) and the identity keys of hosts joined before, pinned by
 * address on first contact the way SSH's known_hosts is. Both are small
 * text files, read through save_read_file and treated as untrusted; a
 * malformed line is skipped, never trusted. Nothing here touches the
 * network. */
#ifndef MC_NETKEYS_H
#define MC_NETKEYS_H

#include "netcrypto.h"

#define NETKEYS_HOST_MAX 255 /* "address:port" */

/* This player's identity from path, made and saved (mode 0600) only if
 * the file is missing. Returns 0 when loaded, 1 when created, or -1 when
 * the file exists but is unusable, or cannot be written: kp is then a
 * fresh key for this session only, and the file is left alone. */
int netkeys_identity(const char *path, nc_keypair *kp);
/* The key pinned for host in path: 1 and pk, 0 if none, or -1 if the
 * file exists but cannot be read (then nothing can be checked). */
int netkeys_known(const char *path, const char *host, uint8_t pk[NC_KEY_BYTES]);
/* Pins (or re-pins) host's key in path. 0, or -1 if it cannot be saved;
 * a file that exists but cannot be read is never written over. */
int netkeys_pin(const char *path, const char *host, const uint8_t pk[NC_KEY_BYTES]);

#endif
