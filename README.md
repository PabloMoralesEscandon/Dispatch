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
- `git` and a pager for the TUI diff view (`d`). The pager is taken from
  `$PAGER`, falling back to `less` then `more`; whichever one you rely on must be
  installed, or the diff runs without paging.
- `gh` (GitHub CLI), optional — required only for `task add --from-issue`.

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

When developing from the recommended parent workflow layout, keep the workflow
root command as a symlink to the checkout command:

```bash
cd work_dir
ln -sf Dispatch/dispatch dispatch
```

Then rebuilding inside `Dispatch` with `./nob` updates both `Dispatch/dispatch`
and the workflow-root `./dispatch`.

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

Fish autoloads that file when the command is available as `dispatch` on
`PATH`. If you are developing from a checkout and invoking `./dispatch`, source
the generated script in the session instead:

```fish
./dispatch completion fish | source
```

The scripts ask Dispatch for dynamic candidates instead of reading
`dispatch.json` directly. Task IDs, groups, agents, and workspaces come from
line-oriented CLI commands such as:

```bash
dispatch completion candidates tasks
dispatch completion candidates groups
dispatch completion candidates agents
dispatch completion candidates workspaces
dispatch completion candidates runners
dispatch completion candidates skills
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
dispatch repo
dispatch repo set <path>
dispatch normalize
dispatch status [--check-issues] [--json]
dispatch doctor [--json]
dispatch log [--task <id>] [--actor <name>] [--agent <name>] [--workspace <id>] [--command <name>] [--action <name>] [--limit <n>] [--json]
```

