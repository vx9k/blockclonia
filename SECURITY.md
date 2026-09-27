# Security policy

blockclonia is a single-player game. It has no network code, no accounts,
no telemetry, no mods or scripting, and it never runs anything it reads
from disk. That leaves a small attack surface, and this policy describes
exactly what it is.

## Reporting a vulnerability

Please report privately. Don't open a public issue.

- **GitHub**: *Security → Report a vulnerability* on the repository
  (private vulnerability reporting), or
- **Email**: security@kthread.dev

Include:

- what an attacker controls (for example "a world folder someone
  downloads") and what they gain (a crash, reading or writing a file
  outside the world, code execution);
- the smallest input that shows it: a world folder, settings file or
  command line, as an attachment;
- the commit you tested (`git rev-parse HEAD`), your OS and, for crashes,
  the output of an ASan build (`cmake --preset asan`).

What to expect:

| | |
|---|---|
| Acknowledgement | within 48 hours |
| First assessment (confirmed or not, severity) | within 7 days |
| Fix on `main` for confirmed issues | within 30 days where feasible |

Say whether you'd like to be credited in the fix. Please give the fix
time to land before publishing details.

## Supported versions

There are no releases yet. Fixes go to the `main` branch, and only `main`
is supported. Packages built from an older commit (`.deb`, `.tar.gz`)
should be rebuilt from `main`.

## What counts as a vulnerability

The game treats these as **untrusted input**:

| Input | Where | Why it is untrusted |
|---|---|---|
| World folders | `world/` or `--world DIR`: `level.dat`, `player.dat`, `c.X.Z.bin` column files | People share worlds. A world folder must never be able to crash the game in a memory-unsafe way, make it read or write files outside that folder, or hang it. |
| Settings file | `blockclonia.cfg` or `--config FILE` | Hand-edited, and possibly copied from someone else. |
| Command line | `--give`, `--hurt`, `--spawn`, sizes and so on | Launchers and scripts pass these through. |

In scope:

- memory-safety bugs (out-of-bounds access, use after free, integer
  overflow into a size) reachable from any input above;
- reading, writing, truncating or following links to files outside the
  folder the game was pointed at, or anything that turns a save into
  writes somewhere else;
- inputs that hang the game or make it allocate without bound;
- flaws in the build that let someone swap a dependency (see *Supply
  chain* below).

Out of scope:

- anything that already needs write access to your home directory or the
  game binary;
- bugs in GPU drivers, the Vulkan loader or validation layers, GLFW,
  miniaudio or mimalloc themselves (report them upstream; do tell us if
  the game triggers them from untrusted input);
- running out of memory or VRAM with extreme but valid settings (such as
  render distance 32 on a small device);
- cheating: it is a single-player game.

## How the code defends itself

- **Validated decoders.** The binary formats (columns, `level.dat`,
  `player.dat` and the inventory inside it) are parsed by code that checks
  every field and rejects the whole file on anything malformed, leaving the
  game's state untouched (`save.c`, `survival.c`, `inventory.c`). The
  settings parser skips unknown keys and out-of-range values line by line
  (`settings.c`). The column decoder, the most complex one, has a
  libFuzzer target (`tests/fuzz_save.c`, built with `cmake --preset fuzz`).
- **Safe file access.** World and settings files are opened with
  `O_NOFOLLOW`, must be regular files (so no FIFOs or devices), and are
  read up to a size cap. Writes go to a temporary file first and are
  renamed into place, so a crash never leaves a half-written save.
- **Bounded work.** Every file is read whole into a buffer with a fixed
  maximum size, and decoding can never produce more data than a column
  holds.
- **Checked arithmetic.** Array allocations go through
  `mem_array_size()`, which aborts on overflow. Allocation failure aborts
  cleanly instead of returning NULL.
- **Compiler and tool gates.** CI builds with gcc and clang using
  `-Werror` and strict conversion warnings, runs the unit tests under
  AddressSanitizer and UndefinedBehaviorSanitizer, and runs clang-tidy
  and cppcheck (`tools/lint.sh`).

## Supply chain

The build downloads pinned releases of mimalloc, GLFW and miniaudio from
their GitHub repositories when the system copies are missing or too old
(`cmake/Dependencies.cmake`). The version is pinned, but the archive hash
is only checked if you set it. For a reproducible or packaged build,
either:

- build with `-DMC_DEPS=SYSTEM` against your distribution's packages; or
- set `MC_MIMALLOC_SHA256`, `MC_GLFW_SHA256` and `MC_MINIAUDIO_SHA256` to
  the archives' SHA-256 so a changed download fails the build.

Shaders are compiled from `shaders/` at build time and embedded in the
binary. Textures and sounds are generated at start-up, so no image or
audio files are parsed at run time.
