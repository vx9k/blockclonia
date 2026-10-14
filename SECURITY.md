# Security policy

blockclonia is a game with optional peer-to-peer multiplayer (off unless
`--host` or `--join` is given) and an optional Discord status over the
Discord app's local socket (off unless turned on in the settings). It has
no accounts, no servers of its own, no telemetry, no mods or scripting,
and it never runs anything it reads from disk or the network. This policy
describes exactly what the attack surface is.

## Reporting a vulnerability

Please report privately. Don't open a public issue.

- **GitHub**: *Security → Report a vulnerability* on the repository
  (private vulnerability reporting), or
- **Email**: security@kthread.dev

Include:

- what an attacker controls (for example "a world folder someone
  downloads", "the host of a game I joined", "a player who joined my
  game", "someone on the same network") and what they gain (a crash,
  reading or writing a file outside the world, reading or forging chat,
  pretending to be another player, code execution);
- the smallest input that shows it: a world folder, settings file,
  command line or packet capture, as an attachment;
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
| World folders | `world/` or `--world DIR`: `world.db` (SQLite), and the older `level.dat`, `player.dat`, `c.X.Z.bin` files, which are imported when present | People share worlds. A world folder must never be able to crash the game in a memory-unsafe way, make it read or write files outside that folder, or hang it. `world.db` is opened without following links, in SQLite's defensive mode, with no extensions, and refused unless its schema is exactly the one this build writes (`save_db.c`). |
| Settings file | `blockclonia.cfg` or `--config FILE` | Hand-edited, and possibly copied from someone else. A file the game cannot read (too big, a symlink, unreadable) is never overwritten. |
| Pipeline cache | `$XDG_CACHE_HOME/blockclonia/blockclonia.pipelines` | Copied between machines or left by another driver, and its contents go to the GPU driver, which may not survive garbage. It is read through `save_read_file` (regular file, no symlink, at most 16 MB) and only handed over when its header names this vendor, device and driver UUID (`gpucaps_pipeline_cache_load`); anything else starts an empty cache. |
| Command line | `--give`, `--hurt`, `--spawn`, `--join`, `--name`, sizes and so on | Launchers and scripts pass these through. |
| Multiplayer peers | UDP (ENet), default port 27450, only with `--host` or `--join` | Anyone can send packets to an open port. A host must survive any joiner, a joiner any host, and both anyone on the path. Every byte is decoded by checked code (`net.c`, `mp.c`, `wire.c`, `health_decode`, `player_decode`, `save_decode_column`). |
| Multiplayer key files | `identity.key` and `known_hosts` next to the settings file | Small text files; a damaged one is not used and not overwritten (`netkeys.c`). |
| Discord's replies | the Discord app's local IPC socket | Size-capped frames, PINGs answered, everything else read and dropped (`discord.c`). |

In scope:

- memory-safety bugs (out-of-bounds access, use after free, integer
  overflow into a size) reachable from any input above;
- reading, writing, truncating or following links to files outside the
  folder the game was pointed at, or anything that turns a save into
  writes somewhere else;
- inputs that hang the game or make it allocate without bound;
- in multiplayer: reading or changing chat between two players without
  being one of them (the host relaying it included), pretending to be
  another player or another host, joining a session's mesh without the
  host announcing you, or a peer's packets crashing, hanging or
  exhausting another player's game;
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
- cheating by the host: the host is the authority, simulates everyone
  and sees all game state (not chat) by design; join hosts you trust;
- an attacker on the network path dropping or delaying packets, or
  disconnecting peers: ENet's own headers (acknowledgements, peer ids)
  are not encrypted, only what they carry is;
- the first connection to a host being intercepted by someone who is
  already in the middle (trust on first use, as with SSH; compare keys
  out of band if that matters);
- a player seeing where other players are, or their names: the host
  shares both with everyone in the session.

## How the code defends itself

