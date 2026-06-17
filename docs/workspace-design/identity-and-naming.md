# Agent Identity And Naming

Task: WD-02

## Decision

Use the Dispatch actor string as the canonical agent identity. The actor passed
to workspace commands must be the same actor used for task lifecycle commands:

```bash
dispatch workspace create WD-02 --actor codex
dispatch start WD-02 --actor codex
```

## Actor Rules

Actor labels must:

- contain only ASCII letters, digits, dot, underscore, or hyphen;
- start with an ASCII letter or digit;
- be no longer than 48 characters;
- not contain `/`, whitespace, shell metacharacters, or path separators.

Dispatch stores the original actor label for display and derives a lowercase
slug for branch and directory names. For example, `Codex_A` displays as
`Codex_A` and uses slug `codex_a`.

## Derived Names

Default branch:

```text
agent/<actor-slug>/<task-id>
```

Default workspace directory:

```text
<repo-name>-agent-<actor-slug>-<task-id>
```

Examples:

```text
actor: codex
task: WD-02
branch: agent/codex/WD-02
directory: Dispatch-agent-codex-WD-02
```

## Collisions

Collisions are handled deterministically:

- If the task already has a workspace record, `workspace create` fails and
  prints the existing path.
- If the branch exists and is already attached to the expected workspace,
  creation is idempotent.
- If the branch exists for another workspace, creation fails unless the user
  passes an explicit `--branch`.
- If the target directory exists and is not the expected worktree, creation
  fails unless the user passes an explicit `--dir`.

## Display

Commands should display both task ownership and workspace ownership:

```text
WD-02  doing  Establish agent identity and naming rules
  actor: codex
  workspace: ../Dispatch-agent-codex-WD-02
  branch: agent/codex/WD-02
```

The actor label in Dispatch history remains the user-provided label, not the
derived slug.
