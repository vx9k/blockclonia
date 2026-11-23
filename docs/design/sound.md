# Sound

How the game should sound: sounds that read as the real thing, heard
from where they happen, through the air, rock and water between (planned
2026-11-23, issue #110).

What is done and what is next lives in [TODO.md](../../TODO.md); this
page holds the reasoning and the detail.

## Where it stands

**Every sound in the game today is a placeholder.** The maintainer's
verdict (2026-11-23): they all sound artificial. `src/audio/sound_synth.c`
builds them from textbook pieces: filtered noise, a few decaying sine
modes, swept sines, envelopes. Each is recognisable as what it stands
for, but none sounds like a recording of it. The mixer (`src/audio/sound.c`)
has only inverse-distance falloff, equal-power panning, a little
attenuation behind the listener and one low-pass under water: no walls,
no echo, no sense of a cave or a field.

So:

- Don't tune, extend or copy the current synthesis recipes. A new sound
  is made the way the overhaul makes them (below), or waits for it.
- Don't count "it plays and the test passes" as done for a sound. A
  sound is done when it holds up next to a recording of the real thing,
  and the maintainer has listened to it.
- The pipeline stays: every sound is synthesized at start-up (no audio
  files), within ~100 ms on a desktop CPU, deterministic for a seed, and
  the audio callback takes no locks, allocates nothing and does no I/O.

## Order

Agreed 2026-11-23: all the effects first, then the sounds themselves, so
each new sound is judged in the setting it will be heard in. One pull
request per step.

**S0. A listening tool** (done: `src/audio/audition.c`).
`--sound-render <dir>` plays every sound through the real mixer and
writes WAV files: every variant and material dry, and one take of each
sound 2 m ahead, 8 m off to the right, 16 m away and under water, with
the wind, water and fire loops. `index.csv` lists each file's length,
peak, RMS and spectral centroid. It is headless and runs without an
audio device. Each later step is judged by rendering before and after
and listening, and by comparing spectra and decay times with
recordings. The settings grow as S1 and S2 add effects: an open field,
a cave and a small room once propagation knows the world, and longer
distances once sounds carry further.

**S1. The spatial core** (in `sound.c` and a new propagation module):

- Occlusion: a few rays from source to listener through the voxel grid.
  Each solid block on the path takes level and treble away by its
  material (wood passes more than stone; leaves, glass and water little).
- Air absorption after ISO 9613-1: high frequencies fade with distance,
  more in dry, cold air. A low-pass per voice, from the band coefficients
  at the air's temperature and humidity (`thermo`, `atmos`).
- Propagation delay: sound travels at the speed of sound from `atmos`
  (about 343 m/s), so distant gunshots and thunder arrive late.
- Under water: the listener's head under water filters everything
  through the water's transfer, and sounds in the water carry farther.
- Reverb: a feedback delay network whose decay per band comes from the
  space around the listener. Short rays estimate its volume, surface and
  mean absorption (rock reflects, leaves and snow absorb), and Sabine and
  Eyring give the decay time. A cave rings, a forest barely does, an open
  field is dry.

**S2. Propagation, direction and hearing:**

- Sound through openings: a coarse flood of the air around the listener
  finds the shortest open path to each source. A sound behind a wall
  comes from the doorway, with the path's length and the diffraction
  around corners setting its level and tone.
- Direction: a head model instead of plain panning. A spherical head
  (head shadow, interaural time difference), a pinna echo and a shoulder
  reflection, after Brown and Duda, synthesized with no measured data
  files, so front, back, above and below can be told apart.
- Hearing for creatures: mobs hear what the player hears, with the same
  propagation, so a shot in a cave carries and footsteps in snow don't.

**S3. Re-synthesis, family by family**, each one a pull request judged
with the S0 renders:

1. Footsteps and landings: granular contact (PhISEM) per surface:
   gravel, sand, snow, leaves, wood, stone, metal; with weight, pace and
   shoes.
2. Breaking, digging, placing, falling blocks: modal synthesis driven by
   contact forces (FoleyAutomatic), with modes per material and size, and
   the debris that follows.
3. Water: bubbles (Minnaert resonances with rising pitch), splashes,
   pouring, streams, rain, swimming.
4. Fire: crackle (wood cells bursting), the roar of the flame, embers.
5. Wind: turbulence and its whistle through edges and leaves, gusts
   coupled to the weather (#108).
6. Body: heartbeat, breathing, eating, drinking, hurt.
7. Weapons: the shot's muzzle blast, the supersonic crack and its echo.
8. Creatures, then the interface last.

## Credit

Every paper or standard a step implements gets a full citation in
[CREDITS.md](../../CREDITS.md) (all authors, title, venue, year, link,
what was taken and which file uses it), and a short comment at the code
that implements it. Only what is implemented is credited, added in the
pull request that implements it. Planned sources:

- S1: ISO 9613-1:1993 (air absorption); Jot and Chaigne 1991, and
  Rocchesso and Smith 1997 (feedback delay networks); Sabine and Eyring
  (reverberation time).
- S2: Brown and Duda 1998, "A Structural Model for Binaural Sound
  Synthesis"; Raghuvanshi and Snyder 2018, "Parametric Directional
  Coding for Precomputed Sound Propagation" (the quantities a path
  carries); Schissler and Manocha 2011, GSound (path caching).
- S3: van den Doel, Kry and Pai 2001, "FoleyAutomatic"; van den Doel
  2005, "Physically based models for liquid sounds"; Cook 1997, PhISEM.
