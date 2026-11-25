# 0011: Ship the agent skills in the binary and install them per runner

- **Status:** Accepted
- **Date:** 2026-11-25

## Context

ADR 0010 added three Agent Skills (`dispatch-plan`, `dispatch-work`,
`dispatch-board`) under `skills/`, but left installing them to the person:
copy or link the folders into a runner's skills directory. Copies made that
way drift: the binary gains or renames a command and the installed skill keeps
describing the old one. Dispatch supports three runners (`claude`, `codex`,
`opencode`), each with its own skills directories. Issue #22 asks for the
skills to ship with the binary and for commands to install, list and update
them, with `init` and `doctor` taking part.

## Decision

**Embed the whole `skills/` tree.** `nob` already turns `AGENTS.md` and
`CLAUDE.md` into byte arrays in the generated `build/dispatch_templates.c`.
It now also walks `skills/` (sorted, hidden entries skipped) and emits every
file, `SKILL.md` and reference files alike, into a
`dispatch_skill_files[]` table of `{skill, path, data, size}`. A build
therefore always installs exactly the skills that describe its own commands,
and the install location needs no source checkout. Nothing in the table is
hand-maintained: adding a skill or a reference file is enough.

**Runner locations.** Checked against each runner's documentation on
2026-11-25:

| Runner | Project (workflow directory) | Global (user) | Source |
| --- | --- | --- | --- |
| Claude Code | `.claude/skills/<name>/SKILL.md` | `~/.claude/skills/<name>/SKILL.md` | <https://code.claude.com/docs/en/skills> |
| Codex | `.agents/skills/<name>/SKILL.md` (scanned from the working directory up to the repository root) | `$HOME/.agents/skills/<name>/SKILL.md` | <https://developers.openai.com/codex/skills> |
| opencode | `.opencode/skills/<name>/SKILL.md` (walks up to the git root) | `~/.config/opencode/skills/<name>/SKILL.md` | <https://opencode.ai/docs/skills> |

All three runners have a skills mechanism, so none is skipped. For opencode
the global directory honours `XDG_CONFIG_HOME` (default `~/.config`), as
opencode's own config does. Codex documents `.agents/skills` rather than a
`.codex` path, and also has admin (`/etc/codex/skills`) and bundled system
scopes that Dispatch does not write to. opencode also reads
`.claude/skills` and `.agents/skills`; with `--runner all` it therefore sees
three identical copies, which it tolerates (a later registration of the same
skill name wins). `--runner opencode` alone is enough for an opencode-only
setup. If a runner ever drops its skills mechanism, its row in the runner
table in `src/dispatch_cli_skills.c` is the single place to change.

**Commands.**

- `dispatch skills install [--project | --global] [--runner R] [--force]`
  writes the skills for the selected runners (default `all`). `--project` is
  the default and needs the workflow directory (a `dispatch.json` in the
  current directory), so skills are not scattered into arbitrary folders.
- `dispatch skills update [--project | --global] [--runner R] [--force]`
  refreshes copies that are already installed, in the workflow directory when
  run there and globally; it never installs missing skills.
- `dispatch skills list [--project | --global] [--runner R]` prints each
  bundled skill with its version and the status of every location.
- `dispatch init` runs the project install for every runner after scaffolding
  `AGENTS.md` and `CLAUDE.md` and prints one line saying where. Re-running
  `init` on an existing board installs missing or outdated skills and leaves
  edited ones alone.
- `dispatch doctor` warns `skills_missing` when a bundled skill is installed
  for no runner anywhere (fix: `dispatch skills install`) and
  `skills_outdated` for each unedited copy an older build installed (fix:
  `dispatch skills update`, with `--global` for global copies). Missing
  copies for a runner the person does not use are not warned about, so a
  Claude-only setup stays quiet.
- All three `skills` commands accept `--json` with coded errors, a `warnings`
  array and, for `install` and `update`, a `result` object.

**Version stamp: a sidecar manifest.** Each installed skill folder gets a
`.dispatch-skill.json`:

```json
{
  "skill": "dispatch-board",
  "dispatch_version": "0.1.0",
  "bundle_hash": "<sha256 of the bundled files>",
  "files": { "SKILL.md": "<sha256>", "references/commands.md": "<sha256>" }
}
```

The alternative, a `metadata` entry in the `SKILL.md` front matter, is valid
Agent Skills syntax, but it would make the installed `SKILL.md` differ from
the bundled one, so the edit check would have to hash the file with the stamp
taken out again, and rewriting the user's front matter on every update risks
damaging a field they added. A sidecar keeps `SKILL.md` byte-identical to the
source (always valid front matter, easy to diff) and holds the hashes too.
Runners load `SKILL.md` and the files it links; a hidden extra file is
ignored.

`DISPATCH_VERSION` (`include/dispatch.h`, shown by `dispatch --version`) is
the release stamp; Dispatch had no version before this ADR and starts at
`0.1.0`. "Older than the binary" is decided by content, not by the version
alone: a copy is `outdated` when its recorded `bundle_hash` differs from the
hash of the skills in this build, so a skill edit in the repository reaches
installed copies even when nobody bumps the version. The version only breaks
the tie in one direction: a copy stamped with a higher version than the
binary is `newer`, and is not downgraded without `--force`.

**Edit protection.** Before writing, each location is classified:

| Status | Meaning | `install` / `update` |
| --- | --- | --- |
| `missing` | no folder | `install` writes it; `update` ignores it |
| `current` | stamp matches this build, files unedited | unchanged |
| `outdated` | older stamp, files unedited | rewritten |
| `modified` | a file listed in the manifest is missing or its SHA-256 changed | skipped with a `skill_modified` warning; `--force` replaces it |
| `unmanaged` | folder without a Dispatch manifest | skipped (`skill_unmanaged`); `--force` replaces it |
| `newer` | stamped by a newer Dispatch | skipped (`skill_newer`); `--force` replaces it |

Skipping is a warning, not a failure: the command exits 0 and says how to
replace the copy. Files a previous install wrote that the new bundle no longer
ships are removed; files the user added are never touched. Files are written
through a temporary file and renamed, and the manifest is written last, so an
interrupted install reads as `modified` rather than `current`. SHA-256 is
implemented in `src/dispatch_sha256.c` so the build gains no new library; it
detects edits and carries no security meaning.

## Consequences

- `dispatch init` creates `.claude/`, `.agents/` and `.opencode/` folders in
  the workflow directory. They belong to the workflow directory, like
  `AGENTS.md`, and can be committed there or ignored.
- Project skills reach sessions started in the workflow directory. Codex and
  opencode stop their upward search at the git root, so a session started
  inside the product repository (its own git repository one level down) does
  not see them; `dispatch skills install --global` covers that case.
- Upgrading the binary and running `dispatch skills update` (or `init`, or
  following the `doctor` hint) brings installed skills in line, without
  losing local edits.
- Changing a skill in `skills/` changes the bundle hash, so every installed
  copy reports `outdated` after the next build; `DISPATCH_VERSION` must still
  be bumped on release so the `newer` guard works across releases.
- `tests/check_skills.sh` now requires the skills to mention `dispatch
  skills`, which `dispatch-board` and its command reference do.
