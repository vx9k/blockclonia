#include "discord.h"
#include "log.h"
#include "mem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SEND_EVERY 15.0  /* s; Discord allows about five updates in 20 s */
#define BACKOFF_MIN 5.0
#define BACKOFF_MAX 60.0
#define TEXT_MAX 128

struct discord {
    char id[24];
    int fd;          /* -1: not connected */
    int ready;       /* READY seen: activity may be sent */
    int no_dial;     /* tests: never look for Discord's socket */
    double next_try, backoff, last_sent;
    int dirty;       /* what to show differs from what was sent */
    int have;        /* there is something to show (details != NULL) */
    char details[TEXT_MAX], state[TEXT_MAX];
    int64_t start;
    uint32_t nonce;
    uint8_t rx[8 + DISCORD_RX_MAX];
    size_t rx_len;
};

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

size_t discord_frame(uint32_t op, const char *json, uint8_t *out, size_t cap)
{
    size_t len = strlen(json);
    if (len > UINT32_MAX || cap < 8 || len > cap - 8) return 0;
    put_u32(out, op);
    put_u32(out + 4, (uint32_t)len);
    /* A frame is length-prefixed bytes, not a C string: no terminator. */
    memcpy(out + 8, json, len); // NOLINT(bugprone-not-null-terminated-result)
    return 8 + len;
}

size_t discord_json_escape(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    if (cap == 0) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8] = {0};
        unsigned char c = *p;
        if (c == '"' || c == '\\') snprintf(esc, sizeof esc, "\\%c", c);
        else if (c < 0x20) snprintf(esc, sizeof esc, "\\u%04x", c);
        else if (c > 0x7e) snprintf(esc, sizeof esc, "?");
        else snprintf(esc, sizeof esc, "%c", c);
        size_t k = strlen(esc);
        if (n + k >= cap) break;
        memcpy(out + n, esc, k);
        n += k;
    }
    out[n] = '\0';
    return n;
}

discord *discord_create(const char *client_id)
{
    size_t len = client_id ? strlen(client_id) : 0;
    if (len == 0 || len > 20) return NULL;
    for (size_t i = 0; i < len; i++)
        if (client_id[i] < '0' || client_id[i] > '9') return NULL;
    discord *d = mem_calloc(1, sizeof *d);
    memcpy(d->id, client_id, len);
    d->fd = -1;
    d->backoff = BACKOFF_MIN;
    d->last_sent = -1e9;
    return d;
}

static void hang_up(discord *d, double now)
{
    if (d->fd >= 0) close(d->fd);
    d->fd = -1;
    d->ready = 0;
    d->rx_len = 0;
    d->dirty = 1; /* show it again after reconnecting */
    d->next_try = now + d->backoff;
    d->backoff = d->backoff * 2.0 > BACKOFF_MAX ? BACKOFF_MAX : d->backoff * 2.0;
}

void discord_destroy(discord *d)
{
    if (!d) return;
    if (d->fd >= 0) close(d->fd);
    mem_free(d);
}

int discord_connected(const discord *d) { return d && d->fd >= 0; }

/* One frame in one write: Discord drops frames sent in pieces. A frame
 * the socket cannot take whole now ends the connection rather than
 * blocking the game or leaving half a frame behind. */
static int send_frame(discord *d, uint32_t op, const char *json, double now)
{
    uint8_t buf[8 + 2048] = {0};
    size_t len = discord_frame(op, json, buf, sizeof buf);
    if (!len) return -1;
#ifdef MSG_NOSIGNAL
    ssize_t sent = send(d->fd, buf, len, MSG_NOSIGNAL);
#else
    ssize_t sent = send(d->fd, buf, len, 0); /* SO_NOSIGPIPE is set on the socket */
#endif
    if (sent != (ssize_t)len) {
        hang_up(d, now);
        return -1;
    }
    return 0;
}

