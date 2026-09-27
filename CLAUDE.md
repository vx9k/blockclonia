# CLAUDE.md

@AGENTS.md

## Notes for Claude Code

- `AGENTS.md` above holds the project rules; this file only adds what is
  specific to working through Claude Code.
- Build in `build/<preset>` through the presets and keep scratch files
  (edit scripts, screenshots, benchmarks) under `build/`, which git
  ignores. Never leave them in the source tree.
- To see what a change looks like, render a frame instead of guessing:
  `./build/dev/blockclonia --no-save --seed 1337 --play --frames 150 --screenshot build/shot.ppm`
  then convert the PPM (`magick build/shot.ppm build/shot.png`) and read
  the PNG. Add `--screen settings`, `--debug`, `--give ...` or `--hurt ...`
  to reach the screen you changed.
- The game opens a window and an audio device. In a sandbox without a
  display, use `xvfb-run -a` with the lavapipe ICD (see AGENTS.md) and
  `--no-sound`.
- Before a renderer change is finished, run it with `--validate` at the
  default API version and at `MC_VK_API=1.0`, and read the log for
  `[warn] vulkan:` lines. The unit tests cannot see these.
- Prefer the smallest diff that fits the surrounding code. Match its
  comment density and naming, and don't reformat lines you didn't change.
- Work goes through pull requests (AGENTS.md, Git): branch from an
  up-to-date `main` as `<type>/<short-description>`, commit there, push
  the branch and open the pull request with `gh pr create`. Use a
  Conventional Commits title and fill in every section of the template.
  Never push to `main` or force-push a branch under review, and don't
  merge. The maintainer merges.
- End the pull request description with the attribution line Claude Code
  adds, below the checklist.
- Write every commit message in the Conventional Commits 1.0.0 form that
  AGENTS.md (Git) describes: `<type>[optional scope][!]: <description>`.
  The attribution trailer Claude Code adds (`Co-Authored-By: ...`) is a
  footer: put it last, after a blank line following the body, with no
  other text below it. Before committing, check the header against the
  allowed types and the 72-column limit.
