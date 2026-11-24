/* The save queue and the database's writer thread: batches merge row by
 * row, reads see queued bytes before the writer commits them, a direct
 * write beats an older queued one, and a game killed mid-save opens to
 * an intact world holding every save it had been told was committed. */
#include "health.h"
#include "mem.h"
#include "os.h"
#include "save_batch.h"
#include "save_db.h"
#include "test_util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void remove_world(const char *dir)
{
    const char *files[] = {"world.db", "world.db-wal", "world.db-shm", "world.db-journal"};
    char p[512] = {0};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        tmp_remove(p);
    }
    tmp_remove(dir);
}

static int op_is(const save_op *o, const char *want)
{
    size_t n = strlen(want);
    return o && o->len == n && memcmp(o->data, want, n) == 0;
}

static int player_is(save_db *db, const char *id, const char *want)
{
    char buf[64] = {0};
    long n = save_db_get_player(db, id, buf, sizeof buf);
    return n == (long)strlen(want) && memcmp(buf, want, (size_t)n) == 0;
}

static int blob_is(save_db *db, const char *name, const char *want)
{
    char buf[64] = {0};
    long n = save_db_get_blob(db, name, buf, sizeof buf);
    return n == (long)strlen(want) && memcmp(buf, want, (size_t)n) == 0;
}

static int column_is(save_db *db, int cx, int cz, const char *want)
{
    uint8_t *b = NULL;
    long n = save_db_get_column(db, cx, cz, &b);
    int ok = n == (long)strlen(want) && memcmp(b, want, (size_t)n) == 0;
    mem_free(b);
    return ok;
}

/* One op per row, the newest bytes winning; rows of different kinds
 * never meet, even under the same name. */
static void test_batch_rows(void)
{
    save_batch b = {0};
    save_batch_init(&b);
    CHECK(save_batch_column(&b, 0, 0, (const uint8_t *)"old", 3) == 0);
    CHECK(save_batch_column(&b, 0, 0, (const uint8_t *)"newer", 5) == 0);
    CHECK(save_batch_column(&b, 0, 1, (const uint8_t *)"z", 1) == 0);
    CHECK(save_batch_player(&b, "a", "pa", 2) == 0);
    CHECK(save_batch_blob(&b, "a", "ba", 2) == 0);
    CHECK(b.n == 4 && b.bytes == 10 && !b.durable);
    CHECK(op_is(save_batch_find(&b, SAVE_OP_COLUMN, 0, 0, NULL), "newer"));
    CHECK(op_is(save_batch_find(&b, SAVE_OP_COLUMN, 0, 1, NULL), "z"));
    CHECK(op_is(save_batch_find(&b, SAVE_OP_PLAYER, 0, 0, "a"), "pa"));
    CHECK(op_is(save_batch_find(&b, SAVE_OP_BLOB, 0, 0, "a"), "ba"));
    CHECK(save_batch_find(&b, SAVE_OP_COLUMN, 1, 0, NULL) == NULL);
    CHECK(save_batch_find(&b, SAVE_OP_PLAYER, 0, 0, "b") == NULL);

    /* Names the tables take, up to 80 bytes, and none beyond. */
    char key[SAVE_KEY_MAX + 1] = {0};
    memset(key, 'k', SAVE_KEY_MAX - 1);
    CHECK(save_batch_blob(&b, key, "", 0) == 0);
    CHECK(op_is(save_batch_find(&b, SAVE_OP_BLOB, 0, 0, key), ""));
    key[SAVE_KEY_MAX - 1] = 'k';
    CHECK(save_batch_blob(&b, key, "x", 1) == -1 && save_batch_player(&b, "", "x", 1) == -1);
    CHECK(b.n == 5 && b.bytes == 10);

    save_batch_drop(&b, SAVE_OP_COLUMN, 0, 0, NULL);
    save_batch_drop(&b, SAVE_OP_COLUMN, 9, 9, NULL); /* not there: nothing happens */
    CHECK(b.n == 4 && b.bytes == 5 && save_batch_find(&b, SAVE_OP_COLUMN, 0, 0, NULL) == NULL);
    CHECK(op_is(save_batch_find(&b, SAVE_OP_PLAYER, 0, 0, "a"), "pa"));
    save_batch_free(&b);
    CHECK(b.n == 0 && b.ops == NULL && b.bytes == 0);
    CHECK(save_batch_player(&b, "a", "again", 5) == 0 && b.n == 1); /* reusable after free */
    save_batch_free(&b);
}

