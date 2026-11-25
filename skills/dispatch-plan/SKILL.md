---
name: dispatch-plan
description: Plan, break down or organise work into Dispatch tasks. Use when asked to plan a feature, turn a GitHub issue into Dispatch tasks, split work into groups, tasks and dependencies, or add review gates on a Dispatch board. Covers reusing groups by their recorded scope, granular task titles, dependencies, issue links and leaving new tasks proposed.
---

# Dispatch: plan work

Dispatch is a command-line workflow board for humans and agents. It organizes
work into groups, tasks, dependencies, assignment, review gates, and completion
history. This skill covers turning a request or an issue into Dispatch groups,
tasks and dependencies. To carry out a task use the `dispatch-work` skill; to
read, approve or tidy the board use the `dispatch-board` skill.

## Critical Rules

1. Use the Dispatch CLI for all workflow state.
2. Never read `dispatch.json` directly.
3. Never edit `dispatch.json` directly.
4. Never infer task state from `dispatch.json`.
5. Pass an explicit `--actor` on every change that accepts one. When you plan,
   the actor is your own agent name (for example `claude` or `codex-server`),
   never `user`. (`group add`, `group edit`, `dep add` and `dep remove` take no
   actor and are logged as `user`; every `task` command does take one.)

The storage file is an implementation detail. Do not inspect it with `cat`,
`sed`, `jq`, `rg`, editors, scripts, or JSON parsers. If you need task state,
ask the CLI (add `--json` when you need to parse it).

## Find The Workflow Directory

Dispatch state normally lives one directory above the product repository:

```text
work_dir/
  .git/
  dispatch.json
  dispatch.log
  AGENTS.md / CLAUDE.md
  repo/
```

Run Dispatch from `work_dir`. If your session started inside the product
repository (`work_dir/repo` or one of its worktrees), the board is usually in
the parent directory: run `cd ..` (or `cd` to the main checkout's parent) and
check that `dispatch repo` prints the repository you are working on. If it says
there is no board, you are in the wrong directory; do not run `dispatch init`
unless the user asks for a new board.

## How Dispatch Works

Groups organize related work. Each group has a short prefix, and task IDs use
that prefix, such as `DE-01` or `VD-03`. A group also records a scope
(`--description`) saying what work belongs in it.

Tasks have a title, description, state, priority, dependency list, review
setting, assignment fields, optional linked GitHub issue, and history. The task
title is the human-readable name and must not include the generated task ID.
The task ID is the stable identifier agents use in commands.

Dependencies are stored as "this task depends on those task IDs".
`dispatch dep add DE-01 DE-02` means `DE-02` depends on `DE-01`: the first
argument is the prerequisite, the second waits for it. A task is blocked when
any dependency is not done, and blocked tasks are not available for work.
Dependency cycles are rejected.

Commit references are stored through the CLI with `dispatch commit add`. They
associate a completed or review-pending task with the Git SHA created for it.

The normal lifecycle is:

```text
proposed -> ready -> doing -> review -> done
```

Tasks require review by default. If a task does not require review
(`--no-review`), `finish` moves it directly from `doing` to `done`. If it does
require review, `finish` moves it to `review`, and the working agent stops that
sequence until the user or reviewer accepts it. Review gates are therefore the
checkpoints of a plan.

## Planning Work

When the user asks you to plan work, create Dispatch tasks through the CLI. Use
groups, meaningful titles, dependencies, and review gates for checkpoints.

### 1. Reuse existing groups

Start every planning session by reviewing the recorded group scopes:

```bash
dispatch group list
dispatch group show <prefix>
```

If an existing group's scope covers the planned work, add the tasks there. A
task's group must match that group's recorded scope; creating a near-duplicate
of an existing group is a planning mistake. If a group you extend has no
recorded scope yet, record one:

```bash
dispatch group edit <prefix> --description <text>
```

Create a new group only when no recorded scope fits, and always give it a
`--description` so the next planner can reuse it:

```bash
dispatch group add "Benchmarks" --prefix BM --description "Performance measurement and regression tracking"
```

Never create a catch-all group such as "Issues", "Backlog", "Misc" or "Todo".
Groups describe an area of the product or a workstream, not where the work
came from.

### 2. Break the work into granular tasks

- One task is one reviewable, committable unit of work. A GitHub issue usually
  becomes several tasks, not one.
- Titles are short imperative phrases ("Design JSON output contract") without
  task IDs; Dispatch prints the ID in its own column.
- Put the detail, acceptance criteria and constraints in `--description`.
- Use `dispatch task edit <TASK-ID> --title <text> --description <text>` to fix
  wording later; the ID and history are kept.

### 3. Add dependencies and review gates

- Connect tasks with `dispatch dep add <dependency-id> <dependent-id>` so work
  happens in order. Use `dispatch dep remove` to undo a wrong edge, and
  `dispatch list` to inspect the resulting sequences.
- Keep review gates (the default) on tasks a human should check before
  dependent work continues: designs, contracts, risky changes, and the final
  task of a sequence.
- Use `--no-review` only for intermediate steps that can safely complete and
  unblock the next task without human acceptance. A direct chain of
  `--no-review` tasks ending in one review-gated task can later be worked in a
  single sequence workspace.

### 4. Link GitHub issues

When the work is tracked in a GitHub issue, link it on `task add` or later on
`task edit`:

```bash
dispatch task add DE "Implement JSON output flag" --issue 42 --actor <your-agent-name>
dispatch task edit DE-07 --issue acme/widgets#42 --actor <your-agent-name>
dispatch task edit DE-07 --clear-issue --actor <your-agent-name>
```

To import an issue's title and body as one task, use `--from-issue` (needs an
authenticated `gh`; it cannot be combined with an explicit title,
`--description`, or `--issue`):

