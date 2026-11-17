#include "texgen.h"
#include "block.h"
#include "item.h"
#include "mobskin.h"
#include "noise.h"

#include <math.h>
#include <stddef.h>

typedef struct { uint8_t r, g, b, a; } rgba;

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static rgba shade(int r, int g, int b, int a, int n)
{
    return (rgba){clamp8(r + n), clamp8(g + n), clamp8(b + n), clamp8(a)};
}

/* Per-pixel noise in [-amp, amp], deterministic per layer. */
static int px_noise(int layer, int x, int y, int amp)
{
    return (int)(hash2i(0xC0FFEEu + (uint32_t)layer * 7919u, x, y) % (uint32_t)(2 * amp + 1)) - amp;
}

/* ---------------------------------------------------------- item art */

/* 16x16 sprites, '.' transparent; other characters index the sprite's
 * palette. Rows top to bottom. */
typedef struct {
    char ch;
    rgba c;
} pal_entry;

typedef struct {
    int layer;
    const char *rows[16];
    pal_entry pal[6];
} sprite;

static const sprite SPRITES[] = {
    {T_ITEM_STICK,
     {"..............##", ".............#w#", "............#w#.", "...........#w#..", "..........#w#...",
      ".........#w#....", "........#w#.....", ".......#w#......", "......#w#.......", ".....#w#........",
      "....#w#.........", "...#w#..........", "..#w#...........", ".#w#............", "#w#.............",
      "##.............."},
     {{'#', {84, 58, 30, 255}}, {'w', {150, 110, 62, 255}}}},
    {T_ITEM_FIBRE,
     {"................", "....g.......g...", "...g.g.....g.G..", "...g..g...g..G..", "..G...g..g...g..",
      "..G....gg....g..", "..g....Gg....G..", "..g...g..G...g..", "...g.g....g.g...", "...Gg......gG...",
      "....g......g....", "...g.g....g.g...", "..g...G..G...g..", ".G.....gg.....g.", "................",
      "................"},
     {{'g', {92, 150, 60, 255}}, {'G', {60, 110, 40, 255}}}},
    {T_ITEM_BANDAGE,
     {"................", "................", ".....######.....", "...##wwwwww##...", "..#wwwwwwwwww#..",
      "..#wwwgggggww#..", ".#wwwg####gwww#.", ".#wwwg#..#gwww#.", ".#wwwg#..#gwww#.", ".#wwwg####gwww#.",
      "..#wwwgggggww#..", "..#wwwwwwwwww#..", "...##wwwwww##ww.", ".....######wwww.", "............ww..",
      "................"},
     {{'#', {128, 128, 120, 255}}, {'w', {242, 242, 234, 255}}, {'g', {205, 205, 198, 255}}}},
    {T_ITEM_SPLINT,
     {"................", "....##....##....", "....ww....ww....", "....ww....ww....", "...tttttttttt...",
      "....ww....ww....", "....ww....ww....", "....ww....ww....", "....ww....ww....", "...tttttttttt...",
      "....ww....ww....", "....ww....ww....", "....ww....ww....", "....##....##....", "................",
      "................"},
     {{'#', {110, 80, 45, 255}}, {'w', {176, 138, 84, 255}}, {'t', {236, 236, 228, 255}}}},
    {T_ITEM_ANTISEPTIC,
     {"................", "......cccc......", "......cccc......", ".......nn.......", ".......nn.......",
      "......bbbb......", ".....bbbbbb.....", "....bbbbbbbb....", "....bllrrllb....", "....blrrrrlb....",
      "....blrrrrlb....", "....bllrrllb....", "....bbbbbbbb....", "....bbbbbbbb....", ".....bbbbbb.....",
      "................"},
     {{'c', {40, 40, 44, 255}},
      {'n', {120, 70, 30, 255}},
      {'b', {140, 78, 30, 255}},
      {'l', {236, 236, 230, 255}},
      {'r', {210, 40, 40, 255}}}},
    {T_ITEM_PAINKILLER,
     {"................", "................", "..ssssssssssss..", "..s.pp.pp.pp.s..", "..spppppppppps..",
      "..spppppppppps..", "..s.pp.pp.pp.s..", "..ssssssssssss..", "..s.pp.pp.pp.s..", "..spppppppppps..",
      "..spppppppppps..", "..s.pp.pp.pp.s..", "..ssssssssssss..", "................", "................",
      "................"},
     {{'s', {170, 176, 186, 255}}, {'p', {250, 250, 250, 255}}}},
    {T_ITEM_ANTIBIOTIC,
     {"................", "................", "..ssssssssssss..", "..s.rw.rw.rw.s..", "..srrwwrrwwrws..",
      "..srrwwrrwwrws..", "..s.rw.rw.rw.s..", "..ssssssssssss..", "..s.rw.rw.rw.s..", "..srrwwrrwwrws..",
      "..srrwwrrwwrws..", "..s.rw.rw.rw.s..", "..ssssssssssss..", "................", "................",
      "................"},
     {{'s', {170, 176, 186, 255}}, {'r', {205, 50, 50, 255}}, {'w', {245, 245, 240, 255}}}},
    {T_ITEM_APPLE,
     {"................", "........#.......", ".......#..gg....", "....rrr#rrgg....", "...rrrrrrrrr....",
      "..rrrRrrrrrrr...", "..rrRRrrrrrrr...", "..rrRrrrrrrrr...", "..rrrrrrrrrrr...", "..rrrrrrrrrrr...",
      "...rrrrrrrrrd...", "...rrrrrrrrd....", "....rrrrrrd.....", ".....rr.rr......", "................",
      "................"},
     {{'#', {90, 60, 30, 255}},
      {'g', {80, 160, 60, 255}},
      {'r', {200, 36, 36, 255}},
      {'R', {245, 140, 130, 255}},
      {'d', {140, 20, 24, 255}}}},
    {T_ITEM_BUCKET,
     {"................", "....########....", "...#........#...", "..#..........#..", "..bbbbbbbbbbbb..",
      "..bkkkkkkkkkkb..", "..bwwwwwwwwwwb..", "...hhhhhhhhhh...", "...wwwwwwwwww...", "...wwwwwwwwww...",
      "...hhhhhhhhhh...", "....wwwwwwww....", "....wwwwwwww....", "....hhhhhhhh....", "................",
      "................"},
     {{'#', {70, 70, 76, 255}},
      {'b', {110, 80, 45, 255}},
      {'k', {60, 42, 24, 255}},
      {'w', {170, 128, 76, 255}},
      {'h', {130, 136, 146, 255}}}},
    {T_ITEM_WATER_BUCKET,
     {"................", "....########....", "...#........#...", "..#..........#..", "..bbbbbbbbbbbb..",
      "..bqqqQqqqqqqb..", "..bwwwwwwwwwwb..", "...hhhhhhhhhh...", "...wwwwwwwwww...", "...wwwwwwwwww...",
      "...hhhhhhhhhh...", "....wwwwwwww....", "....wwwwwwww....", "....hhhhhhhh....", "................",
      "................"},
     {{'#', {70, 70, 76, 255}},
      {'b', {110, 80, 45, 255}},
      {'q', {40, 90, 200, 255}},
      {'Q', {120, 170, 250, 255}},
      {'w', {170, 128, 76, 255}},
      {'h', {130, 136, 146, 255}}}},
    /* An AED: green case, the heart-and-bolt sign, an orange shock button. */
    {T_ITEM_DEFIBRILLATOR,
     {"................", "....hhhhhhhh....", "....h......h....", "..############..", "..#gggggggggg#..",
      "..#ggwwggwwgg#..", "..#gwwwwwwwwg#..", "..#gwwwwrwwwg#..", "..#ggwwrrwwgg#..", "..#gggwrwwggg#..",
      "..#ggggwwgggg#..", "..#gggggggggg#..", "..#googgggggg#..", "..############..", "................",
      "................"},
     {{'#', {40, 44, 48, 255}},
      {'g', {40, 160, 90, 255}},
      {'w', {240, 240, 235, 255}},
      {'r', {215, 40, 40, 255}},
      {'o', {245, 150, 30, 255}},
      {'h', {120, 125, 130, 255}}}},
    {T_ITEM_COAL,
     {"................", "................", "................", "......####......", "....##kkkk#.....",
      "...#kkhkkkk##...", "...#khkkkkkkk#..", "..#kkkkkkhkkk#..", "..#kkkkkkkkkk#..", "..#kkkhkkkkk#...",
      "...#kkkkkkkk#...", "....##kkkk##....", "......####......", "................", "................",
      "................"},
     {{'#', {20, 20, 22, 255}}, {'k', {45, 45, 48, 255}}, {'h', {95, 95, 100, 255}}}},
    {T_ITEM_COPPER_INGOT,
     {"................", "................", "................", "................", "................",
      ".....########...", "....#hhhhhhh##..", "...#hhhhhhh#m#..", "..##########mm#.", "..#mmmmmmmmm#m#.",
      "..#mmmmmmmmm##..", "..###########...", "................", "................", "................",
      "................"},
     {{'#', {110, 50, 25, 255}}, {'h', {235, 150, 95, 255}}, {'m', {190, 100, 55, 255}}}},
    {T_ITEM_IRON_INGOT,
     {"................", "................", "................", "................", "................",
      ".....########...", "....#hhhhhhh##..", "...#hhhhhhh#m#..", "..##########mm#.", "..#mmmmmmmmm#m#.",
      "..#mmmmmmmmm##..", "..###########...", "................", "................", "................",
      "................"},
     {{'#', {70, 72, 78, 255}}, {'h', {215, 218, 222, 255}}, {'m', {160, 163, 170, 255}}}},
    {T_ITEM_RAW_COPPER,
     {"................", "................", "................", "......###.......", "....##ahh##.....",
      "...#aahaaam#....", "..#aaaaamaaa#...", "..#ammaaaahaa#..", "..#aaaahaaaam#..", "...#amaaaamaa#..",
      "...#aaaammaa#...", "....##aaaa##....", "......####......", "................", "................",
      "................"},
     {{'#', {70, 50, 40, 255}}, {'a', {150, 95, 70, 255}}, {'h', {225, 140, 80, 255}}, {'m', {80, 150, 110, 255}}}},
    {T_ITEM_RAW_IRON,
     {"................", "................", "................", "......###.......", "....##ahh##.....",
      "...#aahaaam#....", "..#aaaaamaaa#...", "..#ammaaaahaa#..", "..#aaaahaaaam#..", "...#amaaaamaa#..",
      "...#aaaammaa#...", "....##aaaa##....", "......####......", "................", "................",
      "................"},
     {{'#', {70, 60, 55, 255}}, {'a', {165, 140, 120, 255}}, {'h', {215, 185, 160, 255}}, {'m', {120, 80, 65, 255}}}},
    {T_ITEM_RAW_MEAT,
     {"................", "................", "................", ".....#####......", "....#mmmmm##....",
      "...#mmfmmmmm#...", "..#mmmmmmmfmm#..", "..#mffmmmmmmm#..", "..#mmmmmmmmmm#bb", "...#mmmmfmmm#bwb",
      "....##mmmmm#.bb.", "......#####.....", "................", "................", "................",
      "................"},
     {{'#', {120, 30, 35, 255}},
      {'m', {200, 60, 70, 255}},
      {'f', {240, 210, 200, 255}},
      {'b', {225, 220, 205, 255}},
      {'w', {255, 250, 240, 255}}}},
    {T_ITEM_COOKED_MEAT,
     {"................", "................", "................", ".....#####......", "....#mmmmm##....",
      "...#mmfmmmmm#...", "..#mmmmmmmfmm#..", "..#mffmmmmmmm#..", "..#mmmmmmmmmm#bb", "...#mmmmfmmm#bwb",
      "....##mmmmm#.bb.", "......#####.....", "................", "................", "................",
      "................"},
     {{'#', {70, 40, 20, 255}},
      {'m', {150, 90, 45, 255}},
      {'f', {200, 150, 90, 255}},
      {'b', {225, 220, 205, 255}},
      {'w', {255, 250, 240, 255}}}},
    {T_ITEM_RIFLE,
     {"................", "................", "................", "................", "................",
      "..............mm", ".............mmm", "..........mmmmm.", ".......mmmmmm#..", "wwwwwmmmmm#.....",
      "wwwwwwm#........", "wwww...#........", "ww..............", "................", "................",
      "................"},
     {{'w', {120, 80, 45, 255}}, {'m', {70, 72, 78, 255}}, {'#', {40, 40, 44, 255}}}},
    {T_ITEM_CARTRIDGE,
     {"................", "................", "......cc........", ".....cccc.......", ".....bbbb.......",
      ".....bbbb.......", ".....bbbb.......", ".....bbbb.......", ".....bbbb.......", ".....bbbb.......",
      ".....bbbb.......", ".....dddd.......", "................", "................", "................",
      "................"},
     {{'c', {150, 100, 70, 255}}, {'b', {215, 160, 70, 255}}, {'d', {150, 110, 50, 255}}}},
    {T_ITEM_GUNPOWDER,
     {"................", "................", "................", "................", "................",
      "................", ".......g........", "......gGg.......", ".....ggGgg......", "....gGgggGg.....",
      "...ggggGgggg....", "..gggGgggggGg...", "................", "................", "................",
      "................"},
     {{'g', {50, 50, 55, 255}}, {'G', {90, 90, 95, 255}}}},
    /* An auto-injector: a yellow safety cap, the barrel, an orange tip. */
    {T_ITEM_ADRENALINE,
     {"................", "...........cc...", "..........cyyc..", ".........cyyc...", "........wwwc....",
      ".......wwww.....", "......wwww......", ".....wwwbw......", "....wwwbw.......", "...wwwww........",
      "..owww..........", ".oo.............", "o...............", "................", "................",
      "................"},
     {{'c', {80, 70, 30, 255}},
      {'y', {240, 210, 40, 255}},
      {'w', {230, 232, 235, 255}},
      {'b', {60, 120, 200, 255}},
      {'o', {240, 120, 30, 255}}}},
    /* A brown pharmacy bottle of tablets with a white label. */
    {T_ITEM_MORPHINE,
     {"................", "................", ".....cccccc.....", ".....cccccc.....", "......nnnn......",
      ".....bbbbbb.....", "....bbbbbbbb....", "....bwwwwwwb....", "....bwrrrrwb....", "....bwwwwwwb....",
      "....bwrrrwwb....", "....bwwwwwwb....", "....bbbbbbbb....", "....bbbbbbbb....", ".....bbbbbb.....",
      "................"},
     {{'c', {235, 235, 235, 255}},
      {'n', {90, 60, 30, 255}},
      {'b', {120, 70, 25, 255}},
      {'w', {240, 240, 236, 255}},
      {'r', {60, 90, 170, 255}}}},
    /* A nasal spray: white body, a narrow nozzle, an orange plunger. */
    {T_ITEM_NALOXONE,
     {"................", ".......ww.......", ".......ww.......", ".......ww.......", "......wwww......",
      ".....oooooo.....", ".....oooooo.....", "......wwww......", "......wwww......", "......wkkw......",
      "......wkkw......", "......wwww......", "......wwww......", "......wwww......", "................",
      "................"},
     {{'w', {236, 238, 240, 255}}, {'o', {240, 130, 40, 255}}, {'k', {40, 120, 200, 255}}}},
    /* A glass bottle of amber brandy with a cork. */
    {T_ITEM_SPIRITS,
     {"................", "......kk........", "......kk........", "......gg........", "......gg........",
      ".....gggg.......", "....gaaaag......", "....gaaaag......", "....gaAaag......", "....gaAaag......",
      "....gaaaag......", "....gaaaag......", "....gaaaag......", ".....gggg.......", "................",
      "................"},
     {{'k', {150, 100, 55, 255}}, {'g', {170, 200, 190, 255}}, {'a', {200, 120, 30, 255}}, {'A', {240, 180, 80, 255}}}},
};

