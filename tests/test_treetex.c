/* The trees' textures and blocks: bark that tiles without a seam through
 * its windows and across neighbouring logs, each kind's own colours, moss,
 * end grain with rings round a pith, leaves the sky shows through, and
 * the block table that ties every kind to its layers. */
#include "block.h"
#include "test_util.h"
#include "treetex.h"

#include <stdlib.h>
#include <string.h>

#define STRIP (16 * TREE_BARK_WINDOWS)

static int lum(const uint8_t *p) { return (p[0] * 3 + p[1] * 6 + p[2]) / 10; }

static void texel(int layer, int x, int y, uint8_t out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    CHECK(treetex_texel(layer, x, y, out) == 1);
}

/* Layers outside the trees' runs are left to the rest of texgen, the
 * texel untouched. */
static void test_treetex_range(void)
{
    static const int OTHER[] = {T_STONE, T_DIRT, T_GRASS_TUFT, T_VINES, T_TREE_LEAVES_LAST + 1, T_BARK_FIRST - 1};
    for (size_t i = 0; i < sizeof OTHER / sizeof OTHER[0]; i++) {
        uint8_t out[4] = {1, 2, 3, 4};
        CHECK(treetex_texel(OTHER[i], 3, 3, out) == 0);
        CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4);
    }
    /* The runs follow one another with no layer between them. */
    CHECK(tree_tex_bark(0, 0) == T_BARK_FIRST && tree_tex_sideways(0) == tree_tex_bark(TREE_KINDS - 1, 3) + 1);
    CHECK(tree_tex_mossy(0) == tree_tex_sideways(TREE_KINDS - 1) + 1);
    CHECK(tree_tex_end(0) == tree_tex_mossy(TREE_KINDS - 1) + 1);
    CHECK(tree_tex_leaves(0) == tree_tex_end(TREE_KINDS - 1) + 1);
    CHECK(tree_tex_leaves(TREE_KINDS - 1) == T_TREE_LEAVES_LAST && T_TREE_LAST == T_TREE_LEAVES_LAST);
    CHECK(T_COUNT <= 256);
}

/* Bark windows: window w is rows 16w..16w+15 of the kind's strip, so the
 * last row of one and the first of the next are neighbours in the strip,
 * the fourth's last row runs on into the first's top, and the sideways
 * layer is a 16-texel strip turned a quarter. Every texel is opaque. */
static void test_bark_windows(void)
{
    int bad = 0, opaque = 0;
    for (int k = 0; k < TREE_KINDS; k++) {
        for (int w = 0; w < TREE_BARK_WINDOWS; w++)
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    uint8_t a[4], b[4];
                    texel(tree_tex_bark(k, w), x, y, a);
                    treetex_bark(k, x, w * 16 + y, STRIP, b);
                    bad += memcmp(a, b, 4) != 0;
                    opaque += a[3] == 255;
                }
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint8_t a[4], b[4];
                texel(tree_tex_sideways(k), x, y, a);
                treetex_bark(k, y, x, 16, b);
                bad += memcmp(a, b, 4) != 0;
                opaque += a[3] == 255;
            }
        /* Four windows, four different blocks of bark: no two alike over
         * more than a fifth of their texels. */
        for (int w = 0; w < TREE_BARK_WINDOWS; w++)
            for (int v = w + 1; v < TREE_BARK_WINDOWS; v++) {
                int same = 0;
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 16; x++) {
                        uint8_t a[4], b[4];
                        texel(tree_tex_bark(k, w), x, y, a);
                        texel(tree_tex_bark(k, v), x, y, b);
                        same += memcmp(a, b, 3) == 0;
                    }
                CHECK(same < 256 / 5);
            }
    }
    CHECK(bad == 0 && opaque == (int)TREE_KINDS * 16 * 16 * (TREE_BARK_WINDOWS + 1));
}

/* Seamless: across the strip's wrap, up from the fourth window into the
 * first and across from one log's right edge to the next one's left, the
 * step in brightness is no bigger than the strip's own steps between
 * neighbouring rows and columns. A field that did not wrap would cut a
 * hard line there. */
