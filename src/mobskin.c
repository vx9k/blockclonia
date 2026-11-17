#include "mobskin.h"
#include "block.h"
#include "entity.h"

#include <string.h>

_Static_assert(T_MOB_LAST <= ENTITY_TEX_FRONT_MAX, "a creature's front layer must fit in entity_tex's 7 bits");

/* One colour per letter. Wolves are greys over cream (a grey wolf's
 * agouti coat), boars a near-black brown with a pink-grey snout. */
typedef struct {
    char c;
    uint8_t r, g, b;
    uint8_t fur; /* 1: varies a little from texel to texel */
} paint;

static const paint PALETTE[] = {
    {'K', 26, 24, 24, 0},    {'D', 72, 68, 64, 1},   {'G', 118, 114, 108, 1}, {'L', 158, 152, 142, 1},
    {'C', 208, 198, 178, 1}, {'A', 226, 164, 44, 0}, {'R', 38, 28, 20, 1},    {'O', 62, 46, 34, 1},
    {'B', 92, 68, 46, 1},    {'U', 124, 96, 68, 1},  {'S', 176, 132, 120, 0}, {'N', 70, 40, 40, 0},
    {'T', 232, 222, 196, 0},
};

#define ART_ROWS 16

typedef struct {
    int layer;
    const char *row[ART_ROWS]; /* top to bottom, first column at the front */
} art;

/* Each picture is the face's size in texels (mob.c's boxes, 1/16 m each). */
static const art ARTS[] = {
    /* Wolf body, 6 wide, 6 tall, 13 long: a dark saddle over grey flanks
     * that pale to a cream belly. */
    {T_WOLF_BODY_SIDE,
     {"GDDDDDDDDDDDD", "GGDDDKDDDDDGD", "LGGDGGGGDGGGG", "LGGGGLGGGGGLG", "CLGLLGLLGLLLL", "CCLCCLCCCLCCL"}},
    {T_WOLF_BODY_TOP,
     {"GDDDDG", "GDDDDG", "DDKDDD", "DDDKDD", "GDDDDG", "GDKDDG", "DDDDKD", "GDDDDG", "GDKDDG", "DDDDDD", "GDDKDG",
      "GGDDGG", "GGDDGG"}},
    {T_WOLF_BELLY,
     {"LCCCCL", "CCCCCC", "LCCCCL", "CCLCCC", "LCCCCL", "CCCCLC", "LCCCCL", "CCCCCC", "LCCLCL", "CCCCCC", "LLCCLL",
      "LCCCCL", "LLLLLL"}},
    /* The ruff over the shoulders, 7 x 7 x 5, cream at the chest. */
    {T_WOLF_MANE_SIDE, {"GGDDD", "LGGDD", "LLGGD", "CLLGG", "CLLLG", "CCLLG", "CCLLL"}},
    {T_WOLF_MANE_FRONT, {"GLLLLLG", "LLCCCLL", "LCCCCCL", "LCCCCCL", "LLCCCLL", "GLCCCLG", "GLLCLLG"}},
    /* Head, 5 x 5 x 5: amber eyes under a dark brow; the muzzle covers
     * the lower middle of the front. */
    {T_WOLF_HEAD_SIDE, {"GDDDD", "LGDDD", "KAGGD", "LLGGG", "CCLLG"}},
    {T_WOLF_HEAD_FRONT, {"GDDDG", "LGDGL", "DAGAD", "LLGLL", "CLLLC"}},
    {T_WOLF_HEAD_TOP, {"GDDDG", "DDDDD", "DDKDD", "DDDDD", "GDDDG"}},
    /* Muzzle, 3 wide, 2 tall, 3 long: black nose, dark lip line. */
    {T_WOLF_SNOUT_SIDE, {"KGG", "KCL"}},
    {T_WOLF_SNOUT_FRONT, {"GKG", "CKC"}},
    {T_WOLF_EAR, {"DD", "LC"}},
    /* Tail, 2 x 2 x 7, black-tipped. */
    {T_WOLF_TAIL_SIDE, {"GDDDDKK", "GGLLGKK"}},
    {T_WOLF_TAIL_TOP, {"GD", "DD", "DG", "DD", "DD", "KK", "KK"}},
    {T_WOLF_LEG_UPPER, {"GD", "GG", "LG"}},
    {T_WOLF_LEG_LOWER, {"LG", "LL", "DK"}},
    {T_WOLF_FUR, {"GGDGGGDG", "GDGGLGGG", "GGGDGGDG", "LGGGGGGL", "GGDGGDGG", "GLGGGGGG", "GGGDGLGD", "DGGGGGGG"}},
    /* Boar body, 8 wide, 8 tall, 15 long: bristly near-black over brown. */
    {T_BOAR_BODY_SIDE,
     {"RRORRORRROROROR", "OROOOROOROOOROO", "OOBOOOBOOOBOOBO", "BOBBOBBBOBBOBBB", "BBBUBBBBBUBBBBB", "BBUBBBUBBBBUBBB",
      "OBBBOBBBBOBBBBO", "OOBOOOOBOOOBOOO"}},
    {T_BOAR_BODY_TOP,
     {"OOORROOO", "OORRROOO", "BOORROOB", "BOORROOB", "OOORRBOO", "BOORROOB", "BOOROOOB", "OOORROOO", "BOOROOOB",
      "BOOROOBB", "OOOROOOB", "BOORROOB", "BOOOOOOB", "BBOOOOBB", "BBOOROBB"}},
    {T_BOAR_BELLY,
     {"OBBOOBBO", "BOBBBBOB", "OBBOOBBO", "BOBBBBOB", "OBBOOBBO", "BOBBBBOB", "OBBOOBBO", "BOBBBBOB", "OBBOOBBO",
      "BOBBBBOB", "OBBOOBBO", "BOBBBBOB", "OBBOOBBO", "BOBBBBOB", "OOBBBBOO"}},
    /* The bristle crest along the shoulders, 4 x 2 x 8: its side is the top
     * two rows, its top the left four columns. */
    {T_BOAR_RIDGE, {"RRRRRRRR", "RORRORRO", "RRRORRRR", "RRORRROR", "ORRRRORR", "RRRORORR", "RORRRRRO", "RRRRORRR"}},
    /* Head, 6 wide, 6 tall, 5 long: small black eyes set at the sides. */
    {T_BOAR_HEAD_SIDE, {"OORRR", "BUBOO", "UKUBO", "BBBBO", "UBBUB", "BUBBB"}},
    {T_BOAR_HEAD_FRONT, {"OORROO", "BUBBUB", "KUBBUK", "BBUUBB", "UBBBBU", "BUBBUB"}},
    {T_BOAR_HEAD_TOP, {"OOOOOO", "OORROO", "ORRRRO", "ORRRRO", "OORROO"}},
    /* Snout, 4 x 3 x 3: the flat disc with two nostrils, a tusk at each
     * side. */
    {T_BOAR_SNOUT_SIDE, {"SBB", "STB", "SSB"}},
    {T_BOAR_SNOUT_FRONT, {"SSSS", "SNNS", "TSST"}},
    {T_BOAR_EAR, {"RR", "OB"}},
    {T_BOAR_TAIL, {"O", "O", "R", "R"}},
    {T_BOAR_LEG_UPPER, {"BO", "OO", "OB"}},
    {T_BOAR_LEG_LOWER, {"OO", "KK"}},
    {T_BOAR_FUR, {"OBOOBOOO", "OOOBOOBO", "BOOOOROO", "OOBOOOOB", "OROOBOOO", "OOOOOBOO", "BOOBOOOR", "OOROOOBO"}},
    {T_MOB_SOLE, {"KK", "KK"}},
};

