#define NOB_IMPLEMENTATION
#define NOB_WARN_DEPRECATED
#include "nob.h"

#define BUILD_FOLDER "build/"

const char *sources[] = {
    "src/main.c",
    "src/dispatch.c",
    "src/dispatch_cli.c",
    "src/dispatch_cli_agent.c",
    "src/dispatch_cli_workspace.c",
    "src/dispatch_cli_tasks.c",
    "src/dispatch_cli_completion.c",
    "src/dispatch_cli_doctor.c",
    "src/dispatch_cli_queries.c",
    "src/dispatch_cli_lifecycle.c",
    "src/dispatch_cli_json.c",
    "src/dispatch_cli_log.c",
    "src/dispatch_cli_skills.c",
    "src/dispatch_sha256.c",
    "src/dispatch_exec.c",
    "src/dispatch_gh.c",
    "src/dispatch_issue.c",
    "src/dispatch_json.c",
    "src/dispatch_log.c",
    "src/dispatch_store.c",
    "src/dispatch_utf8.c",
    "src/dispatch_tui.c",
    "src/dispatch_tui_external.c",
    "src/dispatch_tui_actions.c",
    "src/dispatch_tui_forms.c",
    "src/dispatch_tui_render.c",
    "src/dispatch_tui_input.c",
    "src/dispatch_tui_smoke.c",
    NULL,
};

const char *obj_path(const char *path) {
    Nob_String_View name = nob_sv_from_cstr(path_name(path));
    const char *ext = temp_file_ext(path);

    if (ext) {
        nob_sv_chop_suffix(&name, nob_sv_from_cstr(ext));
    }

    return temp_sprintf("build/%.*s.o", (int)name.count, name.data);
}

bool compile_object_file(Cmd *base, const char *source) {
    Cmd cmd = {0};
    cmd_extend(&cmd, base);
    const char *out = obj_path(source);
    cmd_append(&cmd, "-c", source, "-o", out);
    nob_log(INFO, "%s", source);
    return cmd_run(&cmd);
}

static int compare_cstr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Collect the regular files under dir (relative paths below it, sorted, so
 * the generated table is deterministic). Hidden entries are skipped. */
static bool collect_skill_files(const char *root, const char *rel,
                                File_Paths *out) {
    const char *dir = rel[0] ? temp_sprintf("%s/%s", root, rel) : root;
    File_Paths children = {0};
    if (!read_entire_dir(dir, &children))
        return false;
    qsort(children.items, children.count, sizeof(children.items[0]),
          compare_cstr);
    bool ok = true;
    for (size_t i = 0; ok && i < children.count; i++) {
        const char *name = children.items[i];
        if (name[0] == '.')
            continue;
        const char *child_rel =
            rel[0] ? temp_sprintf("%s/%s", rel, name) : temp_strdup(name);
        const char *child = temp_sprintf("%s/%s", root, child_rel);
        Nob_File_Type type = get_file_type(child);
        if (type == NOB_FILE_DIRECTORY)
            ok = collect_skill_files(root, child_rel, out);
        else if (type == NOB_FILE_REGULAR)
            da_append(out, child_rel);
    }
    da_free(children);
    return ok;
}

static void append_bytes(String_Builder *out, const char *data,
                         size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i % 20 == 0)
            sb_append_cstr(out, "\n    ");
        sb_appendf(out, "%d,", (unsigned char)data[i]);
    }
}

/* Embed every file of every skill under skills/ (SKILL.md and reference
 * files) as dispatch_skill_files[], so `dispatch skills install` writes the
 * skills that match the commands of this build (ADR 0011). */
