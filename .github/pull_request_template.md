<!--
Title: a Conventional Commits header, e.g. `fix(physics): stop water duplicating at column edges`.
Branch: `<type>/<short-description>`. See AGENTS.md (Git).
-->

## What and why

<!-- The problem and the approach, not a file list. Breaking changes first. Link the issue with `Closes #N`. -->

## How it was checked

<!-- Commands you ran and their results, tests added or changed, screenshots for anything visible. -->

## Checklist

- [ ] `tools/lint.sh` passes (gcc and clang with `-Werror`, unit tests, clang-tidy, cppcheck, shaders, format)
- [ ] A test covers the change (bug fixes: one that fails without the fix)
- [ ] Renderer changes run with `--validate` without `vulkan:` warnings, also at `MC_VK_API=1.0`
- [ ] Docs updated where behaviour changed (README controls/options/settings, the controls screen in `menu.c`, `usage()`); see AGENTS.md
