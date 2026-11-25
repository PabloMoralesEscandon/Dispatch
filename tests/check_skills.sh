#!/usr/bin/env bash
# Check the Agent Skills under skills/ (ADR 0010).
#
#   tests/check_skills.sh [skills-dir]
#
# 1. Front matter: every expected skill folder has a SKILL.md that starts with
#    a YAML front matter block carrying a non-empty `name` equal to the folder
#    name (lowercase letters, digits and hyphens, at most 64 characters) and a
#    non-empty `description` (at most 1024 characters).
# 2. Drift: every `dispatch <command> [<subcommand>]` written in code in the
#    skills (fenced blocks and inline code spans) is a real command according to
#    `dispatch --help`, and every top-level command listed there is mentioned by
#    at least one skill, apart from the commented exclusions below.
#
# The binary is $DISPATCH_BIN, defaulting to the freshly built ./dispatch of
# this checkout, never one found on PATH.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKILLS_DIR="${1:-$ROOT/skills}"
BIN="${DISPATCH_BIN:-$ROOT/dispatch}"

EXPECTED_SKILLS="dispatch-plan dispatch-work dispatch-board"

# Top-level commands that no skill needs to mention. Keep each one commented.
# (Currently empty: `tui` and `completion install` are covered by
# dispatch-board. Add e.g. `completion` here if a command is plumbing only.)
EXCLUDED_COMMANDS=""

errors=0
err() {
    printf 'FAIL: %s\n' "$1" >&2
    errors=$((errors + 1))
}

if [ ! -x "$BIN" ]; then
    printf 'FAIL: dispatch binary not found at %s (build with ./nob first)\n' "$BIN" >&2
    exit 1
fi
if [ ! -d "$SKILLS_DIR" ]; then
    printf 'FAIL: skills directory not found: %s\n' "$SKILLS_DIR" >&2
    exit 1
fi

# --- 1. Front matter -------------------------------------------------------

front_matter_value() { # file key
    awk -v key="$2" '
        NR == 1 { if ($0 != "---") exit; next }
        $0 == "---" { exit }
        index($0, key ":") == 1 {
            v = substr($0, length(key) + 2)
            sub(/^[ \t]+/, "", v); sub(/[ \t\r]+$/, "", v)
            if (v ~ /^".*"$/ || v ~ /^'\''.*'\''$/) v = substr(v, 2, length(v) - 2)
            print v; exit
        }' "$1"
}

check_front_matter() { # dir
    local dir="$1" skill file name description
    skill="$(basename "$dir")"
    file="$dir/SKILL.md"
    if [ ! -f "$file" ]; then
        err "$skill: SKILL.md is missing"
        return
    fi
    if [ "$(head -n 1 "$file")" != "---" ] ||
        [ "$(awk 'NR > 1 && $0 == "---" { print "closed"; exit }' "$file")" != "closed" ]; then
        err "$skill: SKILL.md does not start with a --- front matter block"
        return
    fi
    name="$(front_matter_value "$file" name)"
    description="$(front_matter_value "$file" description)"
    if [ -z "$name" ]; then
        err "$skill: front matter has no name"
    elif [ "$name" != "$skill" ]; then
        err "$skill: front matter name '$name' differs from folder name '$skill'"
    elif ! printf '%s' "$name" | grep -Eq '^[a-z0-9]+(-[a-z0-9]+)*$' || [ "${#name}" -gt 64 ]; then
        err "$skill: name must be lowercase letters, digits and single hyphens, at most 64 characters"
    fi
    if [ -z "$description" ]; then
        err "$skill: front matter has no description"
    elif [ "${#description}" -gt 1024 ]; then
        err "$skill: description is longer than 1024 characters"
    fi
}

for skill in $EXPECTED_SKILLS; do
    if [ ! -d "$SKILLS_DIR/$skill" ]; then
        err "$skill: skill folder is missing"
    fi
