# TODO

Larger work that has been considered and deliberately deferred, with
what would make it worth doing.

## Roadmap (2026-10-14)

One branch and pull request per item, in this order:

1. **World generation** (#29, `feat/worldgen-caves-ores`): taller mountains,
   caves, coal/copper/iron ore, a furnace and smelting. Old worlds keep
   the generator they were made with.
2. **Tools, weapons and mobs** (`feat/tools-weapons-mobs`, stacked on
   #29): tool tiers, wear (INV2), knife and sword, a rifle with
   bullets and cartridges, boars and wolves. Left for later: creatures
   are not saved (they respawn around players); no visible bullet
   tracer or muzzle flash; no armour; a client's blows and shots are
   judged on the host, so they land one round trip late; more creatures
   (deer, bears, fish).
3. **Inventory screen redesign** (`feat/inventory-redesign`, stacked on
   item 2): recipes in collapsible categories with made/total counts, a
   "can make" filter, a detail card (uses, speed, blow, food, block
   density and span, ingredients against what you have) and a short
   cursor tooltip. Left for later: search by name, favourites, dragging
   a stack across slots to spread it.
4. **Health** (`feat/health-interactive`, stacked on item 3): done: the
   adrenaline rush on a sudden injury (analgesia, strength, tunnel
   vision) and the crash after it, an adrenaline auto-injector (one in
   the starting kit), a manual shock override, painkiller overdose
   (delayed liver damage, vomiting), antibiotics with nothing to treat
   (gut upset), organ damage felt in play (heart caps exertion, liver
   tires and jaundices, kidneys raise potassium, brain makes the view
   drift), hand tremor and a clickable panel with treatment buttons.
   **Not done** (stopped at the token budget, 2026-10-14):
   - drug interactions (painkiller on top of alcohol or opioids; none
     exist yet), opioids and their respiratory depression;
   - fainting from a vasovagal reflex (pain, blood at the sight) as its
     own event (unconsciousness from low brain oxygen already exists);
   - adrenaline from fear (a wolf hunting nearby), not only from pain;
   - a blur or double-vision effect for concussion (needs a renderer
     post-process; the view sway is the stand-in);
   - the panel by mouse only as far as parts and treatments go: the
     organ list and supplies are not clickable, and there is no drag of
     an item from the inventory onto a body part.
5. **Finish multiplayer** (**not started**): the items under Multiplayer
   below, NAT traversal by UDP hole punching through a rendezvous
   server included. Players chose: client-server mode, a headless
   `--server`, host/join screens in the menus, saving joined players
   with the world, lifting the host view-distance limit, column
   re-requests, `fuzz_net` on host handlers, chat safety numbers.

## Native Metal renderer (macOS)

**Status:** deferred. macOS runs the Vulkan renderer through MoltenVK
(see "Building" in the README).

**Why not now.** A Metal backend would be a second renderer next to
`renderer.c` (about 2600 lines), and Metal has no C API, so it means
Objective-C in a C11 project. The shaders would need Metal Shading
Language versions too (SPIRV-Cross, or by hand), and every renderer
feature would have to be built twice, without `gpucaps` and its
`MC_VK_API`/`MC_VK_DISABLE` testing. Nobody working on the project has a
Mac, so none of it could be run or checked by eye. The renderer is
already a good fit for MoltenVK: Vulkan 1.0 at its core, optional
features negotiated, 1-pixel lines only, no `discard`, no geometry
shaders, and a 32-bit float depth buffer, which Apple GPUs have.

**Do it when** all of these hold:

- someone can run the game on an Apple Silicon Mac, and
- it works there through MoltenVK, and
- they have measured a problem MoltenVK causes (frame time, memory,
  stutter from its shader conversion, or a missing feature) that a
  native backend would fix.

**Before starting**, record the F3 frame times (`--debug`) on that Mac,
with MoltenVK's warnings on (`MVK_CONFIG_LOG_LEVEL=2`), so the native
renderer has a baseline to beat.

## Multiplayer

**Status:** peer-to-peer mode merged (#28). Decisions (2026-10-14):
peer-to-peer first, ENet over UDP, every packet sealed with
ChaCha20-Poly1305 (libsodium), chat end-to-end, simulation fully
authoritative on one peer.

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
- [ ] host/join from the menus (a Multiplayer screen with an address
      field); until then only the command line starts a session

**Known limits of the first version** (each also a reason for the
headless server below): the authority's world streams around its own
player, so others must stay within its view distance; remote players'
bodies and inventories are not saved with the world; when the host
leaves, the session ends (no handing the authority over).

**Security follow-ups** from the audit of 2026-10-14 (the rest were
fixed on the branch):

- A host that never answers a column request leaves that column loading
  on the client for good; re-request after a timeout.
- `tests/fuzz_net.c` covers what a host sends, not what a joiner sends
  (inputs, operations, column requests): give it an offline host world.
- Safety numbers for chat: today a player compares key fingerprints from
  the join notices by hand.

**Later** (deferred, in this order):

- *Client-server mode*: the same authority code with the mesh switched
  off: clients connect only to the server, and chat is relayed as
  ciphertext sealed to each recipient's key so it stays end-to-end.
- *Headless server* (`--server`): runs the authority without a window,
  GPU or audio. Needs world streaming around every player instead of one
  camera, and a console for chat and admin.
- *NAT traversal*: peers behind home routers need a forwarded port or a
  rendezvous server for UDP hole punching. Only direct addresses (LAN,
  forwarded port, VPN) work until then.
