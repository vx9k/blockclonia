# Vehicles

Vehicles assembled from parts and fully simulated, from a hand cart to
an engine on refined fuel (planned 2026-11-16).

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

## Why and when

The world is large (generated without limit, 384 m high), and the
underground series and machines make heavy things worth moving: ore,
timber, fuel. Vehicles come after machines, which make their parts
(shafts, gears, pistons, sheet, rod) and their fuel (from the
distillation of crude oil). It was agreed to make them **fully
simulated**: assembled from parts, with gearing, engine curves, wear,
breakdowns and maintenance.

## Principles

- *A vehicle is its parts.* Parts include a frame, wheels and tyres,
  axles, springs and dampers, steering, brakes, an engine, a clutch, a
  gearbox, a differential, a fuel tank, a radiator, and seats and a load
  bed. Each part has a mass, a material and a condition. You build a
  vehicle by placing parts on a frame at a workbench or in a yard, and
  what it can do follows from what it has: a heavy frame on a weak
  engine climbs slowly.
- *Real physics.* The body is a rigid body at the fixed 60 Hz physics
  step, with SI units throughout. Each wheel is a ray or a short sweep
  against the voxel ground, with a spring and damper, a contact patch,
  and a tyre force from slip, scaled by the friction of the block under
  it (`block_def.friction`: ice is slippery, mud bogs down). Rolling
  resistance and air drag (`½ρC_dAv²`) apply, as does gravity on slopes.
- *Engines have curves.* Torque against rpm, idle and redline, fuel use
  from the energy content of the fuel and the engine's efficiency, and
  waste heat into the radiator and `thermo`. A clutch slips and wears.
  Gear ratios and the final drive set the speed and the pulling power.
- *Wear and failure.* Tyres and brake pads wear down. Brakes fade when
  hot. Engine oil degrades, and an engine run hot or without oil damages
  its bearings and can seize. Parts fail one at a time, with symptoms
  first (noise, smoke, pulling to one side), and are repaired or
  replaced with tools.
- *Deterministic and networked.* The authority simulates vehicles like
  any other body, and clients predict the vehicle they drive and
  reconcile. Collisions with terrain, bodies and players go through the
  existing physics, and crashes hurt the people inside through the
  health model, as falls do.

## Progression

1. **Hand cart and wheelbarrow:** pushed, with wheels and axles of wood,
   then of iron.
2. **Animal-drawn wagon:** needs horses or oxen (see animals); the
   animal's strength and stamina set the pull.
3. **Rail:** track, then mine carts (which pair with the underground),
   hand cars, and a steam locomotive (boiler pressure, water and coal
   consumption, a tender).
4. **Steam traction engine:** the boiler from machines on wheels.
5. **Internal combustion:** petrol and diesel engines from machined
   parts and refined fuel; then a car, a truck and a tractor (for
   farming).
6. **Boats:** buoyancy from displaced volume and the hull's density,
   with oars, sail (wind from the climate) and engine.
7. **Later, perhaps:** aircraft, once everything above is solid.

## Rendering

A vehicle is drawn from its parts' box models (the same kind as item
models), instanced, with wheels that turn and steer. The camera can
follow from behind or sit in a seat.

## Saving

Vehicles and their parts' state go in a new table in the world database
(this raises the save `FORMAT`; the schema check stays exact), decoded
through checked decoders like every row.

## Tests

- A cart on a 10 % slope accelerates at `g(sin θ − C_rr cos θ)` within
  tolerance.
- The braking distance from a speed matches `v² / (2 μ g)` on dry stone,
  and is longer on ice.
- The top speed in each gear follows from the engine curve and the
  ratios.
- Fuel used over a run equals the work done divided by the efficiency.
- An engine without coolant overheats and seizes; with coolant it holds.
- Replays are exact, at any frame rate and over the network.
