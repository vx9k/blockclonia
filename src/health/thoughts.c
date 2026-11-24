#include "thoughts.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* How long a feeling may go unmentioned while it lasts, by level: a mild
 * one comes back every few minutes, a severe one every half minute. */
static const float REMIND_S[4] = {0.0f, 150.0f, 75.0f, 35.0f};
#define RISE_HOLD_S 12.0f /* a feeling that rises again this soon waits, unless it turns severe */
#define OUCH_HOLD_S 1.5f  /* one cry per blow, not one per frame of a fall */
#define FADE_IN_S 0.25f
#define FADE_OUT_S 0.8f

typedef struct {
    uint8_t f, level, voice;
    const char *text;
} line;

#define I VOICE_INNER
#define B VOICE_BODY
/* Level 0 is the relief when a feeling of level 2 or more clears. "{part}"
 * becomes the part that hurts most, "{ends}" the fingers or the toes. */
static const line LINES[] = {
    {FEEL_OUCH, 1, I, "Ow!"},
    {FEEL_OUCH, 1, I, "Ouch."},
    {FEEL_OUCH, 2, I, "Argh!"},
    {FEEL_OUCH, 2, I, "That hurts!"},
    {FEEL_OUCH, 3, I, "Aaargh!"},
    {FEEL_OUCH, 3, I, "Oh god, that's bad."},

    {FEEL_WAKE, 1, I, "Ugh... what happened?"},
    {FEEL_WAKE, 1, I, "Where... how long was I out?"},
    {FEEL_WAKE, 1, B, "You come round, groggy."},

    {FEEL_PAIN, 1, I, "My {part} hurts."},
    {FEEL_PAIN, 1, B, "Your {part} aches."},
    {FEEL_PAIN, 2, I, "That really hurts."},
    {FEEL_PAIN, 2, I, "My {part}... it hurts so much."},
    {FEEL_PAIN, 2, B, "A throbbing pain in your {part}."},
    {FEEL_PAIN, 3, I, "I can't take this pain."},
    {FEEL_PAIN, 3, I, "Make it stop..."},
    {FEEL_PAIN, 3, B, "The pain is all you can think about."},
    {FEEL_PAIN, 0, I, "That's better."},
    {FEEL_PAIN, 0, B, "The pain eases."},

    {FEEL_BLEEDING, 1, I, "I'm bleeding."},
    {FEEL_BLEEDING, 1, B, "Blood trickles from the wound."},
    {FEEL_BLEEDING, 2, I, "I need to stop this bleeding."},
    {FEEL_BLEEDING, 2, B, "Blood is running freely."},
    {FEEL_BLEEDING, 3, I, "So much blood... I need a bandage, now!"},
    {FEEL_BLEEDING, 3, B, "Blood is pumping out of you."},
    {FEEL_BLEEDING, 0, I, "The bleeding's stopped."},

    {FEEL_FAINT, 1, B, "You feel lightheaded."},
    {FEEL_FAINT, 1, I, "I feel a bit woozy."},
    {FEEL_FAINT, 2, I, "I feel faint. I've lost a lot of blood."},
    {FEEL_FAINT, 2, B, "Your hands are cold and clammy."},
    {FEEL_FAINT, 3, B, "Everything is going grey."},
    {FEEL_FAINT, 3, I, "I'm going to pass out..."},
    {FEEL_FAINT, 0, B, "The dizziness passes."},

    {FEEL_BREATH, 1, B, "You can't quite catch your breath."},
    {FEEL_BREATH, 2, I, "I can't breathe properly."},
    {FEEL_BREATH, 2, B, "Every breath is a struggle."},
    {FEEL_BREATH, 3, I, "Air... I can't get any air!"},
    {FEEL_BREATH, 3, B, "Your lips feel numb and your vision swims."},
    {FEEL_BREATH, 0, B, "Your breathing settles."},

    {FEEL_AIR, 1, B, "Your lungs start to burn."},
    {FEEL_AIR, 2, I, "I need air!"},
    {FEEL_AIR, 2, B, "Your chest heaves for a breath."},
    {FEEL_AIR, 3, I, "Air! I need air now!"},
    {FEEL_AIR, 0, I, "Air! Sweet air."},

    {FEEL_HEART, 1, B, "Your heart skips and flutters."},
    {FEEL_HEART, 1, I, "My heartbeat feels wrong."},
    {FEEL_HEART, 2, I, "My chest feels tight."},
    {FEEL_HEART, 2, B, "Your heart is racing out of rhythm."},
    {FEEL_HEART, 3, B, "A crushing weight on your chest."},
    {FEEL_HEART, 3, I, "My chest... something's wrong with my heart."},
    {FEEL_HEART, 0, B, "Your heartbeat steadies."},

    {FEEL_BURN, 1, I, "That's hot."},
    {FEEL_BURN, 1, B, "The heat stings your skin."},
    {FEEL_BURN, 2, I, "It burns!"},
    {FEEL_BURN, 2, I, "Hot, hot, hot!"},
    {FEEL_BURN, 3, I, "Aaah! I'm burning!"},
    {FEEL_BURN, 3, B, "Searing pain. Get away from the heat!"},

    {FEEL_COLD, 1, B, "You feel cold."},
    {FEEL_COLD, 1, I, "Brr. It's cold."},
    {FEEL_COLD, 2, I, "I can't stop shivering."},
    {FEEL_COLD, 2, B, "Your teeth are chattering."},
    {FEEL_COLD, 3, I, "So cold... so tired..."},
    {FEEL_COLD, 3, B, "You've stopped shivering. That's not good."},
    {FEEL_COLD, 0, I, "Ah, warmth."},
    {FEEL_COLD, 0, B, "You feel warm again."},

    {FEEL_HOT, 1, B, "You're sweating."},
    {FEEL_HOT, 1, I, "It's hot."},
    {FEEL_HOT, 2, I, "It's too hot. I feel dizzy."},
    {FEEL_HOT, 2, B, "Sweat stings your eyes and your head throbs."},
    {FEEL_HOT, 3, I, "I have to cool down, now."},
    {FEEL_HOT, 3, B, "You feel sick from the heat."},
    {FEEL_HOT, 0, I, "That's cooler."},

    {FEEL_NUMB, 1, B, "Your {ends} are going numb."},
    {FEEL_NUMB, 1, I, "I can't feel my {ends}."},
    {FEEL_NUMB, 2, B, "Your {ends} are white and stiff."},
    {FEEL_NUMB, 3, I, "My {ends}... they're frozen solid."},
    {FEEL_NUMB, 0, B, "Pins and needles as feeling returns."},

    {FEEL_FEVER, 1, I, "I don't feel well."},
    {FEEL_FEVER, 1, B, "The wound throbs, red and hot."},
    {FEEL_FEVER, 2, I, "I feel awful. Hot and cold at once."},
    {FEEL_FEVER, 2, B, "You're shivering, but your skin is burning."},
    {FEEL_FEVER, 3, I, "Something is very wrong with me."},
    {FEEL_FEVER, 3, B, "Your heart races and your thoughts drift."},
    {FEEL_FEVER, 0, I, "I think the fever's broken."},

    {FEEL_SICK, 1, I, "I don't feel well."},
    {FEEL_SICK, 1, B, "You feel queasy."},
    {FEEL_SICK, 2, I, "I think I'm going to be sick."},
    {FEEL_SICK, 2, B, "Your stomach turns over."},
    {FEEL_SICK, 3, I, "Everything aches. Something's badly wrong inside."},
    {FEEL_SICK, 3, B, "You feel poisoned, heavy and sick."},
    {FEEL_SICK, 0, B, "Your stomach settles."},

    {FEEL_DAZED, 1, B, "You feel unsteady on your feet."},
    {FEEL_DAZED, 2, I, "Where... where am I?"},
    {FEEL_DAZED, 2, I, "Everything's spinning."},
    {FEEL_DAZED, 2, B, "Your thoughts won't line up."},
    {FEEL_DAZED, 3, I, "Can't... think..."},
    {FEEL_DAZED, 0, I, "My head's clearing."},

    {FEEL_THIRST, 1, I, "I'm thirsty."},
    {FEEL_THIRST, 1, B, "Your mouth feels dry."},
    {FEEL_THIRST, 2, I, "I really need a drink."},
    {FEEL_THIRST, 2, B, "Your lips are cracked and your head aches."},
    {FEEL_THIRST, 3, I, "Water... I need water."},
    {FEEL_THIRST, 0, I, "That's much better."},

    {FEEL_HUNGER, 1, B, "Your stomach growls."},
    {FEEL_HUNGER, 1, I, "I could eat something."},
    {FEEL_HUNGER, 2, I, "I'm starving."},
    {FEEL_HUNGER, 2, B, "Hunger gnaws at you."},
    {FEEL_HUNGER, 3, I, "I feel weak with hunger."},
    {FEEL_HUNGER, 3, B, "Your hands shake. You need food."},
    {FEEL_HUNGER, 0, I, "Mm, that hit the spot."},

    {FEEL_TIRED, 1, I, "I need to catch my breath."},
    {FEEL_TIRED, 1, B, "You're panting."},
    {FEEL_TIRED, 2, I, "I can't keep going like this."},
    {FEEL_TIRED, 2, B, "Your legs are heavy as lead."},

    {FEEL_RUSH, 1, B, "Your heart is pounding."},
    {FEEL_RUSH, 2, B, "Everything feels sharp and fast."},
    {FEEL_RUSH, 2, I, "Move. Move!"},

    {FEEL_CRASH, 1, B, "You feel shaky and drained."},
    {FEEL_CRASH, 2, I, "I'm so tired. My hands won't stop shaking."},

    {FEEL_DRUNK, 1, B, "A warm glow spreads through you."},
    {FEEL_DRUNK, 1, I, "Mm. That's warming."},
    {FEEL_DRUNK, 2, I, "Whoa. The ground's moving."},
    {FEEL_DRUNK, 2, B, "Your feet don't quite go where you put them."},
    {FEEL_DRUNK, 3, I, "I've had way too much..."},
    {FEEL_DRUNK, 3, B, "The world spins and your stomach heaves."},
    {FEEL_DRUNK, 0, I, "Ugh. My head."},

    {FEEL_DOPED, 1, B, "A warm, heavy calm settles over you."},
    {FEEL_DOPED, 2, I, "So... sleepy..."},
    {FEEL_DOPED, 2, B, "Your eyelids are heavy and your breathing slow."},
    {FEEL_DOPED, 3, B, "Each breath comes later than the last."},
    {FEEL_DOPED, 0, B, "The heaviness lifts."},

    {FEEL_SWOON, 1, B, "You break out in a cold sweat."},
    {FEEL_SWOON, 1, I, "Oh. I don't feel right."},
    {FEEL_SWOON, 2, B, "Your ears ring and your stomach turns."},
    {FEEL_SWOON, 2, I, "I need to get down. Now."},
    {FEEL_SWOON, 3, B, "The world drains of colour."},
    {FEEL_SWOON, 3, I, "Going... down..."},
    {FEEL_SWOON, 0, B, "The colour comes back to the world."},

    {FEEL_FEAR, 1, B, "Something is moving out there."},
    {FEEL_FEAR, 1, I, "Was that a growl?"},
    {FEEL_FEAR, 2, B, "The hair on your neck stands up."},
    {FEEL_FEAR, 2, I, "It's following me."},
    {FEEL_FEAR, 3, B, "Teeth, and closing fast."},
    {FEEL_FEAR, 3, I, "It's coming for me!"},
    {FEEL_FEAR, 0, B, "Whatever it was, it's gone."},

    {FEEL_DARK, 1, I, "It's getting hard to see."},
    {FEEL_DARK, 1, B, "The shapes around you lose their edges."},
    {FEEL_DARK, 2, I, "It's dark. I can barely see my hands."},
    {FEEL_DARK, 2, B, "You strain to see in the dark."},
    {FEEL_DARK, 3, I, "It's pitch black."},
    {FEEL_DARK, 3, I, "I can't see a thing."},
    {FEEL_DARK, 0, I, "Light, finally."},
};
#undef I
#undef B
#define NLINES ((int)(sizeof LINES / sizeof LINES[0]))
_Static_assert(sizeof LINES / sizeof LINES[0] <= 256, "thoughts.last holds a line index in a byte");