/* Tools and blades, drawn from geometry rather than 20 hand-made sprites:
 * a handle along the diagonal from the bottom left, and a head that
 * depends on the kind, coloured by the tier's material. */
static int tool_texel(int layer, int x, int y, rgba *out)
{
    if (layer < T_TOOL_FIRST || layer >= T_COUNT) return 0;
    static const rgba HEAD[TIER_COUNT] = {{150, 112, 64, 255},
                                          {128, 128, 128, 255},
                                          {205, 120, 70, 255},
                                          {205, 208, 214, 255}};
    int kind = (layer - T_TOOL_FIRST) / TIER_COUNT, tier = (layer - T_TOOL_FIRST) % TIER_COUNT;
    float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
    float along = (fx + fy - 16.0f) * 0.70710678f; /* across the handle */
    float up = (fx + (16.0f - fy)) * 0.70710678f; /* along it, 0 at the bottom-left corner */
    int handle = fabsf(along) < 0.75f && up > 1.5f, head = 0;
    switch (kind) {
    case TOOL_PICKAXE:
        handle = handle && up < 17.5f;
        head = fabsf(up - 15.5f) < 1.1f && fabsf(along) < 7.0f - fabsf(along) * 0.15f;
        break;
    case TOOL_AXE:
        handle = handle && up < 18.5f;
        head = up > 12.5f && up < 18.0f && along > 0.5f && along < 5.0f - (18.0f - up) * 0.2f;
        break;
    case TOOL_SHOVEL:
        handle = handle && up < 15.0f;
        head = up >= 14.5f && up < 20.5f && fabsf(along) < 2.6f - fmaxf(0.0f, up - 19.0f);
        break;
    case TOOL_KNIFE:
        handle = handle && up < 8.0f;
        head = up >= 8.0f && up < 16.0f && along > -0.9f && along < 0.9f - (up - 8.0f) * 0.1f;
        break;
    default: /* sword */
        handle = handle && up < 6.5f;
        head = (up >= 6.5f && up < 20.0f && fabsf(along) < 1.0f) || (fabsf(up - 6.5f) < 0.75f && fabsf(along) < 3.2f);
        break;
    }
    *out = (rgba){0, 0, 0, 0};
    int n = px_noise(layer, x, y, 8);
    if (head) {
        rgba c = HEAD[tier];
        *out = shade(c.r, c.g, c.b, 255, n);
    } else if (handle) {
        *out = shade(110, 78, 42, 255, n);
    }
    return 1;
}

