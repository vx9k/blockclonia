#include "netcrypto.h"

#include <sodium.h>
#include <string.h>

/* Bumped with any change to the handshake or the packet layout, so two
 * builds that disagree fail the handshake instead of misreading. */
static const uint8_t HELLO_MAGIC[4] = {'B', 'C', 'N', '1'};
static const char KDF_LABEL[] = "blockclonia net v1 session keys";
#define HEAD_BYTES 9 /* channel, then the counter, little-endian */

int nc_init(void) { return sodium_init() < 0 ? -1 : 0; }

void nc_keypair_new(nc_keypair *kp) { crypto_kx_keypair(kp->pk, kp->sk); }

void nc_wipe(void *p, size_t n) { sodium_memzero(p, n); }

void nc_hello_make(const nc_keypair *id, nc_keypair *eph, uint8_t out[NC_HELLO_BYTES])
{
    nc_keypair_new(eph);
    memcpy(out, HELLO_MAGIC, sizeof HELLO_MAGIC);
    memcpy(out + 4, id->pk, NC_KEY_BYTES);
    memcpy(out + 4 + NC_KEY_BYTES, eph->pk, NC_KEY_BYTES);
}

int nc_session_derive(nc_session *s, const nc_keypair *id, nc_keypair *eph, const uint8_t mine[NC_HELLO_BYTES],
                      const uint8_t theirs[NC_HELLO_BYTES])
{
    memset(s, 0, sizeof *s);
    const uint8_t *their_id = theirs + 4, *their_eph = theirs + 4 + NC_KEY_BYTES;
    int bad = memcmp(theirs, HELLO_MAGIC, sizeof HELLO_MAGIC) != 0 || memcmp(mine, HELLO_MAGIC, 4) != 0 ||
              memcmp(mine + 4, id->pk, NC_KEY_BYTES) != 0 ||
              memcmp(mine + 4 + NC_KEY_BYTES, eph->pk, NC_KEY_BYTES) != 0;
    int order = memcmp(id->pk, their_id, NC_KEY_BYTES);
    if (bad || order == 0) {
        nc_wipe(eph, sizeof *eph);
        return -1;
    }
    /* The side with the lower identity key is "low". d1 is low's identity
     * with high's ephemeral key and d2 the reverse; each side computes
     * both from its own secrets, in the same order. */
    int low = order < 0;
    uint8_t ee[32] = {0}, se[32] = {0}, es[32] = {0};
    int fail = crypto_scalarmult(ee, eph->sk, their_eph) != 0;
    fail |= crypto_scalarmult(se, id->sk, their_eph) != 0;
    fail |= crypto_scalarmult(es, eph->sk, their_id) != 0;
    nc_wipe(eph, sizeof *eph);

    uint8_t keys[2 * NC_KEY_BYTES] = {0};
    if (!fail) {
        crypto_generichash_state h;
        crypto_generichash_init(&h, NULL, 0, sizeof keys);
        crypto_generichash_update(&h, (const uint8_t *)KDF_LABEL, sizeof KDF_LABEL - 1);
        crypto_generichash_update(&h, ee, sizeof ee);
        crypto_generichash_update(&h, low ? se : es, 32);
        crypto_generichash_update(&h, low ? es : se, 32);
        crypto_generichash_update(&h, low ? mine : theirs, NC_HELLO_BYTES);
        crypto_generichash_update(&h, low ? theirs : mine, NC_HELLO_BYTES);
        crypto_generichash_final(&h, keys, sizeof keys);
        nc_wipe(&h, sizeof h);
        /* First half carries low -> high, second half high -> low. */
        memcpy(s->tx, keys + (low ? 0 : NC_KEY_BYTES), NC_KEY_BYTES);
        memcpy(s->rx, keys + (low ? NC_KEY_BYTES : 0), NC_KEY_BYTES);
        memcpy(s->peer, their_id, NC_KEY_BYTES);
    }
    nc_wipe(ee, sizeof ee);
    nc_wipe(se, sizeof se);
    nc_wipe(es, sizeof es);
    nc_wipe(keys, sizeof keys);
    return fail ? -1 : 0;
}

