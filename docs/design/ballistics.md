# Weapons and ballistics

Ammunition types, simulated flight, penetration and wounds, and weapons
built from parts that wear and fail (planned 2026-11-17).

What is done and what is next lives in [TODO.md](../../TODO.md); this
page holds the reasoning and the detail.

## Why and when

The game has one rifle and one cartridge. The bullet is a point under
gravity at a fixed 400 m/s, with no drag. Its damage falls with the
energy it has left, and it stops at the first block, except glass and
ice, which shatter. The rest of the game is simulated in SI units:
structural collapse, heat, a body with organs and blood. Next to that,
the rifle is the shallowest system in the game. The aim is to give it
the same depth: what the round is, how far it drops, what it goes
through and what it does to a body all matter, and a weapon is
something you maintain.

It comes after the agreed roadmap (items and crafting, machines,
vehicles) unless that order changes. It needs the stations from items
and crafting, and lathes and presses from machines, for its parts.

## What it does not include

Everything here is game mechanics: items with stats, recipes made of
items, and physics from published performance figures. Real-world
instructions for making or modifying weapons or ammunition are out of
scope, and the game doesn't need them. That includes handloading data
(the powder charges in reloading manuals), propellant or explosive
synthesis, conversions to automatic fire, and manufacturing
procedures. Crafting stays abstract, as in other games: the right
station, the right items in, a cartridge out.

## Principles

- *Everything leaves the barrel.* A shot starts at the muzzle of the
  rifle as it is held and flies along the bore, never from the eye or
  the crosshair: the flash, the smoke and the tracer start there too.
  Down the sights the bore runs a few centimetres under the line of
  sight, so a close shot strikes that much low until zeroing (step 8)
  tilts the bore up to meet the sight line at a set range. From the hip
  the bore points where the rifle does, which is not where the
  crosshair is. The muzzle comes from the rifle's resting pose at the
  current aim (`viewmodel_muzzle`), the same on the server and the
  client; the view model's sway, lag and recoil are only drawn. A
  barrel pressed through a wall fires from the eye into the wall rather
  than past it.
- *A round is data.* Each ammunition type is a row in a table:
  calibre, bullet mass, muzzle velocity for a reference barrel length,
  ballistic coefficient and the drag model it is given against (G1 or
  G7), and construction (lead, full metal jacket, soft point, hollow
  point, armour-piercing, tracer, buckshot). Construction sets how it
  penetrates and how it wounds. All values come from published
  manufacturer data or typical published figures, in SI units.
- *Flight is integrated, not faked.* Each step of the fixed 60 Hz
  physics applies gravity and drag, `a = -(π/8) · ρ · Cd_ref(M) ·
  v·|v| / BC`, with the standard drag curve for the round's model and
  BC in kg/m² (703 times the published lb/in² figure). The air comes
  from `atmos.c`, which aircraft will share: density ρ from altitude
  and the air temperature `thermo` already has, so a shot on a cold
  mountain flies differently from one in a hot valley, and a seeded
  wind that drifts the bullet in the open but not underground. Time of flight
  is real, so a moving target has to be led. Spin drift and the
  Coriolis effect are left out: they are too small at the game's
  ranges to be worth the code.
- *Blocks have ballistic properties.* Each material class gets an
  areal resistance. A bullet loses energy through a block in
  proportion to its path length and the material, and comes out
  slower and possibly deflected. It can pass through a plank wall or
  glass and stops in stone or earth. Armour-piercing rounds keep more
  energy. A shallow hit on a hard face ricochets with a random spread
  from the seeded state, losing most of its speed. Bullets can break
  brittle blocks and chip others (block damage pairs with
  `collapse`).
- *Wounds go through the health model.* There are no hit points. A hit
  delivers the energy lost inside the body, along a path through the
  hit parts. Construction sets the wound: a jacketed round makes a
  narrow channel and may pass through; a soft point or hollow point
  expands and stops; buckshot makes several small wounds. The health
  model already has bleeding, fractures and organs. Ballistics decides
  which organ the path crosses, how deep it goes and how wide the
  channel is.
- *Armour is a part, not a number.* Later, clothing and armour plates
  give an areal resistance per body part, with a protection level
  that is tested against penetration the same way blocks are. Blunt
  trauma behind a stopped round still bruises or breaks a rib. Armour
  loses protection where it has been hit.
