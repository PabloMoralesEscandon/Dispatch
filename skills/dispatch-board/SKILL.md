---
name: dispatch-board
description: Check, review or tidy the Dispatch board. Use when the person asks what is ready, blocked, in review or in progress on their Dispatch board, asks to ready (approve) or review (accept) Dispatch tasks, to close linked GitHub issues, to check board health, or to clean up stuck assignments, stale workspaces, agents or priorities. For the person's own session rather than a worker agent.
---

# Dispatch: read, approve and tidy the board

Dispatch is a command-line workflow board for humans and agents. This skill is
for the person's own session: reporting the state of the board, making the
approval and review decisions they ask for, and keeping the board healthy. To
plan new tasks use the `dispatch-plan` skill; to carry out a task use the
`dispatch-work` skill. The full command list is in
[references/commands.md](references/commands.md).

## Critical Rules

1. Use the Dispatch CLI for all workflow state.
2. Never read `dispatch.json` directly.
3. Never edit `dispatch.json` directly.
4. Never infer task state from `dispatch.json`.
5. Pass an explicit `--actor` on every change that accepts one, so the history
   shows who decided. Readying and accepting tasks are the user's decisions:
   when the user explicitly tells you to run `ready`, `group ready` or
   `review`, record `--actor user` (or the person's own name if they give
   one). Use your own agent name only for your own actions: tidying you
   initiate (`unassign`, `task priority`, `task move`), and `review` when the
   user explicitly assigned you as the reviewer. Never record your own work as
   `user`.

The storage file is an implementation detail, and so is `dispatch.log`. Do not
inspect or edit them with `cat`, `sed`, `jq`, `rg`, editors, scripts, or JSON
parsers. If you need state, ask the CLI (add `--json` when you need to parse
it).

## Find The Workflow Directory

Dispatch state normally lives one directory above the product repository:

```text
work_dir/
  .git/
  dispatch.json
  repo/
```

Run Dispatch from `work_dir`. If the session started inside the product
repository or one of its task worktrees, the board is usually in the parent of
the main checkout: `cd` there and check that `dispatch repo` prints the
repository you expect. If it reports no board, you are in the wrong directory.

Setting up a board is a one-off the person asks for explicitly:
`dispatch init repo` creates `dispatch.json` (plus `AGENTS.md` and
`CLAUDE.md`) for the repository at `repo`, `dispatch repo set <path>` repoints
an existing board after the repository moved, and
`dispatch completion install bash|zsh|fish` installs shell completion.
`dispatch tui` (or plain `dispatch`) opens the interactive board for the
person; it is not for agents.

## Reading The Board

```bash
dispatch status                 # counts, ready and review queues, warnings
dispatch list                   # active tasks by group (list all: include done)
dispatch ready                  # startable tasks, highest priority first
dispatch reviews                # tasks waiting for acceptance
dispatch proposed               # tasks waiting for approval
dispatch blocked                # blocked tasks and their blockers
dispatch show <TASK-ID>         # one task: state, deps, commits, issue, history
dispatch group list             # groups, scopes and task counts
dispatch log --limit 20         # audit trail of recent changes
```

`dispatch log` filters with `--task`, `--actor`, `--agent`, `--workspace`,
`--command` and `--action` (exact matches), and `--limit <n>` keeps the newest
matches; for example `dispatch log --task DE-01` or
`dispatch log --agent codex --command finish`. `dispatch agent list` and
`dispatch workspace list` show who is working where.

When reporting to the person, lead with what needs their decision (reviews,
proposed tasks, warnings), then what is in progress, then what is ready.

Every read command accepts `--json` (`dispatch status --json`,
`dispatch list all --json`, `dispatch log --task DE-01 --json`) when you need
to parse the result. The contract is in `docs/json-output-contract.md` in the
Dispatch repository; ignore unknown fields and match errors on `error.code`.

## Readying Tasks (Approval)