/* The mineral items (from T_ITEM_RAW_TIN, in item order) reuse three
 * shapes in their own colours: a raw lump, a cast ingot and a heap of
 * powder. */
typedef struct {
    int shape; /* the sprite whose rows it takes */
    pal_entry pal[6];
} recolour;

#define C(r, g, b) {r, g, b, 255}
#define LUMP(o, a, h, m)                             \
    {                                                \
        T_ITEM_RAW_IRON,                             \
        {                                            \
            {'#', o}, {'a', a}, {'h', h}, { 'm', m } \
        }                                            \
    }
#define INGOT(o, h, m)                     \
    {                                      \
        T_ITEM_IRON_INGOT,                 \
        {                                  \
            {'#', o}, {'h', h}, { 'm', m } \
        }                                  \
    }
#define HEAP(g, G)               \
    {                            \
        T_ITEM_GUNPOWDER,        \
        {                        \
            {'g', g}, { 'G', G } \
        }                        \
    }
static const recolour MINERAL_ART[T_ITEM_MINERALS_END - T_ITEM_RAW_TIN] = {
    LUMP(C(35, 25, 20), C(75, 55, 45), C(130, 100, 80), C(40, 30, 25)), /* raw tin */
    LUMP(C(60, 40, 20), C(140, 100, 45), C(200, 160, 80), C(90, 60, 30)), /* raw zinc */
    LUMP(C(50, 52, 58), C(120, 124, 132), C(190, 195, 205), C(85, 88, 95)), /* raw lead */
    LUMP(C(70, 60, 30), C(165, 145, 85), C(215, 195, 120), C(110, 95, 55)), /* raw nickel */
    LUMP(C(70, 70, 75), C(175, 175, 180), C(235, 235, 240), C(60, 60, 65)), /* raw silver */
    LUMP(C(120, 115, 105), C(225, 220, 210), C(240, 200, 60), C(215, 170, 40)), /* raw gold */
    LUMP(C(90, 40, 20), C(180, 95, 55), C(215, 140, 90), C(150, 70, 40)), /* bauxite */
    LUMP(C(20, 20, 22), C(55, 55, 60), C(120, 120, 130), C(40, 40, 45)), /* ilmenite */
    LUMP(C(20, 20, 18), C(50, 50, 45), C(90, 90, 85), C(200, 190, 40)), /* uraninite */
    LUMP(C(120, 100, 115), C(215, 190, 210), C(240, 225, 240), C(180, 200, 175)), /* spodumene */
    HEAP(C(220, 200, 40), C(250, 235, 90)), /* sulfur */
    HEAP(C(210, 210, 200), C(250, 250, 245)), /* saltpeter */
    HEAP(C(225, 205, 200), C(250, 242, 238)), /* salt */
    LUMP(C(150, 150, 155), C(225, 225, 230), C(250, 250, 255), C(200, 200, 210)), /* quartz */
    LUMP(C(30, 30, 34), C(70, 70, 76), C(140, 140, 150), C(50, 50, 55)), /* graphite */
    INGOT(C(90, 92, 96), C(225, 228, 230), C(185, 188, 192)), /* tin */
    INGOT(C(80, 90, 95), C(200, 212, 218), C(160, 172, 180)), /* zinc */
    INGOT(C(45, 48, 58), C(140, 145, 160), C(100, 104, 118)), /* lead */
    INGOT(C(85, 82, 72), C(215, 210, 195), C(175, 170, 155)), /* nickel */
    INGOT(C(110, 110, 115), C(245, 245, 250), C(200, 200, 208)), /* silver */
    INGOT(C(130, 90, 10), C(255, 220, 90), C(220, 170, 40)), /* gold */
    INGOT(C(95, 60, 25), C(215, 160, 90), C(170, 115, 55)), /* bronze */
    INGOT(C(120, 95, 30), C(240, 210, 110), C(200, 165, 70)), /* brass */
    INGOT(C(55, 60, 68), C(200, 206, 214), C(140, 146, 156)), /* steel */
    LUMP(C(15, 15, 15), C(35, 33, 32), C(80, 78, 76), C(25, 24, 24)), /* charcoal */
    HEAP(C(215, 213, 205), C(250, 250, 245)), /* quicklime */
};
#undef C
#undef LUMP
#undef INGOT
#undef HEAP

