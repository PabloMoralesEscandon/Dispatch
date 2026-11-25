/* dispatch skills list|install|update: install the agent skills embedded in
 * the binary into each runner's skills directory (ADR 0011). */

#include "dispatch_cli_internal.h"

#include <jansson.h>

#include "dispatch_sha256.h"

#define SKILL_MANIFEST ".dispatch-skill.json"

/* Where each runner loads Agent Skills from; sources are recorded in ADR
 * 0011. The global directory is relative to HOME, or to XDG_CONFIG_HOME
 * (default HOME/.config) when global_in_config is set. */
static const struct {
    const char *name;
    const char *project_dir;
    const char *global_dir;
    int global_in_config;
} skill_runners[] = {
    {"claude", ".claude/skills", ".claude/skills", 0},
    {"codex", ".agents/skills", ".agents/skills", 0},
    {"opencode", ".opencode/skills", "opencode/skills", 1},
};
#define SKILL_RUNNER_COUNT (sizeof(skill_runners) / sizeof(skill_runners[0]))

const char *skill_status_name(SkillStatus status) {
    switch (status) {
    case SKILL_MISSING:
        return "missing";
    case SKILL_CURRENT:
        return "current";
    case SKILL_OUTDATED:
        return "outdated";
    case SKILL_NEWER:
        return "newer";
    case SKILL_MODIFIED:
        return "modified";
    case SKILL_UNMANAGED:
        return "unmanaged";
    }
    return "unknown";
}

/* --- Bundled skills ------------------------------------------------------ */

/* Bundled skill names, in table order (nob sorts the files by skill). */
size_t skills_bundled_names(const char ***names_out) {
    static const char *names[64];
    static size_t count = 0;
    static int done = 0;
    if (!done) {
        for (size_t i = 0; i < dispatch_skill_file_count; i++) {
            const char *skill = dispatch_skill_files[i].skill;
            if (count > 0 && strcmp(names[count - 1], skill) == 0)
                continue;
            if (count < sizeof(names) / sizeof(names[0]))
                names[count++] = skill;
        }
        done = 1;
    }
    *names_out = names;
    return count;
}

/* A hash over every bundled file of one skill (paths and bytes): it changes
 * whenever this build ships different skill content. */
static void bundle_hash(const char *skill, char hex[65]) {
    DispatchSha256 ctx;
    dispatch_sha256_init(&ctx);
    for (size_t i = 0; i < dispatch_skill_file_count; i++) {
        const DispatchSkillFile *file = &dispatch_skill_files[i];
        if (strcmp(file->skill, skill) != 0)
            continue;
        char size[32];
        snprintf(size, sizeof(size), "%zu", file->size);
        dispatch_sha256_update(&ctx, file->path, strlen(file->path) + 1);
        dispatch_sha256_update(&ctx, size, strlen(size) + 1);
        dispatch_sha256_update(&ctx, file->data, file->size);
    }
    dispatch_sha256_final_hex(&ctx, hex);
}

static int bundled_file_exists(const char *skill, const char *path) {
    for (size_t i = 0; i < dispatch_skill_file_count; i++) {
        if (strcmp(dispatch_skill_files[i].skill, skill) == 0 &&
            strcmp(dispatch_skill_files[i].path, path) == 0)
            return 1;
    }
    return 0;
}

/* Compare dotted numeric versions; non-numeric parts compare as 0. */
static int compare_versions(const char *left, const char *right) {
    while ((left && *left) || (right && *right)) {
        long l = 0, r = 0;
        if (left && *left) {
            l = strtol(left, (char **)&left, 10);
            while (*left && *left != '.')
                left++;
            if (*left == '.')
                left++;
        }
        if (right && *right) {
            r = strtol(right, (char **)&right, 10);
            while (*right && *right != '.')
                right++;
            if (*right == '.')
                right++;
        }
        if (l != r)
            return l < r ? -1 : 1;
    }
    return 0;
}

/* --- Filesystem helpers -------------------------------------------------- */

static int dir_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static int file_hash(const char *path, char hex[65]) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    DispatchSha256 ctx;
    dispatch_sha256_init(&ctx);
    unsigned char buffer[8192];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        dispatch_sha256_update(&ctx, buffer, got);
    int ok = !ferror(file);
    fclose(file);
    if (ok)
        dispatch_sha256_final_hex(&ctx, hex);
    return ok;
}

