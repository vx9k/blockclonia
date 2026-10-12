# TODO

Larger work that has been considered and deliberately deferred, with
what would make it worth doing.

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
