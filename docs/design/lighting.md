# Lighting

Sky light that reaches into caves and under roofs, light from fire and
lamps, and a sun that moves and casts shade (planned 2026-11-16).

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

## Where it stands

- `block.vert` lights a face as `FACE_SHADE[face] * AO_CURVE[ao] *
  daylight`.
- The ambient occlusion is baked into the vertex by the mesher (2 bits).
  Daylight is one number for the whole world, pushed per frame.
  Burning blocks are drawn at full brightness.
- So a cave 50 m down is as bright as a meadow at noon, a room with no
  windows is lit, and nothing but the sky gives off light: a campfire
  heats (`thermo`) but doesn't light. The furnace, lamps, lava and
  underground exploration all need this fixed.
- The block vertex is 4 bytes and every bit is used: position, face, AO,
  texture layer, and the water drop (`mesher.h`). Light has to reach the
  shader some other way, or the format has to change.

## Goals

- **Sky light:** 15 where the sky is open, falling off under overhangs,
  into cave mouths and through windows; dark deep underground.
  Daylight still scales it with the time of day.
- **Block light:** campfires, furnaces, lava, torches and lamps, with a
  colour (warm fire, cool electric light later), flickering for flames.
  Hot blocks glow by their `thermo` temperature (red-hot iron, lava,
  embers), so light follows the same physics as heat.
- **Moving lights:** a held torch or lantern lights the space around the
  player. Vehicle headlights come later.
- **Sun and moon:** the light's direction follows the time of day
  (`thermo_day_time`), with warm, low light at dawn and dusk and
  moonlight at night. Shadows from the sun are an optional
  capability.
- **Weak hardware first:** everything on the bare Vulkan 1.2 path with
  no optional features, at an integrated GPU's frame budget. Anything
  heavier (sun shadows, smoother light) is a `GPUCAP_*` feature with a
  fallback on bare 1.2, per `gpucaps.c`.

## Design

1. **Light values in the world.** Each block cell gets a sky level and a
   block level, 4 bits each (one byte per cell, in a light array next
   to the block array), computed on the worker threads by flood fill
   (BFS):
   - sky light falls straight down from open sky without loss, then
     spreads sideways losing one level per block;
   - block light spreads from emitters losing one level per block.

   An edit relights incrementally (removal then re-addition queues, the
   standard voxel approach), and light that crosses a column border is
   re-derived on both sides, as worldgen does.
2. **Getting light to the shader.** This is the format decision, made
   before any code, with these options:
   - (a) a second 2-byte vertex stream holding the sky and block levels
     per corner, smoothed like AO;
   - (b) a small 3D light texture per section (18³ with padding),
     sampled in the fragment shader with linear filtering, which gives
     smooth light for free and leaves the vertex alone;
   - (c) a larger vertex.

   Options (a) and (b) both work on Vulkan 1.2. Measure memory and
   frame time on an integrated GPU and lavapipe before choosing. The 4-byte vertex
   is kept unless the numbers say otherwise.
3. **Colour.** Block light carries a hue per emitter type, mixed in the
   shader. Either stay single-channel and tint by the nearest emitter,
   or use three channels where memory allows.
4. **Entities and items** take the light at their position (the
   creatures already get a `light` multiplier in `entity_instance`).
5. **Time of day** keeps scaling only the sky light, so a lit room
   stays lit at night.
6. **Optional:** sun shadow maps (cascades, behind a `GPUCAP_*` bit),
   and light shafts and bloom in the existing post-process pass.

## Gameplay

- Darkness matters: the view is truly dark underground and at night
  without a light, and creatures use light (see animals: sight is worse
  in the dark, and some predators avoid fire).
- The inner voice notices darkness ("It's pitch black.").
- Settings: a brightness/gamma slider, and the existing reduce-motion
  setting also turns off flicker.

## Saving and multiplayer

Light is derived from blocks, so it is neither saved nor sent: every
machine recomputes it from the same columns. Moving and coloured lights
come from state the game already has.

## Tests

- Flood fill: an open column is 15 down to the ground; a sealed room is
  dark; a torch gives 14 next to it, falling by 1 per block.
- An edit relights to the same values as a full relight.
- Light across a column border matches on both sides.
- Light never leaks through opaque blocks.
- The renderer runs with `--validate` and zero messages at
  `MC_VK_API=1.2` and on lavapipe, and the frame time on an integrated
  GPU stays within budget.
