# Health

How the body should work: a real one, organ by organ, whose failures
chain the way they do in life, balanced so the game stays playable
(planned 2026-11-23, issue #122, which holds the full list in §1–§16).

What is done and what is next lives in [TODO.md](../../TODO.md); this
page holds the reasoning and the detail.

## Where it stands

`src/health/health.c` is a lumped model: no hit points, injuries change
tissues and organs, and the vitals come out of the model. Stage 1 is in
(#124): the circulation is a pulsing four-chamber heart and vessel
circuit (`circulation.c`), fired by conduction timing (`conduction.c`)
and held by a delayed baroreflex (`autonomic.c`), so blood pressure
builds over tens of seconds of running instead of jumping, and the
monitor traces are the circuit's own pressures. Each body draws its own
constitution, including its fastest heart rate (185–215 bpm).

Known rough edges from the first playtest, left for play feedback
rather than more tests: creative mode still runs the health model, and
the monitor traces can show a step. A drained vessel's pressure is
linear below its unstressed volume; the readings floor at zero, but
the circuit itself still lets a vessel suck.

## Clocks

Two clocks run the body and the world. **Real time** is the player's:
anything they feel from moment to moment, or act against within
seconds, runs at the speed it does in life. The **survival clock**
(`HEALTH_CLOCK`, 24× real time, so a day is an hour of play) runs
what takes hours or days in life, so that it happens within a session
(the maintainer's choice, 2026-11-24). A step's `dt` is real seconds;
`gh` is the survival-clock hours in it.

| Process | Clock | Why |
|---|---|---|
| Heartbeat, circulation, baroreflex | real | felt beat by beat |
| Breathing, O₂ and CO₂, drowning | real | held breath is seconds |
| Bleeding flow and clotting | real | a wound is pressed within seconds |
| Plasma refill | survival, capped at 0.25 mL/kg/min real | hours in life; the cap keeps the capillaries' real limit so an open artery still empties the body |
| Fainting, consciousness, the brain's oxygen | real | |
| Getting used to the sight of blood | real, 25 s | a reflex of the moment |
| Fear, adrenaline, pain | real | |
| Stamina | real | |
| Heat exchange of skin and core, evaporative cooling | real | the chill is felt at once |
| Wet clothes drying | survival | an hour in life by a fire; drying in real time would leave the player wet for a whole session |
| Fever rise | survival | hours in life |
| Digestion, thirst, hunger, insensible water loss | survival | |
| Red-cell regrowth | survival | weeks in life |
| Wound healing, infection, sepsis | survival | days in life |
| Fractures, burns deepening and leaking, crush muscle death | survival | |
| Organ recovery | survival | |
| Morphine, alcohol, painkillers, antibiotics | survival | absorbed and cleared over hours |
| Day and sun | survival | a day is an hour |
| Fire fuel, the heat field, melting, freezing, catching fire | survival | `thermo.c` steps the world's heat in survival-clock seconds |

Before #125 the clock ran at 72× (a 20-minute day), and the
habituation to blood ran on it by mistake: 0.5 survival hours, which
was 25 real seconds only at that speed.

## Principles (the maintainer's, 2026-11-23)

- **As little synthetic behaviour as possible.** Prefer a mechanism that
  produces an effect over a rule that states it. Pressures, pulses and
  waveforms should come from simulated chambers, valves and vessels.
- **Unpredictable the way bodies are**:
  - every body differs (constitution drawn from the seed within real
    population ranges);
  - natural variation emerges from the model (beat-to-beat changes,
    breathing-linked heart-rate swings, slow pressure waves) instead of
    being added as noise on outputs;
  - outcomes are chancy at **real-world odds**: practically never in a
    healthy body, climbing steeply and realistically with real risk
    (a damaged heart, potassium, cold, drugs).
- **Real time constants.** Fast responses use published kinetics.
  Difficulty scales severity and speed, never which mechanisms exist.
- **No vitals on the HUD.** The player learns their state from the body
  (heartbeat, breathing, vision, voice) and reads numbers only from
  devices: a pulse check, a cuff, an oximeter, the defibrillator's
  monitor. The F3 debug panel keeps every number.
- **The inner voice speaks only for serious or new sensations, and only
  as feelings.** "My heart's fluttering", "everything's going grey";
  never "my blood pressure is low" or "I'm in shock". Hidden states stay
  silent until they cause a feeling. Mild states show only as body cues.
- **The player only**, for now. Mobs keep their simple health until the
  animals work; the model is written so a reduced copy can run them.
- Deterministic as ever: fixed 60 Hz step, seeded randomness, exact
  replay.

## Order

As in #122 §12, one stage at a time:

1. **Cardiovascular core** (below, in full).
2. Gas exchange and acid–base: PaO2, PaCO2, lactate, pH.
3. Consciousness levels and the screen effects that replace the black
   screen.
4. The rhythm engine with every arrhythmia, then the new defibrillator.
5. Kidneys, liver and gut, with glycogen, food and hydration.
6. Drugs on the new physiology.
7. Illnesses.
8. The automatic life-support unit.
9. A balance pass with play-testing.

Then the voxel body (§14) and the §15/§16 additions. §13 waits for full
multiplayer. Postures (sitting, crawling, lying down, a realistic
crouch) are their own series, issue #123; until it lands the
circulation takes posture from crouching.

## Stage 1: the cardiovascular core

The maintainer chose (2026-11-23) a pulsatile model with four chambers
over a ventricles-only one (atrial rhythms and blocks would then need
hand-made effects again) and over embedding the Pulse engine (C++, a
large dependency, not built for exact replay). It lands as **one pull
request**.

### The circuit

A lumped (0-D) circuit of elastic compartments, in the line of Guyton,
Heldt et al. (2002) and Ursino (1998). Each compartment holds a blood
volume V; its pressure is `(V - V0) / C` plus the pressure outside it
(chest or abdomen). Flow between neighbours is pressure difference over
resistance, with a hydrostatic term `rho g dh` from each segment's height
above the heart.

- **Heart**: right and left atria and ventricles, inside a pericardium
  whose pressure rises steeply with total heart volume (so the two sides
  interact, and tamponade later has a place to happen).
- **Pulmonary**: arteries, and veins with capillaries.
- **Systemic arteries**: the thoracic aorta, the abdominal aorta.
- **Beds**, each an arterial resistance into a venous compartment:
  brain, upper body (arms, skin), splanchnic (gut, liver), kidneys, legs
  (the exercising muscle). Plus a coronary path from the aortic root to
  the right atrium, so coronary flow (mostly in diastole) is computed,
  not estimated.
- **Veins**: the abdominal vena cava; the thoracic great veins into the
  right atrium.
- **Valves**: the four heart valves close against back-flow and open
  with pressure (ideal diodes with a small resistance, or Mynard et al.
  2012's valve dynamics if diodes prove too stiff). Leg veins have venous
  valves.
- **External pressures**: intrathoracic pressure from breathing
  (around -4 to -8 mmHg on a breath in) acts on everything in the chest;
  straining against a closed throat (Valsalva) raises chest and
  abdominal pressure. A pneumothorax later raises it on its side.
- **Gravity**: each segment's height comes from posture (standing,
  crouched; lying later via #123). Standing pools blood in the legs and
  splanchnic veins.
- **Muscle pump**: walking and running squeeze the leg veins with each
  stride (an external pressure pulse at the step cadence). With the
  venous valves this pumps blood back up by itself.

Blood is the circuit's volume: the sum of the compartments is the blood
volume, and red cells and plasma are tracked so haemoglobin dilutes as
plasma refills.

### The heart's pump and its timing

- Each chamber's elastance varies over its own cycle (Sagawa's
  time-varying elastance, shaped by Stergiopulos et al. 1996's double
  Hill function): stiff in systole, compliant in diastole. Frank–Starling
  and the pressure–volume loop come out of this. Contractility (peak
  elastance) is set by heart function, sympathetic tone and adrenaline.
  Systole shortens less than diastole as the rate rises, so filling time
  falls at high rates.
- **Conduction timing** fires the chambers: an SA node whose rate the
  autonomic nerves set, atrial activation, an AV-node delay that
  sympathetic tone shortens and vagal tone lengthens, refractory
  periods, then ventricular activation. A slow ventricular escape
  pacemaker fires if no conducted beat arrives. This is the skeleton
  stage 4 grows every arrhythmia on. In stage 1 today's rhythms map onto
  it: AF means no atrial contraction and an irregular conducted rate; VT
  is a fast ventricular focus; VF is uncoordinated activation (no
  effective elastance rise); complete block cuts AV conduction and lets
  the escape rhythm run; PEA is electrical activity with no effective
  contraction; asystole is nothing. Ectopic beats come from a stochastic
  focus whose rate follows irritability, at real base rates: occasional
  in a healthy heart, frequent in a stressed one.
- **The monitor traces become real**: the ECG is drawn from the actual
  activation events (a P wave when the atria fire, QRS when the
  ventricles do); the arterial trace is the simulated arterial pressure;
  the pleth follows finger (upper-body) arterial flow; a pulse at the
  finger exists only if pressure gets there.

### Control

- **The baroreflex** follows Ursino (1998): carotid-sinus pressure
  (aortic pressure minus the column up to the neck) sets an afferent
  firing rate through a sigmoid with rate sensitivity. Efferent
  sympathetic and vagal activity act on each effector after a pure delay
  and through a first-order lag: heart period (vagal ~0.2 s delay,
  sympathetic ~2 s), contractility, each bed's resistance, and venous
  unstressed volume (slowest, tens of seconds). Low-pressure receptors
  in the atria add sympathetic tone early in blood loss. The values are
  Ursino's, scaled to this body.
- **Chemoreflex**: low oxygen and high CO2 (from today's gas model until
  stage 2) drive sympathetic tone.
- **Adrenaline**: today's plasma model (`catechol`, 2–3 min half-life)
  stays and acts on the same effectors. Release now follows sympathetic
  drive (blood loss, exercise, pain, fear, low sugar) instead of separate
  rules.
- **Exercise** follows Magosso and Ursino (2002) and Fresiello et al.
  (2016). Central command rises within seconds of the effort, withdraws
  vagal tone (the fast first heart-rate rise) and resets the
  baroreflex. Muscle beds widen with local metabolic demand over tens of
  seconds. The muscle metaboreflex adds sympathetic tone as metabolites
  build. Together, heart rate jumps a little in the first seconds, then
  climbs over one to two minutes. Blood pressure rises gradually, then
  eases back over minutes after stopping. **The jump is gone because
  nothing is wired straight to demand any more.**
- **The vasovagal reflex** and the diving reflex become vagal surges and
  sympathetic withdrawal in the same framework.
- **Natural variation** comes from the loops themselves. Vagal outflow is
  gated by breathing (the heart speeds on a breath in), and chest
  pressure swings venous return. Small band-limited fluctuations in
  sympathetic outflow (seeded) ring through the baroreflex's delay into
  slow ~0.1 Hz pressure waves (Mayer waves, Julien 2006). Today's
  `wander()` noise on heart rate and pressure goes.
- **Bleeding becomes pressure-driven.** A wound drains the compartment
  it opens (an arm artery the upper-body bed, the abdomen the splanchnic
  one) through a wound resistance, so bleeding slows as pressure falls,
  as it does in life. Clotting, dressings and tourniquets act on that
  resistance. Plasma refills from the tissues over hours (Starling
  forces), which dilutes haemoglobin. The refill runs on the survival
  clock (`HEALTH_CLOCK`), like thirst and healing, so most of it happens
  within real minutes to an hour of play. The fast responses stay in
  real time (the maintainer's choice, 2026-11-23).
- **Straining** (Valsalva) comes from what the player already does, not
  a new control: hard block-breaking, pushing against a weight that
  crushes the body, and exertion while holding the breath under water.
  A `health_env` field carries it from `survival.c`. Lifting and pushing
  heavy things join when items and machines exist.

### Every body differs

`health_baseline` grows from today's six numbers to the circulation's
real spread, each drawn from the seed within healthy adult ranges:
- heart size and contractility;
- arterial stiffness;
- blood volume per kilogram;
- baroreflex gain;
- resting vagal tone (resting heart rate);
- venous tone;
- the size of the sympathetic fluctuations;
- fitness (VO2max, critical power).

`health_init` keeps the population mean, so reference tests stay exact.

### What the player feels (no numbers)

- **A heartbeat you hear and feel**, timed to the real ventricular
  beats. It is heard when the heart pounds (hard effort, fear, a big
  stroke) or in silence, and felt as a faint pulse in the view and
  hands. A skipped or early beat is felt as one. It uses the existing
  heartbeat sound; the sound overhaul (#110) remakes the sound itself
  later, not this timing.
- **Vision**: a faint pulse at the edge of vision on a pounding heart.
  Greying and dimming as brain blood flow (the brain bed's real flow)
  falls: a head rush on standing up, worse dehydrated or after blood
  loss. Crouching or lying helps. Straining then letting go greys the
  view for a moment. Standing still long in heat makes you
  light-headed. These extend the post pass `health_senses` already
  drives, with its Vulkan 1.2 fallback.
- **The inner voice rebalance** (principles above) covers all of
  `thoughts.c`, not only the heart and faint lines, and lands in this
  pull request because the new circulation changes when those lines
  trigger.
  - Mild lines go.
  - Lines that name hidden causes are rewritten as feelings.
  - Mild hunger, thirst and tiredness keep one line the first time they
    appear (the "new" rule), so they keep a cue until body cues for them
    exist.

### What stays for later stages

Gas exchange keeps today's model, now fed real cardiac output and
pulmonary flow (stage 2). Consciousness keeps its three levels but reads
brain-bed flow until stage 3 replaces them with a continuous scale. The defibrillator stays as it is (stage 4).
Kidneys, the renin–angiotensin system and ADH come with stage 5;
stage 1's slow volume control is only the plasma refill.

### Limits

- **Cost**: the circuit runs at a fixed sub-step (about 1 ms) inside the
  60 Hz tick, within ~50 µs a tick for one body on the baseline machine.
- **Determinism**: a fixed sub-step count per tick, seeded noise from
  `vrng`, exact-replay tests.
- **Networking**: clients don't run `health_step`; they show what the
  authority sends. The table gains what a client displays:
  - the vitals;
  - the monitor traces, as today;
  - the time of each ventricular beat, so a client's heartbeat and
    vision pulse land on the real beats.

  The baroreflex's delay lines and the compartment volumes stay on the
  authority. All of it fits within `HEALTH_NET_MAX`
  (docs/design/multiplayer.md, "until then").

### Checks

Unit tests, each deterministic:
- **Rest**: values in normal ranges.
  - heart rate 55–85
  - blood pressure about 120/80, MAP 85–100
  - cardiac output 4.5–6 L/min, stroke volume 60–100 mL
  - left ventricular end-diastolic volume 110–150 mL, ejection fraction
    55–70 %
  - central venous pressure 2–8 mmHg, pulmonary artery pressure about
    25/10
  - coronary flow mostly in diastole
- **Running starts**: no blood pressure jump. Systolic changes by under
  10 mmHg in the first second. Heart rate rises fast in the first ~10 s,
  then slowly. Systolic is 160–190 after a minute or two of hard
  running. Recovery after stopping takes minutes, not seconds.
- **Blood loss classes**: I–IV produce the textbook compensation in
  order. Heart rate rises first and pulse pressure narrows; pressure
  holds until about 30 % is lost.
- **Bleeding slows as pressure falls**: the same wound drains less once
  the body is hypotensive.
- **Standing up after crouching**: a brief dip in brain flow that the
  baroreflex corrects within seconds. It is deeper and longer after
  blood loss or dehydration.
- **Valsalva**: the four classic phases of pressure and heart rate.
- **Breathing**: heart rate swings with breathing (respiratory sinus
  arrhythmia), and pressure shows ~0.1 Hz waves in a varied body.
- **Today's rhythms**: each still has its effect. AF lowers output by
  losing the atrial kick, and complete block runs at the escape rate.
- **Exact replay**; and the existing health tests still pass, with
  numbers updated only where the old ones were the formula's artefacts.

By running the game:
- the F3 health panel with the new values and a left ventricular
  pressure–volume loop;
- screenshots of rest, a sprint and a head rush;
- `--validate` at the default API and `MC_VK_API=1.2` for the post-pass
  change.

## Credit

Each method gets its full entry in [CREDITS.md](../../CREDITS.md) in the
pull request that implements it. Expected for stage 1:
- Ursino (1998): the baroreflex with a pulsating heart.
- Heldt, Shim, Kamm and Mark (2002): the circuit and gravity.
- Stergiopulos, Meister and Westerhof (1996): elastance.
- Magosso and Ursino (2002): exercise.
- Fresiello, Meyns, Di Molfetta and Ferrari (2016): exercise, with
  atria.
- Julien (2006): Mayer waves.
- Mynard et al. (2012): valves, if used.
- The Pulse Physiology Engine's methodology, consulted for its circuit
  and validation targets.

## References kept for later stages

Downloaded to `build/health-refs/` (not committed):
- Pulse's methodology documents for every system.
- Fresiello et al. (2016) and Kana's thesis on autonomic models.
- MIT's cardiovascular simulator course notes.
- BodyParts3D 4.0 organ meshes (CC BY 4.0), the source the voxel
  body's tissue grid will be voxelised from (§14, the maintainer's
  choice).
- Visible Human colour cryosections (NLM, free since 2019), for real
  tissue colours in wounds and gore.
