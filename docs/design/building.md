# Building and furniture

Shapes beyond the cube, things that open and hold, and mortar that makes
walls strong (planned 2026-11-16).

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

## Where it stands

Every block is a full cube. A placed stone block cantilevers 7 blocks
against rock's 15 ("only as good as its mortar"), and structures that
lose support collapse. Nothing opens, stores items, or gives off light.

## Shapes

- Slabs, stairs, fences and walls, panes (thin glass), ladders,
  trapdoors and doors.
- The mesher gets a small shape table (a list of boxes per shape), and
  collision uses the same boxes.
- This touches the 4-byte vertex format and chunk meshing, so it is
  designed first and reviewed carefully (the mesher's rules are in
  `mesher.h`). Shapes cost nothing on the Vulkan 1.0 path.

## Things that open, hold and light

- Doors and trapdoors that swing (animated, saved and networked).
- Chests and shelves with their own inventories, saved in the world
  database (this raises the save `FORMAT`).
- Beds: sleeping passes the night (only when everyone in multiplayer
  agrees) and sets where you wake up.
- Tables, chairs and workbenches. The workbench is the station for
  hands-on crafting.
- Lamps, torches and lanterns, once blocks can give off light (see
  [lighting](lighting.md)). Fire already exists as a heat source in
  `thermo`; a lamp is both light and a little heat.

## Mortar and concrete

- Lime mortar (quicklime, sand and water) placed with stone or brick
  raises the span to that of rock, so walls and arches hold. It cures
  over time and is weak while wet.
- Later, cement (limestone and clay, burnt) and concrete, and iron
  reinforcement that carries tension.
- The support check (`physics.c`) reads the span of a block together
  with its mortar.

## Tests

- Every shape's collision boxes match its drawn boxes.
- A mortared wall spans what rock does, and an unmortared one collapses
  where it should.
- Chest contents round-trip through a save, and a corrupt row is
  refused.
