/* Tagged records (tagged.c): every type survives the trip exactly; a
 * reader with an older or newer table keeps what both know, takes its
 * defaults for the rest and skips what it does not know; and damaged or
 * out-of-range bytes mark the reader bad without running past them. */
#include "tagged.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

typedef struct {
    int32_t a;
    uint8_t b[3];
} inner;

typedef struct {
    float f;
    double d;
    int32_t i;
    uint32_t u;
    uint8_t c;
    int64_t l;
    float arr[4];
    inner one;
    inner many[2];
} outer;

static const tag_field INNER_F[] = {TAG_I32R(inner, a, -10, 10), TAG_U8(inner, b)};
static const tag_table INNER = TAG_TABLE(INNER_F);
static const tag_field OUTER_F[] = {
    TAG_F32(outer, f), TAG_F64(outer, d), TAG_I32(outer, i),     TAG_U32(outer, u),
    TAG_U8R(outer, c, 0, 9), TAG_I64(outer, l), TAG_F32(outer, arr), TAG_SUB(outer, one, inner, INNER),
    TAG_SUB(outer, many, inner, INNER),
};
static const tag_table OUTER = TAG_TABLE(OUTER_F);

static outer sample(void)
{
    outer s = {0};
    s.f = 1.5f;
    s.d = -2.25e300;
    s.i = -123456;
    s.u = 0xFEEDF00Du;
    s.c = 9;
    s.l = -5000000000LL;
    for (int k = 0; k < 4; k++) s.arr[k] = (float)k * 0.25f;
    s.one = (inner){-10, {1, 2, 3}};
    s.many[0] = (inner){4, {5, 6, 7}};
    s.many[1] = (inner){10, {8, 9, 255}};
    return s;
}

/* Compared byte for byte, padding included: every struct here starts
 * zeroed or as a copy, so only a field the trip lost can differ. */
static int same(const void *a, const void *b, size_t n) { return memcmp(a, b, n) == 0; }

static size_t put(const void *s, const tag_table *t, uint8_t *buf, size_t cap)
{
    wire_w w = wire_writer(buf, cap);
    tag_put(&w, s, t);
    return w.bad ? 0 : w.len;
}

static void test_tagged_round_trip(void)
{
    uint8_t buf[512] = {0};
    outer s = sample(), back = {0};
    size_t n = put(&s, &OUTER, buf, sizeof buf);
    CHECK(n > 0);
    wire_r r = wire_reader(buf, n);
    tag_get(&r, &back, &OUTER);
    CHECK(!r.bad && r.pos == n);
    CHECK(same(&s, &back, sizeof s));

    /* The writer stops at its capacity instead of running past it. */
    CHECK(put(&s, &OUTER, buf, n - 1) == 0);

    /* An empty record reads as nothing changed. */
    wire_w w = wire_writer(buf, sizeof buf);
    tag_put_empty(&w);
    outer d = sample();
    r = wire_reader(buf, w.len);
    tag_get(&r, &d, &OUTER);
    CHECK(!r.bad && same(&d, &s, sizeof d));

    /* Keys are the names' FNV-1a hashes, so they never change. */
    CHECK(tag_key("") == 2166136261u && tag_key("a") == 0xE40C292Cu);
}

/* The same struct as a later build might describe it: one field gone,
 * one new, an array longer and another field's type changed. */
typedef struct {
    float f;
    double d;
    float i; /* was int32_t */
    float arr[6];
    float added;
    inner many[3];
} later;

static const tag_field LATER_F[] = {
    TAG_F32(later, f), TAG_F64(later, d), TAG_F32(later, i), TAG_F32(later, arr), TAG_F32(later, added),
    TAG_SUB(later, many, inner, INNER),
};
static const tag_table LATER = TAG_TABLE(LATER_F);

static void test_tagged_versions(void)
{
    uint8_t buf[512] = {0};
    /* Newer bytes into the older table: what both know comes across,
     * unknown fields are skipped, and the changed type keeps its default. */
    later l = {0};
    l.f = 3.0f;
    l.d = 4.0;
    l.i = 5.0f;
    for (int k = 0; k < 6; k++) l.arr[k] = (float)(k + 1);
    l.added = 7.0f;
    for (int k = 0; k < 3; k++) l.many[k] = (inner){k, {(uint8_t)k, 0, 0}};
    size_t n = put(&l, &LATER, buf, sizeof buf);
    outer o = sample();
    wire_r r = wire_reader(buf, n);
    tag_get(&r, &o, &OUTER);
    CHECK(!r.bad && r.pos == n);
    CHECK(o.f == 3.0f && o.d == 4.0 && o.i == -123456); /* i changed type: default kept */
    CHECK(o.arr[0] == 1.0f && o.arr[3] == 4.0f);         /* the first four of six */
    CHECK(o.many[1].a == 1 && o.many[1].b[0] == 1);      /* two of three */
    CHECK(o.c == 9 && o.u == 0xFEEDF00Du && o.one.a == -10); /* missing from the bytes: defaults */

    /* Older bytes into the newer table: the longer array keeps its tail
     * and the new field its default. */
    outer s = sample();
    n = put(&s, &OUTER, buf, sizeof buf);
    later d = {0};
    d.arr[5] = 42.0f;
    d.added = -1.0f;
    d.many[2].a = 9;
    r = wire_reader(buf, n);
    tag_get(&r, &d, &LATER);
    CHECK(!r.bad && d.f == 1.5f && d.arr[3] == 0.75f && d.arr[5] == 42.0f && d.added == -1.0f);
    CHECK(d.many[0].a == 4 && d.many[2].a == 9);
}

