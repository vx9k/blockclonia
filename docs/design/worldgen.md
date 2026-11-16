# World generation

The generator overhaul and the settlements planned on top of it.

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

## The overhaul (planned 2026-11-16)

**Why.** A 2 km map of seed 1337 shows one landscape everywhere: even
grass with an even sprinkle of one kind of tree, mountains drawn as
thin white "worms" by ridged noise with a hard snow line at y = 92,
lakes that all wear the same ring of sand, no rivers, and hundreds of
cave mouths per square kilometre. The air is 14 C at sea level
everywhere, so there is no north, south, wet or dry. The goal is land
that reads as natural, from a climate down to single plants, while
still being worth exploring: every few hundred metres something
should look different for a reason you could explain.

**Principles.**

- *Cause before look.* Climate decides biomes, biomes decide soil and
  plants; nothing is coloured by a noise field that means nothing.
  Variety comes from that chain (a cold valley floor next to a warm
  south slope), not from random patches.
- *Real habits, game scale.* Shapes follow the real thing (a spruce is
  a narrow cone of whorls, a river widens downstream, scree gathers at
  the foot of a cliff), squeezed to the world's height the way
  `thermo.c` squeezes the lapse rate.
- *One climate for everything.* The terrain, the plants and the
  survival model (`thermo_air`) read the same field, so a tundra is
  cold to stand in, not only white.
- *Streaming rules stay.* Each column is generated on its own on a
  worker thread from the seed and integer-exact maths, with no shared
  state and no second pass; anything that crosses a column border
  (trees, rivers, houses) is re-derived from the seed on both sides.
  Budget: within 2x today's 1.15 ms per column (`--bench`), measured
  on the Raspberry Pi 4 too.
- *One generator.* There are no players' worlds to keep yet, so the
  game carries one generator and refuses worlds from any other. Each
  step that changes the terrain bumps `WORLDGEN_VERSION`.

**Steps, one pull request each:**