`init` resolves `[repo-path]` (which must exist, defaulting to `.`) to an
absolute path and creates `dispatch.json` if it does not already exist. Pass
the repository path, normally `repo`, when using the recommended parent
workflow layout. It installs the [agent skills](#agent-skills) for every runner
in the workflow directory (project scope) and prints a hint about
`dispatch skills install --global` for sessions started outside the workflow
directory (such as task worktrees). `init` does not write `AGENTS.md` or
`CLAUDE.md` ([ADR 0014](docs/adr/0014-skills-only-agent-instructions.md)); the
agent skills are the sole source of Dispatch workflow instructions. Running
`init` again on an existing board installs skills that are missing or older
than the binary, and leaves skills you edited alone.

`repo` prints the board's configured repository path (`Repo: <path>`) and warns
if it is not a Git repository; there is no `repo show` subcommand.
`repo set <path>` resolves `<path>` (which must exist) to an absolute path and
updates the board's stored repository path in place without recreating the
board or modifying tasks, workspaces, or history. It warns if the target is not
a Git repository and appends a `repo`/`set` entry to `dispatch.log`. Neither
form accepts `--json`.

`normalize` recomputes derived states such as blocked and ready and repairs
stale task assignment invariant violations (see
[task state invariants](docs/task-state-invariants.md)). It also repairs older
agent prompt records by moving them to the canonical
`.dispatch/agents/<name>/<name>-PROMPT.md` prompt path, refreshing outdated
generated prompt content (including prompts predating the skills-only
instructions), regenerating affected agent run scripts, and trimming
stored agent session IDs.

Dispatch serializes board reads and writes through `dispatch.json.lock`. Saves
are written to a temporary file and atomically moved into place, so concurrent
commands either read a complete board or print a short retry message while
another process updates it.

`status` prints a compact board overview: task counts by state, ready and review
queues, blocked count, enabled/archived agent counts, workspace counts, and
warnings such as completed tasks without recorded commits or tasks of
different groups linking the same GitHub issue. Plain `status` never touches
the network; pass `--check-issues` to query GitHub via `gh` and also warn when
every task linking an issue is done but the issue is still open, or an open
task's linked issue is closed. When `gh`
is missing, not logged in, or fails on an individual issue, `status
--check-issues` emits a non-fatal warning and still reports the rest of the
board.
`doctor` checks board readability, repository layout, PATH/symlink setup,
completion installation, agent skills (warning when a skill is installed for
no runner, a copy is older than the binary, or skills are only in project scope
while workspaces live outside the workflow directory), removable template-only
`AGENTS.md` / `CLAUDE.md` files (an informational note, leaving custom files
alone silently; see [Removing Older Instruction Files](#removing-older-instruction-files)),
generated agent prompt/run files (warning when a prompt is outdated;
`normalize` regenerates it), and workspace paths.
`log` prints the `dispatch.log` audit trail; see [Dispatch Log](#dispatch-log).

### Completion

```bash
dispatch completion candidates commands|candidate-kinds|tasks|groups|agents|workspaces|runners|skills
dispatch completion bash
dispatch completion zsh
dispatch completion fish
dispatch completion install fish|bash|zsh
```

`completion candidates` prints line-separated values for shell completion.
Generated shell scripts use those candidate commands for board-specific task,
group, agent, and workspace values, and for the fixed runner and bundled skill
names offered after `agent create --runner` and `skills show`. The `runners`
and `skills` kinds read built-in tables instead of the board, so they also
answer outside a workflow directory.
`completion install <shell>` writes the generated completion script to the
shell-specific user directory, checks whether `dispatch` is on `PATH`, and
prints the reload command.

### Skills

```bash
dispatch skills list [--project | --global] [--runner claude|codex|opencode|all] [--json]
dispatch skills show <name> [--json]
dispatch skills install [--project | --global] [--runner claude|codex|opencode|all] [--force] [--json]
dispatch skills update [--project | --global] [--runner claude|codex|opencode|all] [--force] [--json]
dispatch --version
```

The [agent skills](#agent-skills) are built into the binary, so the skills a
build installs or prints always describe that build's commands.
`skills install` writes them for each selected runner (default `all`):

| Runner | `--project` (default): workflow directory | `--global`: user directory |
| --- | --- | --- |
| `claude` (Claude Code) | `.claude/skills/<name>/` | `~/.claude/skills/<name>/` |
| `codex` (Codex) | `.agents/skills/<name>/` | `~/.agents/skills/<name>/` |
| `opencode` | `.opencode/skills/<name>/` | `$XDG_CONFIG_HOME/opencode/skills/<name>/` (default `~/.config/opencode/skills/`) |

Project installs need the workflow directory (where `dispatch.json` lives).
Use `--global` when agents start inside the product repository or in task
worktrees outside the workflow directory. Each installed skill gets a
`.dispatch-skill.json` file recording the Dispatch version that installed it
and a SHA-256 of every file it wrote. A skill whose files changed since (you
edited it), a skill folder Dispatch did not install, or one installed by a
newer Dispatch is never overwritten: the command prints a warning and skips it,
and `--force` replaces it. Re-running `install` is idempotent.

`skills update` refreshes the copies that are already installed and older
than the binary, in the workflow directory (when run there) and globally,
with the same protection; it never installs missing skills. `skills list`
prints each bundled skill and its version, and every location with its status:
`missing`, `current`, `outdated` (installed by an older build), `modified`
(edited), `unmanaged` (not installed by Dispatch), or `newer`.

`skills show <name>` prints a bundled skill's `SKILL.md` to stdout from any
directory without needing a board. It is the fallback for runners without
native Agent Skills support (such as Antigravity/Gemini CLI or Cursor): a
launcher can embed the rules directly into an agent's prompt without checking
instruction files into the repository, for example:

```bash
dispatch skills show dispatch-work
```

All four `skills` subcommands accept `--json`; see
[ADR 0011](docs/adr/0011-skills-install.md) and
[ADR 0014](docs/adr/0014-skills-only-agent-instructions.md).

### Agents

```bash
dispatch agent create --name <name> --runner codex|claude|opencode [--model <name>] [--no-run-script] [--print-command]
dispatch agent list [--all] [--json]
dispatch agent show <name> [--json]
dispatch agent archive <name>
dispatch agent restore <name>
dispatch agent command <name> [--print-command]
dispatch agent session <name> [--session-id <id>|--clear-session] [--current-task <id>|--clear-current-task] [--last-workspace <id>|--clear-last-workspace]
dispatch agent resume <name> [--print-command]
```

`agent create` registers a named agent and creates local support files under
`.dispatch/agents/<name>/`. The generated prompt is named
`.dispatch/agents/<name>/<name>-PROMPT.md`; it contains only agent-specific
identity, the [agent skills](#agent-skills) to use (`dispatch-work` for task
work, `dispatch-plan` and `dispatch-board` when asked to plan or tend the
board, plus `dispatch skills install --global` and `dispatch skills show` if
they are missing), actor usage (its own name for its own work, `user` for
approvals the user decided), runner metadata, and local scratch/decision paths.
The workflow rules themselves live in the installed agent skills, not in
workflow-directory `AGENTS.md` / `CLAUDE.md` files. The optional `run.sh` starts
the selected runner from the workflow directory: `codex` and `claude` take the
prompt as a positional argument, and `opencode` takes it in `--prompt`, because
its first positional argument is the project directory. Agent directories are
for prompt material, scratch notes, and local decisions; task state remains in
Dispatch and product changes belong in task worktrees.

`agent archive <name>` hides an inactive agent from the default `agent list`
without deleting its record or history. Use `agent list --all` to include
archived agents and `agent restore <name>` to enable one again. Dispatch refuses
to archive agents that still have an active task or active workspace.

`agent session` records runner session metadata on the agent record. The
session ID identifies the runner session to resume, `current_task` records the
task the agent is actively handling, and `last_workspace` records the most
recent workspace used by that agent. Session IDs are trimmed before saving.
These fields are shown by `agent show` and are used by runner-specific resume
commands.

Codex and opencode session IDs are manual metadata. Both CLIs create their own
session ID when an interactive session starts, so Dispatch cannot assign one
ahead of time. After the user knows the session ID, record it with:

```bash
dispatch agent session frontend --session-id <runner-session-id>
dispatch agent show frontend
dispatch agent session frontend --clear-session
```

The same command can record the active task and workspace:

```bash
dispatch agent session frontend --session-id <runner-session-id> --current-task FE-01 --last-workspace FE-01
```

`agent resume` prints a runner-specific shell command for continuing an agent.
Codex agents use the stored session ID when present. Without one, Dispatch emits
a `codex resume --last` command scoped to the agent's remembered workspace when
`last_workspace` is set. opencode agents use `opencode --session <id>` when a
session ID is stored and `opencode --continue` otherwise, with the remembered
workspace passed as the project directory. Claude agents use `claude --resume`
when a session ID is stored. If no Claude session ID exists yet, Dispatch
generates a UUID, stores it on the agent, prints a `claude --session-id` startup
command, and writes the same command to `.dispatch/agents/<name>/resume.sh`.

### Workspaces

```bash
dispatch workspace create <task-id> --actor <agent-id> [--repo <path>] [--dir <path>] [--branch <name>] [--sequence]
dispatch workspace adopt <task-id> --actor <agent-id> --path <dir> [--branch <name>] [--sequence] [--external <tool>:<id>]
dispatch workspace list [--external <tool>:<id>] [--json]
dispatch workspace show <task-id-or-workspace> [--json]
dispatch workspace remove <task-id-or-workspace> [--force] [--delete-worktree]
dispatch workspace repair <task-id-or-workspace>
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
worktree. Use `--dry-run` to preview prune actions. If the workflow directory
and its worktrees move together, `doctor` identifies relocated worktrees and
`workspace repair <id>` updates the recorded repository and worktree paths.

#### Adopting A Worktree Created Elsewhere

`workspace adopt` records a Git worktree that another tool already created,
for example `orca worktree create`, without running `git worktree add`:

```bash
dispatch workspace adopt DE-12 --actor claude --path ../repo-orca-DE-12 --external orca:ws-42
```

The path must be an existing directory registered as a worktree of the
board's repository (`git worktree list`), not the repository itself, and not
already recorded. The task must be ready and unassigned, exactly as for
`workspace create`, and `--sequence` covers the same linear chains. The branch
is read from the worktree; `--branch` must match it, and is required only when
the worktree has a detached HEAD. `--external <tool>:<id>` stores an
`external_id` (a lowercase tool name, a colon, and an ID without spaces) so the
other tool can map the record back to its own workspace; an external ID can
belong to only one active record.

Adopted workspaces behave like created ones for `start`, `show`, `list`, and
the TUI, and `workspace show` prints `Origin: adopted` and the external ID.
Look up a single record by its external ID with:

```bash
dispatch workspace list --external orca:ws-42 [--json]
```

If no active workspace carries that external ID, `workspace list --external`
succeeds with an empty listing (`(no workspaces)` in text mode, `"workspaces": []`
in JSON mode).

The worktree belongs to the tool that made it, so Dispatch leaves it on disk:

- `workspace remove` clears only the record, even if the worktree is dirty or
  already gone. `--delete-worktree` removes the worktree as well, with the
  usual dirty-tree check (`--force` skips it).
- `workspace prune --done` releases done adopted records without touching
  their worktrees, and `workspace prune --stale` clears adopted records whose
  worktree directory no longer exists.
- `doctor` reports a missing adopted worktree as `workspace_path_missing`
  rather than a relocation, warns (`workspace_not_a_worktree`) when an adopted
  path still exists but is no longer a git worktree of the board repository,
  and warns (`workspace_branch_mismatch`) when its checked-out branch differs
  from the recorded branch. `workspace repair` refuses adopted records; remove
  the record and adopt the worktree at its new path instead.

`workspace adopt` adds a `workspace_adopt` entry to the task history and a
`dispatch.log` record. See [ADR 0003](docs/adr/0003-workspace-adopt.md) and
[ADR 0006](docs/adr/0006-adopted-workspace-followups.md) for the design choices.

### TUI

```bash
dispatch
dispatch tui
```

`dispatch` with no command opens the ncurses interface for the current workflow
board, equivalent to `dispatch tui`. It
is designed for a remote shell alongside tmux: Dispatch uses single-key
application controls and does not reserve tmux prefix combinations.

Main navigation:

```text
b          board
a or Tab   agents
w          workspaces
l          logs
Enter/i    inspect selected item
q or Esc   leave inspector
:          command palette
:q         quit
Ctrl+C     quit
?          help
```

The board view shows grouped task rows with state, review flag, commit
presence, and actor metadata. Use arrows or `j`/`k` to move. Filters are
single-key: `1` not-done, `2` all, `3` ready, `4` blocked, `5` review, `6`
doing, `7` done, and `R` attention. `G` cycles groups, `A` cycles actors, `/`
searches, and `c` clears secondary filters. Board, agents, workspaces, and logs
scroll with the selected row.

Task actions are available from the board and task inspector: `r` ready, `s`
start, `f` finish, and `v` review using `DISPATCH_ACTOR` or `user`. `e` edits
the selected task's title and description, and `m` moves it to another existing
group while preserving its ID. `n` opens a full-screen task creation form with
boxed fields, `+` creates a group, `>` adds a dependency to the inspected task,
`<` removes a dependency, and `d` opens the diff for the selected task when
commit metadata exists. The diff covers all of the task's recorded commits, is
rendered with forced color, and is shown through a pager (`$PAGER`, else `less`
or `more`) so it stays on screen until you quit the pager. When no pager is
installed the diff is shown without paging.
The task inspector shows the task's linked GitHub issue, and `o` there opens
the issue URL with `open` on macOS or `xdg-open` elsewhere. Linking and
clearing issues happens through `task add` and `task edit`.
In the task creation form, `Ctrl-O` opens an option picker for the active
field: on the Group field it lists existing groups (typing still creates a new
group), and on the Depends field it lists not-done tasks, excluding done tasks
and dependencies already selected. Inside the picker, `Up`/`Down` (or `j`/`k`)
move, `Enter` selects, and `Esc` cancels; reopen it to add more dependencies.
In the agent creation form, the Runner field takes `Space` to cycle through
`codex`, `claude`, and `opencode`, or `c`, `l`, and `o` to pick one directly.
Bottom-line prompt forms, including group creation, can be cancelled with `Esc`.
Dispatch shortens ncurses' default Escape-key delay so cancellation should feel
immediate while arrow keys remain usable.

The agents view lists enabled agents by default. `A` toggles enabled/all,
`z` archives or restores the selected agent, `y` copies the selected agent run
command with OSC 52 and also to a tmux buffer when available, `e` edits the
agent prompt in `$EDITOR`, `S` opens a session ID form from the agent inspector,
and `x` clears the stored session ID from the agent inspector. Terminals may
ignore OSC 52 clipboard writes; when no clipboard path is available, Dispatch
shows the run command in the status bar.

The workspaces view shows actor, path, branch, task state, workspace state, git
worktree presence, dirty status, and origin (`created` or `adopted`). The
workspace inspector also shows the origin and, for adopted records, the
external ID. `n` runs the existing workspace creation flow, `x` removes the
selected workspace after typing its task ID (noting in the prompt that only the
record is removed for adopted workspaces), `X` force removes after the same
confirmation, and `P` prunes done clean workspaces after typing `prune`.

The logs view reads `dispatch.log` and filters records by actor, command,
action, task, agent, or workspace. Press `F` to set a filter, `C` to clear it,
or `L` from a task or agent inspector to jump to related log records.

The command palette accepts compact commands:

```text
board | agents | workspaces | logs
q | quit
filter ready|blocked|review|doing|done|attention|all|not-done
group <prefix>|all
actor <agent>|all
task <task-id>
agent <name>
workspace <task-id>
log actor|command|action|task|agent|workspace <value>
ready|start|finish|review <task-id>
```

Noninteractive acceptance hooks are available for tests and terminal smoke
checks: `dispatch tui --smoke`, `--inspect-smoke`, `--filter-smoke`,
`--action-smoke`, `--diff-smoke`, `--agents-smoke`, `--workspaces-smoke`,
`--logs-smoke`, `--open-issue-smoke`, `--palette-smoke`, and
`--palette-complete-smoke`.
`--render-smoke <screen> <cols> <rows> [keys]` renders through an ncurses
virtual terminal and prints a normalized plain-text frame for headless tests.

Tmux keymap validation:

```text
No Dispatch action uses Ctrl-a, Ctrl-b, or tmux prefix chords.
Core movement uses arrows or j/k.
Major views use b, a/Tab, w, and l.
Mutations use printable single keys plus focused confirmations for destructive
workspace actions.
The command palette uses :, and quitting is explicit through :q or Ctrl+C,
leaving tmux prefix handling entirely to tmux.
```

Dispatch does not launch or manage tmux panes in v1. Run `dispatch tui` in one
pane and agent `run.sh` scripts or resume commands in other panes.

### Groups

```bash
dispatch group add <name> [--prefix XX] [--description <text>] [--actor <name>]
dispatch group ready <group> [--actor <name>] [--no-review]
dispatch group edit <prefix> --description <text> [--actor <name>]
dispatch group list
dispatch group show <prefix>
```

Groups are workflow lanes or topics. Task IDs are generated from the group
prefix, for example `DE-01`. Pass `--description` on `group add` or update it
later with `group edit` to record what work belongs in the group; `group list`
and `group show` inspect recorded scopes and per-group task counts (see
[group scope](docs/group-scope.md)). `group add` and `group edit` record
`--actor` in `dispatch.log`, or `user` when it is omitted; an agent that
creates or rescopes a group while planning passes its own name.

`group ready` marks every proposed task in the group as ready in one operation.
Tasks already blocked, assigned, doing, in review, or done are left unchanged.
Proposed tasks with unmet dependencies are approved but still display as blocked
until their blockers are done. If `--actor` is omitted, Dispatch records the
actor as `user`. Approval is the user's decision, so an agent that runs
`group ready` at the user's explicit instruction records `--actor user` (or the
person's own name); it never records its own work as `user`. Add `--no-review`
only when the user approves every readied task as safe to finish without a
review gate. If `--no-review` is used on a group that is already readied, it
also applies to ready or blocked tasks in that group that have not been started
yet.

### Tasks

```bash
dispatch task add <group> <title> [--description <text>] [--actor <name>] [--no-review] [--issue <ref>]
dispatch task add <group> --from-issue <ref> [--actor <name>] [--no-review]
dispatch task edit <id> [--title <text>] [--description <text>] [--issue <ref>|--clear-issue] [--actor <name>]
dispatch task move <id> <group> [--actor <name>]
dispatch task delete <id> [--force]
dispatch task priority <id> <value> [--actor <name>]
dispatch show <id> [--json]
dispatch list [all] [group] [--json]
```

Tasks require a group and a title. Titles are human-readable labels and should
not include generated Dispatch IDs such as `DE-01`; Dispatch prints the ID in
its own column. By default, tasks require review after they are finished.
`--actor` records who created the proposed task in task history and
`dispatch.log`; it defaults to `user`. Use `--no-review` only when the task can
safely complete and unblock the next task without human acceptance.

`task edit` changes a task's title, description, or both without changing its
ID, group, workflow state, dependencies, commit references, or prior history.
The edit is appended to task history using the supplied `--actor`, which
defaults to `user`. Pass an empty description to clear it.

#### Linked GitHub issues

A task can link one GitHub issue, so planned work stays traceable to where it
is tracked. Link it when adding the task with `task add --issue <ref>`, change
it with `task edit <id> --issue <ref>`, and remove it with
`task edit <id> --clear-issue`. Dispatch stores the link canonically as
`owner/repo#number` and accepts these forms:

```text
123                                        bare number
#123                                       bare number
acme/widgets#123                           canonical form
https://github.com/acme/widgets/issues/123 issue URL
```

A bare number resolves against the GitHub `origin` remote of the board's
repository, read locally with `git remote get-url origin`. Linking never makes
a network call or runs `gh`, so Dispatch does not check that the issue exists.
Malformed refs, pull request URLs, non-GitHub URLs, and bare numbers on a repo
without a GitHub `origin` are rejected without changing the board.

Linking a different issue replaces the current one. Each change appends an
`issue_link` or `issue_unlink` entry to task history and `dispatch.log`, with
the ref in the history note and in the log record's `context.issue`.

Several tasks may link the same issue: an issue usually describes a feature
that is split into several tasks of one group. Dispatch treats those tasks as
one piece of work ([ADR 0013](docs/adr/0013-shared-issue-links.md)): `status`
warns offline (`duplicate_issue`) only when the tasks linking an issue are in
different groups, which is more likely a mistake. Pass `--check-issues` to
`status` to opt into checking linked issue states on GitHub via `gh` (warning
when every task linking an issue is done but the issue is still open, or an
open task's issue is closed), and pass `--close-issue` to `review` to close
each accepted task's linked issue with a comment once no other task linking it
is still open; see [ADR 0008](docs/adr/0008-check-linked-issues.md). `show`
prints the linked issue, its URL, and any other tasks linking it
(`Issue: acme/widgets#21 (https://github.com/acme/widgets/issues/21) (also
ST-02, ST-03)`), `list` adds `issue:#123` to the task line, and JSON task
objects carry an `issue` object (see [JSON Output](#json-output)).

To create a task directly from an existing GitHub issue, use `--from-issue`:

```bash
dispatch task add DE --from-issue 5
dispatch task add DE --from-issue acme/widgets#12 --actor codex --no-review
```

`--from-issue` accepts the same ref forms as `--issue` (bare numbers resolve
against the board repo's GitHub `origin` remote first), then fetches the issue
with `gh issue view --json title,body,url`. It uses the issue's title as the
task title, its body as the task description (normalizing CRLF line endings to
LF), and links the canonical ref from the returned URL. Pull requests are
rejected. `--from-issue` cannot be combined with an explicit title,
`--description`, or `--issue`; use `task add <group> <title> --issue <ref>`
for custom text, or `task edit` afterwards. Missing `gh`, missing
authentication (`gh auth login`), and nonexistent issues fail cleanly without
changing the board (see [ADR 0007](docs/adr/0007-task-from-github-issue.md)).

`task move` changes the task's group while preserving its ID, workflow state,
dependencies, commit references, workspace associations, agent session links,
and history. The move is appended to task history using the supplied `--actor`,
which defaults to `user`. Because IDs are stable, a moved task can keep an ID
prefix from its original group, such as `DE-02` while listed in group `QA`.

Deleting a task with dependents is rejected unless `--force` is used. Forced
delete also removes that task from other tasks' dependency lists.

`task priority` sets a task's integer priority from `-99` to `99` (default `0`).
Higher values surface first in ready listings (`ready`, the Ready section of
`status`, and `finish`'s `Next ready tasks:`), with ties keeping board order.
Priority orders only ready tasks — dependencies and workflow state still take
precedence, and `list` and `--json` task arrays always keep board order.
Changing priority appends a `priority` entry to task history using `--actor`
(default `user`) and a `task`/`priority` record to `dispatch.log`; `show`
always prints `Priority: <n>`. `task priority` does not accept `--json`.

`list` prints active tasks once, grouped by Dispatch group and in task order.
Completed tasks are hidden by default. If every task in a group is completed,
the group prints a single `(done)` marker instead of every completed task row.
Dependencies are shown on the task line as `depends_on:A,B`. The output can be
limited to one group by ID, prefix, or name. Use `list all` to include
completed tasks, optionally scoped to a group with `list all <group>`.
Tasks with recorded commits show a `commits:N` summary on their task line,
tasks with a linked issue show `issue:#N`, and tasks with a non-zero priority
show `priority:N`.

When stdout is a terminal, `list` uses ANSI color to distinguish group headings,
task IDs, states, and metadata. Set `FORCE_COLOR=1` to force color in captured
output, or `NO_COLOR=1` to disable it.

### Dependencies

```bash
dispatch dep add <dependency-id> <dependent-id> [--actor <name>]
dispatch dep remove <dependency-id> <dependent-id> [--actor <name>]
dispatch blocked [--json]
```

`dep add DE-01 DE-02` means `DE-02` depends on `DE-01`; the first argument is
the prerequisite, and the second argument is the task that waits for it. A task
that has been approved for work is blocked when any dependency is not done.
Tasks that have not been readied yet still display as proposed, even when they
have unmet dependencies. Dependency cycles are rejected.

`dep add` and `dep remove` record the change in `dispatch.log` and in the
dependent task's history (`dependency_add` or `dependency_remove`, with the
dependency ID as the note) under `--actor`, or `user` when it is omitted.
Re-adding an existing dependency changes nothing and adds no history entry.

### Commit References

```bash
dispatch commit add <task-id> <sha> [--actor <name>]
dispatch commit list <task-id>
dispatch commit show <task-id>
```

Commit references attach one or more Git SHAs to a Dispatch task. `show` also
prints recorded commits, which gives review tooling and the TUI a reliable
task-to-diff link.

### Lifecycle

```bash
dispatch ready [--json]
dispatch ready <id> [<id> ...] [--actor <name>] [--no-review]
dispatch reviews [--json]
dispatch proposed [--json]
dispatch start <id> --actor <name>
dispatch finish <id> --actor <name>
dispatch review <id> [<id> ...] [--actor <name>] [--close-issue]
dispatch unassign <id> --actor <name>
```

The normal lifecycle is:

```text
proposed -> ready -> doing -> review -> done
```

Tasks without review gates go directly from `doing` to `done` when finished:

```text
proposed -> ready -> doing -> done
```

Use `ready` with no ID to list work that can be started, ordered by descending
`priority` (see `task priority`) with board order preserved on ties. If no task
is ready, it summarizes waiting review, proposed, and blocked queues. Use
`ready <id>` (one or more IDs) to approve proposed tasks for work; re-readying
a `doing`, `paused`, or `review` task is an explicit revert that clears its
assignment and start provenance (`assigned_to`, `started_by`, `started_at`) so
it can be started cleanly again. Omitted actors on user-facing approval and
review commands default to `user`. Readying and accepting are the user's
decisions: an agent acting on the user's explicit instruction records
`--actor user` (or the person's own name), and uses its own name only for its
own work or when the user assigned it as the reviewer (see the
`dispatch-board` skill). Add `--no-review` when
approving a task that can safely move directly from `doing` to `done` after the
agent finishes it.

`reviews` lists tasks waiting for acceptance. `proposed` lists tasks waiting
for approval.

`start` assigns the task to an actor and prevents a second actor from starting
the same task. `finish` records the completing actor and moves the task to
`review` or `done`. `review` accepts one or more review tasks as done; add
`--close-issue` to also close each task's linked GitHub issue via `gh` with a
comment citing the task, actor, and recorded commits (recording an `issue_close`
entry in `dispatch.log` and warning without failing the review if `gh` cannot
close the issue). An issue that other tasks still link while they are not done
is left open: `review` prints `Left <issue> open for <task>: still linked by
<ids>` (an `issue_left_open` warning under `--json`) and does not call `gh`.

`unassign` clears an abandoned or stuck task assignment (`assigned_to`,
`started_by`, and `started_at`) without editing `dispatch.json`. `--actor` is
required. It is allowed when the task is assigned and in `proposed`, `ready`,
`blocked`, `doing`, or `paused`: a `doing` (or `paused`) task returns to
`ready` (or `blocked` if dependencies are unmet), while a stale assignment on a
`proposed`, `ready`, or `blocked` task is cleared in place. `unassign` refuses
`review` and `done` tasks so completion provenance is not discarded, and
refuses tasks with no assignee. It appends an `unassigned` entry to task
history and an `unassign` record to `dispatch.log`, leaves workspaces
untouched, and accepts `--json` (see
[JSON Results For Mutating Commands](#json-results-for-mutating-commands)). See
[task state invariants](docs/task-state-invariants.md) for the full assignment
contract and `normalize` repair procedure.

The board model also defines a reserved `paused` state (`DISPATCH_STATE_PAUSED`),
which appears as a `paused:0` counter in `status`, `group show`, and JSON
`summary.states`. No CLI command or TUI action transitions a task into `paused`
(`dispatch pause` is not implemented). If a board already contains a `paused`
task, Dispatch loads it and treats it as an in-flight assigned state in
`doctor` and `normalize`, and `ready <id>` or `unassign <id>` clears its
assignment and returns it to `ready` (or `blocked` if dependencies are unmet).

## JSON Output

The read commands `list`, `show`, `ready`, `blocked`, `reviews`, `proposed`,
`status`, `doctor`, `agent list`, `agent show`, `workspace list`,
`workspace show`, and `log` accept `--json`. The option may appear anywhere
after the command name (after the subcommand for `agent` and `workspace`):

```bash
dispatch list --json
dispatch list all Development --json
dispatch show --json DE-01
dispatch ready --json
dispatch status --json
dispatch status --check-issues --json
dispatch agent list --all --json
dispatch workspace show DE-01 --json
dispatch log --task DE-01 --limit 20 --json
```

`ready --json` without a task ID is the read-only queue form; with task IDs,
`ready <id>... --json` is a mutating command (see
[JSON Results For Mutating Commands](#json-results-for-mutating-commands)).

A successful JSON command writes one versioned JSON object and a trailing
newline to stdout and nothing to stderr. It does not include headings, color
escapes, empty-queue hints, or other human text. Errors keep their existing
non-zero exit status, write nothing to stdout, and write a coded JSON error
document to stderr (except `doctor --json` when the board itself cannot be
loaded, which emits its `board_load_failed` check on stdout with empty stderr).
Omitting `--json` retains the normal human-readable output.

Every task response uses the same top-level envelope:

```json
{
  "schema_version": 1,
  "command": "ready",
  "board": {"name": "Dispatch", "repo_path": "repo", "groups": []},
  "query": {
    "task_id": null,
    "group": null,
    "include_done": null,
    "states": ["ready"]
  },
  "summary": {
    "total": 3,
    "returned": 1,
    "states": {
      "proposed": 0,
      "ready": 1,
      "blocked": 1,
      "doing": 1,
      "review": 0,
      "done": 0,
      "paused": 0
    },
    "agents": {"enabled": 1, "archived": 0, "with_current_task": 1},
    "workspaces": {"active": 1, "removed": 0}
  },
  "tasks": [],
  "warnings": []
}
```

`summary.total` and `summary.states` describe the complete board;
`summary.returned` is the length of the filtered `tasks` array. `show` returns
its task as a one-element array. `status` returns ready and review tasks while
its summary still counts every task. Empty selections use an empty array.

Task objects consistently include identity and lifecycle fields, direct
dependencies and dependents, unmet blockers, recorded commits, the active
workspace when present, Unix timestamps, and history. The public `state` is
the state shown by the CLI after dependencies are evaluated; `stored_state` is
the persisted lifecycle state. Unset actor, workspace, and timestamp fields are
JSON `null`.

Task objects also carry `issue`: `null`, or the linked GitHub issue as
`{"repo": "acme/widgets", "number": 12, "url": "https://github.com/acme/widgets/issues/12"}`.

Among task read commands, only `status` populates `warnings`. Warning objects
contain a stable `code`, a human-readable `message`, and nullable `task_id` and
`agent` fields (plus `issue` on per-task issue check warnings). Version 1
reports missing commits, assignment/state mismatches, agent sessions that
reference missing tasks, and tasks that link the same issue as an earlier task
in a different group, plus GitHub issue drift and shared `gh` error warnings when `--check-issues` is
passed. Mutating commands also use `warnings` for non-fatal operational
warnings (such as a failed `dispatch.log` append, scaffolding failure during
`init`, or a non-git path on `repo set`) instead of printing stray text to
stderr.

Agent, workspace, and log responses keep `schema_version`, `command`,
`board` (`name` and `repo_path`), `query`, and `summary`, and put their
records in `agents`, `workspaces`, or `records`:

```json
{
  "schema_version": 1,
  "command": "agent list",
  "board": {"name": "Dispatch", "repo_path": "repo"},
  "query": {"name": null, "include_archived": false},
  "summary": {"total": 2, "returned": 1, "enabled": 1, "archived": 1, "with_current_task": 1},
  "agents": [{"name": "codex", "runner": "codex", "archived": false, "assigned_tasks": ["DE-02"], "workspaces": ["DE-02"]}]
}
```

Agent objects carry the stored agent fields plus the tasks assigned to the
agent and its live workspaces. Workspace records include `adopted` and
`external_id` for worktrees recorded with `workspace adopt`. Log records are
the stored `dispatch.log` records plus their `line` number; see
[Dispatch Log](#dispatch-log).

Consumers should ignore unknown fields. Adding optional fields is compatible;
removing a field or changing its type or meaning requires a new
`schema_version`. The complete field definitions and command selection rules
are in [the JSON output contract](docs/json-output-contract.md).

### JSON Results For Mutating Commands

All mutating commands also accept `--json`, anywhere after the subcommand (or
after the command name for commands without one):

```text
init   repo set   normalize
task add|edit|move|delete|priority   group add|ready|edit   dep add|remove
ready <id>...   start   finish   review   unassign   commit add
agent create|archive|restore|session|command|resume
workspace create|adopt|remove|repair|prune
skills install|update
```

On success they print the same envelope as the read commands (workspace
commands use the workspace envelope and agent commands use the agent envelope;
`skills list|show|install|update` use their own skills envelope, which needs no
board) plus `warnings` and a `result` object, so a tool learns the outcome
without a second query:

```bash
dispatch finish DE-01 --actor codex --json
```

```json
"result": {
  "action": "finish",
  "changed": true,
  "task": "DE-01",
  "previous_state": "doing",
  "new_state": "done",
  "targets": [{"task": "DE-01", "previous_state": "doing", "new_state": "done"}],
  "newly_ready": ["DE-02"],
  "review_required": false
}
```

`task add --json` returns the generated ID in `result.task` and `tasks`.
`changed` is `false` for successful no-ops, such as recording a commit that is
already recorded. `newly_ready` lists every task that became ready.

With `--json`, any failing command (read or mutating) keeps its non-zero exit
status, prints nothing on stdout, and prints a JSON error document on stderr:

```json
{
  "schema_version": 1,
  "command": "start",
  "error": {
    "code": "invalid_state",
    "message": "Cannot start DE-02: task is blocked, not ready",
    "task_id": "DE-02"
  }
}
```

Match on `error.code`; the message is human text and may change. The actions,
extra fields, operational warning codes, and full list of error codes are in
[the JSON output contract](docs/json-output-contract.md#mutation-results).

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
dispatch agent create --name opencode-backend --runner opencode
dispatch agent create --name claude-frontend --runner claude

dispatch workspace create SE-01 --actor codex-server
dispatch workspace create BE-01 --actor opencode-backend
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

## Agent Skills

The `skills/` directory holds three [Agent Skills](https://agentskills.io)
that teach coding agents to use Dispatch by job, wherever their session
starts:

| Skill | Use it to |
| --- | --- |
| [`dispatch-plan`](skills/dispatch-plan/SKILL.md) | plan, break down or organise work into Dispatch groups, tasks, dependencies and review gates |
| [`dispatch-work`](skills/dispatch-work/SKILL.md) | start, continue or finish a Dispatch task in its own workspace, with one commit per task |
| [`dispatch-board`](skills/dispatch-board/SKILL.md) | check, review or tidy the board from the person's own session |

Each skill stands alone as the sole source of Dispatch agent instructions:
every skill carries all seven critical rules (CLI only, never read or edit
`dispatch.json`, an explicit actor on every change, one commit per task),
explains how to find the workflow directory both from `repo/` and from a task
worktree outside the workflow directory, points to the other two skills, and
applies to any command run against a Dispatch board. `dispatch init` installs
them in the workflow directory for Claude Code, Codex and opencode;
`dispatch skills install --global` installs them for every session of a runner
(needed when agents run in task worktrees outside the workflow directory),
`dispatch skills update` keeps installed copies in step with the binary, and
`dispatch skills show <name>` prints a bundled skill's `SKILL.md` to stdout so
launchers for runners without skill support can pass the rules directly in an
agent's prompt (see [Skills](#skills)). `tests/check_skills.sh`, run in CI,
checks their front matter and that every command they mention matches
`dispatch --help` and every command is covered; see
[ADR 0010](docs/adr/0010-agent-skills.md) and
[ADR 0014](docs/adr/0014-skills-only-agent-instructions.md).

### Removing Older Instruction Files

Boards created before [ADR 0014](docs/adr/0014-skills-only-agent-instructions.md)
have `AGENTS.md` and `CLAUDE.md` files written by `dispatch init` (either the
older ~450-line copy or the ADR 0012 slim template). Once the agent skills are
installed, those files are no longer needed and can be removed:

1. Install the skills: run `dispatch skills install` from the workflow
   directory, and `dispatch skills install --global` if agents start inside the
   product repository or in task worktrees outside the workflow directory.
2. Remove `AGENTS.md` and `CLAUDE.md` if they hold only the Dispatch template.
   `dispatch doctor` prints an informational note (`instructions_redundant`)
   for files that match only a Dispatch template and stays quiet for files with
   your own additions — it never deletes or rewrites either file. If you added
   project-specific rules of your own, keep those and delete only the Dispatch
   sections.
3. Run `dispatch normalize` to regenerate older agent prompts (`doctor` reports
   those as `agent_prompt_outdated`), then `dispatch doctor` to confirm there
   are no warnings.

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

## Dispatch Log

Successful mutating Dispatch commands append audit records to `dispatch.log` in
the workflow directory, next to `dispatch.json`. Failed validation and rejected
mutations do not append records. The log is append-only JSON Lines: one UTF-8
JSON object per line, with no surrounding array. Readers should ignore unknown
fields so the format can grow without breaking older tooling.

Each record contains these fields:

- `version`: integer log schema version. The initial version is `1`.
- `timestamp`: UTC timestamp in RFC 3339 format, such as
  `2026-06-19T12:34:56Z`.
- `actor`: actor responsible for the workflow change. Commands with an omitted
  user-facing actor record `user`; agent ownership commands should pass the
  agent's explicit actor name.
- `command`: CLI command family that caused the change, such as `task`,
  `ready`, `start`, `finish`, `review`, `dep`, `group`, `workspace`, or
  `normalize`.
- `action`: specific action within the command, such as `add`, `delete`,
  `ready`, `start`, `finish`, `review`, `dependency_add`, or
  `workspace_create`.
- `targets`: object containing relevant stable IDs. Use keys such as `task`,
  `group`, `dependency`, `dependent`, `agent`, and `workspace`.
- `outcome`: command outcome. Dispatch currently writes `success` records after
  completed mutations.
- `message`: short human-readable result or error summary.

Records may include a `context` object for structured details that support
auditing but are not primary IDs. Examples include `requires_review`,
`no_review`, `force`, `sequence`, `repo_path`, `workspace_path`,
`workspace_branch`, `previous_state`, and `new_state`.

Example records:

```json
{"version":1,"timestamp":"2026-06-19T12:34:56Z","actor":"user","command":"ready","action":"ready","targets":{"task":"DE-01"},"outcome":"success","message":"Readied DE-01","context":{"no_review":false,"new_state":"ready"}}
{"version":1,"timestamp":"2026-06-19T12:35:20Z","actor":"codex","command":"start","action":"start","targets":{"task":"DE-01"},"outcome":"success","message":"Started DE-01","context":{"previous_state":"ready","new_state":"doing"}}
```

The log complements task history stored in the board. Task history remains the
authoritative per-task lifecycle summary; `dispatch.log` is the broader command
audit trail for successful dependency, group, task, lifecycle, agent, and
workspace changes.

Read the log with `dispatch log`, which prints one record per line, oldest
first:

```bash
dispatch log --limit 20
dispatch log --task DE-01
dispatch log --agent codex --command finish --json
```

`--task`, `--actor`, `--agent`, `--workspace`, `--command`, and `--action`
filter on the same fields as the TUI logs view (`--agent` matches the
record's target agent or its actor), but match exactly. Filters combine, and
`--limit <n>` keeps the newest `n` matches. With `--json`, records come back
in a `records` array with their line number in the file, and `summary`
counts the total, matched, returned, and skipped lines. Blank lines are
ignored; lines that are not valid version 1 records are skipped and reported
as warnings, on stderr or in the JSON `warnings` array, without failing the
command. The full rules are in
[the JSON output contract](docs/json-output-contract.md#log-responses), and
the design in [ADR 0004](docs/adr/0004-json-reads-agents-workspaces-log.md).

Because each line is a standalone JSON object, JSON Lines-aware tools can also
read the file directly without loading the board.

Actor attribution follows the same rules as the workflow commands. User-facing
approval and review commands that omit `--actor` record `user`. Agent ownership
and execution commands, such as `workspace create`, `start`, and `finish`,
should pass the agent's explicit actor name so the audit trail identifies the
actor that performed the change.

Agents must still use the Dispatch CLI for workflow changes. `dispatch.log` is
an audit artifact, not an alternate command interface; do not edit it directly,
and do not edit `dispatch.json` to create log entries.

## Tests

Run the automated CLI tests:

```bash
tests/run_cli_tests.sh
tests/run_tui_golden_tests.sh
```

The test runner builds Dispatch, creates temporary boards under `$TMPDIR`
(default `/tmp`), and checks command behavior through the CLI. Both scripts run
the freshly built `./dispatch`, never one found on `PATH`.

The CLI suite runs the TUI golden checks and verifies that a deliberately
mutated frame is rejected. The standalone golden runner creates a deterministic
workflow fixture and compares normalized `100x30` frames for the main views,
forms, and help overlay. Temp paths render as `<tmp>` before layout, so the
committed frames match on Linux and macOS whatever the temp directory
([ADR 0005](docs/adr/0005-portable-tui-goldens.md)). Refresh the committed
frames after an intentional rendering change with:

```bash
UPDATE_GOLDENS=1 tests/run_tui_golden_tests.sh
```

The CLI suite also compares `init` and `doctor` output and generated agent
prompts against `tests/golden/cli/`; refresh those after an intentional output
or prompt change with `UPDATE_GOLDENS=1 tests/run_cli_tests.sh` (which also
refreshes the TUI frames).

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
- global priority-first sorting (`list` stays in group and workflow order; optional `task priority` orders only the `ready` queue)

Use Dispatch groups, dependencies, review gates, and lifecycle commands instead.