```bash
dispatch task add DE --from-issue 42 --actor <your-agent-name>
```

A ref is `#123`, `123`, `owner/repo#123`, or the issue URL; bare numbers
resolve against the repository's GitHub `origin` remote. Several tasks may link
the same issue (a split issue); `status` notes that as a warning so accidental
duplicates are easy to spot.

### 5. Leave tasks proposed

New tasks are `proposed`. Only the user approves them by marking them ready
(see the `dispatch-board` skill). Do not run `dispatch ready <id>` or
`dispatch group ready` while planning unless the user explicitly told you to
ready a specific task or group, and then pass your own actor name. If the user
has not explicitly delegated readiness approval, leave newly planned tasks
proposed and tell the user which ones to ready.

Never record your own changes as `--actor user`: the history must show that an
agent created the plan and that the user approved it.

### Scripting

Every planning command accepts `--json`. `dispatch task add ... --json` returns
the generated ID in `result.task`, so a script can chain `dep add` calls
without parsing human output. Failures keep a non-zero exit status and print a
JSON error document on stderr; match on `error.code`. The full contract is in
`docs/json-output-contract.md` in the Dispatch repository.

## Worked Example: An Issue Becomes Several Tasks

The user says: "Plan issue #42, add JSON output support."

```bash
dispatch group list
# DE   Development   3 open / 12 tasks   Core CLI development work
# QA   Validation    0 open / 4 tasks    (no description)
dispatch group show DE
# The Development scope covers CLI output work, so reuse DE.
# No new group, and no "Issues" group.

dispatch task add DE "Design JSON output contract" --issue 42 --actor claude \
  --description "Define the machine-readable output shape for list, ready, blocked, and show."
# Created DE-13
dispatch task add DE "Implement JSON output flag" --issue 42 --no-review --actor claude \
  --description "Add --json output for the commands in the contract."
# Created DE-14
dispatch task add DE "Test JSON output commands" --issue 42 --actor claude \
  --description "Verify valid JSON and expected fields for every command."
# Created DE-15

dispatch dep add DE-13 DE-14  # DE-14 depends on DE-13
dispatch dep add DE-14 DE-15  # DE-15 depends on DE-14
dispatch list DE
```

Result: the design task has a review gate (a human checks the contract before
code is written), implementation is a `--no-review` step, and the test task
closes the sequence with a review gate. All three link issue #42 and stay
`proposed`. Tell the user the plan and that `dispatch ready DE-13` (or
`dispatch group ready DE`) approves it.

If the issue had been a whole new area with no fitting scope, the same flow
would start with one `dispatch group add ... --description ...` and put all the
tasks in that new group.
