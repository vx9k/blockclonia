## What and why

<!-- What this changes, and the problem it solves. Link the issue if there is one. -->

## How it was checked

<!-- Tests added or changed, what you ran in game, screenshots for anything visible. -->

## Checklist

- [ ] `tools/lint.sh` passes (gcc and clang with `-Werror`, unit tests, clang-tidy, cppcheck, shaders, format)
- [ ] A test covers the change (bug fixes: one that fails without the fix)
- [ ] Renderer changes run with `--validate` without `vulkan:` warnings, also at `MC_VK_API=1.0`
- [ ] Docs updated where behaviour changed (README controls/options/settings, the controls screen in `menu.c`, `usage()`); see AGENTS.md
