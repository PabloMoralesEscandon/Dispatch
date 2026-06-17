# Workspace Command Surface

Task: WD-01

## Decision

Add workspace management under a new top-level `workspace` command:

```bash
dispatch workspace create <task-id> --actor <agent-id> [--repo <path>] [--dir <path>] [--branch <name>]
dispatch workspace list
dispatch workspace show <task-id-or-workspace>
dispatch workspace remove <task-id-or-workspace> [--force]
dispatch workspace prune [--done] [--dry-run]
```

Workspace creation is explicit. `dispatch start` continues to assign a task
only; it does not create a git worktree as a hidden side effect.

## Rationale

Creating a git worktree changes filesystem and git state. Keeping it explicit
makes the operation easy to review, retry, and document. Agents can still use a
short, predictable sequence:

```bash
dispatch workspace create WD-01 --actor codex
dispatch start WD-01 --actor codex
```

## Defaults

- `<task-id>` is required and must refer to a ready, unassigned task.
- `--actor` is required and must match the actor that will start the task.
- `--repo` defaults to the board repository path.
- `--dir` defaults to a sibling of the managed repository.
- `--branch` defaults to `agent/<actor>/<task-id>`.

## Output

On success, `workspace create` prints the workspace path, branch name, task ID,
and actor:

```text
Created workspace WD-01 for codex
  path: ../Dispatch-agent-codex-WD-01
  branch: agent/codex/WD-01
```

The command should fail before making changes if the task is not ready, is
already assigned, or already has a workspace record.