static void test_bark_seamless(void)
{
    static int lums[STRIP][16];
    for (int k = 0; k < TREE_KINDS; k++) {
        for (int y = 0; y < STRIP; y++)
            for (int x = 0; x < 16; x++) {
                uint8_t p[4];
                treetex_bark(k, x, y, STRIP, p);
                lums[y][x] = lum(p);
            }
        int seam_v = 0, seam_h = 0, max_v = 0, max_h = 0;
        for (int x = 0; x < 16; x++) seam_v += abs(lums[STRIP - 1][x] - lums[0][x]);
        for (int y = 0; y < STRIP; y++) seam_h += abs(lums[y][15] - lums[y][0]);
        for (int y = 0; y + 1 < STRIP; y++) {
            int d = 0;
            for (int x = 0; x < 16; x++) d += abs(lums[y][x] - lums[y + 1][x]);
            max_v = d > max_v ? d : max_v;
        }
        for (int x = 0; x < 15; x++) {
            int d = 0;
            for (int y = 0; y < STRIP; y++) d += abs(lums[y][x] - lums[y][x + 1]);
            max_h = d > max_h ? d : max_h;
        }
        /* Sums over 16 texels across and STRIP down; the slack is a few
         * levels a texel, the grain's. */
        CHECK(seam_v * 4 <= max_v * 5 + 4 * 16);
        CHECK(seam_h * 4 <= max_h * 5 + 4 * STRIP);
        /* And it is a pattern, not a flat colour. */
        CHECK(max_v > 2 * 16 && max_h > 2 * STRIP);
    }
}

/* Mean colour of a kind's bark strip. */
static void bark_mean(int k, int rgb[3])
{
    long s[3] = {0};
    for (int y = 0; y < STRIP; y++)
        for (int x = 0; x < 16; x++) {
            uint8_t p[4];
            treetex_bark(k, x, y, STRIP, p);
            for (int c = 0; c < 3; c++) s[c] += p[c];
        }
    for (int c = 0; c < 3; c++) rgb[c] = (int)(s[c] / (STRIP * 16));
}

/* Each kind's bark is its own: chalk-white birch, the darkest mountain
 * pine, cinnamon redwood and copper Scots pine, and no two kinds share a
 * texture. */
static void test_bark_colours(void)
{
    int m[TREE_KINDS][3] = {{0}};
    for (int k = 0; k < TREE_KINDS; k++) bark_mean(k, m[k]);
    for (int k = 0; k < TREE_KINDS; k++) {
        int l = (m[k][0] * 3 + m[k][1] * 6 + m[k][2]) / 10;
        if (k != TREE_BIRCH) CHECK(m[TREE_BIRCH][0] > m[k][0] + 20 && m[TREE_BIRCH][2] > m[k][2] + 20);
        if (k != TREE_MOUNTAIN_PINE)
            CHECK(l > (m[TREE_MOUNTAIN_PINE][0] * 3 + m[TREE_MOUNTAIN_PINE][1] * 6 + m[TREE_MOUNTAIN_PINE][2]) / 10);
        for (int j = k + 1; j < TREE_KINDS; j++) {
            int same = 0;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    uint8_t a[4], b[4];
                    texel(tree_tex_bark(k, 0), x, y, a);
                    texel(tree_tex_bark(j, 0), x, y, b);
                    same += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) < 12;
                }
            CHECK(same < 256 / 2);
        }
    }
    CHECK(m[TREE_BIRCH][0] > 170 && m[TREE_BIRCH][1] > 170 && m[TREE_BIRCH][2] > 160);
    CHECK(m[TREE_REDWOOD][0] - m[TREE_REDWOOD][2] > 60 && m[TREE_PINE][0] - m[TREE_PINE][2] > 60);
    CHECK(m[TREE_CHERRY][0] > m[TREE_CHERRY][1] + 40);
}

/* Moss covers part of the bark, never all of it, and only greens it:
 * where a texel differs from the bark (beyond the bark's grain, which the
 * moss layer leaves out), it is greener than the bark under it. */
static void test_moss(void)
{
    for (int k = 0; k < TREE_KINDS; k++) {
        int green = 0, bare = 0, redder = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint8_t a[4], b[4];
                texel(tree_tex_mossy(k), x, y, a);
                treetex_bark(k, x, y, 16, b);
                CHECK(a[3] == 255);
                if (abs(a[0] - b[0]) <= 4 && abs(a[1] - b[1]) <= 4 && abs(a[2] - b[2]) <= 4) {
                    bare++;
                    continue;
                }
                green += a[1] > a[0] + 10;
                redder += a[1] - a[0] < b[1] - b[0] - 8;
            }
        CHECK(green > 256 / 4 && green < 256 * 4 / 5 && bare > 256 / 8 && redder == 0);
    }
}

