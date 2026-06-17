# Workspace Implementation Plan

Task: WD-08

## Scope

Implement Dispatch support for multi-agent work using:

- shared board state in `dispatch.json`;
- local agent prompt/scratch directories under `.dispatch/agents/`;
- task-scoped git worktrees;
- optional sequence workspaces for linear no-review chains;
- board locking for concurrent agents.

The plan consolidates the decisions from WD-01 through WD-07, WD-10, and WD-11.

## Phase 1: Board Model And Locking

Add board-level workspace and agent records.

Agent record:

```text
name
runner
model
agent_dir
prompt_path
run_script_path
created_at
```

Workspace record:

```text
id
task_id
actor
path
branch
repo_path
state: creating | active | removed
sequence_tasks
review_gate
created_at
updated_at
```

Implementation tasks:

- Add structs and JSON persistence for agents and workspaces.
- Add exclusive lock handling for all board mutations.
- Add validation for unique agent names, workspace paths, branches, and task
  ownership.
- Add tests for concurrent lock behavior at the store/CLI boundary.

## Phase 2: Agent Commands

Add:

```bash
dispatch agent create --name <name> --runner codex|claude [--model <name>] [--no-run-script] [--print-command]
dispatch agent list
dispatch agent show <name>
dispatch agent command <name> [--print-command]
```

Behavior:

- Create `.dispatch/agents/<name>/`.
- Generate `AGENT.md`.
- Generate `scratch/` and `decisions/`.
- Generate `run.sh` unless `--no-run-script` is passed.
- Do not create a permanent agent worktree.

Tests:

- valid and invalid agent names;
- duplicate agent creation;
- runner validation;
- generated file paths;
- `--model`, `--no-run-script`, and `--print-command` behavior.

## Phase 3: Workspace Create

Add:

```bash
dispatch workspace create <task-id> --actor <agent-id> [--repo <path>] [--dir <path>] [--branch <name>] [--sequence]
```

Default branch:

```text
agent/<actor-slug>/<task-id>
```

Default directory:

```text
<repo-basename>-agent-<actor-slug>-<task-id>
```

For `--sequence`, derive the direct linear chain until the first review-required
task and use:

```text
agent/<actor-slug>/<first-task-id>-sequence
<repo-basename>-agent-<actor-slug>-<first-task-id>-sequence
```

Implementation details:

- Reserve a workspace record as `creating` while holding the board lock.
- Release the lock while running git commands.
- Run `git worktree add` with either a new branch or an existing safe branch.
- Reacquire the lock and mark the workspace `active`.
- Roll back only git-recognized worktrees on failure.

Tests:

- task must exist, be ready, and be unassigned;
- workspace create refuses duplicate task/workspace/branch ownership;
- custom `--repo`, `--dir`, and `--branch`;
- branch exists cases;
- rollback behavior for failed git commands;
- `--sequence` accepts only direct linear chains with no-review intermediates.

## Phase 4: Start And Ownership Integration

Update `dispatch start`:

- If a workspace exists for the task, the start actor must match workspace
  actor.
- If no workspace exists, start remains allowed for simple workflows.
- Starting a task never creates a worktree as a side effect.

Update display:

- `dispatch show <task>` prints workspace path and branch when present.
- `dispatch list` may include compact workspace ownership metadata.
- `dispatch ready` remains the user approval gate.

Tests:

- start succeeds when actor matches workspace;
- start fails when actor differs;
- start still works with no workspace;
- list/show include workspace data.

## Phase 5: Workspace Lifecycle Commands

Add:

```bash
dispatch workspace list
dispatch workspace show <task-id-or-workspace>
dispatch workspace remove <task-id-or-workspace> [--force]
dispatch workspace prune [--done] [--stale] [--dry-run]
```

Behavior:

- `list` shows task state, workspace state, actor, branch, and path.
- `show` validates whether the git worktree exists.
- `remove` refuses doing tasks, dirty worktrees, and non-worktree paths unless
  the operation is explicitly safe.
- `prune --done` removes clean workspaces for done tasks.
- `prune --stale` removes stale creating reservations with no matching worktree.

Tests:

- clean removal;
- refusal for dirty or active workspaces;
- force removal of git worktrees only;
- dry-run output;
- stale reservation cleanup.

## Phase 6: Documentation And Agent Instructions

Update:

- `README.md` command reference;
- `AGENTS.md` and `CLAUDE.md` workflow instructions;
- examples for one-agent and multi-agent sessions;
- review-before-commit guidance once the active workflow is changed.

Document the distinction:

- agents share one board;
- agents have private `.dispatch/agents/<name>/` directories;
- agents do not share product worktrees;
- task worktrees are isolated by actor and task;
- sequence workspaces are explicit exceptions.

## Development Order

Recommended implementation tasks:

1. Add board lock.
2. Add agent/workspace data model and persistence.
3. Implement `agent create/list/show/command`.
4. Implement basic `workspace create`.
5. Integrate workspace ownership into `start`, `show`, and `list`.
6. Implement `workspace list/show/remove/prune`.
7. Add `--sequence` support.
8. Update docs and final acceptance tests.

This order makes concurrency safety available before multiple agents begin
mutating the shared board.
