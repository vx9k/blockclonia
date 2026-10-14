# Subagents

Claude Code subagents for this project, in four tiers by how much
reasoning a task needs. Each `.md` file here is one agent: YAML
frontmatter (name, description, model, effort, tools) and a system prompt.
This README has no frontmatter, so Claude Code does not load it as an
agent. (These are not the same thing as `AGENTS.md` at the repository
root, which holds the project's coding rules. Every agent here sees those
rules through `CLAUDE.md`.)

| Agent | Model | Effort | Tools | Use it for |
|---|---|---|---|---|
| `scout` | `claude-haiku-5-5` (Haiku 5.5, pinned) | `low` | Read, Bash | Read-only lookups: where something is defined or used, file summaries, callers, the real error in build or validation output |
| `docs-writer` | `claude-haiku-5-5` (Haiku 5.5, pinned) | `medium` | Read, Edit, Write, Bash | Prose only: README and docs pages, comments, changelogs, templates, formatting |
| `general-dev` | `sonnet` (Sonnet) | `medium` | Read, Edit, Write, Bash | Everyday features, contained refactors, reproducible bugs, tests, code review |
| `deep-reasoning` | `opus` (Opus 5.5) | `high` | Read, Edit, Write, Bash | Architecture, hard cross-module bugs, wide blast radius, security-sensitive changes, Vulkan synchronization, mem.h, chunk meshing, rendering pipeline design |

## What sends a task to each tier

Claude chooses an agent by matching the task against each agent's
`description`, so the descriptions are written as routing rules:

- **scout**: the task is a lookup that changes nothing: where a symbol is
  defined or used, what calls a function, a summary of a file, or the
  real error in build, test or validation output. It never edits and
  never makes a design decision; it reports what it found.
- **docs-writer**: the task is writing or editing text, and the facts it
  needs are already in the code or the request. It edits only comments
  in code files, and stops if a real code change turns out to be needed.
- **general-dev**: the change is understood and its effects stay within
  one or a few modules: a feature, a refactor, a bug with a reproduction,
  a review. It hands off to deep-reasoning when a task turns out bigger.
- **deep-reasoning**: a mistake would be expensive. That covers design
  decisions, bugs that cross modules or won't reproduce, races and GPU
  synchronization, shared formats and headers, chunk meshing and
  rendering pipeline design, and anything that parses untrusted files or
  touches the file system or dependency downloads.

Delegation is up to the main session. It uses subagents when it judges
that delegating helps, or when you ask it to.

## Running a specific agent yourself

- **Ask for it**: "Use the deep-reasoning agent to find why water
  duplicates at column edges."
- **@-mention it** in your prompt: `@agent-general-dev add a --fov option`.
- **Run a whole session as the agent**: `claude --agent docs-writer`.
- **From a tool call** (what Claude does when it delegates): the `Agent`
  tool with `subagent_type` set to the agent's name:

  ```json
  {
    "subagent_type": "deep-reasoning",
    "description": "Audit save decoder bounds",
    "prompt": "Review src/save.c's column decoder for out-of-bounds reads on malformed files..."
  }
  ```

  The call can also pass `model` (`haiku`, `sonnet`, `opus` or `fable`)
  to override the agent's model for that one run.

To change an agent, edit its file here. Claude Code 2.1.283 no longer has
an interactive `/agents` editor. If a new or edited agent doesn't show up,
start a new session.

## How the reasoning depth is set

The `effort` field in the frontmatter controls how much each tier
reasons. It accepts `low`, `medium`, `high`, `xhigh`, `max` or an integer.
deep-reasoning runs Opus at `high` rather than `max`, which Claude Code
documents as "use sparingly for the hardest tasks". general-dev runs
Sonnet at `medium`, and scout runs Haiku 5.5 at `low`, since a lookup
needs the facts found, not a long chain of reasoning. docs-writer runs
Haiku 5.5 at `medium`: documentation needs the facts read carefully
from the code, but not the depth of the engineering tiers. scout and
docs-writer are pinned to the full model ID so they stay on Haiku 5.5
when newer models ship.
Don't pass a `model` when spawning it: the call's `model` overrides the
frontmatter.

The system prompts also contain the phrases "think hard" (general-dev)
and "ultrathink" (deep-reasoning), but in Claude Code 2.1.283 they only
work as ordinary instructions:

- `ultrathink` is only detected in prompt text you type, never in an
  agent's system prompt. Even then it doesn't change the effort level: it
  adds a note asking for deeper reasoning on that turn.
- "think" and "think hard" have no special meaning in this version.

To make a tier reason more or less, change its `effort:` line, not the
wording.

## Notes

- `model` accepts `haiku`, `sonnet`, `opus`, `fable`, `inherit` or a full
  model ID. deep-reasoning uses the `opus` alias and general-dev uses
  `sonnet`; each resolves to the newest model in its family (Opus 5.5 at
  the time of writing), so a new release needs no edit here. scout and
  docs-writer pin `claude-haiku-5-5`. Pin a full model ID instead if a
  tier must stay on one version. If your organisation restricts models,
  a disallowed model falls back to the session's model.
- The advisor is off. `.claude/settings.json` has no `advisorModel`, and
  Claude Code treats an unset key as off. Caveat: running
  `/advisor <model>` saves the choice to the user settings, which turns
  it back on.
- This Claude Code build has no Grep or Glob tools, so every tier gets
  Bash for searching (`grep` here is ugrep). docs-writer's prompt limits
  it to read-only commands.
- No tier includes the `Agent` tool, so subagents don't spawn subagents
  of their own.
- These files are checked into git so everyone working on the project
  gets the same agents. Personal agents go in `~/.claude/agents/`.