#define NARTS (sizeof ARTS / sizeof ARTS[0])

static const art *find(int layer)
{
    for (size_t i = 0; i < NARTS; i++)
        if (ARTS[i].layer == layer) return &ARTS[i];
    return NULL;
}

static const paint *colour(char c)
{
    for (size_t i = 0; i < sizeof PALETTE / sizeof PALETTE[0]; i++)
        if (PALETTE[i].c == c) return &PALETTE[i];
    return NULL;
}

int mobskin_size(int layer, int *w, int *h)
{
    const art *a = find(layer);
    *w = *h = 0;
    if (!a) return 0;
    while (*h < ART_ROWS && a->row[*h]) (*h)++;
    *w = *h ? (int)strlen(a->row[0]) : 0;
    return 1;
}

int mobskin_texel(int layer, int x, int y, uint8_t out[4])
{
    int w = 0, h = 0;
    if (layer < T_MOB_FIRST || layer > T_MOB_LAST || !mobskin_size(layer, &w, &h) || !w) return 0;
    const art *a = find(layer);
    int cx = x < w ? x : w - 1, cy = y < h ? y : h - 1;
    const paint *p = colour(a->row[cy][cx]);
    if (!p) return 0;
    /* Fur varies a few levels from texel to texel, the same way each run. */
    uint32_t k = (uint32_t)layer * 0x9E3779B1u ^ (uint32_t)cx * 0x85EBCA77u ^ (uint32_t)cy * 0xC2B2AE3Du;
    k ^= k >> 15;
    k *= 0x2C1B3C6Du;
    k ^= k >> 13;
    int n = p->fur ? (int)(k % 13u) - 6 : 0;
    const int rgb[3] = {p->r + n, p->g + n, p->b + n};
    for (int i = 0; i < 3; i++) out[i] = (uint8_t)(rgb[i] < 0 ? 0 : (rgb[i] > 255 ? 255 : rgb[i]));
    out[3] = 255;
    return 1;
}

int mobskin_check(void)
{
    for (int l = T_MOB_FIRST; l <= T_MOB_LAST; l++) {
        int w = 0, h = 0;
        if (!mobskin_size(l, &w, &h) || w < 1 || w > 16 || h < 1 || h > 16) return 0;
        const art *a = find(l);
        for (int y = 0; y < h; y++) {
            if ((int)strlen(a->row[y]) != w) return 0;
            for (int x = 0; x < w; x++)
                if (!colour(a->row[y][x])) return 0;
        }
    }
    for (size_t i = 0; i < NARTS; i++)
        if (ARTS[i].layer < T_MOB_FIRST || ARTS[i].layer > T_MOB_LAST) return 0;
    return 1;
}
