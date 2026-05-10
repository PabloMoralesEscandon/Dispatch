#define NOB_IMPLEMENTATION
#define NOB_WARN_DEPRECATED
#include "nob.h"

#define BUILD_FOLDER "build/"

const char *sources[] = {
    "src/main.c",
    "src/dispatch.c",
    "src/dispatch_cli.c",
    "src/dispatch_store.c",
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
        } else
            nob_cmd_append(&cmd, "-g", "-O0");
    } else {
        nob_cmd_append(&cmd, "-g", "-O0");
    }
    if (!mkdir_if_not_exists(BUILD_FOLDER))
        return 1;

    Cmd link_cmd = {0};
    cmd_extend(&link_cmd, &cmd);

    for (size_t i = 0; sources[i] != NULL; i++) {
        if (!compile_object_file(&cmd, sources[i]))
            return 1;
        nob_cmd_append(&link_cmd, obj_path(sources[i]));
    }

    nob_cmd_append(&link_cmd, "-ljansson", "-o", BUILD_FOLDER "dispatch");

    if (!cmd_run(&link_cmd))
        return 1;

    Cmd cmd_symlink = {0};

    nob_cmd_append(&cmd_symlink, "ln", "-sf", BUILD_FOLDER "dispatch",
                   "dispatch");
    if (!cmd_run(&cmd_symlink))
        return 1;

    return 0;
}
