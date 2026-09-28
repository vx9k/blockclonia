# Architecture

How the pieces of blockclonia fit together: the frame, the threads, where
state lives and which module owns what. Each header's top comment has the
detail for its module; this page is the map.

## Modules

```
                        main.c  (window, input, the frame loop, glue)
     ┌──────────┬──────────┬──────────┬───────────┬──────────┬──────────┐
  renderer   world      physics    health     thermo     sound      UI
  gpucaps    worldgen   survival   (body)     (heat,     sound_synth menu, hud,
  (Vulkan)   mesher     interact              day)       audio_device invui, debug,
  gpupool    save       inventory                                    ui, viewmodel
             jobs       item, fx                                     camera, anim
                 block, noise, texgen, mathlib, mem, log  (shared basics)
```

- **`main.c`** is the only file that knows about GLFW input and ties the
  systems together. The `game` struct in it owns every piece of state.
  Each frame step is a function: `frame_menus`, `frame_input`,
  `frame_simulate`, `frame_interact`, `frame_render`, `frame_bookkeeping`.
- **`renderer.c`** is the only file that includes Vulkan. `gpucaps.c`
  decides which Vulkan features to use, from numbers alone, so it is
  unit-tested without a GPU.
- **`audio_device.c`** is the only file that includes miniaudio.
  `audio_null.c` replaces it in silent builds.
- Everything else is plain C with no window, GPU or audio device. It goes
  into the `mccore` library that both the game and `mc_tests` link against.

## One frame

```
glfwPollEvents                    keys, mouse, resize, focus -> g_in
frame_menus                       title/pause/settings open? freeze input, stop the clock
frame_input                       mouse look, hotbar, movement keys -> player_input,
                                  limited by what the body can do (survival_limit_input)
frame_simulate                    while the accumulator holds 1/60 s (at most 5 steps):
    physics_step                      player, falling bodies, items, structure, fluids
    survival_env + thermal_env        what the body is exposed to (air, water, fire, ground)
    survival_impacts                  landings, collisions, falling blocks -> injuries
    fx / sound events                 shattered glass, splashes, thuds, footsteps
    health_step                       circulation, breathing, metabolism, injuries, heat
  thermo_step, fx_step            heat field, day clock, particles (per frame, dt)
  camera_update                   head bob, landing dip, sprint FOV
frame_interact                    raycast, break/place/use (interact_frame), treatments, respawn
world_update                      stream columns around the player, schedule jobs
frame_render
  renderer_begin_frame            wait for the frame slot, acquire a swapchain image
  jobs_poll                       finished generation/meshing jobs: meshes upload now
  build_view                      interpolated camera, sky, entity list, view model
  frame_sound                     listener, ambience loops, fires -> sound commands
  draw_overlay                    HUD, hotbar, inventory, menus, F3 -> the UI batch
  renderer_end_frame              record, submit, present
frame_bookkeeping                 window title, autosave every 60 s
```

Physics runs at a fixed 60 Hz, and rendering interpolates between the last
two steps (`alpha`). If a frame falls more than five steps behind, the
game slows down instead of spiralling. While a menu is open the
accumulator stays at zero, so the world stands still.

## Threads