1. [x] **A 384-block world** (breaking, #63). `WORLD_H` 128 to 384
   (24 sections), with room above the terrain for towers. Column rows
   store their height (old rows load with air above; columns under the
   old ceiling are still written the old way), `NET_PROTOCOL` 6, the
   real lapse rate above 128 m. `SEA_LEVEL` stays 48 and versions 1
   and 2 generate unchanged; the new generator puts its land between
   the sea and about 250 m. Follow-up: store all-air sections sparsely,
   which wins back the +224 MB the sky costs at radius 32.
2. [x] **Climate** (breaking, with step 3). Done as
   `worldgen_sample(seed, x, z)` (height, temperature, rainfall,
   biome) and `worldgen_sea_temp`, which `thermo` reads once
   `thermo_set_climate` turns it on. Planned as
   `worldgen_climate(seed, x, z)`: a mean temperature offset and a
   yearly rainfall, from slow (several km) warped noise,
   with rain shadows behind high ground and cooler, wetter air near
   the sea. `thermo_air` adds the temperature offset to its lapse, so
   the snow line becomes the 0 C isotherm rather than a fixed y.
   Biomes come from temperature and rainfall (a Whittaker diagram):
   tundra, boreal (taiga), temperate mixed forest, meadow, steppe, dry
   scrub, wetland, alpine. Edges blend over tens of metres. Tests:
   the field is continuous, every biome occurs within a few km of
   most seeds, and `thermo_air` agrees with the biome it stands in.
3. [x] **Landforms** (breaking, with step 2). Within budget:
   1.85 ms per column against 1.22 ms before (`--bench`, Debug, x86).
   Planned as: domain-warped continents and coasts; mountain ranges
   with foothills, made from eroded-style noise (octaves damped by the
   slope so peaks are sharp and valleys smooth, not worms); plateaus
   and mesas in dry country; cliffs where layers outcrop; scree at
   their feet; beaches of sand, gravel or bare rock by slope and
   exposure. Caves: far fewer surface mouths, some large chambers,
   ore bands rescaled to the deeper ground.
4. **Underground** (agreed 2026-11-16), as a series of pull requests:
   1. 32-bit item ids, so there is room for many more items (blocks
      stay one byte in the world).
   2. The block table holds each block's material class, so a new
      block is one table row instead of a case in five switches (#68).
   3. Materials: rocks (granite, basalt, limestone, sandstone, shale,
      marble, obsidian), soils (clay, mud, peat, podzol), ores (tin,
      zinc, lead, nickel, silver, gold, bauxite, titanium, uranium,
      lithium, sulfur, saltpeter, rock salt, quartz, graphite), their
      raw items, ingots and alloys (bronze, brass, steel), charcoal and
      quicklime, with furnace recipes. Each gets real density,
      hardness and thermal values for physics and `thermo`. Rock in
      place is one mass (span 15) and breaks into a loose block.
   4. Strata and ores in their host rock: layered rock under the soil,
      outcropping in cliffs; each ore where it really forms (tin in
      granite, galena in limestone, gold in quartz veins); soil depth
      from slope, climate and rock rather than a fixed number per
      biome.
   5. Liquids: lava, crude oil and brine, with the water simulation
      generalised to more than one fluid (density, viscosity,
      temperature).
   6. Aquifers and cave variety: water tables, flooded caves, ice caves
      in cold ground, dripstone in limestone, lava lakes deep down.
   7. Then the old per-file world importer goes.
5. **Rivers and lakes.** A river network from the highlands to the
   sea: valleys carved along warped noise zero lines, whose beds fall
   monotonically downstream so the water is level and stable the
   moment it is generated (fluid physics would otherwise drain it);
   narrow streams in steep cuts up high, wide meanders and estuaries
   near the coast, lakes in closed basins with an outflow. Riverbeds
   of gravel and clay, reeds along slow banks. Tests: no generated
   water flows on the first tick, rivers reach the sea, a river
   crossing a column border matches on both sides.
6. **Trees, redone.** Species by climate, each with its real growth
   habit and its own bark and leaf textures: oak (broad, irregular
   crown, furrowed grey-brown bark), birch (slender, white bark with
   dark lenticels, light canopy), spruce (narrow cone of drooping
   whorls, dark needles), Scots pine (bare trunk, flat crown high up,
   orange plated bark above), and a dry-country tree (juniper or a
   twisted pine). Far more variance: age classes from sapling to old
   giant, crown shape and lean from seeded parameters, dead snags and
   fallen trunks in old forest, forests as stands with clearings and
   edges rather than a uniform sprinkle. Leaves still obey the
   3-block support span. Textures reviewed against reference photos.
7. **Ground plants.** Cross-shaped plant quads in the mesher (two
   crossed planes, no collision, broken by hand), then tall grass,
   ferns in forest, flowers by biome and season, reeds in wetlands,
   bushes, heather on moors, dry shrubs in scrub. Density from
   rainfall and light under the canopy.
8. **Settlements**, as designed below, on top of the new terrain.

**Settlements: villages, houses and towns.** Settlements in the generated
world: houses, for now entirely empty (walls, floor, roof, a doorway
and windows; no furniture, loot or villagers), grouped into villages
and, in larger flat areas, towns with streets.

**Shape.**

- *Placement:* a coarse grid of cells (a few hundred metres across),
  each with one seeded chance of a settlement at a jittered point.
  Settle only on fairly flat, dry land above the sea and below the
  snow line: sample `worldgen_height` around the point and reject a
  slope or water. Towns need a larger flat area than villages.
- *Layout:* from the settlement's seed, lay out a street plan (a main
  road, side streets) and house plots along it, then pick a house
  plan per plot (sizes, one or two storeys, roof shape). Level each
  plot to the street's height and fill the ground under it, so houses
  don't float or sink on slopes.
- *Materials* from blocks that already exist (`B_PLANKS`, `B_LOG`,
  `B_BRICK`, `B_GLASS`, stone), chosen per settlement and by biome.
  Doors need a new block (or are left as open doorways at first).
- *Streaming:* columns are generated one at a time on worker threads
  (`jobs.c`), so a house that crosses a column border has to come out
  the same from both sides. Each column asks which settlements could
  reach it (its cell and the neighbouring ones), re-derives their
  layouts from the seed, and writes only the blocks inside itself. No
  shared state between jobs, and no second pass.
- *Determinism:* layouts come from the seed and integer maths only, so
  every machine builds the same town (multiplayer sends terrain from
  the host anyway, but saves and tests rely on it).
- *Compatibility:* settlements change the terrain, so they bump
  `WORLDGEN_VERSION`; worlds made before keep generating without them.
- *Tests:* the same seed gives the same settlement; a house split
  across a column border matches on both sides; no settlement on
  water or on steep ground; generation time per column stays within
  the streaming budget (`--bench`).

**Later:** furniture and loot, villagers, roads between settlements,
ruins, and settlements on the map or in a `/locate` command.