/* Where the pain is, the way a person would say it. */
static const char *const PART_SAID[BP_COUNT] = {"head",      "chest",    "belly",    "left arm",
                                                "right arm", "left leg", "right leg"};

static float maxf(float a, float b) { return a > b ? a : b; }
static int maxi(int a, int b) { return a > b ? a : b; }

/* 0..3 by the thresholds a < b < c. */
static int grade(float v, float a, float b, float c) { return v >= c ? 3 : (v >= b ? 2 : (v >= a ? 1 : 0)); }

static uint32_t next(thoughts *t)
{
    uint32_t x = t->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t->rng = x;
    return x;
}

static int worst_part(const health *h)
{
    int w = 0;
    for (int i = 1; i < BP_COUNT; i++)
        if (h->part[i].pain > h->part[w].pain) w = i;
    return w;
}

/* The fingers or the toes, whichever froze or cooled further. */
static const char *numb_ends(const health *h)
{
    int arms = maxi(h->part[BP_LARM].frost, h->part[BP_RARM].frost);
    int legs = maxi(h->part[BP_LLEG].frost, h->part[BP_RLEG].frost);
    if (arms != legs) return arms > legs ? "fingers" : "toes";
    float a = h->part[BP_LARM].skin < h->part[BP_RARM].skin ? h->part[BP_LARM].skin : h->part[BP_RARM].skin;
    float l = h->part[BP_LLEG].skin < h->part[BP_RLEG].skin ? h->part[BP_LLEG].skin : h->part[BP_RLEG].skin;
    return a <= l ? "fingers" : "toes";
}

