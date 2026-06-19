# Dispatch

Dispatch is a command-line workflow board for humans and agents. It replaces
the old TDL personal todo app with a project workflow tool built around task
groups, stable task IDs, dependencies, assignment, review gates, and explicit
completion history.

Dispatch is meant to help a user and one or more coding agents coordinate work
without editing the board storage file directly.

The recommended layout keeps Dispatch state one directory above the repository
being changed:

```text
work_dir/
  .git/
  dispatch.json
  repo/
```

Run Dispatch from `work_dir`. The `repo/` directory remains the normal product
repository, while `work_dir` can have its own Git history for workflow state.

## Requirements

- A C compiler such as `clang` or `gcc`
- `jansson`
- A POSIX-like shell for the test runner

## Build

Build the `nob` bootstrapper and then build Dispatch:

```bash
cc nob.c -o nob
./nob
```

The build creates:

```text
build/dispatch
dispatch -> build/dispatch
```

For a release-style build:

```bash
./nob release
```

To clean generated build output:

```bash
./nob clean
```

## Shell Completion

Dispatch can print completion scripts for bash, zsh, and fish:

```bash
dispatch completion bash
dispatch completion zsh
dispatch completion fish
```

For a one-off bash or zsh session, source the generated script:

```bash
source <(dispatch completion bash)
source <(dispatch completion zsh)
```

To install bash completion, write the generated script to your bash-completion
directory:

```bash
mkdir -p ~/.local/share/bash-completion/completions
dispatch completion bash > ~/.local/share/bash-completion/completions/dispatch
```

To install zsh completion, write the generated script as `_dispatch` somewhere
in `fpath`, then initialize completions from your shell startup file:

```bash
mkdir -p ~/.zfunc
dispatch completion zsh > ~/.zfunc/_dispatch
```

```zsh
fpath=(~/.zfunc $fpath)
autoload -Uz compinit
compinit
```

To install fish completion:

```bash
mkdir -p ~/.config/fish/completions
dispatch completion fish > ~/.config/fish/completions/dispatch.fish
```

The scripts ask Dispatch for dynamic candidates instead of reading
`dispatch.json` directly. Task IDs, groups, agents, and workspaces come from
line-oriented CLI commands such as:

```bash
dispatch completion candidates tasks
dispatch completion candidates groups
dispatch completion candidates agents
dispatch completion candidates workspaces
```

Run completions from the workflow directory that contains the board, or make
sure `dispatch` is invoked from that directory, so dynamic candidates come from
the intended board.

## Quick Start

Create a workflow directory, put or clone the code repository inside it, then
create a board that points at that repository:

```bash
mkdir work_dir
cd work_dir
git init
git clone <repo-url> repo
dispatch init repo
```

Add a group, add two tasks, and make the second task depend on the first:

```bash
dispatch group add Development --prefix DE
dispatch task add DE "Design storage model"
dispatch task add DE "Implement storage model"
dispatch dep add DE-01 DE-02  # DE-02 depends on DE-01
dispatch ready DE-01 --actor user
dispatch ready
dispatch list
```

When developing Dispatch itself from this repository, `./dispatch init repo`
has the same shape if you run it from the parent workflow directory.

Start and finish work as an actor:

```bash
dispatch show DE-01
dispatch start DE-01 --actor codex

# Do the work.

dispatch finish DE-01 --actor codex
```

If the task requires review, `finish` moves it to `review` and the agent should
stop that sequence until the user accepts it:

```bash
dispatch review DE-01 --actor user
```

After review, dependent tasks can become ready:

```bash
dispatch ready
```

## Command Reference

### Board

```bash
dispatch init [repo-path]
dispatch normalize
```

`init` creates `dispatch.json` if it does not already exist. Pass the repository
path, normally `repo`, when using the recommended parent workflow layout.
`normalize` recomputes derived states such as blocked and ready.

### Completion

```bash
dispatch completion candidates commands|candidate-kinds|tasks|groups|agents|workspaces
dispatch completion bash
dispatch completion zsh
dispatch completion fish
```

`completion candidates` prints line-separated values for shell completion.
Generated shell scripts use those candidate commands for board-specific task,
group, agent, and workspace values.

### Agents

```bash
dispatch agent create --name <name> --runner codex|claude [--model <name>] [--no-run-script] [--print-command]
dispatch agent list
dispatch agent show <name>
dispatch agent command <name> [--print-command]
```

`agent create` registers a named agent and creates local support files under
`.dispatch/agents/<name>/`. The generated prompt file explains the shared
Dispatch workflow, and the optional `run.sh` starts the selected runner from the
workflow directory. Agent directories are for prompt material, scratch notes,
and local decisions; task state remains in Dispatch and product changes belong
in task worktrees.

