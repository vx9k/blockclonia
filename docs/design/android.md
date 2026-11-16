# Android

What an Android build needs, and the way through each problem found in the code.

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail.

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
