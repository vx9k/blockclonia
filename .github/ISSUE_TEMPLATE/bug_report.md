---
name: Bug report
about: Something in the game or the build doesn't work as it should
title: ''
labels: bug
assignees: ''

---

<!-- Security problems (a world folder or settings file that crashes the
game, reads or writes files it shouldn't, ...) go through SECURITY.md,
not here. -->

**What happened**
What you saw, and what you expected instead.

**How to reproduce**
1. Command line used (for example `blockclonia --seed 1337 --radius 8`)
2. What you did in game
3. What went wrong

If it happens in a particular place, the world seed and your position
(both shown in the F3 overlay) help a lot. A world folder that shows the
problem is even better.

**System**
- OS and version:
- GPU and driver (F3 shows the GPU; `vulkaninfo --summary` shows the driver version):
- Vulkan version and renderer features (the `Vulkan 1.x: ...` line in F3):
- Audio device (F3), if the bug is about sound:
- blockclonia commit (`git rev-parse --short HEAD`) or package version:
- Built with (preset or CMake options, compiler):

**Log**
<!-- Paste the terminal output. For rendering bugs, run with --validate and
include the lines that start with [warn] vulkan: or [error]. -->

```
```

**Screenshots**
F2 saves `screenshot.ppm` in the working directory.