/* mkdir -p for the directory part of path (or path itself if is_dir). */
static int make_parents(const char *path, int is_dir) {
    char *copy = cli_strdup(path);
    size_t len = strlen(copy);
    if (!is_dir) {
        char *slash = strrchr(copy, '/');
        if (!slash) {
            free(copy);
            return 1;
        }
        *slash = '\0';
        len = strlen(copy);
    }
    for (size_t i = 1; i <= len; i++) {
        if (copy[i] != '/' && copy[i] != '\0')
            continue;
        char saved = copy[i];
        copy[i] = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            free(copy);
            return 0;
        }
        copy[i] = saved;
    }
    int ok = dir_exists(copy);
    free(copy);
    return ok;
}

static int write_file_atomic(const char *path, const char *data,
                             size_t size) {
    if (!make_parents(path, 0))
        return 0;
    size_t tmp_size = strlen(path) + 16;
    char *tmp = malloc(tmp_size);
    if (!tmp) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(tmp, tmp_size, "%s.tmp%ld", path, (long)getpid());
    FILE *file = fopen(tmp, "wb");
    if (!file) {
        free(tmp);
        return 0;
    }
    int ok = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0)
        ok = 0;
    if (ok && rename(tmp, path) != 0)
        ok = 0;
    if (!ok)
        unlink(tmp);
    free(tmp);
    return ok;
}

/* --- Targets ------------------------------------------------------------- */

static char *runner_global_base(size_t runner) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    if (skill_runners[runner].global_in_config) {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        char *base = xdg && xdg[0] ? cli_strdup(xdg)
                                   : join_path2(home, ".config");
        char *dir = join_path2(base, skill_runners[runner].global_dir);
        free(base);
        return dir;
    }
    return join_path2(home, skill_runners[runner].global_dir);
}

/* Inspect one installed copy: its status against this build, and the
 * Dispatch version recorded in its manifest. */
static SkillStatus inspect_target(const char *skill, const char *path,
                                  char **installed_version) {
    *installed_version = NULL;
    if (!dir_exists(path))
        return SKILL_MISSING;

    char *manifest_path = join_path2(path, SKILL_MANIFEST);
    json_error_t error;
    json_t *manifest = json_load_file(manifest_path, 0, &error);
    free(manifest_path);
    json_t *files = manifest ? json_object_get(manifest, "files") : NULL;
    const char *stamped_skill =
        manifest ? json_string_value(json_object_get(manifest, "skill")) : NULL;
    if (!json_is_object(files) || !stamped_skill ||
        strcmp(stamped_skill, skill) != 0) {
        json_decref(manifest);
        return SKILL_UNMANAGED;
    }

    const char *version =
        json_string_value(json_object_get(manifest, "dispatch_version"));
    const char *hash =
        json_string_value(json_object_get(manifest, "bundle_hash"));
    *installed_version = version ? cli_strdup(version) : NULL;

    SkillStatus status = SKILL_CURRENT;
    const char *file_path;
    json_t *expected;
    json_object_foreach(files, file_path, expected) {
        char *full = join_path2(path, file_path);
        char actual[65];
        int ok = file_hash(full, actual);
        free(full);
        if (!ok || !json_is_string(expected) ||
            strcmp(actual, json_string_value(expected)) != 0) {
            status = SKILL_MODIFIED;
            break;
        }
    }
    if (status == SKILL_CURRENT) {
        char bundled[65];
        bundle_hash(skill, bundled);
        if (!hash || strcmp(hash, bundled) != 0) {
            status = compare_versions(version, DISPATCH_VERSION) > 0
                         ? SKILL_NEWER
                         : SKILL_OUTDATED;
        }
    }
    json_decref(manifest);
    return status;
}

static void add_target(SkillTargets *targets, const char *skill,
                       const char *runner, const char *scope, char *path) {
    targets->items = cli_realloc_array(targets->items, targets->count + 1,
                                       sizeof(*targets->items));
    SkillTarget *target = &targets->items[targets->count++];
    memset(target, 0, sizeof(*target));
    target->skill = skill;
    target->runner = runner;
    target->scope = scope;
    target->path = path;
    target->status = inspect_target(skill, path, &target->installed_version);
}

int skills_runner_is_valid(const char *runner) {
    if (strcmp(runner, "all") == 0)
        return 1;
    for (size_t r = 0; r < SKILL_RUNNER_COUNT; r++) {
        if (strcmp(runner, skill_runners[r].name) == 0)
            return 1;
    }
    return 0;
}

/* Every (scope, runner, skill) location selected, project first. Returns 0
 * when global was requested and HOME is unset (no global targets added). */
