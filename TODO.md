# TODO

What is being built, what comes next, and what is planned for later.
Each area below has a goal, a status and a checklist. The reasoning and
detail are in [docs/design/](docs/design/README.md), one page per area.

Every item is one branch and one pull request (see AGENTS.md, Git),
unless it says otherwise.

## Now

1. **The underground series** (see [World generation](#world-generation)):
   - [x] Each block's material class in the block table (#68)
   - [x] 32-bit item ids (#70)
   - [x] Materials: 7 rocks, 4 soils, 15 new ores, raw items, ingots
         and alloys, smelting
   - [x] Strata, ores in their host rock, soil depth
   - [ ] Liquids: lava, crude oil, brine
   - [ ] Aquifers and cave variety: flooded caves, ice caves,
         dripstone, lava lakes
   - [ ] Delete the old per-file world importer
2. **Rivers and lakes**, then a new terrain screenshot for the README.
3. **Platforms:** Ubuntu 26.04 (x86_64 and arm64), Alpine Linux and macOS
   only; the Raspberry Pi, armhf and older distributions are dropped.
   - [ ] CI and packages for the supported platforms
   - [x] Vulkan 1.2 as the minimum, without the 1.0 fallbacks

## Next

In this order (agreed 2026-11-16):

1. **[Lighting](#lighting)**: sky light that reaches into caves, light
   from fire, lamps and hot blocks. The underground and the furnace
   both need it.
2. **[Items and crafting](#items-and-crafting)**: 3D models for every
   item, animated objects, hands-on crafting at stations.
3. **[Machines and power](#machines-and-power)**: water, wind and steam
   power, shafts and belts, machines, oil refining.
4. **[Vehicles](#vehicles)**: built from parts and fully simulated,
   from a hand cart to engines on refined fuel.

## Later

Not yet in order:

- [Animals](#animals): more species, cats among them, and behaviour
  from needs and senses
- [Farming and food](#farming-and-food)
- [Weapons and ballistics](#weapons-and-ballistics): ammunition types,
  simulated flight, penetration, wounds, and weapons built from parts
  that wear and jam
- [Building and furniture](#building-and-furniture)
- [World generation](#world-generation): trees, ground plants,
  settlements
- [User interface](#user-interface): the "field kit" redesign
- [Multiplayer](#multiplayer): the headless server and what is left
- [Health](#health): the panel by mouse, the anatomical view
- [Android](#android)
- [Native Metal renderer](#native-metal-renderer-macos): deferred until
  someone with a Mac measures a problem with MoltenVK

## Areas

### World generation

**Goal:** land that looks natural, from a climate down to single
plants, and is worth exploring.
**Status:** generator 3 (a climate, biomes, landforms, caves and ore);
the underground series is in progress.
**Design:** [docs/design/worldgen.md](docs/design/worldgen.md)

- [x] A 384-block world (#63)
- [x] Climate and landforms (#67)
- [ ] Underground: materials, strata, liquids, aquifers, cave variety
      (see Now)
- [ ] Rivers and lakes
- [ ] Trees redone: species by climate, age classes, real growth habits
- [ ] Ground plants: grass, ferns, flowers, reeds, shrubs
- [ ] Settlements: villages, houses and towns

### Lighting

**Goal:** dark caves and nights, light from fire, lamps and hot blocks,
and a sun that moves.
**Status:** the light engine is in: coloured block light and sky light
by flood fill, a 3D light texture per section, torches, lamps, foxfire
and hot-block glow. Next: the visual polish.
**Design:** [docs/design/lighting.md](docs/design/lighting.md)

- [x] Choose how light reaches the shader: a light texture per section
- [x] Sky light and block light by flood fill on the workers, with
      relighting on edits
- [x] Emitters: campfire, lava, torches, lamps, foxfire; glow from
      `thermo` temperature
- [ ] HDR with tone mapping and a brightness setting, bloom, fog tinted
      by the light, a smooth-lighting toggle
- [ ] A held torch or lantern
- [ ] Sun and moon direction and colour by time of day; a brightness
      setting
- [ ] Optional: sun shadows (`GPUCAP_*` bit with a fallback on bare 1.2)

### Items and crafting

**Goal:** every item a solid model; objects that move; crafting by hand
at stations instead of from a list.
**Status:** in progress. Every item is a box model at its real size;
crafting is still a list.
**Design:** [docs/design/items-crafting.md](docs/design/items-crafting.md)

- [x] Item models (boxes at real size) shared by the world, the hand and
      the icon
- [ ] An animated furnace and campfire, driven by `thermo`
- [ ] Stations: workbench, furnace with moulds, crucible for alloys,
      anvil and quench for forging
- [ ] The recipe list becomes a recipe book

### Machines and power

**Goal:** production from simulated machines built from parts, with
real energy, wear and maintenance.
**Status:** planned.
**Design:** [docs/design/machines.md](docs/design/machines.md)

- [ ] Shafts, gears and belts as one rigid group; a hand crank
- [ ] Water wheel and windmill
- [ ] Steam: boiler, engine, flywheel
- [ ] Machines: millstone, sawmill, crusher, bellows, blast furnace,
      rolling mill, lathe
- [ ] Pumps, pipes, tanks and oil distillation
- [ ] Conveyors
- [ ] Later: electricity

### Vehicles

**Goal:** vehicles assembled from parts and fully simulated: gearing,
engine curves, wear, breakdowns and maintenance.
**Status:** planned; needs machines for its parts and fuel.
**Design:** [docs/design/vehicles.md](docs/design/vehicles.md)

- [ ] Vehicle bodies, wheels with springs and tyre slip, saving
- [ ] Hand cart and wheelbarrow
- [ ] Animal-drawn wagon (needs horses or oxen)
- [ ] Rail: track, mine carts, a steam locomotive
- [ ] Steam traction engine
- [ ] Internal combustion: car, truck, tractor
- [ ] Boats

### Animals

**Goal:** many more animals, cats among them, that act from needs and
senses.
**Status:** wolves and boars, driven by state machines and not saved.
**Design:** [docs/design/animals.md](docs/design/animals.md)

- [ ] Behaviour from needs, senses (sight, hearing, smell on the wind)
      and utility choices; pathfinding with a per-tick budget
- [ ] Animals saved with the world
- [ ] Cats: strays you befriend, house cats, wild cats and lynx
- [ ] Wild animals: deer, rabbits, foxes, bears, birds, fish
- [ ] Kept animals: chickens, sheep, goats, cows, pigs, horses, oxen
- [ ] A light body model per species in place of hit points

### Farming and food

**Goal:** crops by climate, kept animals, cooking and preserving that
feed the body model.
**Status:** planned. Food is apples and boar meat.
**Design:** [docs/design/farming.md](docs/design/farming.md)

- [ ] Tilling, sowing and harvesting; growth from temperature, water,
      soil and light
- [ ] Kept animals: feeding, breeding, fences
- [ ] Oven, pot, flour and bread; spoilage and preserving
- [ ] Nutrition beyond energy and water

### Weapons and ballistics

**Goal:** weapons as deep as the rest of the simulation: ammunition
types, real flight, penetration and wounds through the health model,
and weapons built from parts that wear, heat up and jam.
**Status:** one rifle with two-handed aim and a spring mount. Its
bullet flies with G1/G7 drag, air density and wind, and stops at the
first block.
**Design:** [docs/design/ballistics.md](docs/design/ballistics.md)

- [x] Ballistic flight: an ammunition table, G1/G7 drag, air density
      from altitude and temperature, wind
- [x] Muzzle effects: a flash that lights the area, drifting
      black-powder smoke, tracers, all from the muzzle along the bore
- [ ] Penetration and ricochet through blocks by material
- [ ] Wound ballistics into the health model: the path, construction,
      pass-throughs
- [ ] The weapon as parts: barrel, magazine, receiver; reloading and
      fire modes
- [ ] Load patterns: fill magazines, clips and belts to a repeating
      pattern (such as one tracer then two ball), with presets per weapon
- [ ] Condition, barrel heat and malfunctions, with clearing and
      cleaning
- [ ] Sights and zeroing, then optics
- [ ] Armour on the body (after clothing)

### Building and furniture

**Goal:** shapes beyond the cube, things that open and store, and
mortar that makes walls strong.
**Status:** planned. Every block is a cube.
**Design:** [docs/design/building.md](docs/design/building.md)

- [ ] A shape table in the mesher: slabs, stairs, fences, panes, doors
- [ ] Doors, trapdoors, chests, beds, tables, chairs
- [x] Lamps and torches (after lighting)
- [ ] Lime mortar, then cement and concrete

### User interface

**Goal:** the "field kit" look on every screen, meeting WCAG 2.2 AA.
**Status:** designed (2026-11-16), not started.
**Design:** [docs/design/ui.md](docs/design/ui.md)

- [ ] Font, plates, tape, keycaps, sliders, switches, the palette test,
      UI scale; title, pause and settings
- [ ] The HUD: belt, injury alerts, reduce motion
- [x] Item icons, drawn from the item models
- [ ] The character panel on `Tab`
- [ ] The body side with treatment by item and by dragging
- [ ] Bone, Organs and Blood layers

### Multiplayer

**Goal:** play together without a third party, end-to-end encrypted.
**Status:** peer-to-peer and client-server modes, menus and NAT
traversal are merged.
**Design:** [docs/design/multiplayer.md](docs/design/multiplayer.md)

- [ ] Headless server (`--server`), started on `feat/headless-server`
- [ ] Hand the authority over when the host leaves
- [ ] Save joined players' health
- [ ] NAT-PMP/PCP, a rendezvous server, a relay for symmetric NATs

### Health

**Goal:** a body that feels real to hurt and to treat.
**Status:** the lumped physiological model, drugs, the inner voice and
the clickable panel are merged.

- [ ] The panel by mouse all the way: organs, supplies, dragging items
      onto a body part (part of the character panel)
- [ ] An anatomical view with organ overlays coloured by their state

### Android

**Goal:** a signed APK running the same game with touch controls.
**Status:** not started.
**Design:** [docs/design/android.md](docs/design/android.md)

- [ ] Toolchain file and `android` preset; core and `mc_tests` under
      qemu
- [ ] A platform layer behind `main.c` and `renderer.c`
- [ ] Touch controls
- [ ] Paths in the app's private directory
- [ ] Save on pause, rebuild the surface on resume
- [ ] A signed APK with AAudio sound, built in CI
- [ ] Runs on one Vulkan 1.2 phone and one 1.3 or newer device

### Native Metal renderer (macOS)

**Status:** deferred; macOS runs the Vulkan renderer through MoltenVK.
**Design:** [docs/design/metal.md](docs/design/metal.md) says when it
would be worth doing.