static int level_body(const health *h, int f)
{
    switch (f) {
    case FEEL_PAIN: return grade(h->pain, 2.5f, 5.0f, 7.5f);
    case FEEL_BLEEDING: return grade(h->bleed_ext, 5.0f, 30.0f, 100.0f);
    case FEEL_FAINT: return grade(1.0f - h->blood / health_blood_normal(h), 0.12f, 0.22f, 0.32f);
    case FEEL_BREATH:
        if (!h->breathing) return 0;
        return maxi(grade(0.95f - h->sao2, 0.03f, 0.1f, 0.2f), h->pneumothorax > 0.2f ? 2 : 0);
    case FEEL_AIR:
        if (h->breathing) return 0;
        return maxi(grade(h->paco2, 47.0f, 52.0f, 58.0f), h->sao2 < 0.85f ? 3 : 0);
    case FEEL_HEART: {
        int r = h->rhythm == RHYTHM_VT ? 2 : (h->rhythm == RHYTHM_AF || h->rhythm == RHYTHM_AVB3 ? 1 : 0);
        return maxi(r, grade(h->ischemia, 0.6f, 1.0f, 2.0f));
    }
    default: return 0;
    }
}

static int level_heat(const health *h, int f)
{
    switch (f) {
    case FEEL_BURN: {
        float hot = 0.0f;
        for (int i = 0; i < BP_COUNT; i++) hot = maxf(hot, h->part[i].skin + h->part[i].surface);
        return h->in_fire ? 3 : grade(hot, 42.5f, 44.5f, 48.0f);
    }
    case FEEL_COLD: {
        int l = grade(37.0f - h->temp, 0.8f, 1.7f, 3.0f);
        if (health_skin_mean(h) < 27.0f || (h->wet > 0.5f && h->air_temp < 10.0f)) l = maxi(l, 1);
        return l;
    }
    case FEEL_HOT:
        if (h->sepsis > 0.15f) return 0; /* a fever, not the weather */
        return maxi(grade(h->temp, 37.9f, 38.8f, 39.8f), grade(h->radiant, 1500.0f, 2500.0f, 1e9f));
    case FEEL_NUMB: {
        int l = 0;
        float cold = 99.0f;
        for (int i = BP_LARM; i < BP_COUNT; i++) {
            l = maxi(l, h->part[i].frost);
            cold = h->part[i].skin < cold ? h->part[i].skin : cold;
        }
        return maxi(l, cold < 8.0f ? 1 : 0);
    }
    default: return 0;
    }
}

