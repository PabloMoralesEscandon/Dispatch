#include "dispatch_cli.h"

#include "dispatch_cli_internal.h"


const DispatchCliCommand commands[] = {
    {"init", "Create dispatch.json for a target repository"},
    {"repo", "Show or repair the board repo path"},
    {"agent", "Manage agents"},
    {"workspace", "Manage workspaces"},
    {"group", "Manage groups"},
    {"task", "Manage tasks"},
    {"dep", "Manage dependencies"},
    {"commit", "Manage task commit references"},
    {"completion", "Print shell completion candidates"},
    {"ready", "List ready work or mark a task ready"},
    {"reviews", "List tasks waiting for review"},
    {"proposed", "List tasks waiting for approval"},
    {"blocked", "List blocked work and blockers"},
    {"status", "Show board overview and health warnings"},
    {"doctor", "Check Dispatch setup and diagnostics"},
    {"tui", "Open terminal UI"},
    {"show", "Show one task"},
    {"list", "List tasks by group and workflow order"},
    {"start", "Start and assign a ready task"},
    {"finish", "Finish a task"},
    {"review", "Accept a task in review"},
    {"unassign", "Clear a stuck task assignment"},
    {"normalize", "Repair IDs and derived state"},
    {NULL, NULL},
};

static const DispatchCliCommand *find_command(const char *command) {
    if (!command)
        return NULL;

    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(commands[i].name, command) == 0)
            return &commands[i];
    }
    return NULL;
}

void dispatch_cli_print_help(void) {
    puts("Dispatch: a command line workflow board.");
    puts("Usage: dispatch <command> [args]");
    puts("");
    puts("Workflow commands:");
    for (int i = 0; commands[i].name != NULL; i++)
        printf("  %-10s %s\n", commands[i].name, commands[i].summary);
    puts("");
    puts("Implemented now:");
    puts("  init, repo show/set, agent create/list/show/command/session/resume, workspace create/list/show/remove/prune,");
    puts("  group add/ready, task add/edit/move/delete, dep add/remove, commit add/list/show, completion candidates,");
    puts("  ready, reviews, proposed, start, finish, review, normalize, status, doctor, tui, list, show, blocked");
}

int dispatch_cli_is_command(const char *command) {
    if (!command)
        return 0;
    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0 ||
        strcmp(command, "help") == 0) {
        return 1;
    }
    return find_command(command) != NULL;
}

static void scaffold_workflow_dir(void);

static int cmd_init(int argc, char **argv) {
    if (argc > 3) {
        fprintf(stderr, "Usage: dispatch init [repo-path]\n");
        return 1;
    }

    const char *repo_arg = argc == 3 ? argv[2] : ".";
    char *repo_path = dispatch_resolve_path(".", repo_arg);
    if (!repo_path) {
        fprintf(stderr, "Repo path %s does not exist\n", repo_arg);
        return 1;
    }
    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        free(repo_path);
        return 1;
    }

    int existed = 0;
    FILE *file = fopen(DISPATCH_STORE_FILE, "r");
    if (file) {
        existed = 1;
        fclose(file);
    }

    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, repo_path, error,
                                  sizeof(error))) {
        fprintf(stderr, "Could not initialize %s: %s\n", DISPATCH_STORE_FILE,
                error);
        dispatch_store_lock_release(&lock);
        free(repo_path);
        return 1;
    }

    if (existed) {
        printf("%s already exists\n", DISPATCH_STORE_FILE);
        dispatch_store_lock_release(&lock);
        free(repo_path);
        return 0;
    }

    printf("Created %s for repo %s\n", DISPATCH_STORE_FILE, repo_path);
    scaffold_workflow_dir();
    DispatchLogField targets[] = {
        {"repo_path", repo_path},
    };
    append_dispatch_log("user", "init", "init", targets, 1, NULL, 0,
                        "Created dispatch.json");
    dispatch_store_lock_release(&lock);
    free(repo_path);
    return 0;
}

static int board_file_exists(void) {
    FILE *file = fopen(DISPATCH_STORE_FILE, "r");
    if (!file)
        return 0;
    fclose(file);
    return 1;
}

int load_board_or_error(DispatchBoard *board) {
    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 0;
    }
    if (!board_file_exists()) {
        fprintf(stderr,
                "No dispatch board in this directory; run dispatch init "
                "<repo-path> or cd to the workflow directory\n");
        dispatch_store_lock_release(&lock);
        return 0;
    }
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error))) {
        fprintf(stderr, "Could not initialize %s: %s\n", DISPATCH_STORE_FILE,
                error);
        dispatch_store_lock_release(&lock);
        return 0;
    }
    if (!dispatch_store_load(board, DISPATCH_STORE_FILE, error, sizeof(error))) {
        fprintf(stderr, "Could not load %s: %s\n", DISPATCH_STORE_FILE, error);
        dispatch_store_lock_release(&lock);
        return 0;
    }
    dispatch_store_lock_release(&lock);
    return 1;
}