/* Merging moves every op across, the newer batch winning row by row. */
static void test_batch_merge(void)
{
    save_batch old = {0}, now = {0};
    CHECK(save_batch_column(&old, 2, 3, (const uint8_t *)"old", 3) == 0);
    CHECK(save_batch_player(&old, "p", "kept", 4) == 0);
    CHECK(save_batch_column(&now, 2, 3, (const uint8_t *)"new", 3) == 0);
    CHECK(save_batch_blob(&now, "state", "s", 1) == 0);
    now.durable = 1;
    save_batch_merge(&old, &now);
    CHECK(now.n == 0 && now.ops == NULL && now.bytes == 0);
    CHECK(old.n == 3 && old.bytes == 8 && old.durable);
    CHECK(op_is(save_batch_find(&old, SAVE_OP_COLUMN, 2, 3, NULL), "new"));
    CHECK(op_is(save_batch_find(&old, SAVE_OP_PLAYER, 0, 0, "p"), "kept"));
    CHECK(op_is(save_batch_find(&old, SAVE_OP_BLOB, 0, 0, "state"), "s"));

    /* Into an empty batch, and an empty one into a full one. */
    save_batch empty = {0};
    save_batch_merge(&empty, &old);
    CHECK(empty.n == 3 && old.n == 0 && empty.durable);
    save_batch_merge(&empty, &old);
    CHECK(empty.n == 3 && empty.bytes == 8);
    save_batch_free(&empty);
}

/* Submitted rows reach the table and a reopened world. */
static void test_writer_commits(void)
{
    char dir[400] = {0};
    CHECK(tmp_dir_make(dir, sizeof dir, "mc_writer_"));
    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (!db) return;
    CHECK(save_db_committed(db) == 0 && !save_db_failing(db));
    save_batch b = {0};
    CHECK(save_batch_player(&b, "me", "body", 4) == 0);
    CHECK(save_batch_blob(&b, "state", "st", 2) == 0);
    CHECK(save_batch_column(&b, -4, 7, (const uint8_t *)"col", 3) == 0);
    CHECK(save_db_submit(db, &b) == 1 && b.n == 0);
    CHECK(save_db_flush(db) == 0 && save_db_committed(db) == 1);
    CHECK(save_db_submit(db, &b) == 1); /* nothing to write: the same serial */
    CHECK(save_db_flush(db) == 0);
    b.durable = 1;
    CHECK(save_batch_player(&b, "me", "worse", 5) == 0);
    CHECK(save_db_submit(db, &b) == 2 && save_db_flush(db) == 0 && save_db_committed(db) == 2);
    CHECK(player_is(db, "me", "worse") && blob_is(db, "state", "st") && column_is(db, -4, 7, "col"));
    CHECK(save_batch_player(&b, "me", "closing", 7) == 0);
    save_db_submit(db, &b);
    save_db_close(db); /* flushes first */

    db = save_db_open(dir);
    CHECK(db != NULL);
    if (db) {
        CHECK(player_is(db, "me", "closing") && blob_is(db, "state", "st") && column_is(db, -4, 7, "col"));
        save_db_close(db);
    }
    remove_world(dir);
}

/* While the writer waits (here: behind a transaction), reads return the
 * queued bytes, and a direct write replaces them for good. */
static void test_writer_queued_reads(void)
{
    char dir[400] = {0};
    CHECK(tmp_dir_make(dir, sizeof dir, "mc_queue_"));
    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (!db) return;
    CHECK(save_db_put_player(db, "p", "disk", 4) == 0);
    CHECK(save_db_begin(db) == 0);
    save_batch b = {0};
    CHECK(save_batch_player(&b, "p", "queued", 6) == 0);
    CHECK(save_batch_blob(&b, "bl", "qb", 2) == 0);
    CHECK(save_batch_column(&b, 1, 1, (const uint8_t *)"qc", 2) == 0);
    CHECK(save_db_submit(db, &b) == 1);
    CHECK(save_db_committed(db) == 0);
    CHECK(player_is(db, "p", "queued") && blob_is(db, "bl", "qb") && column_is(db, 1, 1, "qc"));
    char small[4] = {0};
    CHECK(save_db_get_player(db, "p", small, sizeof small) == -1); /* too big for the buffer */
    CHECK(save_db_get_blob(db, "bl", small, 1) == -1);
    CHECK(save_db_put_player(db, "p", "direct", 6) == 0);
    CHECK(player_is(db, "p", "direct"));
    CHECK(save_db_commit(db) == 0);
    CHECK(save_db_flush(db) == 0 && save_db_committed(db) == 1);
    CHECK(player_is(db, "p", "direct") && blob_is(db, "bl", "qb") && column_is(db, 1, 1, "qc"));

    /* A direct write that empties the queue leaves nothing to wait for. */
    CHECK(save_db_begin(db) == 0);
    CHECK(save_batch_blob(&b, "only", "x", 1) == 0);
    CHECK(save_db_submit(db, &b) == 2);
    CHECK(save_db_put_blob(db, "only", "y", 1) == 0);
    CHECK(save_db_committed(db) == 2);
    CHECK(save_db_commit(db) == 0);
    CHECK(save_db_flush(db) == 0 && blob_is(db, "only", "y"));
    save_db_close(db);
    remove_world(dir);
}