static int level_inside(const health *h, int f)
{
    switch (f) {
    case FEEL_FEVER: {
        float inf = 0.0f;
        for (int i = 0; i < h->wound_count; i++) inf = maxf(inf, h->wounds[i].infection);
        return maxi(grade(h->sepsis, 0.1f, 0.35f, 0.65f), inf > 0.3f ? 1 : 0);
    }
    case FEEL_SICK: {
        int l = h->gut_upset > 0.0f || h->myoglobin > 5.0f ? 1 : 0;
        l = maxi(l, grade(1.0f - h->organ[ORG_LIVER], 0.4f, 0.65f, 0.85f));
        l = maxi(l, grade(1.0f - h->organ[ORG_KIDNEYS], 0.6f, 0.8f, 0.95f));
        if (h->hepatotox > 0.02f || h->potassium > 6.0f) l = maxi(l, 2);
        return l;
    }
    case FEEL_DAZED: {
        int l = health_impaired(h) || h->confusion > 0.0f ? 2 : 0;
        return maxi(l, grade(1.0f - h->organ[ORG_BRAIN], 0.2f, 0.45f, 0.7f));
    }
    case FEEL_THIRST: return grade(1.0f - health_hydration(h), 0.5f, 0.65f, 0.85f);
    case FEEL_HUNGER: return grade(health_hunger(h), 0.55f, 0.7f, 0.95f);
    case FEEL_TIRED: return grade(0.2f - health_stamina(h), 0.05f, 0.15f, 9.0f);
    case FEEL_RUSH: return grade(h->catechol, 0.6f, 2.0f, 1e9f);
    case FEEL_CRASH: return h->catechol < 0.3f ? grade(h->crash, 0.35f, 0.7f, 9.0f) : 0;
    case FEEL_DRUNK: return grade(h->bac, 0.4f, 1.2f, 2.2f);
    case FEEL_SWOON: return grade(h->vagal, 0.08f, 0.3f, 0.6f);
    case FEEL_FEAR: return grade(h->fear, 0.15f, 0.45f, 0.75f);
    /* A moonlit field is level 2, a lightless cave 3. */
    case FEEL_DARK: return grade(h->dark, 0.6f, 0.85f, 0.99f);
    case FEEL_DOPED: return grade(h->naloxone > 0.0f ? 0.03f * h->opioid : h->opioid, 4.0f, 15.0f, 30.0f);
    default: return 0;
    }
}