static void test_tagged_damage(void)
{
    uint8_t buf[512] = {0};
    outer s = sample();
    size_t n = put(&s, &OUTER, buf, sizeof buf);
    /* Cut anywhere, the record is refused and nothing is read past the cut. */
    for (size_t cut = 0; cut < n; cut++) {
        outer d = {0};
        wire_r r = wire_reader(buf, cut);
        tag_get(&r, &d, &OUTER);
        CHECK(r.bad);
    }
    /* Every single-byte change either reads cleanly or is refused; none
     * reads past the record. */
    int refused = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t copy[512] = {0};
        memcpy(copy, buf, n);
        copy[i] ^= 0xFF;
        outer d = {0};
        wire_r r = wire_reader(copy, n);
        tag_get(&r, &d, &OUTER);
        CHECK(r.pos <= n);
        refused += r.bad;
    }
    CHECK(refused > 0);

    /* Out of range, not finite, and a length that disagrees with the
     * count are all refused. */
    outer bad = sample();
    bad.c = 10;
    n = put(&bad, &OUTER, buf, sizeof buf);
    outer d = {0};
    wire_r r = wire_reader(buf, n);
    tag_get(&r, &d, &OUTER);
    CHECK(r.bad);
    bad = sample();
    bad.one.a = 11;
    n = put(&bad, &OUTER, buf, sizeof buf);
    r = wire_reader(buf, n);
    tag_get(&r, &d, &OUTER);
    CHECK(r.bad);
    bad = sample();
    bad.f = INFINITY;
    n = put(&bad, &OUTER, buf, sizeof buf);
    r = wire_reader(buf, n);
    tag_get(&r, &d, &OUTER);
    CHECK(r.bad);
    /* The first field is f: key 4, type 1, count 2, then its length. */
    n = put(&s, &OUTER, buf, sizeof buf);
    buf[2 + 4 + 1] = 2;
    r = wire_reader(buf, n);
    tag_get(&r, &d, &OUTER);
    CHECK(r.bad);
}

/* Every table a build ships has distinct keys, or two fields would
 * read each other's values. */
static void test_tagged_keys(void)
{
    CHECK(tag_keys_distinct(&OUTER) && tag_keys_distinct(&INNER) && tag_keys_distinct(&LATER));
    static const tag_field DUP_F[] = {TAG_F32(outer, f), TAG_AS("f", outer, d, TAG_F64, 8u, 0, 0)};
    static const tag_table DUP = TAG_TABLE(DUP_F);
    CHECK(!tag_keys_distinct(&DUP));
}

/* Sections are written exactly as TAG_SUB fields of one element, and a
 * missing one leaves its destination alone. */
typedef struct {
    inner one;
    outer two;
} pair;
static const tag_field PAIR_F[] = {TAG_SUB(pair, one, inner, INNER), TAG_SUB(pair, two, outer, OUTER)};
static const tag_table PAIR = TAG_TABLE(PAIR_F);

static void test_tagged_sections(void)
{
    uint8_t a[1024] = {0}, b[1024] = {0};
    pair p = {{3, {1, 2, 3}}, sample()};
    tag_section sec[] = {{"one", &p.one, NULL, &INNER}, {"two", &p.two, NULL, &OUTER}};
    wire_w w = wire_writer(a, sizeof a);
    tag_put_sections(&w, sec, 2);
    size_t n = put(&p, &PAIR, b, sizeof b);
    CHECK(!w.bad && w.len == n && memcmp(a, b, n) == 0);

    inner i = {0};
    outer o = {0};
    tag_section in[] = {{"one", NULL, &i, &INNER}, {"two", NULL, &o, &OUTER}};
    wire_r r = wire_reader(a, n);
    tag_get_sections(&r, in, 2);
    CHECK(!r.bad && r.pos == n && i.a == 3 && same(&o, &p.two, sizeof o));

    /* A section left out on writing keeps the reader's value. */
    tag_section part[] = {{"one", NULL, NULL, &INNER}, {"two", &p.two, NULL, &OUTER}};
    w = wire_writer(a, sizeof a);
    tag_put_sections(&w, part, 2);
    i = (inner){-7, {0, 0, 0}};
    r = wire_reader(a, w.len);
    tag_get_sections(&r, in, 2);
    CHECK(!r.bad && i.a == -7 && o.l == p.two.l);

    /* A damaged section is refused. */
    for (size_t cut = 0; cut < w.len; cut++) {
        r = wire_reader(a, cut);
        tag_get_sections(&r, in, 2);
        CHECK(r.bad);
    }
}

void test_tagged_all(void)
{
    test_tagged_sections();
    test_tagged_round_trip();
    test_tagged_versions();
    test_tagged_damage();
    test_tagged_keys();
}
