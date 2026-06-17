# Workspace Lifecycle Commands

Task: WD-07

## Decision

Dispatch should provide lifecycle commands for inspecting and cleaning workspace
records without hiding git operations behind task state changes.

Supported commands:

```bash
dispatch workspace list
dispatch workspace show <task-id-or-workspace>
dispatch workspace remove <task-id-or-workspace> [--force]
dispatch workspace prune [--done] [--stale] [--dry-run]
```

## List

`workspace list` prints one row per recorded workspace:

```text
WD-07  active  codex  agent/codex/WD-07  ../Dispatch-agent-codex-WD-07
```

It should include task state, workspace state, actor, branch, and path. A later
version may add filters such as `--actor`, `--state`, and `--task-state`.

## Show

`workspace show` prints the complete workspace record and validates whether the
git worktree still exists:

```text
Task: WD-07
Task state: doing
Workspace state: active
Actor: codex
Branch: agent/codex/WD-07
Path: /abs/path/Dispatch-agent-codex-WD-07
Git worktree: present
```

If the path is missing, Dispatch reports it but does not remove the record
without an explicit cleanup command.

## Remove

`workspace remove` removes the git worktree and clears the workspace record.

Default behavior refuses to remove a workspace when:

- the task is `doing`;
- the workspace has uncommitted changes;
- the path is not a git worktree for the managed repository.

`--force` may remove a git worktree with local changes by passing
`git worktree remove --force`, but it still must not delete non-worktree
directories.

## Prune

`workspace prune --done` removes workspaces for tasks in `done` state when their
worktrees are clean.

`workspace prune --stale` removes stale `creating` reservations that have no
matching git worktree.

`--dry-run` prints the actions without changing board or git state.

## Task State Interactions

Task lifecycle commands do not automatically remove workspaces:

- `finish` moves the task to `review` or `done` and leaves the workspace intact;
- `review` moves the task to `done` and leaves the workspace intact;
- `workspace prune --done` is the explicit cleanup operation.

Keeping cleanup explicit preserves review context. The user can inspect the
task branch after `finish` and remove it only after acceptance.