- *Weapons are their parts.* A weapon has a receiver, a barrel, a
  stock, sights, a magazine or loading gate, a trigger group, and
  attachments. Barrel length scales muzzle velocity from the round's
  reference. The parts set mass, handling (raise speed, sway) and
  recoil. Mounting a weapon to the cheek already runs on a spring
  (`viewmodel.c`). A heavier or worse-balanced weapon gets a slower,
  wobblier spring.
- *Recoil from momentum.* Recoil impulse comes from the bullet's
  momentum and the propellant gas, against the weapon's mass. It
  drives the view-model kick and the muzzle climb the player has to
  bring back.
- *Condition and failure.* Every shot wears the barrel and dirties the
  action. Firing raises barrel heat into `thermo`; a hot barrel loses
  accuracy and wears faster. Poor condition, dirt, heat or a bad
  cartridge make malfunctions likely: misfires, failures to feed or
  extract, double feeds. Each has its own clearing action, and the
  view model animates it. Cleaning and replacing parts restore
  condition. Failures come from the seeded random state, so a replay
  gives the same jams.
- *Handling is physical.*
  - **Magazines:** each round is tracked in its magazine, so mixed
    loads fire in order. Reloading with a round still chambered is
    faster than from empty.
  - **Load patterns:** a magazine, stripper clip or belt can be filled
    to a repeating pattern, such as one tracer then two ball rounds.
    - Patterns are edited in the weapon's panel, and can be saved as
      presets per weapon.
    - Loading takes rounds from the inventory in the pattern's order.
    - When a type runs out, the next type in the pattern fills its
      slot, and the HUD says so.
    - The last rounds can be set apart, for example tracers as the
      final three, to show that the magazine is nearly empty.
  - **Fire modes:** a weapon has only the modes its trigger group has.
  - **Zeroing:** each sight has adjustable elevation and a zero range.
    The game has no aim assist, so learning the drop is the skill.
- *Deterministic and networked.* Shots are simulated by the authority
  at the fixed physics step like any other body. Clients send fire and
  aim input, as `B_AIM` does now, and draw their own tracer at once.
  Spread and failures use the seeded random state, and tests replay
  shots exactly.

## Progression

1. **Black powder:** muzzle-loaders and the current rifle. These use
   round balls and conical bullets, at low velocity with steep drop,
   and give off smoke and fouling.
2. **Early cartridges:** single-shot and lever or bolt actions, with
   lead and jacketed bullets.
3. **Smokeless powder:** higher velocity and flatter flight. Needs the
   chemistry from machines.
4. **Magazine-fed and self-loading weapons,** made from machined parts
   on the lathe.
5. **Shotguns:** shot charges and slugs.
6. **Optics:** fixed-power scopes, as a second sight picture with its
   own reticle and zero.

## Order of work

Each step is one pull request:

1. **Ballistic flight.** An ammunition table, drag from G1/G7 curves,
   air density from altitude and temperature, and wind. Tests check
   the flight against closed-form results (a vacuum parabola,
   exponential slowing where drag is flat, the lag rule for wind
   drift) and a published remaining velocity.
2. **Muzzle effects:** a flash that lights the surroundings for a
   moment, black-powder smoke that drifts with the wind, and tracers,
   all leaving the muzzle with the bullet.
3. **Penetration and ricochet** through blocks by material, including
   bullets that pass through walls.
4. **Wound ballistics** into the health model: the path through body
   parts, wounds by construction, and pass-throughs.
5. **The weapon as parts:** barrel, magazine and receiver as an item
   with an inventory, then reloading and fire modes.
6. **Load patterns:** the pattern editor and presets, and loading
   magazines, clips and belts by pattern.
7. **Condition, heat and malfunctions,** with clearing and cleaning.
8. **Sights and zeroing,** then optics.
9. **Armour** on the body, after clothing exists.

## Testing

- Flight: drop, velocity and time of flight at 100, 200 and 300 m
  for each round, compared with the published table it came from.
- Penetration: a round goes through N planks and stops in stone. An
  armour-piercing round goes further than full metal jacket, which
  goes further than a soft point.
- Ricochet: below the critical angle a round always ricochets, and
  above it never. The spread comes from the seed, so a replay is
  identical.
- Wounds: a pass-through leaves an exit wound, and an expanding round
  stays in the body and delivers more energy.
- Load patterns: a 1-tracer, 2-ball pattern fills a 30-round magazine
  as 10 repeats, and fires in that order. When tracers run out, ball
  rounds take their slots. A saved preset survives a save and reload.
- Malfunctions: a replay with the same seed jams on the same shot.
  Clearing works for each kind of malfunction.
- Performance: COMBAT_SHOTS bullets in flight cost less than a set
  budget per physics step on the baseline machine.
