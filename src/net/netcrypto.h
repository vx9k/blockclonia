/* Multiplayer encryption with libsodium: a long-lived identity key per
 * player, the handshake that turns two hellos into session keys, and
 * sealing every packet with ChaCha20-Poly1305 (IETF, full AEAD).
 *
 * The handshake mixes three X25519 results, ephemeral-ephemeral and each
 * side's identity with the other's ephemeral key, with the whole
 * transcript, so the keys are fresh per connection (forward secrecy) and
 * only the holders of the two identity keys can derive them. Nothing is
 * signed: a peer proves its identity by being able to read and write.
 * Deciding whether an identity key is the expected one (pinning it on
 * first contact, or checking it against the authority's list of peers)
 * is the caller's job.
 *
 * Every byte that arrives is untrusted: opening checks the tag before
 * anything else and drops replays. Sockets, peers and message formats are
 * net.c's and netmsg.c's business, not this module's. */
#ifndef MC_NETCRYPTO_H
#define MC_NETCRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define NC_KEY_BYTES 32
#define NC_HELLO_BYTES (4 + 2 * NC_KEY_BYTES) /* magic, identity key, ephemeral key */
#define NC_CHANNELS 4                         /* separate counters, so channels may reorder */
#define NC_OVERHEAD (1 + 8 + 16)              /* channel, counter, tag */

typedef struct {
    uint8_t pk[NC_KEY_BYTES], sk[NC_KEY_BYTES];
} nc_keypair;

typedef struct {
    uint8_t tx[NC_KEY_BYTES], rx[NC_KEY_BYTES];
    uint8_t peer[NC_KEY_BYTES]; /* the other side's identity key */
    uint64_t tx_next[NC_CHANNELS];
    /* Replay window per channel: the highest counter accepted, and a bit
     * for each of the 64 below it that has been seen. */
    uint64_t rx_top[NC_CHANNELS], rx_seen[NC_CHANNELS];
    uint8_t rx_any[NC_CHANNELS];
} nc_session;

/* Starts libsodium; 0 on success. Safe to call more than once. */
int nc_init(void);
void nc_keypair_new(nc_keypair *kp);
/* The key pair whose secret half is sk. */
void nc_keypair_from_secret(nc_keypair *kp, const uint8_t sk[NC_KEY_BYTES]);

/* The hello this side sends: identity key and a fresh ephemeral key,
 * whose secret half eph keeps for nc_session_derive. */
void nc_hello_make(const nc_keypair *id, nc_keypair *eph, uint8_t out[NC_HELLO_BYTES]);

/* Session keys from both hellos. Roles come from the identity keys, so it
 * does not matter who dialled. -1 for a malformed hello, the same
 * identity on both ends, or a key that gives a degenerate shared secret;
 * s is left zeroed then. Wipes eph either way. */
int nc_session_derive(nc_session *s, const nc_keypair *id, nc_keypair *eph, const uint8_t mine[NC_HELLO_BYTES],
                      const uint8_t theirs[NC_HELLO_BYTES]);

/* Seals len bytes for channel ch into out (len + NC_OVERHEAD bytes);
 * returns the sealed length, or 0 if out is too small or ch is invalid. */
size_t nc_seal(nc_session *s, int ch, const uint8_t *msg, size_t len, uint8_t *out, size_t cap);
/* Opens a sealed packet into out (len - NC_OVERHEAD bytes); returns the
 * plaintext length, or -1 for a forged, damaged, replayed or too-old
 * packet. The replay window only moves for packets that authenticate. */
long nc_open(nc_session *s, const uint8_t *in, size_t len, uint8_t *out, size_t cap, int *ch);

/* A key as 64 hex digits plus NUL, and back (-1 unless exactly that). */
void nc_key_hex(const uint8_t key[NC_KEY_BYTES], char out[2 * NC_KEY_BYTES + 1]);
int nc_key_parse(const char *hex, uint8_t key[NC_KEY_BYTES]);

/* The safety number of two identity keys: 30 digits in groups of five,
 * the same whichever key comes first. Two players who read the same
 * number to each other (in person, by voice) know that each one's chat
 * is sealed to the other's real key, and not to a key a host slipped in
 * between them. */
#define NC_SAFETY_CHARS 36 /* six groups of five digits, spaces, NUL */
void nc_safety_number(const uint8_t a[NC_KEY_BYTES], const uint8_t b[NC_KEY_BYTES], char out[NC_SAFETY_CHARS]);

/* Wipes keys from memory (the compiler may not drop it). */
void nc_wipe(void *p, size_t n);

#endif
