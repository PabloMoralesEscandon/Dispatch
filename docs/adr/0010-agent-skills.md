# 0010: Agent skills for planning, working and the board

- **Status:** Accepted
- **Date:** 2026-11-25

## Context

Agents learn Dispatch from the `AGENTS.md` / `CLAUDE.md` that `dispatch init`
scaffolds into the workflow directory: about 450 lines each, near-identical,
and only seen by a session that starts in that directory. A session started in
the product repository (one level down) or anywhere else plans and acts
without them. In practice an agent created a catch-all group, one task per
issue, and recorded its own tasks as `--actor user` (GitHub issue #21).

The [Agent Skills](https://agentskills.io) format lets a runner load
instructions by job, wherever the session starts and only when needed: a
folder holding a `SKILL.md` whose YAML front matter carries a `name` and a
`description` the runner matches against the request, plus optional reference
files.

## Decision

**Three skills, split by job, in `skills/`.**

- `dispatch-plan`: How Dispatch Works and Planning Work. Reuse groups by
  recorded scope, never a catch-all group, granular tasks with dependencies
  and review gates, titles without IDs, issue links (`--issue`,
  `--from-issue`), tasks left `proposed`, a worked example.
- `dispatch-work`: Instruction Visibility, Session Startup, Selecting Work,
  Task Execution Protocol, Commit Discipline, Workspaces And Parallel Agent
  Safety (including `workspace adopt` for worktrees made by tools such as
  Orca), Agent Identity And Workspace Setup, Running A Task Sequence,
  Executing A User-Named Task, Failure Handling.
- `dispatch-board`: the person's own session. Reading the board (`status`,
  `list`, `ready`, `reviews`, `proposed`, `blocked`, `log`), readying and
  reviewing on the user's instruction (including `review --close-issue` and
  the readiness rules from Selecting Work), `status --check-issues`, `doctor`,
  `normalize`, `workspace prune`, `unassign`, `task priority`, and board setup.
  Its `references/commands.md` holds the Core Commands list, updated to the
  current CLI.

Every template section lands in exactly one skill; the Critical Rules and
"how to find the workflow directory" are the only text every skill repeats,
because each skill must stand alone. Descriptions name the job ("plan / break
down / organise work into Dispatch tasks", "start / continue / finish a
Dispatch task", "check / review / tidy the Dispatch board") so runners pick
the right one.

**Actor names.** Every change that accepts `--actor` passes one, and the
actor is whoever made the decision. The skills keep the template's rule:
readying and accepting tasks are the user's decisions, so when an agent runs
`ready`, `group ready` or `review` because the user explicitly told it to, it
records `--actor user` (or the person's own name if they give one). An agent
records its own name only for its own actions: planning (`task add`, `task
edit`, `task move`), working (`workspace create`/`adopt`, `start`, `finish`,
`commit add`), tidying it initiates (`unassign`, `task priority`), and
`review` when the user explicitly assigned it as the reviewer. It never
records its own work as `user`. The history therefore shows that an agent
created a plan and that the user approved it; the README's lifecycle and
group sections say the same. `group add`, `group edit` and `dep add/remove` accept no
`--actor` and are logged as `user`; that is a product gap tracked separately,
and the plan skill says so rather than showing commands that would fail.

**Checks.** `tests/check_skills.sh` runs in CI (its own step, and from
`tests/run_cli_tests.sh`, which also proves it rejects broken copies):

- Front matter: each expected folder has a `SKILL.md` starting with a front
  matter block, a non-empty `name` equal to the folder name and in the Agent
  Skills name format, and a non-empty `description` of at most 1024
  characters.
- Drift: every `dispatch <command> [<subcommand>]` in code (fenced blocks and
  inline spans) in any skill Markdown file must be a command, and for command
  families a subcommand, that `dispatch --help` lists ("Workflow commands"
  and "Implemented now"). Every top-level command in `dispatch --help` must be
  mentioned by at least one skill, except those in a commented exclusion list
  (empty today). The check reads the freshly built binary, never one on
  `PATH`.

## Consequences

- Agents can load the right instructions from any directory once the skills
  are installed in their runner.
- Adding a top-level command or subcommand without documenting it in a skill,
  or renaming one the skills use, fails CI.
- Options are not checked; a renamed flag can still drift.
- The templates `AGENTS.md` / `CLAUDE.md` are unchanged for now, so their
  content currently exists twice in the repository.

### Follow-ups

- Shipping the skills in the binary and a `dispatch skills` install command
  (issue #22; done in [ADR 0011](0011-skills-install.md)).
- Slimming `AGENTS.md` / `CLAUDE.md` to point at the skills (issue #23).
