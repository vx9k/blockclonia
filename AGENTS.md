# AGENTS.md

Working notes for anyone changing this code, human or AI coding agent
(Claude Code, Codex, Copilot, Cursor, ...). `CLAUDE.md` imports this file;
keep the rules here and tool-specific notes there.

blockclonia is a voxel sandbox in C11 and Vulkan: streamed terrain, SI-unit
physics with structural collapse and thermodynamics, a simulated human
body, procedural textures and sound, all tuned to run on a Raspberry Pi 4.
`README.md` covers what the game does, and
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) explains the frame loop,
threads and data flow.

## Build and test

```sh
cmake --preset dev && cmake --build --preset dev   # Debug, fetches pinned deps if the system's are old
ctest --preset dev                                 # unit tests, --bench, render smoke test (if lavapipe + xvfb)
./build/dev/mc_tests                               # the unit tests alone (~2400 checks, a few seconds)
tools/lint.sh                                      # the CI lint gate; run it before you commit
```

- `cmake --preset core` builds everything except the game (no Vulkan, GLFW
  or audio needed); the unit tests only link against the core library.
- Run the game with `./build/dev/blockclonia --play`. Useful flags for
  checking changes: `--validate` (Vulkan validation layers), `--no-save`,
  `--seed 1337`, `--frames N --screenshot out.ppm`, `--debug` (F3 on),
  `--screen settings|pause|inventory|...`, `--give log:8`, `--hurt fracture`,
  `--time 21`, `--no-sound`. `--help` lists all of them.
- No GPU (CI, containers): `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`
  selects Mesa's software rasterizer; under a headless session wrap the
  command in `xvfb-run -a`.
- Offline: point `FETCHCONTENT_SOURCE_DIR_MIMALLOC`, `..._GLFW` and
  `..._MINIAUDIO` at an existing `build/*/_deps/*-src` checkout.

A change is done when both compilers build it with `-DMC_WERROR=ON`,
`mc_tests` passes, `tools/lint.sh` is clean, and anything visible has been
run (a screenshot through `--frames`/`--screenshot` is the cheapest proof).

## Code rules

The compiler enforces most of these: `cmake/warnings.cmake` turns on
`-Wconversion -Wsign-conversion -Wdouble-promotion -Wshadow -Wvla
-Wframe-larger-than=16384` and more, and CI builds with `-Werror` on gcc
and clang. `.clang-tidy` and `cppcheck` run in `tools/lint.sh`.

- **C11, no extensions.** No VLAs. Stack frames stay under 16 KB, so big
  buffers go on the heap. Casts are explicit wherever a conversion narrows
  or changes sign.
- **Allocate through `src/mem.h` only** (`mem_alloc`, `mem_calloc`,
  `mem_realloc`, `mem_free`, `mem_array_size` for `n * size`). Never call
  `malloc`/`free`/`mi_*` directly. Allocation failure aborts, so callers
  do not check for NULL.
- **Units are SI**: metres, seconds, kilograms, watts; temperatures in °C.
  One block is one metre. World positions are `double` (`dvec3`); rendering
  works in `float`, relative to the camera.
- **Physics and health are deterministic.** They run at a fixed 60 Hz
  (`PHYS_DT`), get their randomness from seeded state (never `rand()` or
  the clock), and tests depend on exact replay. Frame-rate-dependent code
  (animation, camera, UI) takes the frame's `dt` and must look the same at
  30 and 144 fps.
- **Keep modules headless.** Everything except `main.c`, `renderer.c` and
  `audio_device.c` builds without a window, GPU or audio device, and is
  unit-tested that way. Don't include GLFW or Vulkan headers anywhere else.
  UI code writes quads into a `ui` batch; sound code mixes into a buffer.
- **Naming**: `module_verb` for functions (`world_set`, `inv_add`),
  `UPPER_CASE` for macros and `static const` tables, a `g_` prefix for
  globals. Every header starts with a comment that says what the module
  does and what it deliberately doesn't. Public functions get a comment
  unless the name says everything.
- **Comments explain why**, in plain sentences: the physics behind a number,
  the hardware reason for a layout, the bug a check prevents. Don't narrate
  what the next line does.
- **Formatting** is `.clang-format` (4 spaces, 120 columns). CI only checks
  the lines you changed, so don't reformat untouched code.

## Subsystem rules

### Renderer (`renderer.c`, `gpucaps.c`, `shaders/`)