/* End grain: a rim of bark all round; inside it a dark pith and rings for
 * the true woods, and none for the palm's fibres or the baobab's pulp, the
 * palm's fibre ends speckled more than the baobab's. */
static void test_end_grain(void)
{
    for (int k = 0; k < TREE_KINDS; k++) {
        uint8_t pith[4], heart[4], rim[4], inner[4];
        texel(tree_tex_end(k), 7, 7, pith);
        texel(tree_tex_end(k), 5, 9, heart);
        int rim_sum[3] = {0}, rim_n = 0, spread = 0, first[3] = {0};
        for (int i = 0; i < 16; i++)
            for (int e = 0; e < 4; e++) {
                int x = e == 0 ? 0 : e == 1 ? 15 : i, y = e == 2 ? 0 : e == 3 ? 15 : i;
                texel(tree_tex_end(k), x, y, rim);
                if (!rim_n)
                    for (int c = 0; c < 3; c++) first[c] = rim[c];
                for (int c = 0; c < 3; c++) {
                    rim_sum[c] += rim[c];
                    spread = abs(rim[c] - first[c]) > spread ? abs(rim[c] - first[c]) : spread;
                }
                rim_n++;
            }
        /* The rim is one colour, give or take its grain. */
        CHECK(spread <= 16 && rim_n == 64);
        int ringed = k != TREE_PALM && k != TREE_BAOBAB;
        if (ringed) CHECK(lum(pith) + 20 < lum(heart));
        else CHECK(lum(pith) + 20 >= lum(heart));
        /* Rings: inside the sapwood, texels darker than the heartwood's
         * brightest lie at several distances from the pith, not one. */
        int bright = 0, rings = 0;
        for (int y = 1; y < 15; y++)
            for (int x = 1; x < 15; x++) {
                int r2 = (2 * x - 15) * (2 * x - 15) + (2 * y - 15) * (2 * y - 15);
                texel(tree_tex_end(k), x, y, inner);
                if (r2 < 4 * 36 && lum(inner) > bright) bright = lum(inner);
            }
        for (int r = 2; r < 6; r++) {
            int dark = 0;
            for (int y = 1; y < 15; y++)
                for (int x = 1; x < 15; x++) {
                    int r2 = (2 * x - 15) * (2 * x - 15) + (2 * y - 15) * (2 * y - 15);
                    if (r2 < 4 * r * r || r2 >= 4 * (r + 1) * (r + 1)) continue;
                    texel(tree_tex_end(k), x, y, inner);
                    dark += lum(inner) + 14 <= bright;
                }
            rings += dark >= 3;
        }
        if (ringed) CHECK(rings >= 2);
    }
    /* A palm's cut shows the dark ends of its fibres all over, a baobab's
     * a few. */
    int speck[2] = {0};
    for (int i = 0; i < 2; i++) {
        int k = i ? TREE_BAOBAB : TREE_PALM, bright = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int y = 1; y < 15; y++)
                for (int x = 1; x < 15; x++) {
                    uint8_t p[4];
                    texel(tree_tex_end(k), x, y, p);
                    if (!pass) bright = lum(p) > bright ? lum(p) : bright;
                    else speck[i] += lum(p) + 22 <= bright;
                }
    }
    CHECK(speck[0] > speck[1] && speck[1] > 0);
}

/* Leaves: each texel solid or clear, never more than 6 clear in a 4x4
 * block (the coarse mips stay closed), enough clear for the sky; maple
 * red in autumn, cherry pink in blossom, the conifers' needles darker than
 * any broad leaf. */
