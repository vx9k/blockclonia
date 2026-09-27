# blockclonia

A Minecraft-style voxel sandbox written from scratch in C11 and Vulkan 1.0,
with physics that tries to behave like the real world and a renderer tuned
for low-end hardware. All memory comes from [mimalloc](https://github.com/microsoft/mimalloc).

![structural collapse](docs/collapse.png)

## What's in it

- **Infinite terrain** streamed around the player in 16×128×16 columns:
  continents, hills, ridged mountains with snow, beaches, oceans and trees.
- **Realistic physics** (1 block = 1 m, SI units, fixed 60 Hz step):
  - Gravity 9.81 m/s², quadratic air drag (terminal velocity ≈ 49 m/s for
    the player), buoyancy from real densities (a human barely floats, a log
    floats, stone sinks) and water drag.
  - Walking is limited by friction: you can only accelerate or brake at
    μ·g, so ice (μ = 0.03) is genuinely slippery and stopping on grass
    takes about a metre.
  - **Structural integrity**: every block needs a load path to the ground.
    Each material can cantilever a set number of blocks (stone 7, timber 6,
    brick 5, dirt 1); sand and gravel only rest on what's directly below.
    Knock out a support and everything it held becomes falling rigid
    bodies, which land and re-solidify. Cut a tree's trunk and the tree
    comes down.
  - **Water** is a volume-conserving cellular automaton with 8 levels per
    block: it falls, spreads, levels out and never duplicates itself.
- **Block editing**: break, place, pick; nine block types in the hotbar
  including water.
- **Saves**: only columns you changed are written, one RLE-compressed file
  per column, written atomically.
- **A body that works like one** (press **H**): see [Health](#health).

## Built for low-end devices

| Technique | Why it matters on weak hardware |
|---|---|
| 4-byte vertices (position, face, AO, texture layer packed in one `uint32`) | 4–8× less vertex bandwidth than a naive float layout |
| Greedy meshing with baked ambient occlusion | Far fewer triangles; AO costs nothing at runtime |
| One vertex pool buffer for every chunk mesh, one shared index buffer | One bind per frame; never hits `maxMemoryAllocationCount` |
| Integrated GPUs write meshes straight into shared memory | No staging copy on UMA devices |
| Push constants instead of uniform buffers | No descriptor updates per frame |
| Reversed-Z infinite projection with a float depth buffer | Stable depth at any distance |
| No `discard` in any shader, depth attachment never stored | Keeps early-Z and on-chip depth on tile-based mobile GPUs |
| Camera-relative rendering, doubles for physics positions | No jitter far from spawn |
| CPU frustum culling per 16³ section, front-to-back opaque, back-to-front translucent | Less overdraw |
| Meshing and generation on worker threads from private snapshots | Main thread never waits, edits never race |
| Procedural textures with mipmaps, generated at startup | No image files to ship or parse |
| Vulkan 1.0 core with zero optional features | Runs on anything with a Vulkan driver, including Raspberry Pi (V3DV) |

## Controls

| Key | Action |
|---|---|
| Mouse | Look (click the window to capture the cursor) |
| W A S D | Walk |
| Space | Jump / swim up |
| Left Ctrl | Sprint |
| F | Toggle fly mode (Space/Shift to go up/down) |
| Left click | Break block |
| Right click | Place block |
| Middle click | Pick block |
| 1–9 or scroll | Select block |
| F2 | Screenshot (`screenshot.ppm`) |
| H | Health panel (while open: 1–7 or ↑/↓ pick a body part, B bandage, S splint, D disinfect, P painkiller, A antibiotics) |
| E / R | Eat an apple / drink (standing in or facing water) |
| Enter | Respawn after death |
| Esc | Release cursor, press again to quit |

## Health

![the H panel](docs/health.png)

The player is a 75 kg adult simulated in real time: a circulation model
(blood volume, heart rate, stroke volume, vascular resistance, a
baroreflex), breathing driven by CO₂ and O₂, an oxygen store with a real
dissociation curve, core temperature, water and energy balance, and
stamina as an anaerobic reserve. Slow processes (infection, healing,
thirst, hunger) run on a survival clock 72× faster than real time.

- **The HUD** shows heart rate, blood pressure and SpO₂ with a live ECG
  strip, bars for blood, water, food and stamina, and alerts. Failing
  brain oxygen narrows vision; fainting blacks the screen out.
- **The H panel** has a body diagram and the status of each of the seven
  parts (fractures, cuts, bleeding rate, infection, internal bleeding,
  bruising) with advice for the selected one; the organs (brain, heart,
  lungs, liver, kidneys, gut); blood, hydration, food, glycogen, stamina,
  pain and temperature; lab-style numbers (Hb, lactate, cardiac output,
  blood gases); and a bedside monitor with ECG, arterial pressure, pleth
  and capnography traces that move with every beat and breath.
- **Injuries come from the physics**: landings (softened by leaves, snow
  or sand), running into walls, falling blocks by mass and speed, and
  smashing glass by hand. Cuts clot unless an artery is cut; walking on an
  unsplinted broken leg bleeds inside and can push the bone through the
  skin; dirty wounds get infected and can turn septic with a fever.
- **Treatment uses supplies** from a starting kit and from the world:
  leaves give plant fibre and apples, logs give sticks, two fibres make an
  improvised bandage, two sticks and a fibre make a splint.
- **Vitals respond to what you do**: sprinting raises heart rate, pressure
  and breathing and spends stamina; bleeding drops pressure while the
  heart races; holding your breath under water slows the heart (diving
  reflex) until you black out and inhale water.

You can die of blood loss, drowning, suffocation, head injury, cardiac
arrest, sepsis, dehydration, starvation, hypothermia or heatstroke.

## Building

Dependencies: a C11 compiler, CMake ≥ 3.21, Ninja, the Vulkan headers and
loader, `glslc` (shaderc), GLFW 3.3+, mimalloc 2.1+.

Debian, Ubuntu, Raspberry Pi OS:

```sh
sudo apt install build-essential cmake ninja-build glslc libvulkan-dev \
                 libglfw3-dev libmimalloc-dev mesa-vulkan-drivers
cmake --preset native            # tuned for this machine (use this on a Pi)
cmake --build --preset native
./build/native/blockclonia
```

- If the distro's mimalloc is older than 2.1 (Pi OS bookworm, Ubuntu
  22.04), the build downloads and compiles mimalloc 2.1.7 (and GLFW 3.4 if
  GLFW is missing). `-DMC_DEPS=SYSTEM` forbids downloads; `FETCH` always
  uses the pinned releases.
- `cmake --list-presets` shows the rest: `dev`, `release` (-O2 + LTO,
  portable), `profile`, `asan`, `core` (no Vulkan or GLFW needed), `fuzz`,
  `ci-gcc`/`ci-clang` (-Werror), and cross builds for the Pi 4/5
  (`pi4-aarch64`, `pi5-aarch64`, `pi4-armhf`; see the header of
  `cmake/toolchains/rpi-aarch64.cmake`).
- `-DMC_CPU=pi4|pi5|native|x86-64-v2` picks the CPU to tune for. 32-bit
  Pi OS's compiler has NEON off by default, so set it there.
- Packages: `cd build/release && cpack` makes a `.tar.gz` and a `.deb`.

### Running

```
blockclonia [--seed N] [--radius 2-32] [--size WxH] [--no-vsync]
            [--threads N] [--pool-mb N] [--gpu N] [--world DIR | --no-save]
            [--validate] [--frames N] [--screenshot F] [--look YAW,PITCH]
            [--spawn X,Z] [--demo] [--bench] [--mem-stats]
            [--health-panel] [--hurt LIST]
```

`--hurt` starts you injured, to try treatments or take screenshots: a
comma list of `bleed`, `artery`, `fracture`, `open-fracture`, `infection`
and `concussion`.

For very weak devices start with `--radius 4`. `--demo` builds a tower
with a timber cantilever in front of you and knocks out its middle.
`--bench` runs CPU benchmarks without opening a window.

Headless (no GPU), with Mesa's software rasterizer:

```sh
Xvfb :99 &
DISPLAY=:99 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build/native/blockclonia --frames 300 --screenshot shot.ppm --no-save
```

## Memory allocation

Game code allocates through `src/mem.h`, which wraps mimalloc's `mi_*` API
and aborts cleanly on out-of-memory. mimalloc is linked first, so on Linux
its shared library also overrides `malloc` for GLFW, the Vulkan loader and
the GPU driver (`LD_DEBUG=bindings` confirms they bind to
`libmimalloc.so`). Explicit `VkAllocationCallbacks` backed by mimalloc
are available with `MC_VK_ALLOC=1` for platforms without the override;
they are off by default because some drivers (Mesa lavapipe) free memory
they did not allocate through the callbacks.

## Tests

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
tools/lint.sh      # -Werror builds (gcc, clang), clang-tidy, cppcheck, shaders, clang-format
```

`ctest` runs the unit tests, `--bench`, and, when `xvfb-run` and Mesa's
lavapipe are installed, a 120-frame render with the validation layer.

The unit tests cover the vertex-pool allocator, save round-trips and
corruption rejection, the mesher (greedy merging, worst case, water),
world edits and mesh invalidation, free fall time against √(2h/g), no
tunnelling at 60 m/s, wall collision, jump height, friction (ice vs
stone), cantilever limits, tree felling, sand, buoyancy, water volume
conservation and raycasting; resting and exercising vitals, arterial
bleeding with and without a dressing, breath-holding and drowning,
fractures and splints, infection with and without treatment, thirst and
hunger, landings through the player physics, and the UI batch (text
metrics, wrapping, buffer overflow, whole screens).

## Layout

```
src/main.c       entry point, input, fixed-timestep loop
src/renderer.c   Vulkan renderer
src/world.c      column streaming, block access, job scheduling
src/worldgen.c   terrain generation        src/noise.c   gradient noise
src/mesher.c     greedy mesher with AO     src/texgen.c  procedural textures
src/physics.c    player, bodies, structure, water, raycast
src/save.c       world files               src/jobs.c    thread pool
src/gpupool.c    vertex pool allocator     src/mem.c     mimalloc wrappers
src/health.c     the body: circulation, breathing, injuries, treatment
src/survival.c   turns physics events into injuries, limits movement
src/hud.c        HUD, H panel, monitor     src/ui.c      2D overlay batch and font
src/log.c        logging, save-on-fatal hook
shaders/         GLSL, compiled to SPIR-V and embedded at build time
tests/           unit tests and the save-file fuzzer
cmake/           dependencies, shaders, warnings, packaging, Pi toolchains
tools/lint.sh    the lint gate CI runs
```