int skills_collect_targets(int project, int global, const char *runner,
                           SkillTargets *out) {
    memset(out, 0, sizeof(*out));
    const char **names;
    size_t name_count = skills_bundled_names(&names);
    int ok = 1;
    for (int pass = 0; pass < 2; pass++) {
        if ((pass == 0 && !project) || (pass == 1 && !global))
            continue;
        for (size_t r = 0; r < SKILL_RUNNER_COUNT; r++) {
            if (runner && strcmp(runner, "all") != 0 &&
                strcmp(runner, skill_runners[r].name) != 0)
                continue;
            char *base = pass == 0 ? cli_strdup(skill_runners[r].project_dir)
                                   : runner_global_base(r);
            if (!base) {
                ok = 0;
                continue;
            }
            for (size_t s = 0; s < name_count; s++) {
                add_target(out, names[s], skill_runners[r].name,
                           pass == 0 ? "project" : "global",
                           join_path2(base, names[s]));
            }
            free(base);
        }
    }
    return ok;
}

void skills_targets_free(SkillTargets *targets) {
    for (size_t i = 0; i < targets->count; i++) {
        free(targets->items[i].path);
        free(targets->items[i].installed_version);
    }
    free(targets->items);
    memset(targets, 0, sizeof(*targets));
}

/* --- Installing ---------------------------------------------------------- */

/* Write the bundled files of target->skill into target->path, drop files a
 * previous install wrote that this build no longer ships, then write the
 * manifest last so an interrupted install is reported as modified. */
static int write_skill(const SkillTarget *target, char *error,
                       size_t error_size) {
    char *manifest_path = join_path2(target->path, SKILL_MANIFEST);
    json_error_t json_error;
    json_t *old = json_load_file(manifest_path, 0, &json_error);

    json_t *files = json_object();
    for (size_t i = 0; i < dispatch_skill_file_count; i++) {
        const DispatchSkillFile *file = &dispatch_skill_files[i];
        if (strcmp(file->skill, target->skill) != 0)
            continue;
        char *full = join_path2(target->path, file->path);
        int ok = write_file_atomic(full, file->data, file->size);
        if (!ok) {
            snprintf(error, error_size, "could not write %s: %s", full,
                     strerror(errno));
            free(full);
            json_decref(files);
            json_decref(old);
            free(manifest_path);
            return 0;
        }
        free(full);
        char hex[65];
        dispatch_sha256_hex(file->data, file->size, hex);
        json_object_set_new(files, file->path, json_string(hex));
    }

    json_t *old_files = old ? json_object_get(old, "files") : NULL;
    if (json_is_object(old_files)) {
        const char *old_path;
        json_t *value;
        json_object_foreach(old_files, old_path, value) {
            (void)value;
            if (strstr(old_path, "..") || old_path[0] == '/' ||
                bundled_file_exists(target->skill, old_path))
                continue;
            char *full = join_path2(target->path, old_path);
            unlink(full);
            char *slash = strrchr(full, '/');
            if (slash && slash > full + strlen(target->path)) {
                *slash = '\0';
                rmdir(full);
            }
            free(full);
        }
    }
    json_decref(old);

    char hash[65];
    bundle_hash(target->skill, hash);
    json_t *manifest = json_object();
    json_object_set_new(manifest, "skill", json_string(target->skill));
    json_object_set_new(manifest, "dispatch_version",
                        json_string(DISPATCH_VERSION));
    json_object_set_new(manifest, "bundle_hash", json_string(hash));
    json_object_set_new(manifest, "files", files);
    char *text = json_dumps(manifest, JSON_INDENT(2));
    json_decref(manifest);
    int ok = text && write_file_atomic(manifest_path, text, strlen(text));
    if (!ok)
        snprintf(error, error_size, "could not write %s: %s", manifest_path,
                 strerror(errno));
    free(text);
    free(manifest_path);
    return ok;
}

static const char *skip_reason(SkillStatus status) {
    switch (status) {
    case SKILL_MODIFIED:
        return "was edited after it was installed";
    case SKILL_UNMANAGED:
        return "was not installed by Dispatch";
    case SKILL_NEWER:
        return "was installed by a newer Dispatch";
    default:
        return "";
    }
}

static const char *skip_code(SkillStatus status) {
    switch (status) {
    case SKILL_MODIFIED:
        return "skill_modified";
    case SKILL_UNMANAGED:
        return "skill_unmanaged";
    default:
        return "skill_newer";
    }
}