int locked_board_load_or_error(LockedBoard *locked) {
    char error[256] = {0};
    memset(locked, 0, sizeof(*locked));

    if (!dispatch_store_lock_acquire(&locked->lock, DISPATCH_STORE_FILE, 1000,
                                     error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 0;
    }

    if (!board_file_exists()) {
        fprintf(stderr,
                "No dispatch board in this directory; run dispatch init "
                "<repo-path> or cd to the workflow directory\n");
        dispatch_store_lock_release(&locked->lock);
        return 0;
    }
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error))) {
        fprintf(stderr, "Could not initialize %s: %s\n", DISPATCH_STORE_FILE,
                error);
        dispatch_store_lock_release(&locked->lock);
        return 0;
    }
    if (!dispatch_store_load(&locked->board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "Could not load %s: %s\n", DISPATCH_STORE_FILE, error);
        dispatch_store_lock_release(&locked->lock);
        return 0;
    }

    locked->loaded = 1;
    return 1;
}

int locked_board_save_or_error(LockedBoard *locked) {
    char error[256] = {0};
    if (!dispatch_store_save(&locked->board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "Could not save %s: %s\n", DISPATCH_STORE_FILE, error);
        return 0;
    }
    return 1;
}

void locked_board_close(LockedBoard *locked) {
    if (!locked)
        return;
    if (locked->loaded) {
        dispatch_board_free(&locked->board);
        locked->loaded = 0;
    }
    dispatch_store_lock_release(&locked->lock);
}

int title_starts_with_dispatch_id(const char *title) {
    size_t prefix_len = 0;
    if (!title)
        return 0;

    while (prefix_len < 3 && isalnum((unsigned char)title[prefix_len]))
        prefix_len++;
    if (prefix_len == 0 || title[prefix_len] != '-')
        return 0;
    if (!isdigit((unsigned char)title[prefix_len + 1]) ||
        !isdigit((unsigned char)title[prefix_len + 2])) {
        return 0;
    }

    char next = title[prefix_len + 3];
    return next == '\0' || isspace((unsigned char)next);
}

const char *task_display_title(const DispatchTask *task) {
    if (!task || !task->title || !task->id)
        return "";

    size_t id_len = strlen(task->id);
    if (strncmp(task->title, task->id, id_len) != 0 ||
        !isspace((unsigned char)task->title[id_len])) {
        return task->title;
    }

    const char *title = task->title + id_len;
    while (isspace((unsigned char)*title))
        title++;
    return title[0] ? title : task->title;
}

char *cli_strdup(const char *value) {
    char *copy = strdup(value ? value : "");
    if (!copy) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return copy;
}

const char *bool_string(int value) {
    return value ? "true" : "false";
}

void replace_optional_string(char **target, const char *value) {
    free(*target);
    *target = value && value[0] ? cli_strdup(value) : NULL;
}

char *trimmed_copy(const char *value) {
    const char *start = value ? value : "";
    while (isspace((unsigned char)*start))
        start++;
    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
        end--;

    size_t len = (size_t)(end - start);
    char *copy = malloc(len + 1);
    if (!copy) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

void append_dispatch_log(const char *actor, const char *command,
                         const char *action, const DispatchLogField *targets,
                         size_t target_count, const DispatchLogField *context,
                         size_t context_count, const char *message) {
    DispatchLogRecord record = {
        .actor = actor && actor[0] ? actor : "user",
        .command = command,
        .action = action,
        .outcome = "success",
        .message = message,
        .targets = targets,
        .target_count = target_count,
        .context = context,
        .context_count = context_count,
    };

    char error[256] = {0};
    if (!dispatch_store_log_append(DISPATCH_LOG_FILE, &record, error,
                                   sizeof(error))) {
        fprintf(stderr, "Warning: could not append %s: %s\n",
                DISPATCH_LOG_FILE, error);
    }
}

void *cli_realloc_array(void *items, size_t count, size_t item_size) {
    void *new_items = realloc(items, count * item_size);
    if (!new_items) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return new_items;
}

char *join_path2(const char *left, const char *right) {
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    int needs_slash = left_len > 0 && left[left_len - 1] != '/';
    char *path = malloc(left_len + (size_t)needs_slash + right_len + 1);
    if (!path) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(path, left, left_len);
    if (needs_slash)
        path[left_len++] = '/';
    memcpy(path + left_len, right, right_len + 1);
    return path;
}

int make_dir_if_needed(const char *path) {
    if (mkdir(path, 0700) == 0)
        return 1;
    if (errno == EEXIST) {
        struct stat info;
        return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
    }
    return 0;
}

static char *agent_prompt_filename(const char *name) {
    const char *suffix = "-PROMPT.md";
    size_t size = strlen(name) + strlen(suffix) + 1;
    char *filename = malloc(size);
    if (!filename) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(filename, size, "%s%s", name, suffix);
    return filename;
}

char *agent_prompt_path_for(const char *agent_dir, const char *name) {
    char *filename = agent_prompt_filename(name);
    char *path = join_path2(agent_dir, filename);
    free(filename);
    return path;
}

int create_agent_dirs(const char *agent_dir, char **scratch_dir,
                             char **decisions_dir) {
    if (!make_dir_if_needed(".dispatch") ||
        !make_dir_if_needed(".dispatch/agents") ||
        !make_dir_if_needed(agent_dir)) {
        fprintf(stderr, "Could not create agent directory %s: %s\n", agent_dir,
                strerror(errno));
        return 0;
    }

    *scratch_dir = join_path2(agent_dir, "scratch");
    *decisions_dir = join_path2(agent_dir, "decisions");
    if (!make_dir_if_needed(*scratch_dir) ||
        !make_dir_if_needed(*decisions_dir)) {
        fprintf(stderr, "Could not create agent support directories: %s\n",
                strerror(errno));
        return 0;
    }
    return 1;
}

/* Write an instruction file only when it does not exist yet: scaffolding
 * must never clobber user edits. */
static void scaffold_workflow_file(const char *path, const char *content) {
    FILE *file = fopen(path, "r");
    if (file) {
        fclose(file);
        return;
    }
    file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Could not create %s: %s\n", path, strerror(errno));
        return;
    }
    fputs(content, file);
    fclose(file);
    printf("Created %s\n", path);
}