- **Vulkan 1.0 with no optional features must keep working.** It is what
  the Raspberry Pi 4 and older Mesa offer. Newer features (1.1–1.4 core,
  extensions) are optional: `gpucaps.c` decides from what the device reports,
  and every feature has a 1.0 fallback path. Add a feature by adding a
  `GPUCAP_*` bit, its rule in `gpucaps_choose`, a test in
  `tests/test_gpucaps.c`, and the fallback.
- **Guard newer headers at compile time.** CI builds against Vulkan headers
  1.3.239 (Debian bookworm) and 1.3.275 (Ubuntu 24.04), so anything from
  1.4 goes under `#ifdef VK_API_VERSION_1_4`. Load entry points newer than
  1.0 with `vkGetDeviceProcAddr`/`vkGetInstanceProcAddr`, never by linking.
- **Test the fallbacks** on a capable machine with `MC_VK_API=1.0` (up to
  `1.4`) to cap the API version and `MC_VK_DISABLE=all` (or a comma list:
  `mdi,dynrend,sync2,timeline,aniso,hostcopy,budget,timestamps,pcache,hostreset,relaxed`).
  Any renderer change must run with `--validate` and zero validation
  messages, on a real GPU and on lavapipe.
- The hot path is built for weak GPUs: 4-byte vertices, one pool buffer,
  push constants rather than uniform buffers, no `discard`, reversed-Z. Read
  the comments in `renderer.h` and `mesher.h` before changing formats, and
  update `shaders/push_vert.glsl` when constants shared with C change (the
  `_Static_assert`s catch most of them).

### Files on disk (`save.c`, `settings.c`, `survival.c`, `inventory.c`)

- **Everything read from disk is untrusted.** World folders get shared.
  Decoders validate every field and reject malformed input without
  touching the destination. File access goes through `save_read_file` and
  `save_write_file`: regular files only, no symlinks followed, writes made
  atomic through a temporary file and `rename`. Don't open world files any
  other way.
- Formats are versioned by a magic tag (`INV1`, ...). Change the tag when
  the layout changes, and keep reading the old one when that is cheap.
- New parsers get a round-trip test, a corruption test and, when they read
  large or structured data, a libFuzzer target like `tests/fuzz_save.c`.

### Sound (`sound.c`, `sound_synth.c`, `audio_device.c`)

- The audio callback runs on the device's thread: **no locks, allocation,
  I/O or logging there.** The game thread talks to it only through the
  lock-free command ring in `sound.c`.
- Every sound is synthesized at start-up; the project ships no audio or
  image files. Keep synthesis under ~100 ms total on a desktop CPU
  (`test_sound` has a benchmark).

### Threads

Worker threads (`jobs.c`) generate terrain and build meshes from private
snapshots. Their results are applied on the main thread in `jobs_poll`.
Game state is main-thread only: don't touch `world`, `physics` or `health`
from a job.

## Tests

- `tests/test_main.c` holds the older suites; newer areas have their own
  file (`test_thermo.c`, `test_health2.c`, `test_bodies.c`,
  `test_visual.c`, `test_sound.c`, `test_gpucaps.c`) with an entry point
  declared in `tests/test_util.h` and called from `main`. A new test file
  also goes into the `mc_tests` source list in `CMakeLists.txt`.
- Use the `CHECK(cond)` macro and the helpers in `test_util.h`: a flat
  test world (`tw_init`, `run_steps`) for physics, and `new_body`,
  `calm_env` and `live` for the health model. Tests must not need a window,
  GPU, audio device or network, and must not write to the working
  directory: make a directory under `$TMPDIR` (falling back to `/tmp`)
  with `mkdtemp`.
- A bug fix comes with a test that fails without it.

## When behaviour changes, update

- **Controls**: the README table, the controls screen in `menu.c`
  (`controls_screen`), and the HUD hints.
- **Command-line options**: `usage()` in `main.c` and the README.
- **Settings**: `settings.c` (field table, defaults), the settings screen
  in `menu.c`, and the README.
- **Renderer features**: `gpucaps.h` and the README's renderer table.
- **Dependencies**: `cmake/Dependencies.cmake` (version pins), the README
  build section, and `.github/workflows/ci.yml`.

## Git

- Every change reaches `main` through a pull request. Don't push to
  `main` directly, and don't rewrite history that is already on it.
