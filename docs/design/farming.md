# Farming and food

Growing crops, keeping animals, and cooking and preserving food (planned
2026-11-16).

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

## Where it stands

Food today is apples from leaves and meat from boars, cooked on a
campfire (`item_food`: kcal and water). The body model already tracks
energy and hydration, and the climate gives every place a temperature
and a yearly rainfall. Farming builds on both.

## Crops

- Crops by climate: wheat and barley (temperate, steppe), potatoes and
  cabbage (cool and wet), maize (warm), flax (for fibre and cloth),
  berry bushes in forest, and fruit trees beside the apple.
- Growth depends on temperature (degree-days above a base temperature),
  rain or irrigation, soil (clay holds water, sand drains, podzol is
  acid and poor) and light. Frost kills tender crops, and drought
  wilts them.
- Tilling with a hoe (a new tool kind), sowing seeds, and harvesting
  with a sickle or scythe. Fields need water within reach or
  irrigation channels from the fluid simulation.
- Growth catches up from elapsed time when a column is loaded again,
  like machines, so farms grow while you are away.

## Animals kept

Chickens (eggs), sheep (wool, then cloth), goats and cows (milk), pigs,
and horses and oxen (pulling: see vehicles). They need food, water and
shelter, breed, and are kept in by fences and gates. Their behaviour
comes from the animal AI (see animals).

## Cooking and preserving

- Stations: a campfire, then an oven (bread from flour; flour from a
  millstone, see machines) and a pot (stews, boiling water).
- Food spoils over time, faster when warm. Salting (from rock salt),
  smoking, drying and cool storage slow it down.
- Boiling makes water safe. Untreated water from stagnant pools can
  carry gut infection, through the existing infection model.

## Nutrition

The body model gains protein and a few micronutrients (vitamin C and
iron to begin with) on top of energy and water. A long, poor diet has
slow, real effects (scurvy, anaemia), shown through the inner voice and
the panel. These stay mild and slow, so a varied diet is rewarded
without being a chore.

## Tests

- A crop planted at a fixed temperature and water supply ripens after
  the expected degree-days.
- Frost and drought kill the crops they should and spare the others.
- Spoilage follows temperature, and preserving extends it by the
  expected factor.
- Boiled water never infects.
- Growth that catches up from elapsed time equals growth stepped tick
  by tick.
