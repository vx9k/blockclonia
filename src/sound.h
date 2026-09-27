/* Sound: procedural effects mixed in software.
 *
 * Every sound is synthesized at start-up from noise, oscillators and
 * envelopes shaped for its material and event (no audio files, like the
 * textures), then played by a fixed pool of voices with distance
 * attenuation, stereo panning and a low-pass that muffles everything under
 * water. The game thread only posts commands into a lock-free queue; the
 * audio thread mixes in its callback without locks or allocation.
 *
 * The output device is separate (audio_device.h), so this module runs in
 * the unit tests, which mix into a buffer. */
#ifndef MC_SOUND_H
#define MC_SOUND_H

#include "mathlib.h"
#include <stdint.h>

#define SOUND_RATE 48000
#define SOUND_CHANNELS 2
#define SOUND_MAX_FIRES 8

/* One-shot events. Material variants come from the block id passed with
 * the event (grass, stone, wood, sand, gravel, snow, glass, leaves, ...). */
typedef enum {
    SND_STEP,        /* a footstep on a material */
    SND_LAND,        /* landing; intensity ~ impact speed / 10 m/s */
    SND_DIG,         /* hitting a block while breaking it */
    SND_BREAK,       /* a block broken */
    SND_PLACE,       /* a block placed */
    SND_SHATTER,     /* glass or ice shattering */
    SND_THUD,        /* a falling block hitting the ground; intensity ~ speed / 10 m/s */
    SND_SPLASH,      /* something entering water; intensity ~ speed / 10 m/s */
    SND_SWIM,        /* a swim stroke */
    SND_PICKUP,      /* an item picked up */
    SND_DROP,        /* an item thrown */
    SND_HURT,        /* the body hurt; intensity = how badly */
    SND_EAT,
    SND_DRINK,
    SND_BUCKET_FILL,
    SND_BUCKET_POUR,
    SND_FIRE_FEED,   /* fuel added to a fire */
    SND_FIRE_OUT,    /* a fire going out */
    SND_HEARTBEAT,   /* one lub-dub; intensity = how hard it is felt */
    SND_BREATH,      /* one laboured breath; intensity = effort */
    SND_UI_CLICK,
    SND_UI_HOVER,
    SND_UI_OPEN,
    SND_UI_CLOSE,
    SND_CRAFT,
    SND_COUNT
} sound_id;

typedef struct sound sound;

/* rate and channels are the mixer's output format (the device converts if
 * it has to). Never fails: without a device the game just renders nothing. */
sound *sound_create(uint32_t seed, uint32_t rate, int channels);
void sound_destroy(sound *s);

/* 0..1 each; effective volume is master * effects (or * ambient). */
void sound_set_volumes(sound *s, float master, float effects, float ambient);

/* Where the ears are. yaw/pitch as the camera; underwater muffles. */
void sound_listener(sound *s, dvec3 pos, float yaw, float pitch, int underwater);

/* Plays a one-shot at a world position, or at the listener when pos is
 * NULL (UI, the body's own sounds). material is a block id or 0. */
void sound_play(sound *s, int id, const dvec3 *pos, uint8_t material, float intensity);

/* Continuous ambience, set every frame (0 silences): wind strength 0..1,
 * nearby water 0..1. Burning campfires as positions (crackle loops). */
void sound_ambience(sound *s, float wind, float water);
void sound_fires(sound *s, const dvec3 *pos, int n);

/* Once per frame on the game thread: sends the listener, ambience and
 * fires to the mixer. */
void sound_update(sound *s, float dt);

/* The audio callback: mixes `frames` interleaved frames into out. Called
 * on the audio thread (or by tests); lock-free, allocation-free. */
void sound_render(void *user, float *out, uint32_t frames);

/* For the F3 overlay: voices playing, the mixer's share of real time. */
void sound_stats(const sound *s, int *voices, float *load);

#endif
