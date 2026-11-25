# 0006: Adopted workspace TUI markers, doctor checks, and external lookup

- **Status:** Accepted
- **Date:** 2026-11-25

## Context

ADR 0003 introduced `dispatch workspace adopt` and left three follow-ups
tracked in GitHub issue #13:

- External tools such as the Orca plugin needed to filter the full
  `workspace list --json` output to locate a record by its `external_id`.
- `dispatch doctor` checked only whether an adopted workspace path existed on
  disk; it did not detect an adopted directory that was no longer a git
  worktree of the board repository, nor an adopted worktree whose checked-out
  branch had drifted from the recorded branch.
- The TUI workspaces view did not distinguish adopted workspaces from created
  ones, the workspace inspector did not display `external_id`, and the `x`/`X`
  removal confirmation prompt did not tell the user that an adopted worktree
  stays on disk.

ADR 0003's core invariants remain unchanged: a `doing` task still cannot be
adopted (the flow is adopt, then start), and `--branch` on adopt still only
confirms the worktree's branch and fails on mismatch.

## Decision

**Lookup by external ID (`workspace list --external <tool>:<id>`).**
`workspace list` accepts an optional `--external <tool>:<id>` (at most once,
in any order with `--json`). The argument is validated with
`dispatch_external_id_is_valid`, matching `workspace adopt --external`, so a
malformed value such as a bare tool prefix fails immediately instead of
silently returning an empty list. Valid lookups filter live (non-removed)
records by exact, case-sensitive `external_id` match. Because `workspace list`
is a listing command rather than `show`, a valid external ID with no matching
active record succeeds with exit status 0, printing `(no workspaces)` in text
mode and `"workspaces": []` in JSON mode.

The workspace JSON envelope gains an additive `query.external_id` field (the
filter value for a filtered `workspace list`, `null` otherwise). `query.workspace`
stays `null` for `workspace list` even when a lookup returns one record, and
`summary.total`, `summary.active`, and `summary.removed` continue to count the
whole board while `summary.returned` counts the filtered array. `schema_version`
remains `1`.

**Doctor checks for adopted workspaces.**
When an adopted workspace's path exists on disk and the board's configured
repository is a git repository, `doctor` inspects the path against the board
repository:

- `workspace_not_a_worktree`: emitted when the path is not a live linked
  worktree of the board repository. Verification checks all four conditions:
  the path appears in `git -C <repo> worktree list --porcelain` without a
  `prunable` marker, is not the board repository itself, contains `.git`, and
  shares the same resolved `git rev-parse --git-common-dir` as the board
  repository. Checking `prunable` and `--git-common-dir` catches directories
  deleted and recreated (or reinitialized with `git init`) before
  `git worktree prune` runs.
- `workspace_branch_mismatch`: emitted when the live worktree has a branch
  checked out (`branch refs/heads/...`) that differs from the recorded
  `branch`. Detached HEADs are not flagged as branch mismatches: ADR 0003
  permits adopting a detached worktree with a required `--branch` label and
  stores no separate detached flag, so flagging detached HEADs would warn on
  valid records immediately after adopt.

Both warning kinds are added to version 1 of `docs/json-output-contract.md`
with `subject` set to the workspace ID and actionable `fix` hints.

**TUI markers, inspector, and removal confirmation prompt.**
- The workspaces table gains a fixed-width `Origin` column (`TUI_COL_ORIGIN = 7`)
  between `Git` and `Branch / Path`, displaying `created` (dim muted) or
  `adopted` (bold accent) so every column stays vertically aligned across mixed
  rows. Table paths now pass through `display_path` before truncation so golden
  frames remain portable across macOS and Linux (ADR 0005).
- The workspace inspector shows `Origin` (`created` or `adopted`) under
  `Location`, and `External ID` (`<tool>:<id>` or `-`) for adopted records. The
  task inspector's `Workspace` section also appends `Origin` for adopted
  workspaces, matching `dispatch show`.
- Both `x` and `X` run `dispatch workspace remove` without `--delete-worktree`,
  so both clear only the record on adopted workspaces. Their confirmation
  prompts now state `(adopted worktree kept)` for adopted records
  (`Type <id> to remove record (adopted worktree kept): ` and
  `Type <id> to force remove record (adopted worktree kept): `), while created
  workspaces keep the existing prompt.

## Consequences

- Tools such as Orca can resolve an external ID in one
  `dispatch workspace list --external orca:<id> --json` call and distinguish a
  miss (`returned == 0`) from an error without parsing stderr.
- `doctor` surfaces stale non-worktree directories and branch drift on adopted
  workspaces both in human output and in `doctor --json`.
- Adopted workspaces are visually distinct in the TUI table and inspector, and
  users see before confirming `x`/`X` that the underlying worktree will not be
  deleted.
- Adding the `Origin` column narrows `Branch / Path` by 8 columns in narrow
  terminals (23 columns remaining at 100 columns); the full path and branch
  remain visible in the workspace inspector (`Enter`/`i`).
