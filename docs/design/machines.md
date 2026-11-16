# Machines and power

Factories built from simulated parts: power sources, transmission,
machines and fluids (planned 2026-11-16).

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

## Why

The underground series fills the world with ores, fuels and fluids, and
hands-on crafting makes each item by hand. Machines turn that into
production: more output for the same work, and the parts (shafts, gears,
pistons, sheet and wire) that vehicles are built from. It was agreed to
build machines before vehicles, and to make both **fully simulated**:
assembled from parts, with real physics, wear, breakdowns and
maintenance.

## Principles

- *Real quantities.* Power is torque (N·m) at a speed (rad/s), so watts.
  Heat is in joules and °C, fluids in kg and Pa. A machine's output
  follows from its input and its losses, never from a fixed rate per
  tick.
- *Energy is conserved.* Every link (a gear, a belt, a bearing) loses a
  share of its power to friction as heat, which goes into `thermo`.
  Nothing puts out more energy than it takes in.
- *Built from parts.* A machine is a frame plus parts that each have a
  material, a mass and a condition: a crusher is a frame, a motor
  coupling, two jaws and bearings. Parts wear with use, faster when
  overloaded, hot or unlubricated, and fail one at a time. A failed
  part can be repaired or replaced with tools.
- *Deterministic.* Machines step on the 60 Hz physics tick on the main
  thread, with seeded randomness only (for example, when a worn part
  breaks), so tests and multiplayer replay exactly.
- *Weak hardware still runs.* A network of shafts is solved as one
  rigid group (one speed, summed inertia and loads), not part by part.
  Machines far from every player sleep, and catch up from elapsed time
  when they are loaded again.

## Power sources

1. **Hand crank and treadwheel:** a person's ~75 W sustained. This
   costs energy in the body model.
2. **Water wheel:** power from the flow and head of the fluid
   simulation's water (`P = ρ g Q h η`); it needs a river or a dam.
3. **Windmill:** wind from the climate field, power ∝ v³, with a
   cut-out speed above which it has to be furled or it breaks.
4. **Steam engine:** a boiler (fuel heat → steam pressure, with a
   safety valve; an over-pressure burst is a real hazard), a cylinder
   and a flywheel. Efficiency is a few per cent, as in early engines.
5. **Later, electricity:** a generator, wires with resistance (I²R
   heating), motors, batteries, and lamps once blocks can give off
   light.

## Transmission

Shafts, couplings, gears (with ratios and tooth strength), belts and
pulleys (which slip when overloaded), clutches and brakes. A shaft
network is one rigid group, and gears multiply speed and torque.

## Machines

- Millstone (grain to flour) and sawmill (a log gives more planks than
  by hand).
- Jaw crusher and stamp mill (crushed ore smelts with a better yield).
- Bellows, which make a forge hotter.
- Blast furnace (iron in bulk, with limestone as flux; slag as a
  by-product), and later Bessemer steel.
- Rolling mill (ingots to sheet and rod) and wire drawing.
- Lathe and drill press: shafts, gears, pistons and bolts, the parts
  vehicles need.
- Pumps and pipes, with pressure and flow; tanks.
- Distillation: crude oil separated by boiling range into fuel gas,
  petrol, kerosene, diesel and heavy oil; the heat comes from a boiler.
- Conveyors and chutes to move items between machines.

## Saving and multiplayer

Machines and their parts' state live in the world database, in a new
table (this raises the save `FORMAT` and keeps the strict schema check).
The authority runs them, and clients see snapshots. Adding this raises
the network protocol version.

## Tests

- Energy balance: the output plus the heat lost equals the input, within
  rounding, for every machine.
- Gear and belt ratios give the expected speed and torque.
- The same inputs give the same outputs and the same wear when replayed,
  at any frame rate.
- A boiler above its rated pressure without a working valve bursts; with
  a working valve it holds.
- Sleeping machines catch up to the same state as machines that ran.