int thoughts_level(const health *h, int f)
{
    if (h->dead || !health_awake(h) || f < 0 || f >= FEEL_COUNT) return 0;
    return maxi(maxi(level_body(h, f), level_heat(h, f)), level_inside(h, f));
}

/* Voices a line of feeling f at level; 0 if there is no such line. */
static int say(thoughts *t, const health *h, int f, int level)
{
    int pick[16] = {0};
    int n = 0;
    for (int i = 0; i < NLINES && n < 16; i++)
        if (LINES[i].f == f && LINES[i].level == level) pick[n++] = i;
    if (!n) return 0;
    int j = (int)(next(t) % (uint32_t)n);
    if (n > 1 && pick[j] == t->last[f]) j = (j + 1 + (int)(next(t) % (uint32_t)(n - 1))) % n;
    int k = pick[j];
    t->last[f] = (uint8_t)k;

    const line *l = &LINES[k];
    const char *src = l->text;
    char *out = t->now.text;
    size_t o = 0;
    while (*src && o + 1 < THOUGHT_LEN) {
        const char *word = !strncmp(src, "{part}", 6) ? PART_SAID[worst_part(h)]
                                                      : (!strncmp(src, "{ends}", 6) ? numb_ends(h) : NULL);
        if (word) {
            o += (size_t)snprintf(out + o, THOUGHT_LEN - o, "%s", word);
            if (o >= THOUGHT_LEN) o = THOUGHT_LEN - 1;
            src += 6;
        } else out[o++] = *src++;
    }
    out[o] = '\0';
    /* A muddled mind trails off. */
    if (l->voice == VOICE_INNER && health_impaired(h) && o > 0 && out[o - 1] == '.' && o + 3 < THOUGHT_LEN)
        memcpy(out + o, "..", 3);

    size_t len = strlen(out);
    t->now.voice = l->voice;
    t->now.feeling = f;
    t->now.level = level;
    t->now.age = 0.0f;
    t->now.seed = next(t);
    t->now.life = 2.5f + 0.06f * (float)len;
    t->gap = t->now.life + 2.0f;
    t->since[f] = 0.0f;
    return 1;
}

