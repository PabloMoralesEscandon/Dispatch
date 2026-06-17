# Repository And Workspace Path Model

Task: WD-03

## Decision

Dispatch distinguishes three locations:

- workflow directory: where `dispatch.json` lives and Dispatch commands run;
- managed repository: the product repository recorded by `dispatch init`;
- workspace directory: a git worktree created for one task and actor.

The default layout is:

```text
work_dir/
  dispatch.json
  repo/
  repo-agent-codex-WD-03/
```

## Repository Discovery

`dispatch init [repo-path]` records the managed repository path. The path may be
relative to the workflow directory or absolute. Workspace commands resolve it to
an absolute canonical path before running git commands.

If the recorded repository path is missing or is not a git repository,
`workspace create` fails with a message that names the configured path and
suggests re-running `dispatch init <repo-path>` or using `--repo`.

## Workspace Directory Defaults

The default workspace parent is the parent directory of the managed repository.
The default workspace directory name is:

```text
<repo-basename>-agent-<actor-slug>-<task-id>
```

For this repository:

```text
repo: /home/pablome/github/dispatch-wip/Dispatch
actor: codex
task: WD-03
workspace: /home/pablome/github/dispatch-wip/Dispatch-agent-codex-WD-03
```

## Overrides

`workspace create` accepts:

- `--repo <path>` to override the managed repository for this operation;
- `--dir <path>` to choose the exact worktree directory;
- `--branch <name>` to choose the branch name.

Overrides are stored in the workspace record so later `workspace show`,
`workspace remove`, and `workspace prune` operate on the actual created path.

## Path Safety

Dispatch should never infer workspace state by scanning arbitrary sibling
directories. It should trust recorded workspace paths and validate them before
destructive operations.

Before creating a workspace, Dispatch validates:

- the repository path exists and is a git repository;
- the target workspace directory does not exist, or is already the expected git
  worktree for the same branch and task;
- the workspace path is not equal to the managed repository path;
- relative override paths resolve under the current workflow directory.

Absolute override paths are allowed because users may intentionally keep
worktrees on another volume, but Dispatch must display the resolved absolute
path before creation.