done
for dir in "$SKILLS_DIR"/*/; do
    [ -d "$dir" ] || continue
    check_front_matter "${dir%/}"
done

# --- 2. Drift against dispatch --help -------------------------------------

help_text="$("$BIN" --help)"

# Top-level commands: the indented first words of the "Workflow commands:"
# section.
top_commands="$(printf '%s\n' "$help_text" | awk '
    /^Workflow commands:/ { on = 1; next }
    on && /^[^ ]/ { exit }
    on && NF { print $1 }')"
if [ -z "$top_commands" ]; then
    printf 'FAIL: could not read the command list from dispatch --help\n' >&2
    exit 1
fi

# Subcommands: the "Implemented now:" section lists entries such as
# "agent create/list/show" and "repo [set]", separated by commas. Prints
# "<command> <sub>" pairs, and "<command> -" when the subcommand is optional.
sub_commands="$(printf '%s\n' "$help_text" | awk '
    /^Implemented now:/ { on = 1; next }
    on && /^[^ ]/ { exit }
    on { text = text " " $0 }
    END {
        n = split(text, entries, ",")
        for (i = 1; i <= n; i++) {
            m = split(entries[i], w, " ")
            if (m < 2) continue
            sub_list = w[2]
            if (sub_list ~ /^\[.*\]$/) {
                print w[1] " -"
                sub_list = substr(sub_list, 2, length(sub_list) - 2)
            }
            k = split(sub_list, subs, "/")
            for (j = 1; j <= k; j++) print w[1] " " subs[j]
        }
    }')"

is_top_command() { printf '%s\n' "$top_commands" | grep -Fxq -- "$1"; }
has_subcommands() { printf '%s\n' "$sub_commands" | awk -v c="$1" '$1 == c { f = 1 } END { exit !f }'; }
subcommand_optional() { printf '%s\n' "$sub_commands" | grep -Fxq -- "$1 -"; }
is_subcommand() { printf '%s\n' "$sub_commands" | grep -Fxq -- "$1 $2"; }

# Code text from every Markdown file of the skills: fenced block lines, and
# inline code spans from prose lines. Each output line is
# "<file>:<line>\t<code>".
code_text() {
    find "$SKILLS_DIR" -name '*.md' -type f | LC_ALL=C sort | while IFS= read -r file; do
        awk -v f="${file#"$SKILLS_DIR"/}" '
            /^[ \t]*```/ { fence = !fence; next }
            fence { print f ":" NR "\t" $0; next }
            {
                line = $0
                while (match(line, /`[^`]+`/)) {
                    print f ":" NR "\t" substr(line, RSTART + 1, RLENGTH - 2)
                    line = substr(line, RSTART + RLENGTH)
                }
            }' "$file"
    done
}

# Every "dispatch <word> [<word>]" in code. A command must follow "dispatch"
# directly; words that are options, placeholders or IDs end the match.
mentions="$(code_text | awk -F '\t' '
    {
        where = $1; code = " " $2
        while (match(code, /[ \/(]dispatch +[a-z][a-z-]*( +[a-z][a-z-]*)?/)) {
            m = substr(code, RSTART + 1, RLENGTH - 1)
            code = substr(code, RSTART + RLENGTH)
            split(m, w, " ")
            print where "\t" w[2] "\t" w[3]
        }
    }')"

mentioned_commands=""
while IFS=$'\t' read -r where cmd sub; do
    [ -n "$where" ] || continue
    if ! is_top_command "$cmd"; then
        err "$where: 'dispatch $cmd' is not a command in dispatch --help"
        continue
    fi
    mentioned_commands="$mentioned_commands $cmd"
    if has_subcommands "$cmd"; then
        if [ -z "$sub" ]; then
            if ! subcommand_optional "$cmd"; then
                err "$where: 'dispatch $cmd' needs a subcommand"
            fi
        elif ! is_subcommand "$cmd" "$sub"; then
            err "$where: 'dispatch $cmd $sub' is not a subcommand in dispatch --help"
        fi
    fi
done <<EOF
$mentions
EOF

for cmd in $top_commands; do
    case " $EXCLUDED_COMMANDS " in *" $cmd "*) continue ;; esac
    case "$mentioned_commands " in
        *" $cmd "*) ;;
        *) err "'dispatch $cmd' is listed in dispatch --help but no skill mentions it" ;;
    esac
done

if [ "$errors" -ne 0 ]; then
    printf 'FAIL: %d agent skill check(s) failed\n' "$errors" >&2
    exit 1
fi
printf 'PASS: agent skills (%d mentions checked)\n' "$(printf '%s\n' "$mentions" | grep -c .)"
