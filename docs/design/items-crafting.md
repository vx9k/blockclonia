# Items, objects and crafting

Every item as a solid model, objects that move, and crafting done by
hand at stations (planned 2026-11-16).

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

## Where it stands

Every item is a model of boxes at its real size (`itemmodel.c`), the
same one dropped (`fx.c`), held (`viewmodel.c`) and in the inventory
(`invui.c`). Blocks that do something, like the furnace, are plain cubes
with a static texture.
Crafting is a list on the inventory screen: pick a recipe and the inputs
turn into the output at once. The goal is a world of solid objects that
you make by hand. This comes after the underground materials, so the new
metals, minerals and ingots get their models from the start.

## 3D models for every item

Done (`itemmodel.c`):

- Each item is a few boxes at its real size in metres: a pick with a
  0.8 m handle and a curved head about 0.6 m across, a 10 cm bar ingot
  that tapers to its top, a coopered ten-litre bucket with a rope bail,
  a 7 cm cartridge, a 1.1 m bolt-action rifle, a corked bottle of
  spirits, an amber antiseptic bottle, a glass ampoule.
- Families share a builder: tools by kind with the head's material by
  tier (wood, flint lashed on with fibre, copper, iron), ingots by
  metal, ores as the host rock with the mineral breaking its surface
  (galena and salt as stepped cubes, quartz and sulfur as prisms),
  buckets by their liquid.
- A box has a greyscale material layer (`T_MAT_*`: wood grain, brushed
  metal, cast metal, rock, cloth, glass, plain, flesh, grain) and a tint,
  so one layer serves every metal. The layer is stretched over each
  face, since the boxes are a few centimetres. Glass and liquids blend.
- Held: a tool by the foot of its handle, a gun by the wrist of its
  stock, anything else in the palm, scaled so small things can be seen.
  Dropped: real size, small things enlarged to 25 cm, tools lying flat.
- The icon is the same boxes drawn flat-shaded from a fixed angle (tools
  diagonal), built once and cached, so the three always match.
- At most 16 boxes each, every model within 1.3 m.

## Animated objects

- **Furnace:** the door glows, flames and embers move behind it, smoke
  rises from the top while it burns, and it cools from orange to dark
  after the fire goes out. Its temperature comes from `thermo`, not
  from a timer.
- **Campfire:** settles as its fuel burns down.
- **Later:** a forge and bellows, a crucible, a quench trough, a
  workbench, a chest lid, and a door that swings.
- Animation runs on the frame's `dt` and looks the same at 30 and 144
  fps. Object state stays deterministic and is saved with the world.

## Hands-on crafting instead of a list

Making something happens in the world, at a station, in steps you do
yourself:

- **Workbench:** lay parts out on its top (a head and a handle) and
  work them with the right tool (a hammer, a knife).
- **Furnace:** load ore and fuel into the slots you can see on the
  model, then wait while it heats; ore below its smelting temperature
  doesn't smelt. Pour the melt into a mould, and take the ingot out
  once it has cooled.
- **Alloys:** bronze and brass are made by melting ingots together in a
  crucible, in the right ratio.
- **Forging:** heat a bar, hammer it on an anvil (the number of strikes
  and their rhythm count), and quench it.

Every step uses real quantities: temperatures in °C, masses in kg,
times in s. Recipes stay a data table, but they stop being a menu: the
list becomes a recipe book you read, not one you click. Simple hand
recipes (planks, sticks, a splint) stay instant.

## Multiplayer

Station contents and progress live in the world, so every player sees
the same furnace fill and the same ingot cooling. Adding this raises the
network protocol version.

## Tests

- Every model stays within its box budget and inside 1 m.
- A station turns the same inputs and steps into the same output, at
  any frame rate.
- A furnace below an ore's smelting temperature leaves the ore as it is.