/* Bring one target in line with this build. Sets target->action to
 * installed, updated, replaced, unchanged, skipped, or failed. */
static int apply_target(SkillTarget *target, int force, int quiet,
                        const char *force_command) {
    SkillStatus status = target->status;
    if (status == SKILL_CURRENT) {
        target->action = "unchanged";
        return 1;
    }
    int protected_copy = status == SKILL_MODIFIED ||
                         status == SKILL_UNMANAGED || status == SKILL_NEWER;
    if (protected_copy && !force) {
        target->action = "skipped";
        if (cli_json()) {
            cli_warning(skip_code(status), NULL, NULL,
                        "kept %s: it %s; rerun %s --force to replace it",
                        target->path, skip_reason(status), force_command);
        } else {
            fprintf(stderr,
                    "Warning: kept %s: it %s; rerun %s --force to replace "
                    "it\n",
                    target->path, skip_reason(status), force_command);
        }
        return 1;
    }

    char error[512] = {0};
    if (!write_skill(target, error, sizeof(error))) {
        target->action = "failed";
        cli_error("io_failed", NULL, "Could not install skill %s: %s\n",
                  target->skill, error);
        return 0;
    }
    target->action = status == SKILL_MISSING    ? "installed"
                     : status == SKILL_OUTDATED ? "updated"
                                                : "replaced";
    if (!quiet && !cli_json()) {
        printf("%c%s %s for %s (%s): %s\n", toupper((unsigned char)target->action[0]),
               target->action + 1, target->skill, target->runner,
               target->scope, target->path);
    }
    return 1;
}

typedef struct {
    size_t installed, updated, replaced, unchanged, skipped;
} SkillCounts;

static void count_action(SkillCounts *counts, const char *action) {
    if (!action)
        return;
    if (strcmp(action, "installed") == 0)
        counts->installed++;
    else if (strcmp(action, "updated") == 0)
        counts->updated++;
    else if (strcmp(action, "replaced") == 0)
        counts->replaced++;
    else if (strcmp(action, "unchanged") == 0)
        counts->unchanged++;
    else if (strcmp(action, "skipped") == 0)
        counts->skipped++;
}

int skills_install_for_init(size_t *changed, size_t *skipped) {
    SkillTargets targets;
    skills_collect_targets(1, 0, "all", &targets);
    SkillCounts counts = {0};
    int ok = 1;
    for (size_t i = 0; i < targets.count; i++) {
        if (!apply_target(&targets.items[i], 0, 1, "dispatch skills install"))
            ok = 0;
        count_action(&counts, targets.items[i].action);
    }
    *changed = counts.installed + counts.updated + counts.replaced;
    *skipped = counts.skipped;
    if (!cli_json() && *changed > 0) {
        printf("Installed agent skills for claude, codex and opencode in "
               ".claude/skills, .agents/skills and .opencode/skills\n");
    }
    skills_targets_free(&targets);
    return ok;
}

/* --- Command line -------------------------------------------------------- */

typedef struct {
    int project;
    int global;
    const char *runner;
    int force;
} SkillOptions;

static int parse_skill_options(int argc, char **argv, int allow_force,
                               SkillOptions *options, const char *usage) {
    memset(options, 0, sizeof(*options));
    options->runner = "all";
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--project") == 0) {
            options->project = 1;
        } else if (strcmp(argv[i], "--global") == 0) {
            options->global = 1;
        } else if (allow_force && strcmp(argv[i], "--force") == 0) {
            options->force = 1;
        } else if (strcmp(argv[i], "--runner") == 0 && i + 1 < argc) {
            options->runner = argv[++i];
            if (!skills_runner_is_valid(options->runner)) {
                cli_error("invalid_runner", NULL,
                          "Unknown runner %s; use claude, codex, opencode or "
                          "all\n",
                          options->runner);
                return 0;
            }
        } else {
            cli_error("usage", NULL, "%s", usage);
            return 0;
        }
    }
    if (options->project && options->global) {
        cli_error("usage", NULL, "Pass at most one of --project and --global\n%s",
                  usage);
        return 0;
    }
    return 1;
}

static int board_present(void) {
    struct stat info;
    return stat(DISPATCH_STORE_FILE, &info) == 0;
}

static int require_board(void) {
    if (board_present())
        return 1;
    cli_error("board_missing", NULL,
              "No dispatch board in this directory; project skills are "
              "installed in the workflow directory (run dispatch init "
              "<repo-path> or cd there), or pass --global\n");
    return 0;
}