static void test_leaf_textures(void)
{
    int mean[TREE_KINDS][3] = {{0}};
    for (int k = 0; k < TREE_KINDS; k++) {
        int clear = 0, solid = 0, worst = 0;
        long s[3] = {0};
        for (int by = 0; by < 16; by += 4)
            for (int bx = 0; bx < 16; bx += 4) {
                int block = 0;
                for (int y = by; y < by + 4; y++)
                    for (int x = bx; x < bx + 4; x++) {
                        uint8_t p[4];
                        texel(tree_tex_leaves(k), x, y, p);
                        CHECK(p[3] == 0 || p[3] == 255);
                        if (p[3]) {
                            solid++;
                            for (int c = 0; c < 3; c++) s[c] += p[c];
                        } else {
                            clear++;
                            block++;
                        }
                    }
                worst = block > worst ? block : worst;
            }
        CHECK(worst <= 6 && clear >= 256 / 8 && solid + clear == 256);
        for (int c = 0; c < 3; c++) mean[k][c] = (int)(s[c] / solid);
    }
    CHECK(mean[TREE_MAPLE][0] > 2 * mean[TREE_MAPLE][1]);
    CHECK(mean[TREE_CHERRY][0] > mean[TREE_CHERRY][1] + 30 && mean[TREE_CHERRY][2] > 150);
    static const int NEEDLES[] = {TREE_SPRUCE, TREE_PINE, TREE_REDWOOD, TREE_UMBRELLA, TREE_MOUNTAIN_PINE};
    static const int BROAD[] = {TREE_OAK, TREE_BIRCH, TREE_THORN, TREE_WILLOW, TREE_PALM, TREE_BAOBAB};
    for (size_t i = 0; i < sizeof NEEDLES / sizeof NEEDLES[0]; i++)
        for (size_t j = 0; j < sizeof BROAD / sizeof BROAD[0]; j++) {
            const int *n = mean[NEEDLES[i]], *b = mean[BROAD[j]];
            CHECK((n[0] * 3 + n[1] * 6 + n[2]) / 10 < (b[0] * 3 + b[1] * 6 + b[2]) / 10);
            /* Both green, though. */
            CHECK(n[1] > n[0] && b[1] > b[0]);
        }
}

/* The block table: every kind's log shows its bark round the sides and
 * end grain top and bottom, its leaves their own layer on every face;
 * logs are wood that spans 6, leaves span 3 (a willow's 4, a palm's
 * stiff fronds 5); the names are all different. */
static void test_tree_blocks(void)
{
    for (int k = 0; k < TREE_KINDS; k++) {
        const block_def *log = block_get(g_tree_log[k]), *leaf = block_get(g_tree_leaves[k]);
        CHECK(log->tex[0] == tree_tex_bark(k, 0) && log->tex[1] == tree_tex_bark(k, 0));
        CHECK(log->tex[4] == tree_tex_bark(k, 0) && log->tex[5] == tree_tex_bark(k, 0));
        CHECK(log->tex[2] == tree_tex_end(k) && log->tex[3] == tree_tex_end(k));
        for (int f = 0; f < 6; f++) CHECK(leaf->tex[f] == tree_tex_leaves(k));
        CHECK(log->mat == BM_WOOD && log->span == 6 && leaf->mat == BM_LEAVES);
        CHECK(leaf->span == (k == TREE_WILLOW ? 4 : k == TREE_PALM ? 5 : 3));
        CHECK(log->density > 200.0f && log->density < 900.0f && leaf->density < log->density);
        for (int j = 0; j < TREE_KINDS; j++) {
            if (j == k) continue;
            CHECK(strcmp(log->name, block_get(g_tree_log[j])->name) != 0);
            CHECK(strcmp(leaf->name, block_get(g_tree_leaves[j])->name) != 0);
            CHECK(g_tree_log[j] != g_tree_log[k] && g_tree_leaves[j] != g_tree_leaves[k]);
        }
    }
    /* A baobab's spongy wood is the lightest, a redwood's lighter than an
     * oak's. */
    for (int k = 0; k < TREE_KINDS; k++)
        if (k != TREE_BAOBAB) CHECK(block_get(g_tree_log[TREE_BAOBAB])->density < block_get(g_tree_log[k])->density);
    CHECK(block_get(B_REDWOOD_LOG)->density < block_get(B_LOG)->density);
    /* Vines are a plant hung from above, with their own layer. */
    CHECK(block_plant(B_VINES) && block_get(B_VINES)->tex[0] == T_VINES);
}

void test_treetex_all(void)
{
    test_treetex_range();
    test_bark_windows();
    test_bark_seamless();
    test_bark_colours();
    test_moss();
    test_end_grain();
    test_leaf_textures();
    test_tree_blocks();
}