static int sprite_texel(int layer, int x, int y, rgba *out)
{
    if (tool_texel(layer, x, y, out)) return 1;
    int shape = layer;
    const pal_entry *pal = NULL;
    if (layer >= T_ITEM_RAW_TIN && layer < T_ITEM_MINERALS_END) {
        shape = MINERAL_ART[layer - T_ITEM_RAW_TIN].shape;
        pal = MINERAL_ART[layer - T_ITEM_RAW_TIN].pal;
    }
    for (int i = 0; i < (int)(sizeof SPRITES / sizeof SPRITES[0]); i++) {
        const sprite *s = &SPRITES[i];
        if (s->layer != shape) continue;
        if (!pal) pal = s->pal;
        char ch = s->rows[y][x];
        *out = (rgba){0, 0, 0, 0};
        for (int k = 0; k < 6; k++)
            if (pal[k].ch == ch && ch) {
                int n = px_noise(layer, x, y, 6);
                rgba c = pal[k].c;
                *out = shade(c.r, c.g, c.b, c.a, n);
            }
        return 1;
    }
    return 0;
}

/* A crack pattern: random walks from the centre, more and longer at each
 * stage. Only the crack texels are opaque. */
static rgba crack(int stage, int x, int y)
{
    static uint8_t mask[4][16][16];
    static int built;
    if (!built) {
        for (int s = 0; s < 4; s++) {
            int walks = 2 + s * 2, len = 4 + s * 3;
            for (int w = 0; w < walks; w++) {
                int px = 8, py = 8;
                uint32_t h = hash2i(0xC7AC0u, w, 77);
                for (int k = 0; k < len; k++) {
                    mask[s][py & 15][px & 15] = 1;
                    h = h * 1664525u + 1013904223u;
                    int d = (int)(h >> 29); /* 0..7 */
                    px += (d & 1) ? 1 : -1;
                    if (d & 2) py += (d & 4) ? 1 : -1;
                    else px += (w & 1) ? 1 : -1;
                }
            }
        }
        /* Each stage also keeps the cracks of the stages before it. */
        for (int s = 1; s < 4; s++)
            for (int yy = 0; yy < 16; yy++)
                for (int xx = 0; xx < 16; xx++) mask[s][yy][xx] |= mask[s - 1][yy][xx];
        built = 1;
    }
    return mask[stage][y][x] ? (rgba){20, 18, 16, 210} : (rgba){0, 0, 0, 0};
}