/* The header doubles as the nonce (channel, three zero bytes, counter) and
 * as the associated data, so a packet moved to another channel or counter
 * fails to open. */
static void make_nonce(uint8_t nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES], const uint8_t head[HEAD_BYTES])
{
    memset(nonce, 0, crypto_aead_chacha20poly1305_IETF_NPUBBYTES);
    nonce[0] = head[0];
    memcpy(nonce + 4, head + 1, 8);
}

size_t nc_seal(nc_session *s, int ch, const uint8_t *msg, size_t len, uint8_t *out, size_t cap)
{
    if (ch < 0 || ch >= NC_CHANNELS || cap < NC_OVERHEAD || len > cap - NC_OVERHEAD) return 0;
    uint64_t ctr = s->tx_next[ch];
    if (ctr == UINT64_MAX) return 0; /* never reached; a reused nonce would leak the key stream */
    s->tx_next[ch] = ctr + 1;
    out[0] = (uint8_t)ch;
    for (int i = 0; i < 8; i++) out[1 + i] = (uint8_t)(ctr >> (8 * i));
    uint8_t nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES] = {0};
    make_nonce(nonce, out);
    unsigned long long clen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(out + HEAD_BYTES, &clen, msg, len, out, HEAD_BYTES, NULL, nonce, s->tx);
    return HEAD_BYTES + (size_t)clen;
}

long nc_open(nc_session *s, const uint8_t *in, size_t len, uint8_t *out, size_t cap, int *ch)
{
    if (len < NC_OVERHEAD || in[0] >= NC_CHANNELS || len - NC_OVERHEAD > cap) return -1;
    int c = in[0];
    uint64_t ctr = 0;
    for (int i = 0; i < 8; i++) ctr |= (uint64_t)in[1 + i] << (8 * i);
    /* Replays and packets older than the window are refused before any
     * work is spent on them. */
    if (s->rx_any[c] && ctr <= s->rx_top[c]) {
        uint64_t back = s->rx_top[c] - ctr;
        if (back >= 64 || (s->rx_seen[c] >> back & 1u)) return -1;
    }
    uint8_t nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES] = {0};
    make_nonce(nonce, in);
    unsigned long long mlen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(out, &mlen, NULL, in + HEAD_BYTES, len - HEAD_BYTES, in, HEAD_BYTES,
                                                  nonce, s->rx) != 0)
        return -1;
    /* Bit i of rx_seen stands for counter rx_top - i. */
    if (!s->rx_any[c] || ctr > s->rx_top[c]) {
        uint64_t shift = s->rx_any[c] ? ctr - s->rx_top[c] : 64;
        s->rx_seen[c] = (shift >= 64 ? 0 : s->rx_seen[c] << shift) | 1u;
        s->rx_top[c] = ctr;
        s->rx_any[c] = 1;
    } else {
        s->rx_seen[c] |= (uint64_t)1 << (s->rx_top[c] - ctr);
    }
    if (ch) *ch = c;
    return (long)mlen;
}

void nc_key_hex(const uint8_t key[NC_KEY_BYTES], char out[2 * NC_KEY_BYTES + 1])
{
    sodium_bin2hex(out, 2 * NC_KEY_BYTES + 1, key, NC_KEY_BYTES);
}

int nc_key_parse(const char *hex, uint8_t key[NC_KEY_BYTES])
{
    size_t got = 0;
    const char *end = NULL;
    if (strlen(hex) != 2 * NC_KEY_BYTES ||
        sodium_hex2bin(key, NC_KEY_BYTES, hex, 2 * NC_KEY_BYTES, NULL, &got, &end) != 0 || got != NC_KEY_BYTES ||
        *end != '\0')
        return -1;
    return 0;
}
