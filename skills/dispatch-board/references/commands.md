# Dispatch command reference

Use `dispatch` from the workflow directory, or the absolute path to the binary
if working from another directory. `dispatch --help` prints the same command
families. Commands marked `[--json]` print a versioned JSON document instead of
text; every mutating command also accepts `--json` and then returns a `result`
object (see `docs/json-output-contract.md` in the Dispatch repository).

## Board

```bash
dispatch init repo
dispatch repo
dispatch repo set <path>
dispatch status [--check-issues] [--json]
dispatch doctor [--json]
dispatch normalize
dispatch log [--task <id>] [--actor <name>] [--agent <name>] [--workspace <id>] [--command <name>] [--action <name>] [--limit <n>] [--json]
dispatch completion install fish|bash|zsh
dispatch tui
```

## Agent skills

```bash
dispatch skills list [--project | --global] [--runner claude|codex|opencode|all] [--json]
dispatch skills show <name> [--json]
dispatch skills install [--project | --global] [--runner claude|codex|opencode|all] [--force] [--json]
dispatch skills update [--project | --global] [--runner claude|codex|opencode|all] [--force] [--json]
```

## Reading tasks

```bash
dispatch list [all] [group] [--json]
dispatch show <TASK-ID> [--json]
dispatch ready [--json]
dispatch reviews [--json]
dispatch proposed [--json]
dispatch blocked [--json]
```

## Lifecycle

```bash
dispatch ready <TASK-ID> [<TASK-ID> ...] --actor <name> [--no-review]
dispatch start <TASK-ID> --actor <agent-id>
dispatch finish <TASK-ID> --actor <agent-id>
dispatch review <TASK-ID> [<TASK-ID> ...] --actor <user-or-reviewer-id> [--close-issue]
dispatch unassign <TASK-ID> --actor <name>
```

## Groups

```bash
dispatch group add <name> --prefix <PREFIX> [--description <text>] [--actor <agent-id>]
dispatch group ready <group> --actor <name> [--no-review]
dispatch group edit <prefix> --description <text> [--actor <agent-id>]
dispatch group list
dispatch group show <prefix>
```

## Tasks and dependencies

```bash
dispatch task add <group> <title> [--description <text>] [--actor <agent-id>] [--no-review] [--issue <ref>]
dispatch task add <group> --from-issue <ref> [--actor <agent-id>] [--no-review]
dispatch task edit <TASK-ID> [--title <text>] [--description <text>] [--issue <ref>|--clear-issue] [--actor <agent-id>]
dispatch task move <TASK-ID> <group> [--actor <agent-id>]
dispatch task delete <TASK-ID> [--force]
dispatch task priority <TASK-ID> <value> [--actor <agent-id>]
dispatch dep add <dependency-id> <dependent-id> [--actor <agent-id>]
dispatch dep remove <dependency-id> <dependent-id> [--actor <agent-id>]
```

## Commits

```bash
dispatch commit add <TASK-ID> <sha> [--actor <agent-id>]
dispatch commit list <TASK-ID>
dispatch commit show <TASK-ID>
```

## Agents

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

## Workspaces

```bash
dispatch workspace create <TASK-ID> --actor <agent-id> [--repo <path>] [--dir <path>] [--branch <name>] [--sequence]
dispatch workspace adopt <TASK-ID> --actor <agent-id> --path <dir> [--branch <name>] [--sequence] [--external <tool>:<id>]
dispatch workspace list [--external <tool>:<id>] [--json]
dispatch workspace show <TASK-ID-or-workspace> [--json]
dispatch workspace remove <TASK-ID-or-workspace> [--force] [--delete-worktree]
dispatch workspace repair <TASK-ID-or-workspace>
dispatch workspace prune [--done] [--stale] [--dry-run]
```
