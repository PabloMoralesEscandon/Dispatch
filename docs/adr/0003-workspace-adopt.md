# 0003: Adopt a worktree created outside Dispatch

- **Status:** Accepted
- **Date:** 2026-11-16

## Context

GitHub issue #4 comes from the Orca plugin. In Orca, worktrees are created by
`orca worktree create`, not by Dispatch, but `workspace create` always runs
`git worktree add` itself. Dispatch needs a way to record a workspace it did
not create, so that task ownership, `start` actor checks, `show`, and the TUI
work the same way, without touching Git.

The issue asks for `dispatch workspace adopt <task-id> --actor <name> --path
<dir> [--branch <b>] [--sequence] [--external orca:<workspace-id>]`. Adopt
must check that the path is a worktree of the board repository, reusing the
existing repository and path-conflict checks, and store an optional
`external_id`. `workspace remove` on an adopted workspace should clear only the
record unless `--delete-worktree` is passed, and `doctor` and `prune` must
handle adopted records. Existing boards must keep loading unchanged, and JSON
changes must be additive (ADR 0002).

## Decision

**Storage.** A workspace record gains two optional keys, `adopted` (boolean)
and `external_id` (string). Following ADR 0001, they are written only when
set, so boards without adopted workspaces keep their exact shape, including
after `normalize`. A missing `adopted` means `false`. A non-boolean `adopted`
or an invalid `external_id` fails board load, like other invalid workspace
fields.

**Validation.** Adopt applies the `workspace create` checks in the same order
where they overlap: valid actor, actor not archived, task exists and is ready
and unassigned, no live workspace covers the task, a valid `--sequence` chain,
and a board repository that is a Git repository. The path is then resolved
with `realpath` and must be an existing directory, must not be the repository
itself, and must not be recorded by another live workspace. It must appear in
`git worktree list --porcelain` for the board repository. Paths are compared
textually or by `realpath`, so `/tmp` and `/private/tmp` spellings match; the
same comparison now backs the registered-worktree check of `remove` and
`prune`. A worktree of another repository, a subdirectory of a worktree, and a
plain directory are rejected with `not_a_worktree`. Adopt never runs
`git worktree add`, and it takes the board lock for the whole command, since
the only Git call is a quick listing.

**Branch.** The branch is read from the worktree's `branch refs/heads/...`
line. `--branch` is a check, not an override: if it differs from the
checked-out branch the command fails with the new `branch_mismatch` code, so a
record never contradicts Git. A detached HEAD has no branch to read, so
`--branch` is then required and recorded as given. The branch must not be
recorded by another live workspace.

**External ID.** `--external` takes `<tool>:<id>`: a tool name of 1–32
characters from `[a-z0-9._-]` starting with a letter or digit, and an ID of
1–200 printable ASCII characters without spaces (it may contain further
colons or slashes). The format is checked, not the tool, so other integrations
can use their own prefix. One external ID can belong to only one live record,
so the external tool can look a record up by its own ID. An invalid value is
the new `invalid_external_id` code, and a duplicate is `workspace_conflict`.

**State and history.** An adopted record is written as `active` in one save;
there is no `creating` phase because there is no Git work to roll back. Adopt
appends a `workspace_adopt` entry to the anchor task's history (note:
`path=... branch=... [external=...]`) and a `workspace_adopt` record to
`dispatch.log` with the path, branch, repository, sequence flag, and external
ID. `workspace create` still writes no task history; adopt does because the
worktree's origin is otherwise invisible in the task's own record.

**Remove.** The worktree belongs to the tool that created it. On an adopted
record, `workspace remove` skips the registered-worktree and dirty checks and
clears only the record, so it also works after the tool already deleted the
worktree. `--delete-worktree` restores the normal behavior, including the
dirty check that `--force` skips. On a created record the flag changes
nothing, so scripts can pass it unconditionally. The doing-task check is kept
for both. JSON results gain `worktree_deleted`, and the log record gains
`adopted` and `worktree_deleted` context for adopted records.

**Prune.** `prune --done` releases done adopted records without any Git call,
whether or not the worktree is dirty. `prune --stale` also clears adopted
records whose worktree directory no longer exists, whatever `git worktree
list` still says, because that is how a record goes stale when the tool
deletes its worktree. Adopted records are never skipped, and their text lines
say the worktree was kept.

**Doctor and repair.** A missing adopted path is reported with the existing
`workspace_path_missing` kind and a fix hint pointing to `remove` or
`prune --stale`. Doctor does not look for an adopted worktree at the default
location next to the repository, and `workspace repair` refuses adopted
records: both assume Dispatch's own naming, which an external tool's
worktrees need not follow. To move one, remove the record and adopt again.

**Output.** `workspace show` prints `Origin: adopted` and `External ID:`, `show`
prints `Workspace origin:`, and `workspace list` appends
`[adopted <external-id>]`. These lines appear only for adopted records, so the
output for created workspaces does not change. Workspace JSON, both
`workspace list|show` records and the `workspace` object inside a task, always
carries `adopted` and `external_id`. That is additive, so `schema_version`
stays 1. `workspace adopt --json` uses the workspace mutation envelope with
action `workspace_adopt` and extras `workspace` and `external_id`.

## Consequences

- The Orca plugin can create a worktree with Orca, record it with one
  `workspace adopt --external orca:<id> --json` call, and later find the
  record by its `external_id` in `workspace list --json`.
- Dispatch never deletes a worktree it did not create unless explicitly asked
  to, so `x`/`X` in the TUI workspaces view and `prune --done` are safe on
  adopted workspaces.
- `remove`/`prune` now resolve paths with `realpath` when comparing against
  `git worktree list`, which is slightly more lenient than the previous
  textual match for created workspaces.
- Two new error codes: `branch_mismatch` and `invalid_external_id`.

### Follow-ups

These were left open:

- Adopting for a task that is already `doing` (for example, after the fact)
  is rejected like `workspace create`; it could be allowed for the assignee.
- A `workspace list --external <id>` lookup, and `--json` for
  `workspace repair`.
- The TUI workspaces view does not mark adopted records yet.
- Doctor does not warn when an adopted path exists but is no longer a
  registered worktree, or when its checked-out branch has changed since adopt.