- **Validated decoders.** The binary formats (column rows and the player
  record in `world.db`, the inventory inside it, and the older `level.dat`,
  `player.dat` and column files that are imported) are parsed by code that
  checks every field, rejects non-zero reserved bytes and rejects the whole
  record on anything malformed, leaving the game's state untouched
  (`save.c`, `survival.c`, `inventory.c`). The pipeline cache is checked
  by its header before the driver sees it (`gpucaps_pipeline_cache_ok`).
  The settings parser skips unknown keys and out-of-range values line by
  line (`settings.c`). Each has a libFuzzer target, built with
  `cmake --preset fuzz`: the column decoder (`tests/fuzz_save.c`), the
  world database (`tests/fuzz_save_db.c`), the player, inventory and
  settings decoders (`tests/fuzz_files.c`), and what multiplayer peers
  send (`tests/fuzz_net.c`).
- **Safe file access.** Settings, pipeline cache and imported world files
  are opened with `O_NOFOLLOW`, must be regular files (so no FIFOs or devices), and are
  read up to a size cap. The world database is refused if `world.db` or
  its `-journal`, `-wal` or `-shm` file is not a regular file, since SQLite
  would write through a planted link. Writes go to a temporary file first,
  are synced, and are renamed into place, with the folder synced after, so
  a crash never leaves a half-written save.
- **Bounded work.** Every file is read whole into a buffer with a fixed
  maximum size, and a `world.db` column row larger than that size is refused
  (`save_db.c`). Decoding can never produce more data than a column holds.
- **Checked arithmetic.** Array allocations go through
  `mem_array_size()`, which aborts on overflow. Allocation failure aborts
  cleanly instead of returning NULL.
- **Multiplayer encryption** (`netcrypto.c`, libsodium). Each player
  has an X25519 identity key (`identity.key`, mode 0600). Every
  connection starts with both sides' identity and a fresh ephemeral key;
  the session keys hash three X25519 results (ephemeral with ephemeral,
  each identity with the other's ephemeral key) with the transcript, so
  they are new per connection (forward secrecy) and only the two identity
  holders can derive them. Every packet after that is sealed with
  ChaCha20-Poly1305 (IETF), one key per direction, the channel and a
  counter as nonce and associated data, and a replay window per channel;
  a packet that does not authenticate is dropped before it is read.
- **Who is who.** The host's key is pinned per address on the first join
  (`known_hosts`); a different key later stops the join. The host tells
  each player the others' keys; a direct link between two players is
  accepted only with the key the host announced, and the host writes the
  sender of relayed messages itself. Chat is end-to-end: each pair of
  players has its own session, so a relaying host cannot read or forge
  it. Chat text must be valid UTF-8 without control characters, and each
  sender is rate-limited.
- **Bounded peers.** A joiner's queued inputs, column requests (per
  frame, and only near its player), inventory slot and recipe indices are
  checked before use; block edits, entity counts, positions and the body
  snapshot from a host are range-checked before they touch the game.
- **Compiler and tool gates.** CI builds with gcc and clang using
  `-Werror` and strict conversion warnings, runs the unit tests under
  AddressSanitizer and UndefinedBehaviorSanitizer, and runs clang-tidy
  and cppcheck (`tools/lint.sh`).

## Supply chain

The build downloads pinned releases of mimalloc, GLFW, miniaudio, SQLite
and ENet when the system copies are missing or too old
(`cmake/Dependencies.cmake`). SQLite and ENet archives are checked
against a SHA-256 in the build files; for the others the version is
pinned, but the archive hash is only checked if you set it. libsodium is
never downloaded: it comes from the system, like Vulkan. For a
reproducible or packaged build, either:

- build with `-DMC_DEPS=SYSTEM` against your distribution's packages; or
- set `MC_MIMALLOC_SHA256`, `MC_GLFW_SHA256` and `MC_MINIAUDIO_SHA256` to
  the archives' SHA-256 so a changed download fails the build.

Shaders are compiled from `shaders/` at build time and embedded in the
binary. Textures and sounds are generated at start-up, so no image or
audio files are parsed at run time.
