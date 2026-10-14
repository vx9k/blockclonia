#include "netkeys.h"
#include "log.h"
#include "mem.h"
#include "os.h"
#include "save.h"

#include <stdio.h>
#include <string.h>

#define ID_HEADER "blockclonia identity v1\n"
#define HOSTS_MAX_FILE 65536
#define HOSTS_MAX 1024

int netkeys_identity(const char *path, nc_keypair *kp)
{
    char buf[256] = {0};
    long n = save_read_file(path, buf, sizeof buf - 1);
    if (n >= 0) {
        size_t hl = strlen(ID_HEADER);
        uint8_t sk[NC_KEY_BYTES] = {0};
        char hex[2 * NC_KEY_BYTES + 1] = {0};
        int ok = (size_t)n == hl + 2 * NC_KEY_BYTES + 1 && memcmp(buf, ID_HEADER, hl) == 0 && buf[n - 1] == '\n';
        if (ok) {
            memcpy(hex, buf + hl, 2 * NC_KEY_BYTES);
            ok = nc_key_parse(hex, sk) == 0;
        }
        nc_wipe(buf, sizeof buf);
        nc_wipe(hex, sizeof hex);
        if (ok) {
            nc_keypair_from_secret(kp, sk);
            nc_wipe(sk, sizeof sk);
            return 0;
        }
        log_warn("%s is not a usable identity key; using a new one for this session only", path);
        nc_keypair_new(kp);
        return -1;
    }
    nc_keypair_new(kp);
    /* Only a missing file is made: one that exists but cannot be read (a
     * link, too big, no permission) is the player's to sort out, and
     * replacing it would change who they are to every host they know. */
    if (os_path_kind(path) != OS_MISSING) {
        log_warn("cannot read the identity key %s; using a new one for this session only", path);
        return -1;
    }
    char hex[2 * NC_KEY_BYTES + 1] = {0};
    nc_key_hex(kp->sk, hex);
    int len = snprintf(buf, sizeof buf, "%s%s\n", ID_HEADER, hex);
    int r = len > 0 && save_write_private(path, buf, (size_t)len) == 0 ? 1 : -1;
    nc_wipe(hex, sizeof hex);
    nc_wipe(buf, sizeof buf);
    if (r < 0) log_warn("cannot save the identity key to %s; it lasts this session only", path);
    return r;
}

/* A known_hosts line: "<host> <64 hex digits>". 1 if it parses. */
static int parse_line(const char *line, size_t len, char *host, uint8_t pk[NC_KEY_BYTES])
{
    const char *sp = memchr(line, ' ', len);
    if (!sp || sp == line || (size_t)(sp - line) > NETKEYS_HOST_MAX ||
        len - (size_t)(sp - line) - 1 != 2 * NC_KEY_BYTES)
        return 0;
    for (const char *c = line; c < sp; c++)
        if (*c <= ' ' || *c > '~') return 0;
    char hex[2 * NC_KEY_BYTES + 1] = {0};
    memcpy(hex, sp + 1, 2 * NC_KEY_BYTES);
    if (nc_key_parse(hex, pk) != 0) return 0;
    memcpy(host, line, (size_t)(sp - line));
    host[sp - line] = '\0';
    return 1;
}

/* Calls fn for each valid line of the file in buf; stops when fn returns 1. */
typedef int (*line_fn)(void *user, const char *line, size_t len, const char *host, const uint8_t *pk);

static void each_line(const char *buf, long n, line_fn fn, void *user)
{
    for (long i = 0; n > 0 && i < n;) {
        const char *nl = memchr(buf + i, '\n', (size_t)(n - i));
        size_t len = nl ? (size_t)(nl - (buf + i)) : (size_t)(n - i);
        char h[NETKEYS_HOST_MAX + 1] = {0};
        uint8_t k[NC_KEY_BYTES] = {0};
        if (parse_line(buf + i, len, h, k) && fn(user, buf + i, len, h, k)) return;
        i += (long)len + 1;
    }
}

typedef struct {
    const char *host;
    uint8_t *pk;
    int found;
} find_ctx;

static int find_host(void *user, const char *line, size_t len, const char *host, const uint8_t *pk)
{
    find_ctx *f = user;
    (void)line;
    (void)len;
    if (strcmp(host, f->host) != 0) return 0;
    memcpy(f->pk, pk, NC_KEY_BYTES);
    f->found = 1;
    return 1;
}

int netkeys_known(const char *path, const char *host, uint8_t pk[NC_KEY_BYTES])
{
    char *buf = mem_alloc(HOSTS_MAX_FILE);
    long n = save_read_file(path, buf, HOSTS_MAX_FILE);
    find_ctx f = {host, pk, 0};
    if (n < 0 && os_path_kind(path) != OS_MISSING) f.found = -1;
    else each_line(buf, n, find_host, &f);
    mem_free(buf);
    return f.found;
}

typedef struct {
    const char *host;
    char *out;
    size_t len;
    int others, skip;
} keep_ctx;

static int count_other(void *user, const char *line, size_t len, const char *host, const uint8_t *pk)
{
    keep_ctx *k = user;
    (void)line;
    (void)len;
    (void)pk;
    k->others += strcmp(host, k->host) != 0;
    return 0;
}

static int keep_other(void *user, const char *line, size_t len, const char *host, const uint8_t *pk)
{
    keep_ctx *k = user;
    (void)pk;
    if (strcmp(host, k->host) == 0 || k->skip-- > 0) return 0;
    memcpy(k->out + k->len, line, len);
    k->len += len;
    k->out[k->len++] = '\n';
    return 0;
}

int netkeys_pin(const char *path, const char *host, const uint8_t pk[NC_KEY_BYTES])
{
    size_t hl = strlen(host);
    if (hl < 1 || hl > NETKEYS_HOST_MAX) return -1;
    for (size_t i = 0; i < hl; i++)
        if (host[i] <= ' ' || host[i] > '~') return -1;
    size_t cap = HOSTS_MAX_FILE + NETKEYS_HOST_MAX + 2 * NC_KEY_BYTES + 3;
    char *old = mem_alloc(HOSTS_MAX_FILE);
    char *out = mem_alloc(cap);
    long n = save_read_file(path, old, HOSTS_MAX_FILE);
    if (n < 0 && os_path_kind(path) != OS_MISSING) {
        /* Rewriting what cannot be read would drop every other pin. */
        mem_free(old);
        mem_free(out);
        return -1;
    }
    /* Every other valid line stays, this host's goes last; past HOSTS_MAX
     * the oldest are dropped. */
    keep_ctx k = {host, out, 0, 0, 0};
    each_line(old, n, count_other, &k);
    k.skip = k.others >= HOSTS_MAX ? k.others - HOSTS_MAX + 1 : 0;
    each_line(old, n, keep_other, &k);
    char hex[2 * NC_KEY_BYTES + 1] = {0};
    nc_key_hex(pk, hex);
    int w = k.len < cap ? snprintf(out + k.len, cap - k.len, "%s %s\n", host, hex) : -1;
    int r = w > 0 && k.len + (size_t)w <= HOSTS_MAX_FILE && save_write_file(path, out, k.len + (size_t)w) == 0 ? 0 : -1;
    mem_free(old);
    mem_free(out);
    return r;
}