| Thread | Runs | Touches |
|---|---|---|
| Main | everything in the frame above | all game state |
| Workers (`jobs.c`, cores − 1, at most 8) | terrain generation, loading columns from disk, meshing | only the job's own data: a column being generated, or a snapshot of a section and its neighbours |
| Audio (miniaudio's device thread) | `sound_render`: mixing voices | the mixer's own voices, fed by a lock-free single-producer ring |
| Driver threads | whatever the Vulkan driver does | nothing of ours |

Job results come back to the main thread through `jobs_poll`, which runs
each job's completion callback, so game state never needs a lock. A player
edit re-meshes the affected sections at the front of the queue
(`jobs_submit_front`), so the change shows on the next frames.

## World data

- The world is a ring grid of **columns** (16 × 128 × 16 blocks, one byte
  per block, plus an optional byte of meta per block for water levels and
  campfire fuel). Each column is split into eight 16³ **sections** for
  meshing and culling.
- Columns load in a spiral around the player out to the render radius, and
  unload (after saving any edits) beyond it. A column is only visible to
  the game once its state is `COL_READY`.
- **Edits** go through `world_set`, which marks the section meshes dirty
  and fires `on_block_changed`. Physics uses that hook to queue a
  structural check and wake water, and thermo uses it to track heat
  sources.
- **Meshes** are built on workers into 4-byte vertices (see `mesher.h`).
  The renderer's `on_mesh_ready` hook copies them into a big GPU vertex
  pool, managed by `gpupool.c`: directly on integrated GPUs, through a
  per-frame staging buffer on discrete ones. When the staging buffer is
  full, the upload is refused and retried next frame.

## Physics, body and heat

- `physics.c` owns the player's rigid body, falling blocks (up to 4096),
  dropped items (512) and the water automaton. Blocks carry real
  properties (`block.h`: density, friction, restitution, hardness,
  cantilever span, heat capacity, conductivity), and behaviour follows
  from them rather than from special cases.
- `survival.c` turns what physics reports each step (landing speed, hits,
  pinned mass, submersion) into injuries and a `health_env`.
- `health.c` is a lumped model of a 75 kg adult. It never reads the world:
  everything arrives through `health_env` and the injury functions.
- `thermo.c` keeps a sparse heat field around the player, the day clock,
  campfires burning their fuel, and melting and freezing. Slow processes
  (healing, thirst, conduction, fuel) run on the **survival clock**, 72×
  real time, so a day lasts 20 minutes.

## Rendering

Each frame, `renderer_end_frame`:

1. copies staged meshes into the vertex pool;
2. culls sections against the frustum on the CPU, walking outward from
   the camera so the opaque pass is roughly front to back;
3. draws opaque terrain, skipping each section's face groups that point
   away from the camera;
4. draws opaque entities (falling blocks, items, particles) as one
   instanced cube draw;
5. draws translucent terrain back to front, then translucent entities;
6. draws the block selection outline, then the first-person arm and held
   item over a cleared depth buffer, then the crosshair;
7. draws the 2D overlay (HUD, menus, F3) as a single indexed draw of the
   quads `ui.c` wrote straight into mapped memory.

Positions are sent relative to the camera, so precision holds far from
spawn. Depth is reversed-Z. Constants go in push constants, so there is
one descriptor set for the whole frame. `gpucaps.h` lists the optional
Vulkan features and their fallbacks.

## Sound

`sound_synth.c` builds every effect at start-up, in about 50 ms: modal
resonators for impacts, filtered noise for footsteps and water, formants
for voices, and seamless loops for wind, water and fire. The game calls
`sound_play` and `sound_ambience` from the main thread. They post
commands into a lock-free ring, and the audio thread mixes 48 voices with
distance attenuation, panning, an underwater low-pass filter, and a
limiter.

## Saving

- Each world lives in one SQLite database, `world.db`, in the `world/`
  folder, holding the seed, the player (position, view, time of day,
  inventory) and one row per edited column (`save_db.c`); `save.c` still
  encodes and decodes the payloads. A folder written by an older build
  (`level.dat`, `player.dat`, `c.X.Z.bin`) is imported into the database
  the first time it is opened, leaving the old files in place.
- SQLite's exclusive locking mode keeps a world open in only one game at
  a time; a second instance pointed at the same folder disables saving
  instead of sharing the database.
- The game saves every 60 seconds, when a column unloads, when you leave
  to the title screen, on quit, and from the fatal-error hook
  (`log_set_fatal_hook`), so a lost GPU device doesn't lose the world.
- Settings live in `blockclonia.cfg`, a text file with one `key value`
  per line.
- `SECURITY.md` explains why every one of these files is treated as
  untrusted input.
