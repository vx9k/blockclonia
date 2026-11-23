/* The synthesized sound bank behind sound.c: every effect is generated at
 * start-up from noise, filters, resonant modes and envelopes, and kept as
 * 16-bit mono clips at SYNTH_RATE. Internal to the sound module. */
#ifndef MC_SOUND_SYNTH_H
#define MC_SOUND_SYNTH_H

#include <stdint.h>

/* 24 kHz keeps everything up to 12 kHz, above the highest mode any effect
 * uses, at half the memory of 48 kHz; the mixer resamples anyway (it
 * varies the pitch of every play). */
#define SYNTH_RATE 24000

/* Materials that sound different underfoot or under the hand. */
typedef enum {
    MAT_GENERIC,
    MAT_GRASS,
    MAT_DIRT,
    MAT_STONE,
    MAT_WOOD,
    MAT_SAND,
    MAT_GRAVEL,
    MAT_SNOW,
    MAT_LEAVES,
    MAT_GLASS,
    MAT_WATER,
    MAT_COUNT
} synth_material;

/* Ambient loops. */
typedef enum { LOOP_WIND, LOOP_WATER, LOOP_FIRE, LOOP_COUNT } synth_loop;

#define SYNTH_VARIANTS 3

typedef struct {
    const int16_t *pcm;
    uint32_t len;        /* samples */
    float gain;          /* playback gain that levels it against the others */
} synth_clip;

typedef struct {
    int16_t *pcm;        /* one allocation holding every clip */
    uint32_t used, cap;  /* samples */
    /* [sound_id][material][variant]; sounds without material variants use
     * MAT_GENERIC. A clip with len 0 falls back to MAT_GENERIC. */
    synth_clip clip[32][MAT_COUNT][SYNTH_VARIANTS];
    synth_clip loop[LOOP_COUNT]; /* seamless: sample len-1 flows into 0 */
    double build_ms;     /* how long synthesis took */
} synth_bank;

/* The material class of a block id (grass, stone, wood, ...). */
synth_material synth_material_of(uint8_t block);
/* Whether a sound_id has material variants. */
int synth_has_materials(int id);
/* How many variants a sound_id has (frequent sounds more, 1..3). */
int synth_variants(int id);

/* Builds everything; deterministic for a seed. Returns 0 on success. */
int synth_build(synth_bank *b, uint32_t seed);
void synth_free(synth_bank *b);

/* The clip to play (variant picked by the caller), never NULL-sounding:
 * falls back to the generic material, then to any variant. */
const synth_clip *synth_get(const synth_bank *b, int id, synth_material m, int variant);

#endif