/* Flames rising from the bottom: a noisy height per column that flickers
 * between frames, yellow at the core fading to red and then transparent. */
static rgba flame(int frame, int x, int y)
{
    int h0 = 7 + (int)(hash2i(0xF1A3Eu + (uint32_t)frame * 31u, x, 0) % 7u);
    int hx = (x == 0 || x == 15) ? h0 / 2 : h0;
    int up = 15 - y; /* 0 at the bottom */
    if (up > hx) return (rgba){0, 0, 0, 0};
    float t = (float)up / (float)(hx > 0 ? hx : 1);
    int n = px_noise(T_FIRE0 + frame, x, y, 18);
    if (t < 0.35f) return shade(255, 236, 140, 235, n);
    if (t < 0.7f) return shade(250, 160, 40, 225, n);
    return shade(215, 70, 25, (int)(200.0f * (1.2f - t)), n);
}

/* Ore blocks: the rock around the mineral, the mineral, and how sparse
 * its clusters are (higher, fewer). Most sit in plain stone; bauxite and
 * rock salt are the rock themselves, gold and lithium sit in quartz-rich
 * rock. */
static const struct {
    int layer;
    rgba rock, mineral;
    int sparse;
} ORE_ART[] = {
    {T_COAL_ORE, {125, 125, 125, 255}, {30, 30, 32, 255}, 55},
    {T_COPPER_ORE, {125, 125, 125, 255}, {200, 110, 60, 255}, 55},
    {T_IRON_ORE, {125, 125, 125, 255}, {196, 160, 130, 255}, 55},
    {T_TIN_ORE, {125, 125, 125, 255}, {45, 35, 30, 255}, 55}, /* cassiterite, near black */
    {T_ZINC_ORE, {125, 125, 125, 255}, {150, 110, 50, 255}, 55}, /* resinous sphalerite */
    {T_LEAD_ORE, {125, 125, 125, 255}, {165, 170, 180, 255}, 55}, /* metallic galena */
    {T_NICKEL_ORE, {125, 125, 125, 255}, {180, 160, 95, 255}, 55}, /* brassy pentlandite */
    {T_SILVER_ORE, {125, 125, 125, 255}, {220, 220, 228, 255}, 62},
    {T_GOLD_ORE, {205, 200, 192, 255}, {240, 195, 50, 255}, 65}, /* flecks in white quartz */
    {T_BAUXITE, {175, 100, 60, 255}, {120, 60, 35, 255}, 40}, /* red earth with darker pisoliths */
    {T_TITANIUM_ORE, {125, 125, 125, 255}, {35, 35, 40, 255}, 55}, /* black ilmenite */
    {T_URANIUM_ORE, {125, 125, 125, 255}, {185, 190, 60, 255}, 55}, /* pitchblende's yellow alteration crust */
    {T_LITHIUM_ORE, {190, 185, 180, 255}, {205, 160, 200, 255}, 50}, /* pink spodumene in pale pegmatite */
    {T_SULFUR_ORE, {125, 125, 125, 255}, {230, 210, 50, 255}, 50},
    {T_SALTPETER_ORE, {125, 125, 125, 255}, {240, 240, 235, 255}, 50}, /* a white crust */
    {T_ROCK_SALT, {225, 205, 200, 255}, {245, 240, 238, 255}, 40}, /* halite, pink-white throughout */
    {T_QUARTZ_ORE, {125, 125, 125, 255}, {235, 235, 240, 255}, 45},
    {T_GRAPHITE_ORE, {110, 110, 115, 255}, {50, 50, 55, 255}, 45},
};