void thoughts_init(thoughts *t, uint32_t seed)
{
    memset(t, 0, sizeof *t);
    /* Scramble the seed (a murmur3 finaliser): xorshift barely stirs small
     * seeds at first, so seeds 1, 2, 3 would all pick the same lines. */
    uint32_t x = seed + 0x9e3779b9u;
    x = (x ^ (x >> 16)) * 0x85ebca6bu;
    x = (x ^ (x >> 13)) * 0xc2b2ae35u;
    x ^= x >> 16;
    t->rng = x ? x : 0x2545f491u;
    for (int f = 0; f < FEEL_COUNT; f++) t->since[f] = 1e6f;
}

/* Fell silent: lose consciousness or die, and every feeling starts over. */
static void go_quiet(thoughts *t)
{
    t->now.age = t->now.life;
    memset(t->voiced, 0, sizeof t->voiced);
    t->out = 1;
}

int thoughts_step(thoughts *t, const health *h, float dt)
{
    if (!t->rng) thoughts_init(t, 0);
    t->now.age += dt;
    t->gap = maxf(0.0f, t->gap - dt);
    for (int f = 0; f < FEEL_COUNT; f++) t->since[f] += dt;
    float jump = h->hurt_flash - t->flash;
    t->flash = h->hurt_flash;
    if (h->dead || !health_awake(h)) {
        go_quiet(t);
        return 0;
    }

    /* A blow gets a cry at once, whatever else is being said. */
    if (jump > 0.12f && t->since[FEEL_OUCH] >= OUCH_HOLD_S) {
        return say(t, h, FEEL_OUCH, grade(jump, 0.12f, 0.35f, 0.6f));
    }
    if (t->out) {
        t->out = 0;
        return say(t, h, FEEL_WAKE, 1);
    }

    /* Everything that wants saying; the most urgent wins the gap. */
    int best = -1, best_level = 0, best_score = -1;
    for (int f = FEEL_PAIN; f < FEEL_COUNT; f++) {
        int lv = thoughts_level(h, f);
        int was = t->voiced[f];
        int score = -1, say_level = lv;
        if (lv > was && (t->since[f] >= RISE_HOLD_S || lv == 3)) score = lv * 100 + 50 - f;
        else if (lv > 0 && lv >= was && t->since[f] >= REMIND_S[lv]) score = lv * 100 - f;
        else if (lv == 0 && was >= 2) {
            score = 10 - f / 4;
            say_level = 0;
        } else if (lv < was) t->voiced[f] = (uint8_t)lv; /* eased without a word */
        if (score > best_score) {
            best = f;
            best_level = say_level;
            best_score = score;
        }
    }
    if (best < 0 || t->gap > 0.0f) return 0;
    t->voiced[best] = (uint8_t)best_level;
    return say(t, h, best, best_level);
}

