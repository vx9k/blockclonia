/* The Discord status (discord.c) over a socketpair standing in for the
 * Discord client: frames and JSON escaping, the handshake, waiting for
 * READY, the update limit, PINGs, and hanging up on a malformed or
 * oversized frame. */
#include "discord.h"
#include "test_util.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Reads one whole frame the game sent, without blocking: 1 if there was
 * one, 0 if nothing is waiting. */
static int peer_read(int fd, uint32_t *op, char *json, size_t cap)
{
    uint8_t head[8] = {0};
    ssize_t got = recv(fd, head, sizeof head, MSG_DONTWAIT);
    if (got != (ssize_t)sizeof head) return 0;
    uint32_t len = get_u32(head + 4);
    if (len >= cap) return 0;
    got = recv(fd, json, len, MSG_DONTWAIT);
    if (got != (ssize_t)len) return 0;
    json[len] = '\0';
    *op = get_u32(head);
    return 1;
}

static void peer_send(int fd, uint32_t op, const char *json)
{
    uint8_t buf[256] = {0};
    size_t len = discord_frame(op, json, buf, sizeof buf);
    CHECK(len > 0);
    CHECK(send(fd, buf, len, 0) == (ssize_t)len);
}

static void test_frame_and_escape(void)
{
    uint8_t buf[16] = {0};
    CHECK(discord_frame(DISCORD_OP_FRAME, "{}", buf, sizeof buf) == 10);
    CHECK(get_u32(buf) == DISCORD_OP_FRAME && get_u32(buf + 4) == 2 && memcmp(buf + 8, "{}", 2) == 0);
    CHECK(discord_frame(DISCORD_OP_FRAME, "{}", buf, 9) == 0); /* does not fit */

    char out[64] = {0};
    CHECK(discord_json_escape("a\"b\\c\n", out, sizeof out) == 13);
    CHECK(strcmp(out, "a\\\"b\\\\c\\u000a") == 0);
    discord_json_escape("caf\xc3\xa9", out, sizeof out);
    CHECK(strcmp(out, "caf??") == 0);
    /* Cut short between escapes, never inside one. */
    CHECK(discord_json_escape("\"\"\"", out, 4) == 2);
    CHECK(strcmp(out, "\\\"") == 0);
}

static void test_create(void)
{
    CHECK(discord_create(NULL) == NULL);
    CHECK(discord_create("") == NULL);
    CHECK(discord_create("12a4") == NULL);
    CHECK(discord_create("123456789012345678901") == NULL); /* 21 digits */
    discord *d = discord_create("123456789012345678");
    CHECK(d != NULL);
    CHECK(!discord_connected(d));
    discord_destroy(d);
    discord_destroy(NULL);
}