static rgba texel(int layer, int x, int y)
{
    int n = px_noise(layer, x, y, 12);
    rgba sp = {0};
    if (sprite_texel(layer, x, y, &sp)) return sp;
    uint8_t px[4] = {0};
    if (mobskin_texel(layer, x, y, px)) return (rgba){px[0], px[1], px[2], px[3]};
    if (layer >= T_CRACK0 && layer <= T_CRACK3) return crack(layer - T_CRACK0, x, y);
    if (layer >= T_FIRE0 && layer <= T_FIRE3) return flame(layer - T_FIRE0, x, y);
    for (int i = 0; i < (int)(sizeof ORE_ART / sizeof ORE_ART[0]); i++) {
        if (ORE_ART[i].layer != layer) continue;
        /* Rock with clusters of mineral: a texel is ore where a coarse
         * hash says so and a neighbour agrees, giving blobs, not dust. */
        rgba r = ORE_ART[i].rock, m = ORE_ART[i].mineral;
        int ore = px_noise(layer, x / 2, y / 2, 100) > ORE_ART[i].sparse && px_noise(layer, x, y, 100) > -20;
        return ore ? shade(m.r, m.g, m.b, 255, n / 2) : shade(r.r, r.g, r.b, 255, n);
    }
    switch (layer) {
    case T_SKIN: return shade(214, 170, 136, 255, n / 3);
    case T_SLEEVE: {
        int seam = (y % 5 == 4) ? -14 : 0;
        return shade(56, 88, 140, 255, seam + n / 2);
    }
    case T_CAMPFIRE_BASE: {
        /* Crossed logs over glowing embers. */
        int log = (y >= 5 && y <= 10) || (x >= 5 && x <= 10);
        if (log) return shade(96, 70, 40, 255, (x % 4 == 0 ? -14 : 0) + n / 2);
        return (hash2i(0xE3B, x, y) & 3u) ? shade(60, 44, 36, 255, n) : shade(240, 110, 30, 255, n);
    }
    case T_ASH: return shade(110, 106, 102, 255, px_noise(layer, x, y, 20));
    case T_STONE: return shade(125, 125, 125, 255, n);
    case T_GRANITE: {
        /* Coarse crystals: black mica and pink feldspar in grey quartz. */
        int v = px_noise(layer, x, y, 100);
        if (v > 70) return shade(40, 38, 40, 255, n / 3);
        if (v < -60) return shade(205, 160, 150, 255, n / 2);
        return shade(170, 162, 158, 255, n / 2);
    }
    case T_BASALT: /* fine-grained and dark, with gas bubbles frozen in */
        if (px_noise(layer, x / 2, y / 2, 100) > 88) return shade(32, 32, 35, 255, n / 4);
        return shade(62, 62, 66, 255, n / 2);
    case T_LIMESTONE: /* pale, with a fleck of shell here and there */
        if (px_noise(layer, x, y, 100) > 88) return shade(160, 150, 125, 255, n / 3);
        return shade(198, 190, 165, 255, n / 2);
    case T_SANDSTONE: { /* laid down in beds of slightly different sand */
        int bed = (y + (px_noise(layer, x / 4, 0, 1) + 1)) % 5 == 0;
        return shade(bed ? 180 : 205, bed ? 140 : 165, bed ? 95 : 115, 255, n / 2);
    }
    case T_SHALE: /* thin laminae of compacted mud */ return shade(82, 84, 90, 255, (y % 3 == 0 ? -14 : 0) + n / 3);
    case T_MARBLE: { /* white calcite with grey veins running across */
        int v = ((x + y + px_noise(layer, 0, y / 4, 2)) % 9 + 9) % 9;
        if (v <= 1) return shade(150, 150, 156, 255, n / 3);
        return shade(228, 226, 222, 255, n / 4);
    }
    case T_OBSIDIAN: /* black glass catching the light along its conchoidal faces */
        if ((x + y) % 7 == 0 && px_noise(layer, x / 2, y, 100) > 0) return shade(78, 72, 92, 255, n / 3);
        return shade(28, 22, 36, 255, n / 3);
    case T_CLAY: return shade(150, 128, 112, 255, n / 3);
    case T_MUD:
        if (px_noise(layer, x, y, 100) > 80) return shade(110, 90, 75, 255, n / 3); /* a wet shine */
        return shade(78, 58, 42, 255, n / 2);
    case T_PEAT: /* dark, with the fibres of the plants it was */
        if (px_noise(layer, x, y / 3, 100) > 60) return shade(90, 65, 40, 255, n / 3);
        return shade(58, 40, 28, 255, n / 2);
    case T_PODZOL: { /* fallen needles over the dark top of the soil */
        int v = px_noise(layer, x, y, 100);
        if (v > 70) return shade(150, 95, 45, 255, n / 3);
        if (v < -80) return shade(70, 55, 40, 255, n / 3);
        return shade(100, 75, 48, 255, n / 2);
    }
    case T_FURNACE_TOP: {
        int edge = x == 0 || y == 0 || x == 15 || y == 15;
        return shade(edge ? 100 : 118, edge ? 100 : 118, edge ? 100 : 118, 255, n / 2);
    }
    case T_FURNACE_FRONT: {
        /* Stone blocks around a dark firebox mouth with embers inside. */
        if (x >= 4 && x <= 11 && y >= 7 && y <= 13) {
            if (y >= 11) return shade(220, 90, 25, 255, px_noise(layer, x, y, 60));
            return shade(28, 24, 22, 255, n / 4);
        }
        int row = y / 4, mortar = y % 4 == 3 || ((x + (row % 2) * 4) % 8) == 7;
        return mortar ? shade(80, 80, 80, 255, n / 3) : shade(130, 130, 130, 255, n);
    }
    case T_BEDROCK: return shade(60, 60, 60, 255, px_noise(layer, x, y, 30));
    case T_DIRT: return shade(134, 96, 67, 255, n);
    case T_GRASS_TOP: return shade(95, 159, 53, 255, n);
    case T_GRASS_SIDE: {
        int edge = 3 + (int)(hash2i(99, x, 0) % 2u);
        return y < edge ? shade(95, 159, 53, 255, n) : shade(134, 96, 67, 255, n);
    }
    case T_SAND: return shade(219, 207, 163, 255, n / 2);
    case T_GRAVEL: {
        int p = px_noise(layer, x / 2, y / 2, 40);
        return shade(136, 126, 126, 255, p);
    }
    case T_LOG_SIDE: return shade(102, 81, 49, 255, (x % 4 == 0 ? -18 : 0) + n / 2);
    case T_LOG_TOP: {
        int dx = x * 2 - 15, dy = y * 2 - 15;
        int d2 = dx * dx + dy * dy;
        if (x == 0 || y == 0 || x == 15 || y == 15) return shade(102, 81, 49, 255, n / 2);
        int ring = (d2 / 40) % 2 ? -14 : 0;
        return shade(176, 144, 90, 255, ring + n / 3);
    }
    case T_LEAVES: return shade(58, 110, 38, 255, px_noise(layer, x, y, 22));
    case T_PLANKS: {
        int seam = (y % 4 == 3) ? -30 : 0;
        int joint = ((x + (y / 4) * 5) % 16 == 0) ? -20 : 0;
        return shade(162, 130, 78, 255, seam + joint + n / 2);
    }
    case T_GLASS: {
        int border = x == 0 || y == 0 || x == 15 || y == 15;
        int glint = (x == y + 3 || x == y + 4) && x > 4 && x < 12;
        if (border) return (rgba){220, 235, 240, 230};
        if (glint) return (rgba){240, 250, 255, 140};
        return (rgba){200, 225, 235, 45};
    }
    case T_BRICK: {
        int row = y / 4, mortar_y = y % 4 == 3;
        int mortar_x = ((x + (row % 2) * 4) % 8) == 7;
        if (mortar_y || mortar_x) return shade(175, 170, 160, 255, n / 3);
        return shade(150, 70, 55, 255, n);
    }
    case T_SNOW: return shade(240, 245, 250, 255, n / 3);
    case T_ICE: return shade(160, 190, 250, 190, n / 2);
    case T_WATER: return shade(30, 75, 185, 195, n / 2);
    default: return (rgba){255, 0, 255, 255};
    }
}

