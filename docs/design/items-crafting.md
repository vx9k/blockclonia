# Items, objects and crafting

Every item as a solid model, objects that move, and crafting done by
hand at stations (planned 2026-11-16).

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

## Where it stands

Every item except the tools is a card: its 16x16 sprite on a thin slab,
whether dropped (`fx.c`) or held (`viewmodel.c`). Blocks that do
something, like the furnace, are plain cubes with a static texture.
Crafting is a list on the inventory screen: pick a recipe and the inputs
turn into the output at once. The goal is a world of solid objects that
you make by hand. This comes after the underground materials, so the new
metals, minerals and ingots get their models from the start.

## 3D models for every item

- Each item is a small model built from pixel-art boxes, the way the
  creatures are (`ENTITY_TEX_METRIC`, 1/16 m texels, generated in code;
  still no image files). For example: an ingot is a tapered bar, a raw
  ore a lumpy nugget, a bucket has a handle, a bandage is a roll, and
  there is a bottle and a rifle.
- The dropped item, the held item and the inventory icon all use the
  same model. The icon is rendered from it once at start-up, so the
  three always match.
- Models are kept small for weak GPUs: about a dozen boxes, instanced
  like the creatures, and each within 1 m along every axis.

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
