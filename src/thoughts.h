/* Thoughts: the player's inner voice. Every frame it reads the body and,
 * now and then, puts what the body feels into words: a thought in the
 * first person ("That hurts.", "I don't feel well.") or a sensation in the
 * second ("You feel cold."). The player learns that something is wrong,
 * and roughly what, without opening the H panel; the numbers stay there.
 *
 * Each feeling (pain, cold, thirst, a racing heart...) has a level 0..3
 * read from the health model. A feeling is voiced when its level rises,
 * again after a while if it lasts (sooner the worse it is), and once more
 * with a line of relief when a strong one clears. A gap between lines
 * keeps the voice from chattering; the most urgent feeling wins it.
 *
 * It only reads the body: nothing here changes health, so it is not part
 * of the deterministic simulation and runs on the frame's dt. A
 * multiplayer client runs it on the body the host sends. It does not draw
 * (hud.c does), and an unconscious or dead body thinks nothing. */
#ifndef MC_THOUGHTS_H
#define MC_THOUGHTS_H

#include "health.h"

#include <stdint.h>

#define THOUGHT_LEN 72

typedef enum {
    FEEL_OUCH,     /* a fresh injury: the hurt flash jumped */
    FEEL_WAKE,     /* coming round after being out */
    FEEL_PAIN,
    FEEL_BLEEDING, /* blood running out of a wound */
    FEEL_FAINT,    /* blood volume lost */
    FEEL_BREATH,   /* low oxygen with the lungs working */
    FEEL_AIR,      /* holding breath, under water or pinned */
    FEEL_HEART,    /* arrhythmia, angina */
    FEEL_BURN,     /* skin hot enough to hurt */
    FEEL_COLD,
    FEEL_HOT,
    FEEL_NUMB,     /* frostbite, numb fingers and toes */
    FEEL_FEVER,    /* infection and sepsis */
    FEEL_SICK,     /* nausea, a poisoned liver, an upset gut */
    FEEL_DAZED,    /* confusion, concussion, a hurt brain */
    FEEL_THIRST,
    FEEL_HUNGER,
    FEEL_TIRED,
    FEEL_RUSH,     /* an adrenaline rush */
    FEEL_CRASH,    /* the shaky spell after it */
    FEEL_DRUNK,
    FEEL_DOPED,    /* an opioid's warm, heavy calm */
    FEEL_SWOON,    /* a vasovagal spell: about to faint */
    FEEL_COUNT
} feeling;

typedef enum { VOICE_INNER, VOICE_BODY } thought_voice;

typedef struct {
    char text[THOUGHT_LEN]; /* empty before the first thought */
    int voice;              /* thought_voice: VOICE_INNER is drawn in quotes */
    int feeling;            /* feeling */
    int level;              /* 1..3, 0 for relief */
    float age;              /* s since it was voiced */
    float life;             /* s it stays on screen */
} thought;

typedef struct {
    thought now;
    float gap;                  /* s until another line may come */
    float since[FEEL_COUNT];    /* s since the feeling was last voiced */
    uint8_t voiced[FEEL_COUNT]; /* level last voiced; 0 when it has cleared */
    uint8_t last[FEEL_COUNT];   /* line last used, so it doesn't come twice running */
    float flash;                /* hurt flash seen last frame */
    int out;                    /* the body was unconscious last frame */
    uint32_t rng;
} thoughts;

/* Starts silent; seed picks among the ways to say each thing. A zeroed
 * struct works too (it seeds itself on the first step). */
void thoughts_init(thoughts *t, uint32_t seed);

/* Advances by dt seconds of real time and voices at most one new line.
 * Returns 1 if t->now changed. */
int thoughts_step(thoughts *t, const health *h, float dt);

/* How strongly the body feels f right now, 0 (not at all) .. 3. */
int thoughts_level(const health *h, int f);

/* How many ways there are to say feeling f at level (0 is relief). */
int thoughts_lines(int f, int level);

/* How visible t->now is (0..1): fades in, holds, fades out. */
float thoughts_alpha(const thoughts *t);

#endif
