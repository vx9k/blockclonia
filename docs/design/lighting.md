# Lighting

Sky light that reaches into caves and under roofs, light from fire and
lamps, and a sun that moves and casts shade (planned 2026-11-16).

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

## Where it stands

The light engine is in (`light.c`, the light jobs in `world.c`, the atlas
in `renderer.c`); the visual polish (sun direction, HDR and tone mapping,
bloom, a held torch) is next.

- **Per cell:** 16 bits, sky in bits 0-3 and red, green and blue in
  4-7, 8-11 and 12-15. That is B4G4R4A4's texel, so the shader reads
  `.rgb` as block light and `.a` as sky with no unpacking. A section
  whose cells all match (open sky, solid rock) stores one value.
- **Relighting:** a light job per column floods a snapshot of the 3 x 3
  columns around it (sources up to 15 blocks out reach it) and keeps the
  middle 18 x 18 footprint. A full flood of the column, rather than
  removal and re-addition queues: it runs on a worker, never
  touches the main thread, and can't drift from a full relight. An edit
  marks dirty only the columns within 15 blocks, and only when it
  changes what lets light through or gives it off. Sections are meshed
  only once their column is lit, and the sections whose light changed
  (and their neighbours) are remeshed.
- **To the shader (option b):** each meshed section's light is an 18³
  box in one 3D atlas texture, filtered linearly; the vertex stays 4
  bytes. Opaque texels take the dimmest of their open neighbours, level
  by level, so the filter neither darkens the edge of a lit floor nor
  carries light through a one-block wall into a room's corners. Atlas
  sides are 7, 14, 28 ... slots (126 texels and its doublings), which
  Intel tiles with 4% padding instead of up to 120%.
- **Hot blocks** glow from `thermo`'s field (`light_glow`, red at 500 °C
  to white at 2000 °C), passed to the light jobs through a hook.
- **The curve:** 0.8 per level below 15, the sky scaled by daylight;
  block light ignores the time of day.

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