static void test_session(void)
{
    int sv[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    discord *d = discord_create("42");
    discord_attach_fd(d, sv[0]);
    char json[4096] = {0};
    uint32_t op = 99;
    CHECK(peer_read(sv[1], &op, json, sizeof json));
    CHECK(op == DISCORD_OP_HANDSHAKE && strstr(json, "\"client_id\":\"42\"") != NULL);

    /* Nothing is sent before Discord says READY. */
    discord_set(d, "Almost dead (\"x\")", "In Home", 100);
    discord_poll(d, 1.0);
    CHECK(!peer_read(sv[1], &op, json, sizeof json));

    /* READY arriving in two pieces still counts. */
    uint8_t ready[64] = {0};
    size_t rl = discord_frame(DISCORD_OP_FRAME, "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\"}", ready, sizeof ready);
    CHECK(send(sv[1], ready, 5, 0) == 5);
    discord_poll(d, 2.0);
    CHECK(send(sv[1], ready + 5, rl - 5, 0) == (ssize_t)(rl - 5));
    discord_poll(d, 2.0);
    CHECK(peer_read(sv[1], &op, json, sizeof json));
    CHECK(op == DISCORD_OP_FRAME && strstr(json, "\"cmd\":\"SET_ACTIVITY\"") != NULL);
    CHECK(strstr(json, "\"details\":\"Almost dead (\\\"x\\\")\"") != NULL);
    CHECK(strstr(json, "\"state\":\"In Home\"") != NULL);
    CHECK(strstr(json, "\"start\":100") != NULL);

    /* The same status is not sent again; a new one waits out the limit. */
    discord_set(d, "Almost dead (\"x\")", "In Home", 100);
    discord_poll(d, 3.0);
    CHECK(!peer_read(sv[1], &op, json, sizeof json));
    discord_set(d, "Healthy", NULL, 100);
    discord_poll(d, 10.0);
    CHECK(!peer_read(sv[1], &op, json, sizeof json));
    discord_poll(d, 17.5);
    CHECK(peer_read(sv[1], &op, json, sizeof json));
    CHECK(strstr(json, "\"details\":\"Healthy\"") != NULL && strstr(json, "\"state\"") == NULL);

    /* A PING is answered with its own payload. */
    peer_send(sv[1], DISCORD_OP_PING, "{\"n\":7}");
    discord_poll(d, 18.0);
    CHECK(peer_read(sv[1], &op, json, sizeof json));
    CHECK(op == DISCORD_OP_PONG && strcmp(json, "{\"n\":7}") == 0);

    /* No details clears the status. */
    discord_set(d, NULL, NULL, 0);
    discord_poll(d, 40.0);
    CHECK(peer_read(sv[1], &op, json, sizeof json));
    CHECK(strstr(json, "SET_ACTIVITY") != NULL && strstr(json, "\"activity\"") == NULL);

    /* CLOSE from Discord ends the connection. */
    peer_send(sv[1], DISCORD_OP_CLOSE, "{}");
    discord_poll(d, 41.0);
    CHECK(!discord_connected(d));
    discord_destroy(d);
    close(sv[1]);

    /* READY written with spaces, as other clients of the IPC may. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    d = discord_create("42");
    discord_attach_fd(d, sv[0]);
    CHECK(peer_read(sv[1], &op, json, sizeof json));
    discord_set(d, "Healthy", NULL, 0);
    peer_send(sv[1], DISCORD_OP_FRAME, "{\"evt\" : \"READY\", \"cmd\": \"DISPATCH\"}");
    discord_poll(d, 1.0);
    CHECK(peer_read(sv[1], &op, json, sizeof json));
    CHECK(strstr(json, "SET_ACTIVITY") != NULL);
    /* "READY" anywhere but in "evt" does not count. */
    discord_destroy(d);
    close(sv[1]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    d = discord_create("42");
    discord_attach_fd(d, sv[0]);
    CHECK(peer_read(sv[1], &op, json, sizeof json));
    discord_set(d, "Healthy", NULL, 0);
    peer_send(sv[1], DISCORD_OP_FRAME, "{\"evt\":\"ERROR\",\"data\":{\"message\":\"READY\"}}");
    discord_poll(d, 1.0);
    CHECK(!peer_read(sv[1], &op, json, sizeof json));
    peer_send(sv[1], DISCORD_OP_CLOSE, "{}");
    discord_poll(d, 41.0);
    CHECK(!discord_connected(d));
    discord_destroy(d);
    close(sv[1]);
}

/* What Discord sends is untrusted: a frame claiming more than
 * DISCORD_RX_MAX bytes, or a peer that goes away, hangs up rather than
 * waiting or reading past the buffer. */
static void test_hostile_peer(void)
{
    int sv[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    discord *d = discord_create("42");
    discord_attach_fd(d, sv[0]);
    uint8_t head[8] = {0};
    put_u32(head, DISCORD_OP_FRAME);
    put_u32(head + 4, DISCORD_RX_MAX + 1u);
    CHECK(send(sv[1], head, sizeof head, 0) == (ssize_t)sizeof head);
    discord_poll(d, 1.0);
    CHECK(!discord_connected(d));
    discord_poll(d, 100.0); /* attached for the tests: never dials Discord's socket */
    CHECK(!discord_connected(d));
    discord_destroy(d);
    close(sv[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    d = discord_create("42");
    discord_attach_fd(d, sv[0]);
    close(sv[1]);
    discord_set(d, "Healthy", NULL, 0);
    discord_poll(d, 1.0);
    CHECK(!discord_connected(d));
    discord_destroy(d);
}

void test_discord_all(void)
{
    test_frame_and_escape();
    test_create();
    test_session();
    test_hostile_peer();
}