- Branches, pull request titles and descriptions follow the rules below.
- Commit messages follow
  [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/)
  (details below).
- Don't commit build output, `world/`, `blockclonia.cfg`,
  `blockclonia.pipelines` or screenshots (`.gitignore` covers them).
- Security issues go through [`SECURITY.md`](SECURITY.md), not public issues.

### Branches and pull requests

- Branch from an up-to-date `main`. Name the branch
  `<type>/<short-description>`: the Conventional Commits type the change
  will carry, a slash, then a few lower-case words joined by hyphens
  (`fix/water-column-edges`, `feat/camera-fov`). Put the issue number
  first when there is one: `fix/57-water-column-edges`.
- One topic per pull request. A refactor and a feature go in separate
  pull requests, even when one needs the other.
- **Title**: a Conventional Commits header, following the same rules as a
  commit subject (below), e.g.
  `fix(physics): stop water duplicating at column edges`. A squash merge
  uses the title as the commit subject on `main`, so the title has to
  stand on its own. Mark a breaking change with `!` in the title too.
- **Description**: fill in every section of
  `.github/pull_request_template.md`.
  - *What and why*: the problem and the approach taken, not a list of
    files.
  - *How it was checked*: the commands you ran and their results, and
    screenshots for anything visible.
  - *Checklist*: tick only what you actually did.

  Put any breaking change and anything the reviewer should look at first
  at the top, and link the issue with `Closes #N`.
- Open the pull request as a draft while work is in progress. Mark it
  ready when CI passes and the checklist is done.
- Before review starts you may rebase your own branch on `main` and push
  with `--force-with-lease`. Once review has started, add commits instead
  of rewriting them.
- The maintainer reviews and merges. Don't merge your own pull request.

### Commit messages

Every commit message follows
[Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/):

```
<type>[optional scope][!]: <description>

[optional body]

[optional footer(s)]
```

What the specification requires:

- The header is a type, an optional scope in parentheses, an optional
  `!`, then a colon, a space and the description.
- `feat` MUST be used for a commit that adds a feature, and `fix` for one
  that fixes a bug. Other types are allowed.
- A scope is a noun naming a section of the codebase, e.g. `fix(save):`.
- The body is optional, free-form, and starts one blank line after the
  description.
- Footers are optional and start one blank line after the body. Each is a
  token, then `: ` or ` #`, then a value (`Refs #42`,
  `Co-Authored-By: Name <email>`). Tokens use `-` in place of spaces; the
  only exception is `BREAKING CHANGE`.
- A breaking change is marked with `!` just before the colon, with a
  `BREAKING CHANGE: <description>` footer, or with both. The footer token
  must be uppercase. `BREAKING-CHANGE` means the same thing.

What this project adds on top:

- **Types**: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`
  (CMake, dependencies, packaging), `ci` (`.github/workflows`), `style`
  (formatting only, no behaviour change), `chore` (anything else that
  touches no shipped code) and `revert`. Don't invent others.
- **Scopes** are optional. When you use one, make it the module's file
  name without the extension (`renderer`, `gpucaps`, `mesher`, `physics`,
  `thermo`, `health`, `save`, `settings`, `sound`, `worldgen`, `menu`,
  ...), or `shaders`, `tests`, `lint`, `agents`, `readme` or `docs` for
  those areas. Leave the scope out when a change spans several modules.
- **Description**: imperative mood, starting with a lower-case letter
  (names and identifiers such as `Vulkan` or `INV1` keep their case), no
  full stop, and the whole header within 72 columns
  (`fix(save): reject columns with negative heights`).
- **Body**: say what changed and why, wrapped at 72 columns.
- **Breaking changes**: mark anything that breaks users with `!` and a
  `BREAKING CHANGE:` footer. That includes a save or settings format old
  files can no longer be read from, a removed or renamed command-line
  option or settings key, and a raised minimum Vulkan version or
  dependency.
- One logical change per commit. If a header needs two types, split the
  commit.

Examples (illustrative, not real history):

```
feat(camera): add a --fov option for the field of view
```

```
fix(physics): stop water duplicating at column edges

The flow step read a neighbour's level after that neighbour had already
been updated this tick, so water was counted twice. Snapshot the levels
before the step.

Refs #57
```

```
feat(inventory)!: store item durability in the saved inventory

BREAKING CHANGE: the inventory tag changes from INV1 to INV2, and
inventories saved by older builds are no longer read.
```
