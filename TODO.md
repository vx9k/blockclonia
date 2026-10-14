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

## Android

**Status:** not started. Nothing in the tree builds for Android yet; the
Linux and macOS builds are the only ones tested.

**Goal.** A signed APK that runs the same game as the desktop build:
the Vulkan renderer, physics, the body model, saves and multiplayer,
with touch controls in place of the keyboard and mouse.

**Requirements.**
- Android NDK (r27 or newer) and the Android SDK, with the CMake and
  Ninja packages from the SDK manager. Target API 26 or newer for
  AAudio, so the audio path matches what miniaudio uses on desktop.
- A device with a Vulkan 1.1 driver. Vulkan 1.0 phones exist, and the
  renderer already has a 1.0 fallback (`MC_VK_API=1.0`), but test it
  on one before claiming it works.
- Gradle with the Android Gradle Plugin for the package, plus a signing
  key kept outside the repository.
- A device or emulator for testing. The emulator's Vulkan goes through
  SwiftShader or the host GPU, so it does not prove a phone's driver
  behaves the same.

**Caveats found in the code, and the way through each.**

1. **Window and input come from GLFW.** GLFW has no Android backend.
   GLFW is used only in `main.c` (window, keys, mouse, text) and in
   `renderer.c` (the Vulkan surface, through
   `glfwGetRequiredInstanceExtensions`). Solution: put a small platform
   layer behind those calls, with a desktop implementation (GLFW) and an
   Android one (`android_native_app_glue`, `ANativeWindow`,
   `VK_KHR_android_surface`, `AInputEvent`). Keep `main.c` and
   `renderer.c` talking to the layer, not to GLFW. The headless modules
   already build without a window, so this is the only seam.

2. **No keyboard, mouse or cursor capture.** `main.c` maps keys to
   movement and the UI, and captures the mouse for look. Solution: touch
   controls: a left stick for walking, a drag area for looking, buttons
   for jump, sprint, break and place, a hotbar and an inventory toggle.
   The controls screen and the README's table need an Android column.
   Hardware keyboards and mice, where attached, should keep working.

3. **Text entry.** The chat, the world name and the multiplayer address
   use typed characters. Solution: the Android soft keyboard through
   `InputMethodManager`, with the same ASCII and UTF-8 checks the
   desktop path already applies (`menu.c` and `net_chat_valid`).

4. **File paths.** `paths.c` builds locations from `$XDG_CONFIG_HOME`,
   `$XDG_DATA_HOME` and `$HOME`, and falls back to the working directory.
   Android has none of these. Solution: a platform path hook that returns
   the app's private files directory (`getFilesDir()` through the
   activity), so settings, worlds and the pipeline cache all live there
   and survive updates. The "copy from the old location" logic in
   `main.c` stays desktop-only.

5. **Discord status.** `discord.c` reads environment variables and loads
   the desktop client's IPC. Solution: compile it out on Android with
   `MC_DISCORD_APP_ID` empty, which already disables it on builds
   without an application.

6. **UPnP and the network.** miniupnpc builds, but a phone on mobile
   data sits behind carrier NAT, which UPnP cannot open, and Wi-Fi
   routers often run it off. Solution: keep the code, let it report
   "no router answered", and rely on hole punching (`/punch`) and the
   LAN. Android also needs the `INTERNET` permission. Traffic is UDP
   and already sealed, so no cleartext-policy change is needed. Network
   use stops when the app is paused; a joined game
   should pause and resume rather than drop, which the session code
   does not do yet.

7. **Audio.** miniaudio supports AAudio and OpenSL ES on Android, and
   `audio_device.c` already calls it through one entry point, so this
   should need only the build flags. Solution: test latency on a real
   device, and drop the audio thread's priority only where the platform
   allows it (`os_thread_lower_priority` is Linux-only today).

8. **Lifecycle.** Android pauses and destroys the activity at any time.
   The game saves on close and autosaves every minute, but nothing saves
   on pause. Solution: on `onPause`, flush the world and the player
   (`world_save_all` and the player record) and pause the simulation;
   on `onResume`, rebuild the swapchain (`renderer_on_resize` already
   handles a changed surface size).

9. **Memory.** Phones have 4 to 12 GB shared with the rest of the
   system, and Android kills background apps under pressure. `sysinfo.c`
   reads RAM and cache sizes from `sysctl` on macOS and from `/proc`
   elsewhere, so detection should work, but the budgets in `sys_plan`
   were tuned for desktops and the Pi. Solution: a conservative plan for
   the `android` target, then measure the view distance a mid-range
   phone holds at 60 fps.

10. **The debug overlay and screenshots.** `--screenshot` and `--validate`
    are desktop tools, and the validation layers ship only with the SDK.
    Solution: keep the F3 overlay on Android, and test the renderer with
    Android GPU Inspector or the device's own profiler instead.

11. **Dependencies on Android.** ENet, SQLite, mimalloc and libsodium
    all build for Android, and `MC_DEPS=FETCH` already fetches the
    pinned sources. libsodium needs its Android build (`libsodium` from
    the NDK-friendly release, or `ndk-build`), and that is the one that
    needs a new CMake toolchain path. Solution: a `cmake/android.cmake`
    toolchain file and an `android` preset, with the same warnings
    settings as the desktop presets.

12. **Tests and CI.** The unit tests run on the host, and CI has no
    Android job. Solution: first, a cross build of the core library and
    `mc_tests` for `arm64-v8a` run under `qemu` (the Pi jobs already do
    this for ARM), then an APK build job, and an emulator run of the
    render smoke test with SwiftShader.

**Steps** (tick as they land):

- [ ] Toolchain file and an `android` CMake preset; the core library and
      `mc_tests` build for `arm64-v8a` and run under qemu
- [ ] Platform layer behind `main.c`/`renderer.c` (window, surface, input,
      text), GLFW kept for desktop
- [ ] Touch controls and the controls screen's Android column
- [ ] File paths from the app's private directory; no desktop-only
      code paths on Android
- [ ] Pause and resume: save on pause, rebuild the surface on resume
- [ ] APK with `AAudio` sound, signed in CI from a secret key
- [ ] Runs on one real Vulkan 1.1 phone and one Vulkan 1.0 device, with
      the F3 numbers in the README

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
- *Headless server* (`--server`): runs the authority without a window,
  GPU or audio. Needs world streaming around every player instead of one
  camera, and a console for chat and admin.
- [x] *NAT traversal*: the host's router forwards the game's port by
  UPnP (miniupnpc), so friends join by address alone; where UPnP is off,
  STUN (stun.l.google.com) tells each game its public address, the
  joiner redials for a minute, and the host's `/punch ADDR:PORT` opens
  its router to the joiner. Left: NAT-PMP/PCP for routers that do those
  but not UPnP, a rendezvous server so nobody swaps addresses by hand
  when UPnP is off, the joiner's address on screen while it waits (only
  the log has it), and a relay for symmetric NATs.
- *Headless server* (`--server`): started on `feat/headless-server`
  (local work, 2026-10-14): `mp_set_dedicated` (the server's stand-in
  player is never seen or hit), `mp_kick`/`net_kick`,
  `worldgen_find_spawn` shared with `main.c`. Still to write: `server.c`
  (open/tick/close, a stdin console with /players, /kick, /save, /stop),
  a `blockclonia-server` executable on the core library, `--server` in
  `main.c`, and a test driving a server and a client in one thread.
