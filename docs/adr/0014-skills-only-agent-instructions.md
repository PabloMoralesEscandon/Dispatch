# 0014: Drop AGENTS.md and CLAUDE.md: make the skills the only agent instructions

- **Status:** Accepted
- **Date:** 2026-11-27
- **Supersedes:** [0012](0012-slim-templates.md)

## Context

After ADRs 0010–0012, the workflow directory's `AGENTS.md` and `CLAUDE.md`
templates were slimmed down to a short summary of what the three Agent Skills
(`dispatch-plan`, `dispatch-work`, `dispatch-board`) already say: the Critical
Rules, how to find the workflow directory, and which skill handles which job.
Keeping those files on boards costs two things without adding anything the
skills lack (GitHub issue #31):

1. **They are not seen where agents work.** Runners load `AGENTS.md` /
   `CLAUDE.md` from the session's working directory and its ancestors. Agents
   work in task worktrees that commonly live outside the workflow directory
   (Orca places them in `~/orca/workspaces/<repo>/<name>`, and
   `workspace create --dir` can place them anywhere). Sessions started there
   load neither file; skills installed with `--global` are what reach every
   session.
2. **They are a second copy to keep current.** `doctor` already needed
   `instructions_outdated` for the pre-ADR-0012 long text, every rule change
   had to update both the skills and the templates, and older boards kept stale
   copies because `doctor` rightly never rewrites them.

## Decision

**`dispatch init` stops writing `AGENTS.md` and `CLAUDE.md`.** A fresh
`dispatch init` creates `dispatch.json`, creates `.dispatch/agents`, installs
the bundled agent skills in project scope for Claude Code, Codex and opencode,
and prints a one-line hint to run `dispatch skills install --global` for
sessions started outside the workflow directory. A board with no instruction
files is now the normal case. `nob.c` no longer embeds `AGENTS.md` or
`CLAUDE.md` into the binary — only `skills/` is embedded (in
`build/dispatch_skills_bundle.c`). This repository keeps its own top-level
`AGENTS.md` and `CLAUDE.md` as development rules for working on Dispatch
itself.

**`dispatch doctor` drops the missing-file warning, notes removable templates,
and warns on project-only skills with outside workspaces.**

- `AGENTS.md missing in workflow directory` (`agents_md_missing`) and
  `AGENTS.md found in workflow directory` are removed; the per-skill
  `skills_missing` checks take their place.
- `instructions_outdated` is replaced by an informational note (`[info]` in
  text output; `status: "info"`, `kind: "instructions_redundant"` in
  `doctor --json`, without incrementing `summary.warnings`). It fires only when
  an existing `AGENTS.md` or `CLAUDE.md` matches only a Dispatch template:
  either the exact ADR 0012 slim template (matched by SHA-256 after LF
  normalization, since only one slim template was ever scaffolded) or any
  historical long template scaffolded before ADR 0012 (matched structurally by
  its `# Dispatch Agent Instructions` title, known Dispatch `##` headings
  including `## Critical Rules`, `## Task Execution Protocol` and
  `## Failure Handling`, absence of other `#`/`###` headings, and its closing
  reviewer sentence). Any file with custom content or local additions to a
  template is left alone silently, and `doctor` never deletes or rewrites
  instruction files.
- `skills_project_only` (`status: "warn"`, `subject: null`, fix
  `dispatch skills install --global`) fires once when at least one bundled skill
  is installed only in project scope (no global copy for any runner) while at
  least one active workspace lives outside the workflow directory. Workspaces
  inside the workflow directory, removed workspaces, and boards with global
  skills installed for at least one runner stay quiet.
- `agent_prompt_outdated` continues to catch prompts predating the skills
  section or embedding `AGENTS.md`, and now also catches ADR 0012 prompts that
  still point at `AGENTS.md` / `CLAUDE.md` so `dispatch normalize` refreshes
  them.

**Agent prompts drop all references to `AGENTS.md` / `CLAUDE.md`.**
`agent create` (and `normalize`) writes `.dispatch/agents/<name>/<name>-PROMPT.md`
with the agent's identity, actor usage, the three skills (`dispatch-work`,
`dispatch-plan`, `dispatch-board`), and the fallbacks (`dispatch skills install`,
`dispatch skills install --global`, `dispatch skills show <name>`).

**Fallback for runners without Agent Skills support: `dispatch skills show
<name>`.** Rather than chasing every runner's proprietary rule file format in
`skills install`, Dispatch adds `dispatch skills show <name> [--json]`, which
prints a bundled skill's `SKILL.md` from the embedded bundle to stdout and
works from any directory without a board. A launcher for a runner without
native skill support (such as Antigravity/Gemini CLI or Cursor) can embed the
output directly into an agent's prompt without checking instruction files into
the repository. `dispatch completion candidates skills` (and the zsh, bash and
fish completion scripts) completes the bundled skill names.

**Standalone skills with wider trigger descriptions.** Each of `dispatch-plan`,
`dispatch-work` and `dispatch-board` is checked to stand alone as the sole
source of instructions:

- every skill carries all seven Critical Rules;
- every skill explains how to locate the workflow directory both from
  `work_dir/repo` (`..`) and from a task worktree outside `work_dir`
  (`git worktree list` or `git rev-parse --git-common-dir`, then one directory
  above the main checkout, verified with `dispatch repo`);
- every skill points to the other two skills and to `dispatch-board`'s
  `references/commands.md`;
- every skill's YAML front-matter `description` states that it applies to any
  command run against a Dispatch board, so the `dispatch.json` and `--actor`
  rules are in force as soon as an agent touches the board.

## Consequences

- One copy of the Dispatch agent instructions: `skills/` embedded in the
  binary.
- Fresh boards contain no `AGENTS.md` or `CLAUDE.md` and pass `dispatch doctor`
  with zero warnings once completions and `dispatch` on `PATH` are in place.
- Boards created before this change keep working without errors: untouched
  Dispatch templates get a non-warning `[info]` note suggesting removal once
  skills are installed, while custom `AGENTS.md` / `CLAUDE.md` files receive no
  noise.
- `doctor --json` adds `status: "info"` (compatible extension to schema version
  1), `instructions_redundant` (`info`), and `skills_project_only` (`warn`);
  `agents_md_missing` and `instructions_outdated` are retired.
- `skills show --json` adds the `skill_not_found` error code.
- Updating `skills/` changes their bundle hashes, so installed copies report
  `outdated` until `dispatch skills update` refreshes them.
- `tests/golden/cli/` now holds portable goldens for `init.txt`,
  `doctor-clean.txt`, `doctor-template-and-outside.txt`, and the updated agent
  prompts; `tests/fixtures/slim-AGENTS.md` preserves the ADR 0012 slim template
  alongside `tests/fixtures/legacy-AGENTS.md` for `doctor` tests.
