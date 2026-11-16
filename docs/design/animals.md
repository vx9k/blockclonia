# Animals and their behaviour

More animals, cats among them, with behaviour driven by needs and senses
instead of a fixed state machine (planned 2026-11-16).

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

## Where it stands

There are two creatures. Boars graze by day and run when hurt; wolves
hunt players at night or when provoked. Each creature is a player body
(`physics_move_player`) driven by a small state machine, with hit points
in place of a body. Creatures are not saved: they spawn around players
and despawn far away.

## New animals

- **Cats:** house cats in settlements, which can be befriended (see
  below), and wild cats and lynx in forest and boreal country.
- **Wild:** deer, rabbits, foxes, bears, birds and fish, placed by
  biome and climate.
- **Kept:** chickens, sheep, goats, cows, pigs, horses and oxen (see
  farming and vehicles).
- Each animal is a pixel-art box model like the wolf and boar, with its
  own sounds synthesized at start-up.

## Behaviour

- **Needs:** hunger, thirst, tiredness, fear, and comfort (warmth and
  shelter), rising and falling over time.
- **Senses:**
  - Sight is a cone, blocked by blocks, and worse at night.
  - Hearing reacts to the game's sound events (footsteps, digging,
    shots), fading with distance.
  - Smell is carried by the wind from the climate, so approaching from
    downwind matters.
- **Choosing what to do:** each tick, the animal scores the actions open
  to it from its needs and what it senses (utility AI): graze, drink,
  rest, flee, hunt, follow the herd, groom, play, sleep. It does the
  best one, with some resistance to switching so it doesn't dither.
- **Moving:** paths over the voxel grid (A* with step, jump and drop
  limits and swimming), with a budget per tick and the result reused,
  so a hundred animals stay cheap. Herds and packs keep together;
  predators stalk and flank.
- **Days and seasons:** sleep at night or by day by species; shelter in
  rain; breeding by season.

## Cats

- A stray keeps its distance at first. Food left out and patience build
  trust over days.
- A befriended cat follows you home, sleeps by a warm fire, wanders,
  and brings back what it catches (rats, birds).
- It purrs when petted, and it is choosy: some days it ignores you.
- It has a coat pattern and colour from a seeded genome, which kittens
  inherit from their parents.

## A lighter body

Hit points become a small body model per species: blood, injuries per
body region, exhaustion and hunger. It is far lighter than the human
one, but the same idea, so a wounded deer slows down and a well-fed cat
heals.

## Saving and multiplayer

Animals are saved with the world (a new table, raising the save
`FORMAT`), so befriended cats and herds stay where they are. The
authority runs their behaviour, and clients see snapshots, as today.

## Tests

- Utility choices: a hungry, safe grazer grazes, and a frightened one
  flees.
- Sight is blocked by walls, and smell follows the wind.
- Paths respect the step, jump and drop limits and the per-tick budget.
- Trust builds from feeding and fades with neglect.
- Kittens inherit coat genes.
- Animals round-trip through a save, and a corrupt row is refused.
- Behaviour is deterministic from the seed.
