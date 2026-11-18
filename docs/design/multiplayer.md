# Multiplayer

How the peer-to-peer and client-server modes work, and what is left.

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

**Status:** peer-to-peer mode merged (#28). Decisions (2026-10-14):
peer-to-peer first, ENet over UDP, every packet sealed with
ChaCha20-Poly1305 (libsodium), chat end-to-end, simulation fully
authoritative on one peer.

**Finished last** (decided 2026-11-18). The remaining multiplayer work
waits until every other area in TODO.md is built. Every new system
(items, machines, vehicles, animals) changes what the authority
simulates and sends; syncing each one while it still changes means
syncing it twice. Doing it once, over a finished game, gives players a
stable, seamless and synchronized game instead of one that drifts out
of step as features land.

**Shape of the peer-to-peer mode.**

- Every peer holds one ENet host and a direct connection to every other
  peer (a full mesh). Joining one peer is enough: it answers with the
  addresses of the others, and the newcomer dials them.
- The peer that created the session is the *authority*. It simulates
  the world (physics, water, collapse, heat) and every player's body and
  health from the inputs the others send, and saves the world. The
  others send inputs, predict their own movement and reconcile against
  the authority's snapshots.
- Terrain comes from the authority in the save format, then edits as
  they happen. (Generating it from the seed on every peer was dropped:
  float contraction differs between compilers and CPUs, so terrain could
  differ at noise thresholds.)
- Crypto per connection: static X25519 identity keys (TOFU-pinned like
  SSH's known_hosts), an ephemeral `crypto_kx` exchange for session keys,
  then `crypto_aead_chacha20poly1305_ietf` on every payload with a
  counter nonce and a replay window. ENet's own headers (acks, peer ids)
  stay in the clear: an on-path attacker can drop or delay traffic but
  cannot read or forge it.
- Chat goes straight to each recipient over that pair's session, never
  through the authority, so it is end-to-end by construction.

**Steps** (tick as they land):

- [x] libsodium and ENet in `cmake/Dependencies.cmake`, CI, BUILDING.md
- [x] `netcrypto.c`: keys, handshake, AEAD seal/open, replay window, tests
- [x] message codecs (`wire.c`, `mp.c`), validated decoders, fuzz target
- [x] `net.c`: ENet mesh, handshake, peers joining and leaving
- [x] physics and health for more than one player
- [x] authority loop: inputs in, snapshots and edits out; client
      prediction and reconciliation (`mp.c`)
- [x] remote players drawn (entity list), chat UI (T), `--host`/`--join`,
      README and controls
- [x] host/join from the menus: a Multiplayer screen (name, address,
      how players connect, host a world) and "Open to other players" in
      the pause menu. The name and address are not remembered between
      runs yet (settings hold numbers only).

**Known limits** (also reasons for the headless server below): when the
host leaves, the session ends (no handing the authority over); health is
saved for nobody yet. Done: the authority's world loads around every
player (anchors, `world_set_anchors`), so players roam freely; joined
players' place and inventory are saved with the world by identity key
(`mp_save_players`).

**Security follow-ups** from the audit of 2026-10-14 (the rest were
fixed on the branch):

- [x] A host that never answers a column request left that column
  loading on the client for good: the client now asks again every few
  seconds (`world_retry_requests`).
- [x] `tests/fuzz_net.c` now covers what joiners send too (inputs,
  operations, column requests), through an offline host
  (`net_offline_host`) that steps its simulation after every message.
- [x] Safety numbers for chat: `/safety [name]` shows a 30-digit number
  from both identity keys to read to each other.

**Later** (deferred, in this order):

- [x] *Client-server mode* (`--host --client-server`): the host hands
  out no addresses, so players connect to it only, and chat is relayed
  as ciphertext sealed to each recipient's key.
- [x] *NAT traversal*: the host's router forwards the game's port by
  UPnP (miniupnpc), so friends join by address alone; where UPnP is off,
  STUN (stun.l.google.com) tells each game its public address, the
  joiner redials for a minute, and the host's `/punch ADDR:PORT` opens
  its router to the joiner. Left: NAT-PMP/PCP for routers that do those
  but not UPnP, a rendezvous server so nobody swaps addresses by hand
  when UPnP is off, the joiner's address on screen while it waits (only
  the log has it), and a relay for symmetric NATs.
- *Headless server* (`--server`): runs the authority without a window,
  GPU or audio, with a console for chat and admin. Started on
  `feat/headless-server` (local work, 2026-10-14): `mp_set_dedicated` (the server's stand-in
  player is never seen or hit), `mp_kick`/`net_kick`,
  `worldgen_find_spawn` shared with `main.c`. Still to write: `server.c`
  (open/tick/close, a stdin console with /players, /kick, /save, /stop),
  a `blockclonia-server` executable on the core library, `--server` in
  `main.c`, and a test driving a server and a client in one thread.

## Until then

New systems are written so the final multiplayer pass is sync work,
not a rewrite:

- **The authority decides.** Game state changes only in the
  deterministic step (fixed `PHYS_DT`, seeded randomness), driven by
  inputs, never by the frame, the clock or the renderer. A client can
  then predict it and the host can replay it.
- **State is plain data with a checked codec.** Anything another player
  must see has a fixed-size, versionable encoding with a validating
  decoder (as `save.c` does), so a wire message is a small step from
  the save format and the fuzz targets cover it.
- **Effects are derived, not sent.** Light, particles, sound and
  animation follow from synced state (as burning creatures and held
  torches do through `mp.c`'s flags and held item), so they cost no
  extra messages.
- **Players are indices, not "the" player.** Code takes a player index
  or a list of bodies (`combat_targets`, `mob_sense`), never a single
  global player.

What the final pass then adds, in this order:

1. A protocol version and feature flags in the handshake, so a
   mismatched game is refused with a clear message; every later message
   change bumps it.
2. The headless server (above).
3. Sync for every system built by then.
4. Interest management and a bandwidth budget per player.
5. Lag compensation for shots, reconnects, timeouts and bans.
6. A test running a host and several clients in one process, beside
   `fuzz_net.c`.