static void prepare_fd(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

static void handshake(discord *d, double now)
{
    char json[96] = {0};
    snprintf(json, sizeof json, "{\"v\":1,\"client_id\":\"%s\"}", d->id);
    send_frame(d, DISCORD_OP_HANDSHAKE, json, now);
}

void discord_attach_fd(discord *d, int fd)
{
    if (d->fd >= 0) close(d->fd);
    prepare_fd(fd);
    d->fd = fd;
    d->no_dial = 1;
    d->ready = 0;
    d->rx_len = 0;
    handshake(d, 0.0);
}

/* Where the Discord client listens: discord-ipc-0..9 in the first temp
 * folder the environment names, also inside the Flatpak and Snap
 * sandboxes' folders there. */
static int dial(discord *d, double now)
{
    static const char *const VARS[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
    static const char *const SUBS[] = {"", "app/com.discordapp.Discord/", "snap.discord/"};
    const char *base = "/tmp";
    for (size_t i = 0; i < sizeof VARS / sizeof VARS[0]; i++) {
        const char *v = getenv(VARS[i]);
        if (v && v[0]) {
            base = v;
            break;
        }
    }
    for (size_t s = 0; s < sizeof SUBS / sizeof SUBS[0]; s++)
        for (int n = 0; n < 10; n++) {
            struct sockaddr_un addr = {0};
            addr.sun_family = AF_UNIX;
            int len = snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%sdiscord-ipc-%d", base, SUBS[s], n);
            if (len < 0 || (size_t)len >= sizeof addr.sun_path) continue;
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return -1;
            prepare_fd(fd);
            if (connect(fd, (const struct sockaddr *)&addr, sizeof addr) == 0) {
                d->fd = fd;
                d->backoff = BACKOFF_MIN;
                log_info("discord: connected to %s", addr.sun_path);
                handshake(d, now);
                return 0;
            }
            close(fd);
        }
    return -1;
}

/* Whether a frame is the READY event. Only the "evt" field is looked at,
 * allowing the whitespace JSON permits around the colon: Discord sends
 * compact JSON, other clients speaking its IPC may not. */
static int is_ready(const char *json)
{
    const char *p = strstr(json, "\"evt\"");
    if (!p) return 0;
    p += 5;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return strncmp(p, "\"READY\"", 7) == 0;
}

/* Reads what Discord sent and acts on whole frames. */
static void read_frames(discord *d, double now)
{
    for (;;) {
        if (d->rx_len == sizeof d->rx) {
            hang_up(d, now);
            return;
        }
        ssize_t got = recv(d->fd, d->rx + d->rx_len, sizeof d->rx - d->rx_len, 0);
        if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            hang_up(d, now);
            return;
        }
        if (got < 0) break;
        d->rx_len += (size_t)got;
        while (d->rx_len >= 8) {
            uint32_t op = get_u32(d->rx), len = get_u32(d->rx + 4);
            if (len > DISCORD_RX_MAX) {
                log_warn("discord: oversized frame (%u bytes); disconnecting", len);
                hang_up(d, now);
                return;
            }
            if (d->rx_len < 8 + (size_t)len) break;
            char json[DISCORD_RX_MAX + 1];
            memcpy(json, d->rx + 8, len);
            json[len] = '\0';
            memmove(d->rx, d->rx + 8 + len, d->rx_len - 8 - len);
            d->rx_len -= 8 + len;
            if (op == DISCORD_OP_CLOSE) {
                hang_up(d, now);
                return;
            }
            if (op == DISCORD_OP_PING) {
                if (send_frame(d, DISCORD_OP_PONG, json, now) != 0) return;
            } else if (op == DISCORD_OP_FRAME && !d->ready && is_ready(json)) {
                d->ready = 1;
                d->dirty = 1;
            }
        }
    }
}

static void send_activity(discord *d, double now)
{
    char json[1536] = {0};
    int pid = (int)getpid();
    unsigned nonce = ++d->nonce;
    int n = 0;
    if (!d->have) {
        n = snprintf(json, sizeof json, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d},\"nonce\":\"%u\"}", pid, nonce);
    } else {
        char det[TEXT_MAX * 6] = {0}, st[TEXT_MAX * 6] = {0}, when[48] = {0};
        discord_json_escape(d->details, det, sizeof det);
        discord_json_escape(d->state, st, sizeof st);
        if (d->start > 0) snprintf(when, sizeof when, ",\"timestamps\":{\"start\":%lld}", (long long)d->start);
        n = snprintf(json, sizeof json,
                     "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{\"type\":0,\"details\":\"%s\"%s%s%s%s,"
                     "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"blockclonia\"}}},\"nonce\":\"%u\"}",
                     pid, det, st[0] ? ",\"state\":\"" : "", st, st[0] ? "\"" : "", when, nonce);
    }
    if (n <= 0 || (size_t)n >= sizeof json) return;
    if (send_frame(d, DISCORD_OP_FRAME, json, now) == 0) {
        d->dirty = 0;
        d->last_sent = now;
    }
}

void discord_set(discord *d, const char *details, const char *state, int64_t start)
{
    if (!d) return;
    int have = details != NULL;
    if (!state) state = "";
    if (!details) details = "";
    if (have == d->have && start == d->start && strncmp(details, d->details, TEXT_MAX - 1) == 0 &&
        strncmp(state, d->state, TEXT_MAX - 1) == 0)
        return;
    d->have = have;
    d->start = start;
    snprintf(d->details, sizeof d->details, "%s", details);
    snprintf(d->state, sizeof d->state, "%s", state);
    d->dirty = 1;
}

void discord_poll(discord *d, double now)
{
    if (!d) return;
    if (d->fd < 0) {
        if (d->no_dial || now < d->next_try) return;
        if (dial(d, now) != 0) {
            d->next_try = now + d->backoff;
            d->backoff = d->backoff * 2.0 > BACKOFF_MAX ? BACKOFF_MAX : d->backoff * 2.0;
            return;
        }
        if (d->fd < 0) return; /* the handshake could not be sent: hung up already */
    }
    read_frames(d, now);
    if (d->fd >= 0 && d->ready && d->dirty && now - d->last_sent >= SEND_EVERY) send_activity(d, now);
}