### Workspaces

```bash
dispatch workspace create <task-id> --actor <agent-id> [--repo <path>] [--dir <path>] [--branch <name>] [--sequence]
dispatch workspace list
dispatch workspace show <task-id-or-workspace>
dispatch workspace remove <task-id-or-workspace> [--force]
dispatch workspace prune [--done] [--stale] [--dry-run]
```

`workspace create` creates an isolated Git worktree and records its actor,
branch, repository path, and task coverage. By default, the workspace belongs to
one task. With `--sequence`, Dispatch records one workspace for a direct linear
chain of no-review tasks that ends at the first review gate.

`start` does not create worktrees as a side effect. If a task already has a
workspace, the start actor must match the workspace actor. `workspace remove`
removes a recorded Git worktree and clears its record, refusing doing tasks,
dirty worktrees, and non-worktree paths unless the operation is explicitly safe.
`workspace prune --done` cleans up clean workspaces for done tasks, and
`workspace prune --stale` clears stale creating records with no matching
worktree. Use `--dry-run` to preview prune actions.

### Groups

```bash
dispatch group add <name> [--prefix XX]
dispatch group ready <group> --actor <name> [--no-review]
```

Groups are workflow lanes or topics. Task IDs are generated from the group
prefix, for example `DE-01`.

`group ready` marks every proposed task in the group as ready in one operation.
Tasks already blocked, assigned, doing, in review, or done are left unchanged.
Proposed tasks with unmet dependencies are approved but still display as blocked
until their blockers are done. The actor should normally be the user; an agent
should ready tasks only when the user explicitly asks it to do so. Add
`--no-review` only when the user approves every readied task as safe to finish
without a review gate. If `--no-review` is used on a group that is already
readied, it also applies to ready or blocked tasks in that group that have not
been started yet.

### Tasks

```bash
dispatch task add <group> <title> [--description <text>] [--no-review]
dispatch task delete <id> [--force]
dispatch show <id>
dispatch list [group]
```

Tasks require a group and a title. Titles are human-readable labels and should
not include generated Dispatch IDs such as `DE-01`; Dispatch prints the ID in
its own column. By default, tasks require review after they are finished. Use
`--no-review` only when the task can safely complete and unblock the next task
without human acceptance.

Deleting a task with dependents is rejected unless `--force` is used. Forced
delete also removes that task from other tasks' dependency lists.

`list` prints active tasks once, grouped by Dispatch group and in task order.
Completed tasks are hidden by default. If every task in a group is completed,
the group prints a single `(done)` marker instead of every completed task row.
Dependencies are shown on the task line as `depends_on:A,B`. The output can be
limited to one group by ID, prefix, or name.

When stdout is a terminal, `list` uses ANSI color to distinguish group headings,
task IDs, states, and metadata. Set `FORCE_COLOR=1` to force color in captured
output, or `NO_COLOR=1` to disable it.

### Dependencies

```bash
dispatch dep add <dependency-id> <dependent-id>
dispatch dep remove <dependency-id> <dependent-id>
dispatch blocked
```

`dep add DE-01 DE-02` means `DE-02` depends on `DE-01`; the first argument is
the prerequisite, and the second argument is the task that waits for it. A task
is blocked when any dependency is not done. Dependency cycles are rejected.

### Lifecycle

```bash
dispatch ready [<id> --actor <name> [--no-review]]
dispatch start <id> --actor <name>
dispatch finish <id> --actor <name>
dispatch review <id> --actor <name>
```

The normal lifecycle is:

```text
proposed -> ready -> doing -> review -> done
```

Tasks without review gates go directly from `doing` to `done` when finished:

```text
proposed -> ready -> doing -> done
```

Use `ready` with no ID to list work that can be started. Use
`ready <id> --actor <name>` to approve a proposed task for work. The actor
should normally be the user; an agent should mark tasks ready only when the user
explicitly instructs it to do so. Add `--no-review` when approving a task that
can safely move directly from `doing` to `done` after the agent finishes it.

`start` assigns the task to an actor and prevents a second actor from starting
the same task. `finish` records the completing actor and moves the task to
`review` or `done`. `review` accepts a review task as done.

## Agent Workflow

Agents should use Dispatch as their task protocol:

```bash
dispatch ready
dispatch blocked
dispatch show <TASK-ID>
dispatch start <TASK-ID> --actor <agent-id>

# Do the requested work and run relevant checks.

dispatch finish <TASK-ID> --actor <agent-id>
git status --short
git add <files changed for this task>
git commit -m "<task title>"
dispatch ready
```

