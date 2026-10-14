/* Discord Rich Presence ("Playing blockclonia: In the menus") over the
 * Discord client's local IPC socket, without Discord's SDK: frames of a
 * little-endian opcode and length, then JSON. Nothing blocks and nothing
 * here can stop the game: the socket is non-blocking, a closed peer never
 * raises SIGPIPE, reconnecting backs off, and an update is sent only when
 * it changed and at most every 15 s.
 *
 * What Discord sends back is untrusted: frames are size-capped, PINGs are
 * answered, READY is noticed and everything else is read and dropped.
 * Strings sent are JSON-escaped. The player turns this on in the settings;
 * it does not decide what to show (main.c does). */
#ifndef MC_DISCORD_H
#define MC_DISCORD_H

#include <stddef.h>
#include <stdint.h>

typedef struct discord discord;

/* NULL unless client_id is a Discord application id (1-20 digits). */
discord *discord_create(const char *client_id);
/* Closes the connection, which also clears the presence. */
void discord_destroy(discord *d);

/* What to show. Copied; sent when it differs from what was last sent.
 * details NULL clears the presence. start: Unix seconds the "elapsed"
 * clock counts from, 0 for none. */
void discord_set(discord *d, const char *details, const char *state, int64_t start);
/* Connects, reads and writes as needed; call once a frame with a
 * monotonic clock in seconds. Never blocks. */
void discord_poll(discord *d, double now);
int discord_connected(const discord *d);

/* For the tests: use an already connected socket instead of looking for
 * Discord's, and never dial one. */
void discord_attach_fd(discord *d, int fd);
/* An IPC frame (opcode, length, json) into out; returns its length, or
 * 0 if it does not fit. */
size_t discord_frame(uint32_t op, const char *json, uint8_t *out, size_t cap);
/* s as the inside of a JSON string (quotes, backslashes and control
 * characters escaped, bytes outside ASCII replaced by '?'); returns the
 * length written, cut short (never mid-escape) if cap is too small. */
size_t discord_json_escape(const char *s, char *out, size_t cap);

#define DISCORD_OP_HANDSHAKE 0u
#define DISCORD_OP_FRAME 1u
#define DISCORD_OP_CLOSE 2u
#define DISCORD_OP_PING 3u
#define DISCORD_OP_PONG 4u
#define DISCORD_RX_MAX 16384 /* the largest frame accepted from Discord */

#endif