static int require_home(void) {
    const char *home = getenv("HOME");
    if (home && home[0])
        return 1;
    cli_error("home_unset", NULL,
              "HOME is not set; cannot locate the global skills directories\n");
    return 0;
}

static const char *scope_name(int project, int global) {
    if (project && !global)
        return "project";
    if (global && !project)
        return "global";
    return "all";
}

static json_t *target_json(const SkillTarget *target, int with_action) {
    json_t *object = json_object();
    json_object_set_new(object, "skill", json_string(target->skill));
    json_object_set_new(object, "runner", json_string(target->runner));
    json_object_set_new(object, "scope", json_string(target->scope));
    json_object_set_new(object, "path", json_string(target->path));
    json_object_set_new(object, "status",
                        json_string(skill_status_name(target->status)));
    json_object_set_new(object, "installed_version",
                        target->installed_version
                            ? json_string(target->installed_version)
                            : json_null());
    if (with_action)
        json_object_set_new(object, "action",
                            target->action ? json_string(target->action)
                                           : json_null());
    return object;
}

static json_t *query_json(const SkillOptions *options, int project,
                          int global) {
    json_t *query = json_object();
    json_object_set_new(query, "scope", json_string(scope_name(project, global)));
    json_object_set_new(query, "runner", json_string(options->runner));
    return query;
}

static int skills_list(int argc, char **argv) {
    const char *usage = "Usage: dispatch skills list [--project | --global] "
                        "[--runner claude|codex|opencode|all] [--json]\n";
    SkillOptions options;
    if (!parse_skill_options(argc, argv, 0, &options, usage))
        return 1;
    int project = options.project || !options.global;
    int global = options.global || !options.project;
    if (options.project && !require_board())
        return 1;
    if (options.global && !require_home())
        return 1;
    /* Without a board here there is no workflow directory to report on. */
    if (!options.project && !board_present())
        project = 0;

    SkillTargets targets;
    if (!skills_collect_targets(project, global, options.runner, &targets)) {
        if (cli_json())
            cli_warning("home_unset", NULL, NULL,
                        "HOME is not set; global skills were not checked");
        else
            fprintf(stderr,
                    "Warning: HOME is not set; global skills were not "
                    "checked\n");
    }
    const char **names;
    size_t name_count = skills_bundled_names(&names);

    if (cli_json()) {
        json_t *root = json_object();
        json_object_set_new(root, "schema_version", json_integer(1));
        json_object_set_new(root, "command", json_string("skills list"));
        json_object_set_new(root, "dispatch_version",
                            json_string(DISPATCH_VERSION));
        json_object_set_new(root, "query", query_json(&options, project, global));
        json_t *skills = json_array();
        for (size_t s = 0; s < name_count; s++) {
            json_t *skill = json_object();
            json_object_set_new(skill, "name", json_string(names[s]));
            json_object_set_new(skill, "version", json_string(DISPATCH_VERSION));
            json_t *files = json_array();
            for (size_t i = 0; i < dispatch_skill_file_count; i++) {
                if (strcmp(dispatch_skill_files[i].skill, names[s]) == 0)
                    json_array_append_new(
                        files, json_string(dispatch_skill_files[i].path));
            }
            json_object_set_new(skill, "files", files);
            json_t *installs = json_array();
            for (size_t i = 0; i < targets.count; i++) {
                if (strcmp(targets.items[i].skill, names[s]) == 0)
                    json_array_append_new(installs,
                                          target_json(&targets.items[i], 0));
            }
            json_object_set_new(skill, "installations", installs);
            json_array_append_new(skills, skill);
        }
        json_object_set_new(root, "skills", skills);
        skills_targets_free(&targets);
        return cli_json_emit_object(root);
    }

    printf("Bundled skills (dispatch %s):\n", DISPATCH_VERSION);
    for (size_t s = 0; s < name_count; s++) {
        printf("  %s %s\n", names[s], DISPATCH_VERSION);
        for (size_t i = 0; i < targets.count; i++) {
            const SkillTarget *target = &targets.items[i];
            if (strcmp(target->skill, names[s]) != 0)
                continue;
            printf("    %-7s %-8s %-9s %s", target->scope, target->runner,
                   skill_status_name(target->status), target->path);
            if (target->installed_version &&
                target->status != SKILL_CURRENT)
                printf(" (installed by %s)", target->installed_version);
            printf("\n");
        }
    }
    if (!project)
        printf("Project skills: run from the workflow directory to see them\n");
    skills_targets_free(&targets);
    return 0;
}