int thoughts_lines(int f, int level)
{
    int n = 0;
    for (int i = 0; i < NLINES; i++) n += LINES[i].f == f && LINES[i].level == level;
    return n;
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float ramp(float v, float a, float b) { return clamp01((v - a) / (b - a)); }

/* Colours by feeling, r g b; 0,0,0 keeps the voice's own. */
static const uint8_t TINT[FEEL_COUNT][3] = {
    [FEEL_OUCH] = {255, 130, 115},  [FEEL_PAIN] = {255, 130, 115},  [FEEL_BLEEDING] = {235, 90, 90},
    [FEEL_BURN] = {255, 165, 80},   [FEEL_HOT] = {255, 180, 110},   [FEEL_FEVER] = {250, 170, 120},
    [FEEL_COLD] = {150, 210, 255},  [FEEL_NUMB] = {170, 200, 240},  [FEEL_SICK] = {185, 215, 130},
    [FEEL_DRUNK] = {240, 200, 110}, [FEEL_DOPED] = {200, 170, 255}, [FEEL_SWOON] = {195, 195, 200},
    [FEEL_FAINT] = {195, 195, 200}, [FEEL_AIR] = {150, 225, 235},   [FEEL_BREATH] = {150, 225, 235},
    [FEEL_FEAR] = {225, 160, 215},  [FEEL_DARK] = {150, 150, 175},
};

thought_style thoughts_style(const thought *t, const health *h)
{
    thought_style s = {255, 244, 220, 0.0f, 0.0f, 0.0f};
    if (t->voice == VOICE_BODY) s = (thought_style){175, 200, 225, 0.0f, 0.0f, 0.0f};
    if (t->feeling >= 0 && t->feeling < FEEL_COUNT && TINT[t->feeling][0]) {
        s.r = TINT[t->feeling][0];
        s.g = TINT[t->feeling][1];
        s.b = TINT[t->feeling][2];
    }
    /* The opioid's share of sedation, without the drink's. */
    float opi = health_sedation(h) - h->bac / 3.5f;
    s.wave = clamp01(ramp(h->bac, 0.4f, 2.5f) + ramp(opi, 0.1f, 0.45f) + 0.8f * h->vagal);
    s.glitch = fmaxf(ramp(0.88f - h->sao2, 0.0f, 0.25f), ramp(0.85f - h->brain_o2, 0.0f, 0.4f));
    s.garble = ramp(1.0f - h->organ[ORG_BRAIN], 0.1f, 0.8f);
    return s;
}

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    x *= 0xC2B2AE35u;
    return x ^ (x >> 16);
}

static int is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

void thoughts_garble(char *out, size_t n, const char *in, float garble, uint32_t seed)
{
    if (!n) return;
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < n; i++) {
        char c = in[i];
        uint32_t r = mix(seed ^ ((uint32_t)i * 0x9E3779B1u));
        /* Up to ~70% of the letters touched at the worst: still a voice,
         * hardly words. */
        if (!is_letter(c) || (float)(r & 0xFFFFu) / 65536.0f >= 0.7f * garble) {
            out[o++] = c;
            continue;
        }
        uint32_t how = (r >> 16) % 10u;
        if (how < 4) { /* a wrong letter, keeping the case */
            char base = c >= 'a' ? 'a' : 'A';
            out[o++] = (char)(base + (char)((r >> 20) % 26u));
        } else if (how < 7 && is_letter(in[i + 1]) && o + 2 < n) { /* two letters swapped */
            out[o++] = in[i + 1];
            out[o++] = c;
            i++;
        } else if (how >= 7) { /* lost */
        } else out[o++] = c;
    }
    out[o] = '\0';
}

float thoughts_alpha(const thoughts *t)
{
    const thought *n = &t->now;
    if (!n->text[0] || n->age >= n->life) return 0.0f;
    if (n->age < FADE_IN_S) return n->age / FADE_IN_S;
    float left = n->life - n->age;
    return left < FADE_OUT_S ? left / FADE_OUT_S : 1.0f;
}
