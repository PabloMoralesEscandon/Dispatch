# Git Worktree Operation Contract

Task: WD-04

## Decision

`dispatch workspace create` should be a thin, predictable wrapper around
`git worktree add` with explicit preflight checks and rollback.

Default creation command:

```bash
git -C <repo> worktree add -b <branch> <workspace-dir> HEAD
```

When the branch already exists and is safe to reuse:

```bash
git -C <repo> worktree add <workspace-dir> <branch>
```

## Preflight Checks

Before running `git worktree add`, Dispatch must check:

- `<repo>` exists and has a `.git` directory or gitdir;
- `git -C <repo> rev-parse --is-inside-work-tree` succeeds;
- the current repository is not in the middle of merge, rebase, cherry-pick, or
  bisect state;
- the target workspace directory does not already contain unrelated files;
- the task is ready and unassigned;
- no workspace record already exists for the task.

A dirty managed repository does not block workspace creation. Uncommitted
changes in the source checkout are not copied into the new worktree, so Dispatch
prints a warning but proceeds.

## Branch Policy

Default branch names are task-scoped:

```text
agent/<actor-slug>/<task-id>
```

If the branch does not exist, create it from the managed repository's current
`HEAD`.

If the branch exists:

- reuse it only when it is not checked out in another worktree;
- fail if it is checked out elsewhere;
- fail if the user did not explicitly pass `--branch` and the existing branch
  name does not match the default for the actor and task.

Detached `HEAD` repositories are allowed. The default branch still starts from
the detached commit.

## Remotes

Workspace creation is local-only. Dispatch does not fetch, pull, push, or set an
upstream. Remote integration belongs to explicit git commands outside Dispatch.

## Rollback

If any step after directory creation fails, Dispatch must attempt:

```bash
git -C <repo> worktree remove --force <workspace-dir>
```

Rollback should not delete arbitrary directories. It may remove only a path that
git reports as a worktree for the managed repository.

If rollback fails, Dispatch prints the path and branch that need manual cleanup.
The command exits non-zero and does not write a workspace record.

## Existing Directories

If `<workspace-dir>` already exists:

- succeed only when it is already the expected worktree for the expected branch
  and task;
- otherwise fail with a message that asks for a different `--dir` or manual
  cleanup.

Dispatch never runs recursive delete on a non-worktree directory.
