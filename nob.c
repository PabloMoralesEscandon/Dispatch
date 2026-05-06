#define NOB_IMPLEMENTATION
#define NOB_WARN_DEPRECATED
#include "nob.h"

#define BUILD_FOLDER "build/"
#define SRC_FOLDER "src/"

const char *obj_path(const char *path) {
    Nob_String_View name = nob_sv_from_cstr(path_name(path));
    const char *ext = temp_file_ext(path);

    if (ext) {
        nob_sv_chop_suffix(&name, nob_sv_from_cstr(ext));
    }

    return temp_sprintf("build/%.*s.o", (int)name.count, name.data);
}

bool compile_object_files(Nob_Walk_Entry entry) {
    Cmd *base = entry.data;
    Cmd cmd = {0};
    cmd_extend(&cmd, base);
    const char *out = obj_path(entry.path);
    const char *ext = temp_file_ext(entry.path);
    if ((ext != NULL) && (!strcmp(".c", ext))) {
        cmd_append(&cmd, "-c", entry.path, "-o", out);
        nob_log(INFO, "%*s%s", (int)entry.level * 2, "", entry.path);
        if (!strcmp(".c", ext)) {
            if (!cmd_run(&cmd))
                return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);
    Cmd cmd = {0};
    Cmd cmd_clean = {0};
    nob_cmd_append(&cmd_clean, "rm", "-rf", "build", "tdl");
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

    if (!nob_walk_dir(SRC_FOLDER, compile_object_files, .data = &cmd))
        return 1;

    Nob_File_Paths files = {0};

    if (!read_entire_dir(BUILD_FOLDER, &files))
        return 1;

    for (size_t i = 2; i < files.count; ++i) {
        const char *file = temp_sprintf(BUILD_FOLDER "%s", files.items[i]);
        const char *ext = temp_file_ext(file);
        if (ext == NULL || strcmp(ext, ".o") != 0)
            continue;
        nob_cmd_append(&cmd, file);
    }

    nob_cmd_append(&cmd, "-ljansson", "-o", BUILD_FOLDER "tdl");

    if (!cmd_run(&cmd))
        return 1;

    Cmd cmd_symlink = {0};

    nob_cmd_append(&cmd_symlink, "ln", "-sf", BUILD_FOLDER "tdl", "tdl");
    if (!cmd_run(&cmd_symlink))
        return 1;

    return 0;
}