/* Restoring a backup drops what was queued for the world it replaces. */
static void test_writer_restore(void)
{
    char dir[400] = {0}, copy[512] = {0};
    CHECK(tmp_dir_make(dir, sizeof dir, "mc_restore_"));
    snprintf(copy, sizeof copy, "%s/copy.db", dir);
    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (!db) return;
    save_batch b = {0};
    CHECK(save_batch_player(&b, "p", "then", 4) == 0);
    save_db_submit(db, &b);
    CHECK(save_db_copy(db, copy) == 0); /* flushes first, so the copy has it */
    CHECK(save_db_begin(db) == 0);
    CHECK(save_batch_player(&b, "p", "later", 5) == 0);
    save_db_submit(db, &b);
    CHECK(save_db_commit(db) == 0);
    CHECK(save_db_restore(db, copy) == 0);
    CHECK(save_db_flush(db) == 0 && player_is(db, "p", "then"));
    save_db_close(db);
    tmp_remove(copy);
    remove_world(dir);
}

/* Many saves in a row, read back while the writer commits: a read always
 * sees the newest bytes, queued or written. */
static void test_writer_many(void)
{
    char dir[400] = {0};
    CHECK(tmp_dir_make(dir, sizeof dir, "mc_many_"));
    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (!db) return;
    uint8_t col[2048] = {0};
    char v[16] = {0};
    int fresh = 1;
    for (int i = 1; i <= 300; i++) {
        save_batch b = {0};
        int n = snprintf(v, sizeof v, "%d", i);
        CHECK(save_batch_player(&b, "p", v, (size_t)n) == 0);
        memset(col, i & 0xff, sizeof col);
        for (int c = 0; c < 4; c++) CHECK(save_batch_column(&b, c, i % 8, col, sizeof col) == 0);
        b.durable = i % 50 == 0;
        save_db_submit(db, &b);
        fresh &= player_is(db, "p", v);
        uint8_t *got = NULL;
        long len = save_db_get_column(db, 3, i % 8, &got);
        fresh &= len == (long)sizeof col && got && got[0] == (uint8_t)(i & 0xff);
        mem_free(got);
    }
    CHECK(fresh);
    CHECK(save_db_flush(db) == 0 && save_db_committed(db) == 300 && player_is(db, "p", "300"));
    save_db_close(db);
    remove_world(dir);
}

/* Saves until killed, reporting each serial the writer has committed. */
_Noreturn static void crash_child(const char *dir, int fd)
{
    save_db *db = save_db_open(dir);
    if (!db) _exit(2);
    uint8_t col[8192] = {0};
    char v[16] = {0};
    for (unsigned i = 1;; i++) {
        save_batch b = {0};
        int n = snprintf(v, sizeof v, "%u", i);
        save_batch_player(&b, "p", v, (size_t)n);
        memset(col, (int)(i & 0xffu), sizeof col);
        for (int c = 0; c < 8; c++) save_batch_column(&b, c, (int)(i % 16u), col, sizeof col);
        b.durable = i % 4u == 0;
        save_db_submit(db, &b);
        if (save_db_flush(db) == 0 && write(fd, &i, sizeof i) != (ssize_t)sizeof i) _exit(3);
    }
}

/* The game killed at any moment of a save (SIGKILL: nothing runs on the
 * way out) opens to an intact world that holds every save the writer had
 * reported committed. */
static void test_writer_crash(void)
{
    char dir[400] = {0};
    CHECK(tmp_dir_make(dir, sizeof dir, "mc_crash_"));
    unsigned best = 0;
    for (int round = 0; round < 10; round++) {
        int fds[2] = {0};
        CHECK(pipe(fds) == 0);
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            close(fds[0]);
            crash_child(dir, fds[1]);
        }
        close(fds[1]);
        os_sleep(0.03 + 0.017 * round);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        unsigned last = 0, v = 0;
        while (read(fds[0], &v, sizeof v) == (ssize_t)sizeof v) last = v;
        close(fds[0]);

        save_db *db = save_db_open(dir); /* refuses a damaged file */
        CHECK(db != NULL);
        if (!db) break;
        char buf[16] = {0};
        long n = save_db_get_player(db, "p", buf, sizeof buf - 1);
        unsigned got = n > 0 ? (unsigned)strtoul(buf, NULL, 10) : 0;
        CHECK(got >= last);
        best = got > best ? got : best;
        save_db_close(db);
    }
    CHECK(best > 0); /* the child got some saves in */
    remove_world(dir);
}

/* Every logged body event counts, which is what asks for a durable save. */
static void test_body_events_count(void)
{
    health *h = mem_alloc(sizeof *h);
    health_init(h, 1);
    uint32_t before = h->log_count;
    health_log(h, "test %d", 1);
    health_log(h, "test %d", 2);
    CHECK(h->log_count == before + 2 && strcmp(h->log[0], "test 2") == 0);
    mem_free(h);
}

void test_save_writer_all(void)
{
    TEST_PART(test_batch_rows);
    TEST_PART(test_batch_merge);
    TEST_PART(test_writer_commits);
    TEST_PART(test_writer_queued_reads);
    TEST_PART(test_writer_restore);
    TEST_PART(test_writer_many);
    TEST_PART(test_writer_crash);
    TEST_PART(test_body_events_count);
}