If `finish` reports that review is required, the agent must stop that sequence
and wait for the user. If `finish` reports `done` and prints newly ready tasks,
the agent may continue only when the user asked it to keep working through
available tasks.

Agents may share repository instructions and Dispatch CLI output when explaining
workflow decisions. Hidden runtime instructions, tool policies, credentials, and
system or developer prompts are not Dispatch workflow state and should be
summarized only at a high level when they affect behavior.

Parallel agents should not share a Git working tree. Use `workspace create` to
make one Git worktree and one branch per task or task sequence:

```bash
dispatch workspace create DE-12 --actor codex-server
dispatch workspace create FE-04 --actor claude-frontend
```

Dispatch controls task assignment. Git worktrees control file and commit
isolation.

### Single-Agent Workspace Example

```bash
dispatch agent create --name codex-server --runner codex --print-command
dispatch workspace create DE-01 --actor codex-server
dispatch workspace show DE-01

cd repo-agent-codex-server-DE-01
../dispatch start DE-01 --actor codex-server

# edit, test, and commit the task changes

../dispatch finish DE-01 --actor codex-server
```

The workspace remains available for review after `finish`. After acceptance and
merge, clean it up explicitly:

```bash
dispatch workspace prune --done
```

### Multi-Agent Workspace Example

Three agents can work at the same time by using one actor and workspace per
area:

```bash
dispatch agent create --name codex-server --runner codex
dispatch agent create --name codex-backend --runner codex
dispatch agent create --name claude-frontend --runner claude

dispatch workspace create SE-01 --actor codex-server
dispatch workspace create BE-01 --actor codex-backend
dispatch workspace create FE-01 --actor claude-frontend
```

Each agent runs from the workflow directory, starts only its assigned task, and
edits only its recorded worktree. A human or designated integrator merges
accepted branches back to the main repository one at a time.

### Sequence Workspace Example

For a direct chain where intermediate tasks do not require review, use one
workspace for the sequence:

```bash
dispatch task add DE "Prepare API shape" --no-review
dispatch task add DE "Implement API shape" --no-review
dispatch task add DE "Review API workflow"
dispatch dep add DE-01 DE-02
dispatch dep add DE-02 DE-03
dispatch ready DE-01 --actor user
dispatch ready DE-02 --actor user
dispatch ready DE-03 --actor user
dispatch workspace create DE-01 --actor codex-server --sequence
```

The agent works through `DE-01`, `DE-02`, and `DE-03` in the same branch, making
one commit per task. The sequence stops when `DE-03` reaches `review`, and the
user reviews the final branch state.

## Planning Example

When a user asks an agent to plan a feature, the agent should create tasks and
dependencies through Dispatch:

```bash
dispatch group add Validation --prefix VD
dispatch task add VD "Define CLI test scenarios"
dispatch task add VD "Implement CLI test runner"
dispatch task add VD "Run final acceptance pass"
dispatch dep add VD-01 VD-02  # VD-02 depends on VD-01
dispatch dep add VD-02 VD-03  # VD-03 depends on VD-02
dispatch ready VD-01 --actor user
```

This creates a sequence where implementation waits for planning, and final
acceptance waits for implementation. Agents create planned tasks as `proposed`;
the user decides which proposed tasks become `ready` unless the user explicitly
delegates that approval step.

## Storage

Dispatch stores its board in `dispatch.json` in the workflow directory where the
CLI is run. That directory should usually be one level above the repository
being modified. The file contains the workspace repository path, board name,
groups, tasks, dependencies, ownership fields, timestamps, review flags, and
history entries.

Agents should treat `dispatch.json` as an implementation detail. Do not read or
edit it directly during normal workflow. Use Dispatch commands to inspect and
change board state.

## Tests

Run the automated CLI tests:

```bash
tests/run_cli_tests.sh
```

The test runner builds Dispatch, creates temporary boards under `/tmp`, and
checks command behavior through the CLI.

Current coverage includes initialization, groups, task creation, dependencies,
blocked state, assignment lockout, finish/review lifecycle, ungated
continuation, guarded delete, and rejection of removed TDL commands/options.

## Removed TDL Surface

Dispatch no longer supports the old personal todo commands or metadata:

- `add`, `mod`, `done`, `del`, `clear`
- project inheritance commands
- due dates
- recurrence
- notification metadata
- categories
- priority-first sorting

Use Dispatch groups, dependencies, review gates, and lifecycle commands instead.