static int skills_apply(int argc, char **argv, int update) {
    const char *usage =
        update ? "Usage: dispatch skills update [--project | --global] "
                 "[--runner claude|codex|opencode|all] [--force] [--json]\n"
               : "Usage: dispatch skills install [--project | --global] "
                 "[--runner claude|codex|opencode|all] [--force] [--json]\n";
    const char *command = update ? "dispatch skills update"
                                 : "dispatch skills install";
    SkillOptions options;
    if (!parse_skill_options(argc, argv, 1, &options, usage))
        return 1;

    int project, global;
    if (update) {
        project = options.project || !options.global;
        global = options.global || !options.project;
        if (options.project && !require_board())
            return 1;
        if (options.global && !require_home())
            return 1;
        if (!options.project && !board_present())
            project = 0;
        const char *home = getenv("HOME");
        if (!options.global && (!home || !home[0]))
            global = 0;
    } else {
        project = !options.global;
        global = options.global;
        if (project && !require_board())
            return 1;
        if (global && !require_home())
            return 1;
    }

    SkillTargets targets;
    skills_collect_targets(project, global, options.runner, &targets);
    SkillCounts counts = {0};
    int ok = 1;
    size_t acted = 0;
    for (size_t i = 0; i < targets.count; i++) {
        SkillTarget *target = &targets.items[i];
        /* update refreshes installed copies only. */
        if (update && target->status == SKILL_MISSING)
            continue;
        acted++;
        if (!apply_target(target, options.force, 0, command))
            ok = 0;
        count_action(&counts, target->action);
    }
    if (!ok) {
        skills_targets_free(&targets);
        return 1;
    }

    if (cli_json()) {
        json_t *root = json_object();
        json_object_set_new(root, "schema_version", json_integer(1));
        json_object_set_new(root, "command",
                            json_string(update ? "skills update"
                                               : "skills install"));
        json_t *query = query_json(&options, project, global);
        json_object_set_new(query, "force", json_boolean(options.force));
        json_object_set_new(root, "query", query);
        json_t *result = json_object();
        json_object_set_new(result, "action",
                            json_string(update ? "skills_update"
                                               : "skills_install"));
        json_object_set_new(
            result, "changed",
            json_boolean(counts.installed + counts.updated + counts.replaced >
                         0));
        json_object_set_new(result, "dispatch_version",
                            json_string(DISPATCH_VERSION));
        json_object_set_new(result, "installed",
                            json_integer((json_int_t)counts.installed));
        json_object_set_new(result, "updated",
                            json_integer((json_int_t)counts.updated));
        json_object_set_new(result, "replaced",
                            json_integer((json_int_t)counts.replaced));
        json_object_set_new(result, "unchanged",
                            json_integer((json_int_t)counts.unchanged));
        json_object_set_new(result, "skipped",
                            json_integer((json_int_t)counts.skipped));
        json_t *skills = json_array();
        for (size_t i = 0; i < targets.count; i++) {
            if (targets.items[i].action)
                json_array_append_new(skills,
                                      target_json(&targets.items[i], 1));
        }
        json_object_set_new(result, "skills", skills);
        json_object_set_new(root, "result", result);
        skills_targets_free(&targets);
        return cli_json_emit_object(root);
    }

    if (update && acted == 0) {
        printf("No Dispatch skills are installed here; run dispatch skills "
               "install\n");
    } else {
        printf("Skills: %zu installed, %zu updated, %zu replaced, %zu "
               "unchanged, %zu skipped (dispatch %s)\n",
               counts.installed, counts.updated, counts.replaced,
               counts.unchanged, counts.skipped, DISPATCH_VERSION);
    }
    skills_targets_free(&targets);
    return 0;
}

int cmd_skills(int argc, char **argv) {
    if (argc < 3) {
        cli_error("usage", NULL,
                  "Usage: dispatch skills list|install|update [options]\n");
        return 1;
    }
    if (strcmp(argv[2], "list") == 0)
        return skills_list(argc, argv);
    if (strcmp(argv[2], "install") == 0)
        return skills_apply(argc, argv, 0);
    if (strcmp(argv[2], "update") == 0)
        return skills_apply(argc, argv, 1);
    cli_error("usage", NULL,
              "Unknown skills command: %s\nUsage: dispatch skills "
              "list|install|update [options]\n",
              argv[2]);
    return 1;
}