/* On a fresh init, create the .dispatch/ tree and the workflow instruction
 * files from the embedded templates. */
static void scaffold_workflow_dir(void) {
    if (!make_dir_if_needed(".dispatch") ||
        !make_dir_if_needed(".dispatch/agents")) {
        fprintf(stderr, "Could not create .dispatch directories: %s\n",
                strerror(errno));
    }
    scaffold_workflow_file("AGENTS.md", dispatch_template_agents_md);
    scaffold_workflow_file("CLAUDE.md", dispatch_template_claude_md);
}

int dispatch_cli_dispatch(int argc, char **argv) {
    if (argc < 2) {
        char *tui_argv[] = {
            argv[0],
            "tui",
            "--smoke",
        };
        if (getenv("DISPATCH_TUI_DEFAULT_SMOKE"))
            return dispatch_tui_main(3, tui_argv);
        return dispatch_tui_main(2, tui_argv);
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "help") == 0) {
        dispatch_cli_print_help();
        return 0;
    }

    const DispatchCliCommand *command = find_command(argv[1]);
    if (!command) {
        fprintf(stderr, "Unknown Dispatch command: %s\n", argv[1]);
        return 1;
    }

    if (strcmp(command->name, "init") == 0)
        return cmd_init(argc, argv);
    if (strcmp(command->name, "repo") == 0)
        return cmd_repo(argc, argv);
    if (strcmp(command->name, "agent") == 0)
        return cmd_agent(argc, argv);
    if (strcmp(command->name, "workspace") == 0)
        return cmd_workspace(argc, argv);
    if (strcmp(command->name, "group") == 0)
        return cmd_group(argc, argv);
    if (strcmp(command->name, "task") == 0)
        return cmd_task(argc, argv);
    if (strcmp(command->name, "dep") == 0)
        return cmd_dep(argc, argv);
    if (strcmp(command->name, "commit") == 0)
        return cmd_commit(argc, argv);
    if (strcmp(command->name, "completion") == 0)
        return cmd_completion(argc, argv);
    if (strcmp(command->name, "normalize") == 0)
        return cmd_normalize();
    if (strcmp(command->name, "list") == 0)
        return cmd_list(argc, argv);
    if (strcmp(command->name, "show") == 0)
        return cmd_show(argc, argv);
    if (strcmp(command->name, "blocked") == 0)
        return cmd_blocked(argc, argv);
    if (strcmp(command->name, "status") == 0)
        return cmd_status(argc, argv);
    if (strcmp(command->name, "doctor") == 0)
        return cmd_doctor(argc, argv);
    if (strcmp(command->name, "tui") == 0)
        return dispatch_tui_main(argc, argv);
    if (strcmp(command->name, "ready") == 0)
        return cmd_ready(argc, argv);
    if (strcmp(command->name, "reviews") == 0)
        return cmd_queue_list(argc, argv, DISPATCH_STATE_REVIEW, "reviews");
    if (strcmp(command->name, "proposed") == 0)
        return cmd_queue_list(argc, argv, DISPATCH_STATE_PROPOSED, "proposed");
    if (strcmp(command->name, "start") == 0)
        return cmd_start(argc, argv);
    if (strcmp(command->name, "finish") == 0)
        return cmd_finish(argc, argv);
    if (strcmp(command->name, "review") == 0)
        return cmd_review(argc, argv);
    if (strcmp(command->name, "unassign") == 0)
        return cmd_unassign(argc, argv);

    fprintf(stderr, "Command '%s' is not implemented.\n", command->name);
    return 2;
}
