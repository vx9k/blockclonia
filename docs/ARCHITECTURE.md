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
             trees
                 block, noise, texgen, treetex, mathlib, mem, log  (shared basics)
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
  jobs_poll                       finished generation/light/meshing jobs: meshes and light upload now
  build_view                      interpolated camera, sky, entity list, view model
  frame_sound                     listener, ambience loops, fires -> sound commands
  draw_overlay                    HUD, hotbar, inventory, menus, F3 -> the UI batch
  renderer_end_frame              record, submit, present
frame_bookkeeping                 window title, autosave every 5 s, a synced save after a body event
```

Physics runs at a fixed 60 Hz, and rendering interpolates between the last
two steps (`alpha`). If a frame falls more than five steps behind, the
game slows down instead of spiralling. While a menu is open the
accumulator stays at zero, so the world stands still.

## Threads

| Thread | Runs | Touches |
|---|---|---|
| Main | everything in the frame above | all game state |
| Workers (`jobs.c`, cores − 1 while memory allows; `sys_plan`) | terrain generation (decoding a saved column first), lighting a column, meshing | only the job's own data: a column being generated, a snapshot of the 3 x 3 columns being lit, or a snapshot of a section and its neighbours with their light |
| Save writer (`save_db.c`, one per open world) | committing the batches of saved rows the main thread encoded | the database connection, under its lock, and the queue of batches |
| Audio (miniaudio's device thread) | `sound_render`: mixing voices | the mixer's own voices, fed by a lock-free single-producer ring |
| Driver threads | whatever the Vulkan driver does | nothing of ours |

Job results come back to the main thread through `jobs_poll`, which runs
each job's completion callback, so game state never needs a lock. A player
edit re-meshes the affected sections at the front of the queue
(`jobs_submit_front`), so the change shows on the next frames.

A generation job gets its own copy of the generator and the seed when it is
queued, and the bytes of a saved column, read on the main thread. It never
reads `world`, so the main thread can change the world while it runs.

## World data

- The world is a ring grid of **columns** (16 × 384 × 16 blocks, one byte
  per block, plus an optional byte of meta per block for water levels and
  campfire fuel). Each column is split into twenty-four 16³ **sections**
  for meshing and culling; sections of sky are skipped. Worlds from before
  were 128 blocks high: their columns load with air above, and a column
  with nothing above that old ceiling is still saved in the old format.
- Columns load in a spiral around the player out to the render radius, and
  unload (after saving any edits) beyond it. A column whose save fails stays
  loaded and is saved again on the next attempt, so its edits wait until a
  save succeeds. A column is only visible to the game once its state is
  `COL_READY`.
- **Edits** go through `world_set`, which marks the section meshes dirty
  and fires `on_block_changed`. Physics uses that hook to queue a
  structural check and wake water, and thermo uses it to track heat
  sources. Structural checks run under a fixed work budget per step, not a
  clock, so the same checks run on every machine.
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
  (healing, thirst, conduction, fuel) run on the **survival clock**, 24×
  real time, so a day lasts an hour. `docs/design/health.md` (Clocks)
  lists which process runs on which clock.

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
Vulkan features and their fallbacks; each one is used only where the
device offers it at the API version in use. The Vulkan allocation
callbacks, used only with `MC_VK_ALLOC=1`, are defined in `renderer.c`,
and `mem.c` supplies the aligned allocations they call.

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
  folder, holding the seed, every player, the world's state and one row
  per edited column (`save_db.c`); `save.c` still encodes and decodes the
  columns. A folder written by an older build
  (`level.dat`, `player.dat`, `c.X.Z.bin`) is imported into the database
  the first time it is opened, leaving the old files in place.
- Players and the world's state are tagged records (`tagged.c`): named
  sections of fields keyed by a hash of their names, so a build reads
  what it knows, skips what it does not, and keeps its defaults for
  fields a record lacks. Adding a field changes no format. A player's
  record (`survival.c`) holds the player, the inventory and the whole
  body (`health_tags.c`), dead or alive, so leaving a world neither heals
  nor revives; joined players' bodies are saved the same way. The
  world's state (`world_state.c`, the `blobs` row `state`) holds the
  time of day and the days passed, plus empty sections for systems
  still to come (vehicles, machines, weather, animals). Every field has
  a range, and a record that breaks one is refused whole.
- Each save encodes the edited columns, the players and the world's state
  on the main thread into one batch (`save_batch.c`) and hands it to the
  world's writer thread, which commits it in one transaction, so they
  always match on disk. The main thread never waits for the disk. Batches
  waiting for the writer merge row by row, the newer bytes winning, and
  reads look in that queue before the table, so a column that unloads
  before its save lands loads back as it was left. A commit that fails
  (a full disk) goes back to the queue and is tried again every half
  second, nothing dropped; the world keeps playing meanwhile. A saved
  column that cannot be read is shown as generated terrain and never
  written over, so its real data stays on disk.
- Saves run with SQLite's write-ahead log at `synchronous = NORMAL`,
  which keeps every committed save when the game crashes or is killed.
  Saves that must also survive a power cut or an OS crash (an event in
  the body's log, dying, respawning, quitting, a fatal error) are durable
  batches, committed at `synchronous = FULL`; they cost the writer an
  fsync, not the main thread. A test kills a saving process at random
  moments and checks that the world opens intact with every save it was
  told had landed.
- The database format is a number in the file's header (`SAVE_DB_FORMAT`).
  Every format's exact schema is kept in `save_db.c`, so a file whose
  tables match a known older format is upgraded on open: copied first to
  `world.db.format<N>`, then rebuilt in one transaction that either
  finishes or leaves the file as it was. Format 2 made the tables STRICT
  with CHECKs on keys and sizes. Format 3 gave every player a row in
  `players` (the single player's is `local`, others are keyed by their
  identity key) and made `meta` hold typed values, the world's name among
  them. Opening also runs `quick_check`, and
  closing hands back freed pages through incremental vacuum.
- `backups.c` keeps rolling copies in `<world>/backups`: one each time a
  world is opened to play (at most one per ten minutes), pruned to the
  three newest plus the newest of each of the last seven days. The copies
  are written with SQLite's backup API in rollback-journal mode, so each
  is a plain file. Restoring one checks it like any world, copies the
  world as it is first, then writes the copy over the open world page by
  page in one write.
- SQLite's exclusive locking mode keeps a world open in only one game at
  a time; a second instance pointed at the same folder disables saving
  instead of sharing the database.
- The game saves every 5 seconds, at once (durably) whenever something
  is logged about the body (an injury, a treatment, a change of rhythm,
  death), when a column unloads, when you leave to the title screen, on
  quit, and from the fatal-error hook (`log_set_fatal_hook`), which also
  waits for the writer, so a lost GPU device doesn't lose the world.
- Settings live in `blockclonia.cfg`, a text file with one `key value`
  per line. A settings file the game cannot read is left alone, never
  overwritten with the defaults.
- `SECURITY.md` explains why every one of these files is treated as
  untrusted input.