Only the user approves proposed tasks by marking them ready. Agents ready a
task or group only when the user explicitly instructs it, for that specific
task or group. All readiness changes must include an actor so the approval is
visible in task history, and the approval is the user's, so record `user` (or
the person's own name if they give one):

```bash
dispatch ready DE-13 DE-14 --actor user
dispatch group ready DE --actor user
```

Use `dispatch ready <id> --actor user --no-review` only when the user
approves the task as safe to finish without a review gate. Use
`dispatch group ready <group> --actor user --no-review` only when the user
approves every readied task in the group as safe to finish without review.
When used again on an already readied group, it also marks ready or blocked
tasks that have not been started yet as no-review.

Readying a task that is `doing` or `review` is an explicit revert: it clears
the assignment so the task can be started cleanly again. Confirm with the
person before doing that.

`dispatch task priority <TASK-ID> <value> --actor <your-agent-name>` (from `-99` to `99`,
default `0`) moves a task up or down the ready queue without changing
dependencies.

## Reviewing Tasks (Acceptance)

Review is the person's decision. Accept a task only when the person tells you
to, or when they explicitly assigned you as the reviewer.

```bash
dispatch reviews
dispatch show DE-13             # check the work and its recorded commits
dispatch commit show DE-13
# The user said "accept DE-13": record the user's decision.
dispatch review DE-13 --actor user
dispatch review DE-13 DE-15 --actor user --close-issue
# The user explicitly assigned you as reviewer: record your own name.
dispatch review DE-13 --actor <your-agent-name>
```

`--close-issue` closes each accepted task's linked GitHub issue through `gh`
with a comment naming the task, reviewer and commits. A `gh` failure is a
warning: the review itself still succeeds, so report the warning and offer to
close the issue by hand. After review, dependent tasks may become ready;
`dispatch ready` shows them.

## Health Checks And Tidying

```bash
dispatch status --check-issues  # also compare linked issues with GitHub
dispatch doctor                 # board, layout, PATH, agents, workspace paths
dispatch normalize              # recompute derived state, repair invariants
```

- `status` warns offline about done tasks without commits, assignment
  mismatches and several tasks linking the same issue. `--check-issues` asks
  GitHub (via `gh`) and warns when a done task's issue is still open or an open
  task's issue is already closed; missing or logged-out `gh` is a warning, not
  a failure.
- `doctor` reports problems with suggested fixes; `normalize` is the safe
  repair for derived state and stale assignments.
- A stuck or abandoned assignment (an agent died mid-task) is cleared with
  `dispatch unassign <TASK-ID> --actor <your-agent-name>`: a `doing` task returns to
  `ready` (or `blocked`). It refuses `review` and `done` tasks and leaves
  workspaces alone.
- Workspaces: `dispatch workspace prune --done --dry-run` previews cleaning up
  clean workspaces of done tasks; drop `--dry-run` to apply.
  `dispatch workspace prune --stale` clears records whose worktree never
  appeared or is gone. `dispatch workspace remove <TASK-ID>` removes one
  (adopted worktrees keep their files unless `--delete-worktree`), and
  `dispatch workspace repair <TASK-ID>` updates paths after the workflow
  directory moved (`doctor` points it out).
- Tasks: `dispatch task move <TASK-ID> <group> --actor <your-agent-name>` moves a task to
  the group whose scope fits, keeping its ID; `dispatch task delete <TASK-ID>`
  removes one (refused when other tasks depend on it unless `--force`).
- Agents: `dispatch agent show <name>`, `dispatch agent archive <name>` hides
  an inactive agent, `dispatch agent restore <name>` brings it back, and
  `dispatch agent resume <name> --print-command` prints the command that
  resumes its runner session.

Destructive tidying (`task delete`, `workspace remove --force`,
`workspace remove --delete-worktree`, `workspace prune` without `--dry-run`)
needs the person's go-ahead; show them what would change first.
