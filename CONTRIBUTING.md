# Contributing

Thanks for looking. blockclonia is a small C11 codebase with strict
compiler settings, and changes are easiest to review when they arrive in
the same shape as the code around them.

## Before you start

- **Bugs**: open an issue with the bug template. The F3 overlay shows your
  GPU, Vulkan version and renderer features; include them, plus the
  terminal output.
- **Features**: open an issue first for anything larger than a small fix,
  so we can agree on the approach before you write it.
- **Security problems**: follow [SECURITY.md](SECURITY.md), not the issue
  tracker.

## Making a change

1. Build with `cmake --preset dev && cmake --build --preset dev`
   (requirements are in the [README](README.md#building)).
2. Read [AGENTS.md](AGENTS.md). It has the code rules (allocation,
   units, determinism, headless modules, file formats, the Vulkan 1.0
   baseline) and a list of the places to update when behaviour changes.
   [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains how the pieces
   fit together.
3. Add or update tests in `tests/`. A bug fix comes with a test that
   fails without it.
4. Run `tools/lint.sh`. It builds with gcc and clang using `-Werror`,
   runs the unit tests, clang-tidy and cppcheck, checks the shaders, and
   checks formatting on the lines you changed. CI runs the same script.
5. For anything visible, try it in the game. For renderer changes, run
   with `--validate` and make sure the log shows no `vulkan:` warnings.

## Pull requests

Every change reaches `main` through a pull request. The full rules are
in [AGENTS.md](AGENTS.md) (Git). In short:

- Name your branch `<type>/<short-description>`, e.g.
  `fix/water-column-edges`.
- Keep each pull request to one topic. A refactor and a feature go in
  separate pull requests.
- Give the pull request a Conventional Commits title, e.g.
  `fix(physics): stop water duplicating at column edges`. A squash merge
  uses the title as the commit subject on `main`.
- Fill in every section of the template: what changed and why, how you
  checked it, and the checklist. Link the issue with `Closes #N`.
- Open it as a draft until CI passes and the checklist is done.
- Write commit messages in the
  [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/)
  form, `<type>[optional scope][!]: <description>`, e.g.
  `fix(save): reject columns with negative heights`, then a body that
  says what changed and why. AGENTS.md (Git) lists the types, scopes and
  how to mark breaking changes.
- Don't reformat code you didn't otherwise change.
- Screenshots help for anything visual (F2 saves `screenshot.ppm`).