uint32_t texgen_offset(int layer, int mip)
{
    uint32_t off = 0;
    for (int m = 0; m < mip; m++) {
        uint32_t s = TEX_SIZE >> m;
        off += s * s * 4u * T_COUNT;
    }
    uint32_t s = TEX_SIZE >> mip;
    return off + s * s * 4u * (uint32_t)layer;
}

uint32_t texgen_total_bytes(void) { return texgen_offset(0, TEX_MIPS); }

void texgen_texel(int layer, int x, int y, uint8_t out[4])
{
    rgba c = texel(layer, x & (TEX_SIZE - 1), y & (TEX_SIZE - 1));
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = c.a;
}

void texgen_build(uint8_t *out)
{
    for (int l = 0; l < T_COUNT; l++) {
        uint8_t *dst = out + texgen_offset(l, 0);
        for (int y = 0; y < TEX_SIZE; y++)
            for (int x = 0; x < TEX_SIZE; x++) {
                rgba c = texel(l, x, y);
                uint8_t *p = dst + (y * TEX_SIZE + x) * 4;
                p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
            }
        /* Box-filtered mips keep distant terrain from shimmering. */
        for (int m = 1; m < TEX_MIPS; m++) {
            const uint8_t *src = out + texgen_offset(l, m - 1);
            uint8_t *d = out + texgen_offset(l, m);
            int s = TEX_SIZE >> m, ps = s * 2;
            for (int y = 0; y < s; y++)
                for (int x = 0; x < s; x++)
                    for (int c = 0; c < 4; c++) {
                        int sum = src[((2 * y) * ps + 2 * x) * 4 + c] + src[((2 * y) * ps + 2 * x + 1) * 4 + c] +
                                  src[((2 * y + 1) * ps + 2 * x) * 4 + c] + src[((2 * y + 1) * ps + 2 * x + 1) * 4 + c];
                        d[(y * s + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
                    }
        }
    }
}