static bool generate_skills(String_Builder *out) {
    File_Paths skills = {0};
    if (!read_entire_dir("skills", &skills))
        return false;
    qsort(skills.items, skills.count, sizeof(skills.items[0]), compare_cstr);

    String_Builder table = {0};
    size_t file_count = 0;
    bool ok = true;
    for (size_t s = 0; ok && s < skills.count; s++) {
        const char *skill = skills.items[s];
        if (skill[0] == '.')
            continue;
        const char *skill_dir = temp_sprintf("skills/%s", skill);
        if (get_file_type(skill_dir) != NOB_FILE_DIRECTORY)
            continue;
        File_Paths files = {0};
        if (!collect_skill_files(skill_dir, "", &files)) {
            ok = false;
            break;
        }
        for (size_t f = 0; ok && f < files.count; f++) {
            String_Builder content = {0};
            if (!read_entire_file(temp_sprintf("%s/%s", skill_dir,
                                               files.items[f]),
                                  &content)) {
                ok = false;
                break;
            }
            sb_appendf(out, "static const char skill_file_%zu[] = {",
                       file_count);
            append_bytes(out, content.items, content.count);
            sb_append_cstr(out, "0};\n");
            sb_appendf(&table, "    {\"%s\", \"%s\", skill_file_%zu, %zu},\n",
                       skill, files.items[f], file_count, content.count);
            file_count++;
            sb_free(content);
        }
        da_free(files);
    }
    if (ok) {
        sb_append_cstr(out, "const DispatchSkillFile dispatch_skill_files[] = {\n");
        sb_append_buf(out, table.items, table.count);
        sb_append_cstr(out, "    {0, 0, 0, 0},\n};\n");
        sb_appendf(out, "const size_t dispatch_skill_file_count = %zu;\n",
                   file_count);
    }
    sb_free(table);
    da_free(skills);
    return ok;
}

/* Embed the agent skills under skills/ into the binary so `dispatch init`,
 * `dispatch skills install`, and `dispatch skills show` work without assuming
 * the source repo is present at the install location (ADR 0011, ADR 0014). */
bool generate_skills_bundle(const char *out_path) {
    String_Builder out = {0};
    sb_append_cstr(&out, "/* Generated by nob from skills/; do not edit. */\n"
                         "#include \"dispatch.h\"\n");

    if (!generate_skills(&out)) {
        sb_free(out);
        return false;
    }

    bool ok = write_entire_file(out_path, out.items, out.count);
    if (ok)
        nob_log(INFO, "%s", out_path);
    sb_free(out);
    return ok;
}

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);
    Cmd cmd = {0};
    Cmd cmd_clean = {0};
    nob_cmd_append(&cmd_clean, "rm", "-rf", "build", "tdl", "dispatch");
    nob_cmd_append(&cmd, "clang", "-Iinclude", "-Wall", "-Wextra");
    if (argc > 1) {
        if (!strcmp(argv[1], "clean")) {
            if (!cmd_run(&cmd_clean))
                return 1;
            return 0;
        }
        if (!strcmp(argv[1], "release")) {
            if (!cmd_run(&cmd_clean))
                return 1;
            nob_cmd_append(&cmd, "-DNDEBUG", "-O3");
        } else if (!strcmp(argv[1], "asan")) {
            /* Sanitizer build for CI: catches memory errors and undefined
             * behavior while the test suite runs. */
            nob_cmd_append(&cmd, "-g", "-O1", "-fsanitize=address,undefined",
                           "-fno-omit-frame-pointer");
        } else if (!strcmp(argv[1], "werror")) {
            /* Warnings-as-errors build for CI: regressions in warning
             * cleanliness fail the build. */
            nob_cmd_append(&cmd, "-g", "-O0", "-Werror");
        } else
            nob_cmd_append(&cmd, "-g", "-O0");
    } else {
        nob_cmd_append(&cmd, "-g", "-O0");
    }
    if (!mkdir_if_not_exists(BUILD_FOLDER))
        return 1;

    Cmd link_cmd = {0};
    cmd_extend(&link_cmd, &cmd);

    const char *skills_src = BUILD_FOLDER "dispatch_skills_bundle.c";
    if (!generate_skills_bundle(skills_src))
        return 1;
    if (!compile_object_file(&cmd, skills_src))
        return 1;
    nob_cmd_append(&link_cmd, obj_path(skills_src));

    for (size_t i = 0; sources[i] != NULL; i++) {
        if (!compile_object_file(&cmd, sources[i]))
            return 1;
        nob_cmd_append(&link_cmd, obj_path(sources[i]));
    }

    /* Link the wide-character ncurses explicitly: -lncurses resolves to the
     * narrow library on some distributions, which silently drops multibyte
     * input and wide-character rendering. */
    nob_cmd_append(&link_cmd, "-ljansson", "-lncursesw", "-o",
                   BUILD_FOLDER "dispatch");

    if (!cmd_run(&link_cmd))
        return 1;

    Cmd cmd_symlink = {0};

    nob_cmd_append(&cmd_symlink, "ln", "-sf", BUILD_FOLDER "dispatch",
                   "dispatch");
    if (!cmd_run(&cmd_symlink))
        return 1;

    return 0;
}
