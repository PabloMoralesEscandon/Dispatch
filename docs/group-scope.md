# Group Scope And Reuse Contract

This contract defines how groups record their scope, how agents inspect
existing groups, and how planning instructions make group reuse the default.
It exists because agents kept creating new groups or misplacing tasks:
groups carried no recorded statement of what belongs in them, and the only
way to see the groups at all was buried in the full `dispatch list` output.

## The Scope Field

`DispatchGroup` gains a `description` field — one or two sentences stating
what work belongs in the group.

- Stored as a `"description"` string on each group record. Boards written
  before this field exist load cleanly: a missing or `null` description is
  treated as empty, never an error.
- Saved boards always write the field (empty string when unset).
- The board JSON envelope's `board.groups` objects gain the same
  `description` member (additive to `schema_version` 1; consumers that
  ignore unknown members are unaffected).

## CLI Surface

Set at creation:

```text
dispatch group add <name> --prefix <PREFIX> [--description <text>]
```

Set or update on an existing group:

```text
dispatch group edit <prefix> --description <text>
```

`group edit` takes the group prefix (the stable ID agents already use in
task IDs), records the acting change in the description field only — name
and prefix stay immutable through this command — and prints the updated
scope. Editing a missing group is an error naming the prefix.

Inspect before planning:

```text
dispatch group list
dispatch group show <PREFIX>
```

`group list` prints one line per group in board order: prefix, name, open
and total task counts, and the description. A group with no description
shows `-` so the gap is visible and fixable:

```text
DE   Development       3 open / 12 tasks   Core CLI development work
QOL  QoL               0 open / 4 tasks    -
```

`group show <PREFIX>` prints one group's detail: name, prefix, description,
task counts by presentation state, and the group's ready tasks so a planner
can see at a glance whether the group is active. Unknown prefixes are an
error.

## Planning Instructions

The Planning Work section of the embedded CLAUDE.md/AGENTS.md templates
changes so reuse is the default:

1. First step: run `dispatch group list` and read the scopes.
2. If an existing group's scope covers the planned work, add tasks there.
3. Create a new group only when no recorded scope fits, and give the new
   group a `--description` at creation so the next planner can reuse it.
4. A task's group must match that group's recorded scope; creating a
   near-duplicate of an existing group is a planning mistake.

The worked example in the templates leads with `dispatch group list` and
shows reuse; `dispatch group add` appears as the deliberate exception, with
`--description` in the example invocation.

## Backfill

Existing boards are healed operationally, not by migration: once the
tooling lands, active groups get a short scope via `dispatch group edit`.
Done-and-closed groups may stay undescribed; the listing's `-` marks them
honestly.
