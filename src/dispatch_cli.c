#include "dispatch_cli.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dispatch.h"
#include "dispatch_store.h"

typedef struct {
    const char *name;
    const char *summary;
} DispatchCliCommand;

typedef struct {
    DispatchBoard board;
    DispatchStoreLock lock;
    int loaded;
} LockedBoard;

static const DispatchCliCommand commands[] = {
    {"init", "Create dispatch.json for a target repository"},
    {"agent", "Manage agents"},
    {"workspace", "Manage workspaces"},
    {"group", "Manage groups"},
    {"task", "Manage tasks"},
    {"dep", "Manage dependencies"},
    {"ready", "List ready work or mark a task ready"},
    {"blocked", "List blocked work and blockers"},
    {"show", "Show one task"},
    {"list", "List tasks by group and workflow order"},
    {"start", "Start and assign a ready task"},
    {"finish", "Finish a task"},
    {"review", "Accept a task in review"},
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
    puts("  init, agent create/list/show/command, workspace create/list/show/remove/prune,");
    puts("  group add/ready, task add, dep add/remove, ready, start,");
    puts("  finish, review, normalize, list, show, blocked");
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

static int cmd_init(int argc, char **argv) {
    if (argc > 3) {
        fprintf(stderr, "Usage: dispatch init [repo-path]\n");
        return 1;
    }

    const char *repo_path = argc == 3 ? argv[2] : ".";
    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        fprintf(stderr, "%s\n", error);
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
        return 1;
    }

    if (existed) {
        printf("%s already exists\n", DISPATCH_STORE_FILE);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    printf("Created %s for repo %s\n", DISPATCH_STORE_FILE, repo_path);
    dispatch_store_lock_release(&lock);
    return 0;
}

static int load_board_or_error(DispatchBoard *board) {
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error))) {
        fprintf(stderr, "Could not initialize %s: %s\n", DISPATCH_STORE_FILE,
                error);
        return 0;
    }
    if (!dispatch_store_load(board, DISPATCH_STORE_FILE, error, sizeof(error))) {
        fprintf(stderr, "Could not load %s: %s\n", DISPATCH_STORE_FILE, error);
        return 0;
    }
    return 1;
}

static int locked_board_load_or_error(LockedBoard *locked) {
    char error[256] = {0};
    memset(locked, 0, sizeof(*locked));

    if (!dispatch_store_lock_acquire(&locked->lock, DISPATCH_STORE_FILE, 1000,
                                     error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
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

static int locked_board_save_or_error(LockedBoard *locked) {
    char error[256] = {0};
    if (!dispatch_store_save(&locked->board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "Could not save %s: %s\n", DISPATCH_STORE_FILE, error);
        return 0;
    }
    return 1;
}

static void locked_board_close(LockedBoard *locked) {
    if (!locked)
        return;
    if (locked->loaded) {
        dispatch_board_free(&locked->board);
        locked->loaded = 0;
    }
    dispatch_store_lock_release(&locked->lock);
}

static int title_starts_with_dispatch_id(const char *title) {
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

static const char *task_display_title(const DispatchTask *task) {
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

static char *cli_strdup(const char *value) {
    char *copy = strdup(value ? value : "");
    if (!copy) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return copy;
}

static void *cli_realloc_array(void *items, size_t count, size_t item_size) {
    void *new_items = realloc(items, count * item_size);
    if (!new_items) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return new_items;
}

static char *join_path2(const char *left, const char *right) {
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

static int make_dir_if_needed(const char *path) {
    if (mkdir(path, 0700) == 0)
        return 1;
    if (errno == EEXIST) {
        struct stat info;
        return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
    }
    return 0;
}

static int create_agent_dirs(const char *agent_dir, char **scratch_dir,
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

static int agent_name_is_valid(const char *name) {
    if (!name || name[0] == '\0')
        return 0;
    for (size_t i = 0; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static int agent_runner_is_valid(const char *runner) {
    return runner &&
           (strcmp(runner, "codex") == 0 || strcmp(runner, "claude") == 0);
}

static char *agent_command_for(const char *runner, const char *model,
                               const char *prompt_path) {
    const char *model_flag = model && model[0] ? " --model " : "";
    const char *model_value = model && model[0] ? model : "";
    const char *format = NULL;
    if (strcmp(runner, "codex") == 0) {
        format = "codex%s%s --prompt-file \"%s\"";
    } else {
        format = "claude%s%s --prompt-file \"%s\"";
    }

    size_t size = strlen(format) + strlen(model_flag) + strlen(model_value) +
                  strlen(prompt_path) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, format, model_flag, model_value, prompt_path);
    return command;
}

static int write_agent_prompt(const char *path, const char *name,
                              const char *runner) {
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Could not write %s: %s\n", path, strerror(errno));
        return 0;
    }

    fprintf(file, "# Dispatch Agent: %s\n\n", name);
    fprintf(file, "You are the `%s` agent running with `%s`.\n\n", name,
            runner);
    fprintf(file, "Rules:\n");
    fprintf(file, "- Work from the workflow directory that contains dispatch.json.\n");
    fprintf(file, "- Use the Dispatch CLI for all workflow state.\n");
    fprintf(file, "- Never read or edit dispatch.json directly.\n");
    fprintf(file, "- Start only ready, unassigned tasks assigned to your work.\n");
    fprintf(file, "- Assume other agents may be working in parallel.\n");
    fprintf(file, "- Do not edit another agent's task worktree or scratch directory.\n");
    fprintf(file, "- Keep temporary notes in .dispatch/agents/%s/scratch/.\n",
            name);
    fprintf(file, "- Keep agent-local decisions in .dispatch/agents/%s/decisions/.\n",
            name);
    fprintf(file, "- Write repository documentation only when the user asks or a Dispatch task requires it.\n");
    fclose(file);
    return 1;
}

static int write_agent_run_script(const char *path, const char *command) {
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Could not write %s: %s\n", path, strerror(errno));
        return 0;
    }

    fprintf(file, "#!/usr/bin/env bash\n");
    fprintf(file, "set -euo pipefail\n");
    fprintf(file, "cd \"$(dirname \"$0\")/../../..\"\n");
    fprintf(file, "exec %s\n", command);
    fclose(file);
    if (chmod(path, 0700) != 0) {
        fprintf(stderr, "Could not mark %s executable: %s\n", path,
                strerror(errno));
        return 0;
    }
    return 1;
}

static DispatchAgent *append_agent_record(DispatchBoard *board,
                                          const char *name,
                                          const char *runner,
                                          const char *model,
                                          const char *agent_dir,
                                          const char *prompt_path,
                                          const char *run_script_path) {
    if (dispatch_board_find_agent(board, name))
        return NULL;
    if (board->agents.count >= board->agents.capacity) {
        board->agents.capacity =
            board->agents.capacity == 0 ? 4 : board->agents.capacity * 2;
        board->agents.items = cli_realloc_array(
            board->agents.items, board->agents.capacity,
            sizeof(*board->agents.items));
    }

    DispatchAgent *agent = &board->agents.items[board->agents.count++];
    memset(agent, 0, sizeof(*agent));
    agent->name = cli_strdup(name);
    agent->runner = cli_strdup(runner);
    agent->model = model && model[0] ? cli_strdup(model) : NULL;
    agent->agent_dir = cli_strdup(agent_dir);
    agent->prompt_path = cli_strdup(prompt_path);
    agent->run_script_path =
        run_script_path && run_script_path[0] ? cli_strdup(run_script_path)
                                              : NULL;
    agent->created_at = time(NULL);
    return agent;
}

static int cmd_agent_create(int argc, char **argv) {
    const char *name = NULL;
    const char *runner = NULL;
    const char *model = NULL;
    int no_run_script = 0;
    int print_command = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && (i + 1) < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--runner") == 0 && (i + 1) < argc) {
            runner = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && (i + 1) < argc) {
            model = argv[++i];
        } else if (strcmp(argv[i], "--no-run-script") == 0) {
            no_run_script = 1;
        } else if (strcmp(argv[i], "--print-command") == 0) {
            print_command = 1;
        } else {
            fprintf(stderr,
                    "Usage: dispatch agent create --name <name> --runner codex|claude [--model <name>] [--no-run-script] [--print-command]\n");
            return 1;
        }
    }

    if (!agent_name_is_valid(name)) {
        fprintf(stderr,
                "Agent name must contain only letters, digits, '-' or '_'\n");
        return 1;
    }
    if (!agent_runner_is_valid(runner)) {
        fprintf(stderr, "Agent runner must be codex or claude\n");
        return 1;
    }

    char *agent_dir_base = join_path2(".dispatch/agents", name);
    char *prompt_path = join_path2(agent_dir_base, "AGENT.md");
    char *run_script_path = no_run_script ? NULL : join_path2(agent_dir_base, "run.sh");
    char *scratch_dir = NULL;
    char *decisions_dir = NULL;
    char *command = agent_command_for(runner, model, prompt_path);

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked)) {
        free(agent_dir_base);
        free(prompt_path);
        free(run_script_path);
        free(command);
        return 1;
    }
    if (dispatch_board_find_agent(&locked.board, name)) {
        locked_board_close(&locked);
        fprintf(stderr, "Agent %s already exists\n", name);
        free(agent_dir_base);
        free(prompt_path);
        free(run_script_path);
        free(command);
        return 1;
    }

    if (!create_agent_dirs(agent_dir_base, &scratch_dir, &decisions_dir) ||
        !write_agent_prompt(prompt_path, name, runner) ||
        (!no_run_script && !write_agent_run_script(run_script_path, command)) ||
        !append_agent_record(&locked.board, name, runner, model, agent_dir_base,
                             prompt_path, run_script_path) ||
        !locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        free(agent_dir_base);
        free(prompt_path);
        free(run_script_path);
        free(scratch_dir);
        free(decisions_dir);
        free(command);
        return 1;
    }

    printf("Created agent %s (%s)\n", name, runner);
    printf("  prompt: %s\n", prompt_path);
    printf("  scratch: %s\n", scratch_dir);
    printf("  decisions: %s\n", decisions_dir);
    if (run_script_path)
        printf("  run script: %s\n", run_script_path);
    if (print_command)
        printf("  command: %s\n", command);

    locked_board_close(&locked);
    free(agent_dir_base);
    free(prompt_path);
    free(run_script_path);
    free(scratch_dir);
    free(decisions_dir);
    free(command);
    return 0;
}

static int cmd_agent_list(int argc, char **argv) {
    (void)argv;
    if (argc != 3) {
        fprintf(stderr, "Usage: dispatch agent list\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (board.agents.count == 0) {
        printf("(no agents)\n");
        dispatch_board_free(&board);
        return 0;
    }

    for (size_t i = 0; i < board.agents.count; i++) {
        DispatchAgent *agent = &board.agents.items[i];
        printf("%-16s %-8s %s\n", agent->name, agent->runner,
               agent->agent_dir);
    }

    dispatch_board_free(&board);
    return 0;
}

static int cmd_agent_show(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: dispatch agent show <name>\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchAgent *agent = dispatch_board_find_agent(&board, argv[3]);
    if (!agent) {
        dispatch_board_free(&board);
        fprintf(stderr, "No agent named %s\n", argv[3]);
        return 1;
    }

    printf("Name: %s\n", agent->name);
    printf("Runner: %s\n", agent->runner);
    printf("Model: %s\n", agent->model ? agent->model : "-");
    printf("Agent dir: %s\n", agent->agent_dir);
    printf("Prompt: %s\n", agent->prompt_path);
    printf("Run script: %s\n",
           agent->run_script_path ? agent->run_script_path : "-");

    dispatch_board_free(&board);
    return 0;
}

static int cmd_agent_command(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "Usage: dispatch agent command <name> [--print-command]\n");
        return 1;
    }
    if (argc == 5 && strcmp(argv[4], "--print-command") != 0) {
        fprintf(stderr, "Unknown agent command option: %s\n", argv[4]);
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchAgent *agent = dispatch_board_find_agent(&board, argv[3]);
    if (!agent) {
        dispatch_board_free(&board);
        fprintf(stderr, "No agent named %s\n", argv[3]);
        return 1;
    }

    char *command =
        agent_command_for(agent->runner, agent->model, agent->prompt_path);
    printf("%s\n", command);
    free(command);

    dispatch_board_free(&board);
    return 0;
}

static int cmd_agent(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "create") == 0)
        return cmd_agent_create(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "list") == 0)
        return cmd_agent_list(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "show") == 0)
        return cmd_agent_show(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "command") == 0)
        return cmd_agent_command(argc, argv);

    fprintf(stderr,
            "Usage: dispatch agent create --name <name> --runner codex|claude [--model <name>] [--no-run-script] [--print-command]\n");
    fprintf(stderr, "       dispatch agent list\n");
    fprintf(stderr, "       dispatch agent show <name>\n");
    fprintf(stderr, "       dispatch agent command <name> [--print-command]\n");
    return 1;
}

static void print_workspace_create_usage(void) {
    fprintf(stderr,
            "Usage: dispatch workspace create <task-id> --actor <name> [--repo <path>] [--dir <path>] [--branch <name>] [--sequence]\n");
}

static void print_workspace_remove_usage(void) {
    fprintf(stderr,
            "Usage: dispatch workspace remove <task-id-or-workspace> [--force]\n");
}

static void print_workspace_prune_usage(void) {
    fprintf(stderr,
            "Usage: dispatch workspace prune [--done] [--stale] [--dry-run]\n");
}

static char *current_workflow_path(void) {
    char *path = getcwd(NULL, 0);
    if (!path) {
        fprintf(stderr, "Could not resolve current directory: %s\n",
                strerror(errno));
        return NULL;
    }
    return path;
}

static char *path_dirname_copy(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return cli_strdup(".");
    if (slash == path)
        return cli_strdup("/");

    size_t length = (size_t)(slash - path);
    char *dirname = malloc(length + 1);
    if (!dirname) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(dirname, path, length);
    dirname[length] = '\0';
    return dirname;
}

static const char *path_basename_view(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static char *resolve_future_path(const char *workflow_dir, const char *path) {
    if (!path || path[0] == '\0')
        return NULL;
    if (path[0] == '/')
        return cli_strdup(path);

    char *dirname = path_dirname_copy(path);
    const char *basename = path_basename_view(path);
    if (basename[0] == '\0' || strcmp(basename, ".") == 0 ||
        strcmp(basename, "..") == 0) {
        free(dirname);
        return NULL;
    }

    char *parent = dispatch_resolve_path(workflow_dir, dirname);
    free(dirname);
    if (!parent)
        return NULL;

    char *resolved = join_path2(parent, basename);
    free(parent);
    return resolved;
}

static int workspace_record_is_live(const DispatchWorkspace *workspace) {
    return workspace->state != DISPATCH_WORKSPACE_REMOVED;
}

static int string_list_contains_cli(const DispatchStringList *list,
                                    const char *value) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], value) == 0)
            return 1;
    }
    return 0;
}

static void string_list_append_cli(DispatchStringList *list,
                                   const char *value) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = cli_realloc_array(list->items, list->capacity,
                                        sizeof(*list->items));
    }
    list->items[list->count++] = cli_strdup(value);
}

static void string_list_free_cli(DispatchStringList *list) {
    for (size_t i = 0; i < list->count; i++)
        free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int workspace_covers_task(const DispatchWorkspace *workspace,
                                 const char *task_id) {
    return strcmp(workspace->task_id, task_id) == 0 ||
           string_list_contains_cli(&workspace->sequence_tasks, task_id);
}

static int workspace_branch_exists(const DispatchBoard *board,
                                   const char *branch) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace_record_is_live(workspace) &&
            strcmp(workspace->branch, branch) == 0) {
            return 1;
        }
    }
    return 0;
}

static int workspace_path_exists(const DispatchBoard *board, const char *path) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace_record_is_live(workspace) &&
            strcmp(workspace->path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static DispatchWorkspace *live_workspace_for_task(const DispatchBoard *board,
                                                  const char *task_id) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace_record_is_live(workspace) &&
            workspace_covers_task(workspace, task_id)) {
            return workspace;
        }
    }
    return NULL;
}

static void workspace_free_fields(DispatchWorkspace *workspace) {
    free(workspace->id);
    free(workspace->task_id);
    free(workspace->actor);
    free(workspace->path);
    free(workspace->branch);
    free(workspace->repo_path);
    for (size_t i = 0; i < workspace->sequence_tasks.count; i++)
        free(workspace->sequence_tasks.items[i]);
    free(workspace->sequence_tasks.items);
    free(workspace->review_gate);
}

static int remove_workspace_record(DispatchBoard *board, const char *task_id) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (strcmp(workspace->task_id, task_id) != 0)
            continue;

        workspace_free_fields(workspace);
        for (size_t j = i + 1; j < board->workspaces.count; j++)
            board->workspaces.items[j - 1] = board->workspaces.items[j];
        board->workspaces.count--;
        return 1;
    }
    return 0;
}

static DispatchWorkspace *append_workspace_reservation(
    DispatchBoard *board, const char *task_id, const char *actor,
    const char *repo_path, const char *workspace_path, const char *branch,
    const DispatchStringList *sequence_tasks, const char *review_gate) {
    if (board->workspaces.count >= board->workspaces.capacity) {
        board->workspaces.capacity = board->workspaces.capacity == 0
                                         ? 4
                                         : board->workspaces.capacity * 2;
        board->workspaces.items = cli_realloc_array(
            board->workspaces.items, board->workspaces.capacity,
            sizeof(*board->workspaces.items));
    }

    DispatchWorkspace *workspace =
        &board->workspaces.items[board->workspaces.count++];
    memset(workspace, 0, sizeof(*workspace));
    workspace->id = cli_strdup(task_id);
    workspace->task_id = cli_strdup(task_id);
    workspace->actor = cli_strdup(actor);
    workspace->repo_path = cli_strdup(repo_path);
    workspace->path = cli_strdup(workspace_path);
    workspace->branch = cli_strdup(branch);
    for (size_t i = 0; sequence_tasks && i < sequence_tasks->count; i++)
        string_list_append_cli(&workspace->sequence_tasks,
                               sequence_tasks->items[i]);
    workspace->review_gate = review_gate ? cli_strdup(review_gate) : NULL;
    workspace->state = DISPATCH_WORKSPACE_CREATING;
    workspace->created_at = time(NULL);
    workspace->updated_at = workspace->created_at;
    return workspace;
}

static int workspace_all_tasks_done(const DispatchBoard *board,
                                    const DispatchWorkspace *workspace) {
    const DispatchStringList *tasks = &workspace->sequence_tasks;
    if (tasks->count == 0) {
        DispatchTask *task =
            dispatch_board_find_task((DispatchBoard *)board,
                                     workspace->task_id);
        return task &&
               dispatch_task_effective_state(board, task) ==
                   DISPATCH_STATE_DONE;
    }

    for (size_t i = 0; i < tasks->count; i++) {
        DispatchTask *task =
            dispatch_board_find_task((DispatchBoard *)board,
                                     tasks->items[i]);
        if (!task ||
            dispatch_task_effective_state(board, task) !=
                DISPATCH_STATE_DONE) {
            return 0;
        }
    }
    return 1;
}

static DispatchTask *single_dependent_task(const DispatchBoard *board,
                                           const char *task_id,
                                           size_t *dependent_count) {
    DispatchTask *dependent = NULL;
    *dependent_count = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *candidate = &board->tasks.items[i];
        if (!string_list_contains_cli(&candidate->depends_on, task_id))
            continue;
        dependent = candidate;
        (*dependent_count)++;
    }
    return *dependent_count == 1 ? dependent : NULL;
}

static int task_depends_only_on(const DispatchTask *task,
                                const char *dependency_id) {
    return task->depends_on.count == 1 &&
           strcmp(task->depends_on.items[0], dependency_id) == 0;
}

static int build_workspace_sequence(const DispatchBoard *board,
                                    DispatchTask *start,
                                    DispatchStringList *sequence_tasks,
                                    char **review_gate) {
    DispatchTask *current = start;
    for (size_t guard = 0; guard < board->tasks.count; guard++) {
        if (current->assigned_to) {
            fprintf(stderr, "Sequence task %s must be unassigned\n",
                    current->id);
            return 0;
        }
        DispatchState state = dispatch_task_effective_state(board, current);
        if (state != DISPATCH_STATE_READY && state != DISPATCH_STATE_BLOCKED) {
            fprintf(stderr, "Sequence task %s must be ready or blocked\n",
                    current->id);
            return 0;
        }
        if (live_workspace_for_task(board, current->id)) {
            fprintf(stderr, "Workspace already exists for %s\n", current->id);
            return 0;
        }

        string_list_append_cli(sequence_tasks, current->id);
        if (current->requires_review) {
            *review_gate = cli_strdup(current->id);
            return 1;
        }

        size_t dependent_count = 0;
        DispatchTask *next =
            single_dependent_task(board, current->id, &dependent_count);
        if (!next) {
            fprintf(stderr,
                    "Sequence task %s must have exactly one dependent\n",
                    current->id);
            return 0;
        }
        if (!task_depends_only_on(next, current->id)) {
            fprintf(stderr, "Sequence task %s must depend only on %s\n",
                    next->id, current->id);
            return 0;
        }
        current = next;
    }

    fprintf(stderr, "Sequence from %s does not reach a review gate\n",
            start->id);
    return 0;
}

static char *shell_quote(const char *value) {
    size_t size = 3;
    for (size_t i = 0; value[i] != '\0'; i++)
        size += value[i] == '\'' ? 4 : 1;

    char *quoted = malloc(size);
    if (!quoted) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    size_t out = 0;
    quoted[out++] = '\'';
    for (size_t i = 0; value[i] != '\0'; i++) {
        if (value[i] == '\'') {
            memcpy(quoted + out, "'\\''", 4);
            out += 4;
        } else {
            quoted[out++] = value[i];
        }
    }
    quoted[out++] = '\'';
    quoted[out] = '\0';
    return quoted;
}

static int run_shell_command_quiet(const char *command) {
    int status = system(command);
    return status == 0;
}

static int git_branch_exists(const char *repo_path, const char *branch) {
    char *repo_q = shell_quote(repo_path);
    char *branch_q = shell_quote(branch);
    size_t size = strlen("git -C  rev-parse --verify --quiet refs/heads/ >/dev/null 2>&1") +
                  strlen(repo_q) + strlen(branch_q) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size,
             "git -C %s rev-parse --verify --quiet refs/heads/%s >/dev/null 2>&1",
             repo_q, branch_q);
    int exists = run_shell_command_quiet(command);
    free(repo_q);
    free(branch_q);
    free(command);
    return exists;
}

static int git_worktree_add(const char *repo_path, const char *workspace_path,
                            const char *branch, int branch_exists) {
    char *repo_q = shell_quote(repo_path);
    char *workspace_q = shell_quote(workspace_path);
    char *branch_q = shell_quote(branch);
    const char *format_existing =
        "git -C %s worktree add %s %s >/dev/null 2>&1";
    const char *format_new =
        "git -C %s worktree add -b %s %s HEAD >/dev/null 2>&1";
    const char *format = branch_exists ? format_existing : format_new;
    size_t size = strlen(format) + strlen(repo_q) + strlen(workspace_q) +
                  strlen(branch_q) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    if (branch_exists) {
        snprintf(command, size, format, repo_q, workspace_q, branch_q);
    } else {
        snprintf(command, size, format, repo_q, branch_q, workspace_q);
    }
    int ok = run_shell_command_quiet(command);
    free(repo_q);
    free(workspace_q);
    free(branch_q);
    free(command);
    return ok;
}

static int git_worktree_remove_force(const char *repo_path,
                                     const char *workspace_path) {
    char *repo_q = shell_quote(repo_path);
    char *workspace_q = shell_quote(workspace_path);
    size_t size = strlen("git -C  worktree remove --force  >/dev/null 2>&1") +
                  strlen(repo_q) + strlen(workspace_q) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size,
             "git -C %s worktree remove --force %s >/dev/null 2>&1", repo_q,
             workspace_q);
    int ok = run_shell_command_quiet(command);
    free(repo_q);
    free(workspace_q);
    free(command);
    return ok;
}

static int git_worktree_remove(const char *repo_path,
                               const char *workspace_path) {
    char *repo_q = shell_quote(repo_path);
    char *workspace_q = shell_quote(workspace_path);
    size_t size = strlen("git -C  worktree remove  >/dev/null 2>&1") +
                  strlen(repo_q) + strlen(workspace_q) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, "git -C %s worktree remove %s >/dev/null 2>&1",
             repo_q, workspace_q);
    int ok = run_shell_command_quiet(command);
    free(repo_q);
    free(workspace_q);
    free(command);
    return ok;
}

static int git_worktree_is_registered(const char *repo_path,
                                      const char *workspace_path) {
    char *repo_q = shell_quote(repo_path);
    size_t line_size = strlen("worktree ") + strlen(workspace_path) + 1;
    char *line = malloc(line_size);
    if (!line) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(line, line_size, "worktree %s", workspace_path);
    char *line_q = shell_quote(line);
    const char *format =
        "git -C %s worktree list --porcelain | grep -Fx %s >/dev/null 2>&1";
    size_t size = strlen(format) + strlen(repo_q) + strlen(line_q) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, format, repo_q, line_q);
    int registered = run_shell_command_quiet(command);
    free(repo_q);
    free(line);
    free(line_q);
    free(command);
    return registered;
}

static int git_worktree_is_dirty(const char *workspace_path) {
    char *workspace_q = shell_quote(workspace_path);
    size_t size = strlen("git -C  status --porcelain 2>/dev/null") +
                  strlen(workspace_q) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, "git -C %s status --porcelain 2>/dev/null",
             workspace_q);

    FILE *pipe = popen(command, "r");
    free(workspace_q);
    free(command);
    if (!pipe)
        return 1;

    int ch = fgetc(pipe);
    int status = pclose(pipe);
    return ch != EOF || status != 0;
}

static int mark_workspace_active(const char *task_id) {
    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 0;

    DispatchWorkspace *workspace =
        dispatch_board_find_workspace(&locked.board, task_id);
    if (!workspace) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace reservation disappeared for %s\n", task_id);
        return 0;
    }

    workspace->state = DISPATCH_WORKSPACE_ACTIVE;
    workspace->updated_at = time(NULL);
    int ok = locked_board_save_or_error(&locked);
    locked_board_close(&locked);
    return ok;
}

static void remove_workspace_reservation_after_failure(const char *task_id) {
    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return;
    remove_workspace_record(&locked.board, task_id);
    (void)locked_board_save_or_error(&locked);
    locked_board_close(&locked);
}

static int cmd_workspace_create(int argc, char **argv) {
    if (argc < 5) {
        print_workspace_create_usage();
        return 1;
    }

    const char *task_id = argv[3];
    const char *actor = NULL;
    const char *repo_option = NULL;
    const char *dir_option = NULL;
    const char *branch_option = NULL;
    int sequence = 0;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
            actor = argv[++i];
        } else if (strcmp(argv[i], "--repo") == 0 && (i + 1) < argc) {
            repo_option = argv[++i];
        } else if (strcmp(argv[i], "--dir") == 0 && (i + 1) < argc) {
            dir_option = argv[++i];
        } else if (strcmp(argv[i], "--branch") == 0 && (i + 1) < argc) {
            branch_option = argv[++i];
        } else if (strcmp(argv[i], "--sequence") == 0) {
            sequence = 1;
        } else {
            print_workspace_create_usage();
            return 1;
        }
    }

    if (!actor || actor[0] == '\0') {
        print_workspace_create_usage();
        return 1;
    }
    if (!dispatch_actor_label_is_valid(actor)) {
        fprintf(stderr,
                "Actor must start with an ASCII letter or digit and contain only letters, digits, '.', '_' or '-'\n");
        return 1;
    }

    char *workflow_dir = current_workflow_path();
    if (!workflow_dir)
        return 1;

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked)) {
        free(workflow_dir);
        return 1;
    }
    DispatchBoard *board = &locked.board;

    DispatchTask *task = dispatch_board_find_task(board, task_id);
    if (!task) {
        locked_board_close(&locked);
        fprintf(stderr, "No task with id %s\n", task_id);
        free(workflow_dir);
        return 1;
    }
    if (dispatch_task_effective_state(board, task) != DISPATCH_STATE_READY ||
        task->assigned_to) {
        locked_board_close(&locked);
        fprintf(stderr, "Task %s must be ready and unassigned\n", task_id);
        free(workflow_dir);
        return 1;
    }
    if (live_workspace_for_task(board, task_id)) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace already exists for %s\n", task_id);
        free(workflow_dir);
        return 1;
    }

    DispatchStringList sequence_tasks = {0};
    char *review_gate = NULL;
    if (sequence &&
        !build_workspace_sequence(board, task, &sequence_tasks, &review_gate)) {
        locked_board_close(&locked);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(workflow_dir);
        return 1;
    }

    const char *configured_repo =
        repo_option && repo_option[0] ? repo_option : board->repo_path;
    char *repo_path = dispatch_resolve_path(workflow_dir, configured_repo);
    if (!repo_path || !dispatch_path_is_git_repository(repo_path)) {
        fprintf(stderr,
                "Configured repository is not a git repository: %s\n",
                configured_repo);
        locked_board_close(&locked);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(workflow_dir);
        free(repo_path);
        return 1;
    }

    char *sequence_name = NULL;
    if (sequence) {
        size_t size = strlen(task_id) + strlen("-sequence") + 1;
        sequence_name = malloc(size);
        if (!sequence_name) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        snprintf(sequence_name, size, "%s-sequence", task_id);
    }

    const char *name_for_defaults = sequence ? sequence_name : task_id;
    char *branch =
        branch_option && branch_option[0]
            ? cli_strdup(branch_option)
            : dispatch_default_workspace_branch(actor, name_for_defaults);
    if (!branch || branch[0] == '\0') {
        locked_board_close(&locked);
        fprintf(stderr, "Could not derive workspace branch\n");
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        return 1;
    }

    char *workspace_path = dir_option && dir_option[0]
                               ? resolve_future_path(workflow_dir, dir_option)
                               : dispatch_default_workspace_path(repo_path,
                                                                 actor,
                                                                 name_for_defaults);
    if (!workspace_path) {
        locked_board_close(&locked);
        fprintf(stderr, "Could not resolve workspace directory\n");
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        return 1;
    }

    if (strcmp(repo_path, workspace_path) == 0) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace path must not equal repository path\n");
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }
    if (workspace_branch_exists(board, branch)) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace branch already reserved: %s\n", branch);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }
    if (workspace_path_exists(board, workspace_path)) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace path already reserved: %s\n",
                workspace_path);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }
    struct stat workspace_info;
    if (stat(workspace_path, &workspace_info) == 0) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace path already exists: %s\n",
                workspace_path);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }

    append_workspace_reservation(board, task_id, actor, repo_path,
                                 workspace_path, branch,
                                 sequence ? &sequence_tasks : NULL,
                                 review_gate);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }

    locked_board_close(&locked);

    int branch_exists = git_branch_exists(repo_path, branch);
    if (!git_worktree_add(repo_path, workspace_path, branch, branch_exists)) {
        (void)git_worktree_remove_force(repo_path, workspace_path);
        remove_workspace_reservation_after_failure(task_id);
        fprintf(stderr, "Could not create git worktree for %s\n", task_id);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }

    if (!mark_workspace_active(task_id)) {
        if (!git_worktree_remove_force(repo_path, workspace_path)) {
            fprintf(stderr,
                    "Could not roll back git worktree %s; manual cleanup may be required\n",
                    workspace_path);
        }
        remove_workspace_reservation_after_failure(task_id);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }

    printf("Created workspace %s for %s\n", task_id, actor);
    printf("  path: %s\n", workspace_path);
    printf("  branch: %s\n", branch);
    printf("  state: active\n");
    if (sequence) {
        printf("  tasks:");
        for (size_t i = 0; i < sequence_tasks.count; i++)
            printf("%s%s", i == 0 ? " " : ",", sequence_tasks.items[i]);
        printf("\n");
        printf("  review gate: %s\n", review_gate ? review_gate : "-");
    }

    string_list_free_cli(&sequence_tasks);
    free(review_gate);
    free(sequence_name);
    free(workflow_dir);
    free(repo_path);
    free(branch);
    free(workspace_path);
    return 0;
}

static int workspace_git_worktree_present(const DispatchWorkspace *workspace) {
    if (!workspace || !workspace->path || workspace->path[0] == '\0')
        return 0;

    struct stat info;
    if (stat(workspace->path, &info) != 0 || !S_ISDIR(info.st_mode))
        return 0;

    char *git_path = join_path2(workspace->path, ".git");
    int present = stat(git_path, &info) == 0 &&
                  (S_ISDIR(info.st_mode) || S_ISREG(info.st_mode));
    free(git_path);
    return present;
}

static int cmd_workspace_list(int argc, char **argv) {
    (void)argv;
    if (argc != 3) {
        fprintf(stderr, "Usage: dispatch workspace list\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (board.workspaces.count == 0) {
        printf("(no workspaces)\n");
        dispatch_board_free(&board);
        return 0;
    }

    for (size_t i = 0; i < board.workspaces.count; i++) {
        DispatchWorkspace *workspace = &board.workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
            continue;
        DispatchTask *task = dispatch_board_find_task(&board,
                                                      workspace->task_id);
        const char *task_state =
            task ? dispatch_state_name(dispatch_task_effective_state(&board,
                                                                     task))
                 : "missing";
        printf("%-8s %-10s %-10s %-16s %s  %s\n", workspace->task_id,
               task_state, dispatch_workspace_state_name(workspace->state),
               workspace->actor, workspace->branch, workspace->path);
    }

    dispatch_board_free(&board);
    return 0;
}

static int cmd_workspace_show(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: dispatch workspace show <task-id-or-workspace>\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchWorkspace *workspace = live_workspace_for_task(&board, argv[3]);
    if (!workspace || workspace->state == DISPATCH_WORKSPACE_REMOVED) {
        dispatch_board_free(&board);
        fprintf(stderr, "No workspace for %s\n", argv[3]);
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(&board, workspace->task_id);
    const char *task_state =
        task ? dispatch_state_name(dispatch_task_effective_state(&board, task))
             : "missing";

    printf("Task: %s\n", workspace->task_id);
    printf("Task state: %s\n", task_state);
    printf("Workspace state: %s\n",
           dispatch_workspace_state_name(workspace->state));
    printf("Actor: %s\n", workspace->actor);
    printf("Branch: %s\n", workspace->branch);
    printf("Path: %s\n", workspace->path);
    printf("Repo: %s\n", workspace->repo_path);
    if (workspace->sequence_tasks.count > 0) {
        printf("Sequence tasks:");
        for (size_t i = 0; i < workspace->sequence_tasks.count; i++)
            printf("%s%s", i == 0 ? " " : ",",
                   workspace->sequence_tasks.items[i]);
        printf("\n");
        printf("Review gate: %s\n",
               workspace->review_gate ? workspace->review_gate : "-");
    }
    printf("Git worktree: %s\n",
           workspace_git_worktree_present(workspace) ? "present" : "missing");

    dispatch_board_free(&board);
    return 0;
}

static int cmd_workspace_remove(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        print_workspace_remove_usage();
        return 1;
    }

    const char *target = argv[3];
    int force = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else {
            print_workspace_remove_usage();
            return 1;
        }
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    DispatchWorkspace *workspace = live_workspace_for_task(&locked.board,
                                                           target);
    if (!workspace || workspace->state == DISPATCH_WORKSPACE_REMOVED) {
        locked_board_close(&locked);
        fprintf(stderr, "No workspace for %s\n", target);
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(&locked.board,
                                                  workspace->task_id);
    DispatchState task_state = task ? dispatch_task_effective_state(&locked.board,
                                                                    task)
                                    : DISPATCH_STATE_PROPOSED;
    if (!force && task_state == DISPATCH_STATE_DOING) {
        fprintf(stderr,
                "Workspace task %s is doing; use --force to remove\n",
                workspace->task_id);
        locked_board_close(&locked);
        return 1;
    }

    char *record_task_id = cli_strdup(workspace->task_id);
    char *repo_path = cli_strdup(workspace->repo_path);
    char *workspace_path = cli_strdup(workspace->path);
    locked_board_close(&locked);

    if (!git_worktree_is_registered(repo_path, workspace_path)) {
        fprintf(stderr, "Workspace path is not a git worktree: %s\n",
                workspace_path);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    if (!force && git_worktree_is_dirty(workspace_path)) {
        fprintf(stderr, "Workspace has uncommitted changes: %s\n",
                workspace_path);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    int removed = force ? git_worktree_remove_force(repo_path, workspace_path)
                        : git_worktree_remove(repo_path, workspace_path);
    if (!removed) {
        fprintf(stderr, "Could not remove git worktree: %s\n",
                workspace_path);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    if (!locked_board_load_or_error(&locked)) {
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }
    if (!remove_workspace_record(&locked.board, record_task_id)) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace record disappeared for %s\n",
                record_task_id);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }
    locked_board_close(&locked);

    printf("Removed workspace %s\n", record_task_id);
    printf("  path: %s\n", workspace_path);
    free(record_task_id);
    free(repo_path);
    free(workspace_path);
    return 0;
}

typedef enum {
    PRUNE_CANDIDATE_DONE,
    PRUNE_CANDIDATE_STALE
} PruneCandidateKind;

typedef struct {
    char *task_id;
    char *repo_path;
    char *workspace_path;
    PruneCandidateKind kind;
} PruneCandidate;

typedef struct {
    PruneCandidate *items;
    size_t count;
    size_t capacity;
} PruneCandidates;

static void prune_candidates_append(PruneCandidates *candidates,
                                    const DispatchWorkspace *workspace,
                                    PruneCandidateKind kind) {
    if (candidates->count >= candidates->capacity) {
        candidates->capacity =
            candidates->capacity == 0 ? 4 : candidates->capacity * 2;
        candidates->items = cli_realloc_array(candidates->items,
                                              candidates->capacity,
                                              sizeof(*candidates->items));
    }

    PruneCandidate *candidate = &candidates->items[candidates->count++];
    candidate->task_id = cli_strdup(workspace->task_id);
    candidate->repo_path = cli_strdup(workspace->repo_path);
    candidate->workspace_path = cli_strdup(workspace->path);
    candidate->kind = kind;
}

static void prune_candidates_free(PruneCandidates *candidates) {
    for (size_t i = 0; i < candidates->count; i++) {
        free(candidates->items[i].task_id);
        free(candidates->items[i].repo_path);
        free(candidates->items[i].workspace_path);
    }
    free(candidates->items);
    memset(candidates, 0, sizeof(*candidates));
}

static int remove_workspace_records_by_id(DispatchStringList *task_ids) {
    if (task_ids->count == 0)
        return 1;

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 0;

    for (size_t i = 0; i < task_ids->count; i++)
        (void)remove_workspace_record(&locked.board, task_ids->items[i]);

    int ok = locked_board_save_or_error(&locked);
    locked_board_close(&locked);
    return ok;
}

static int cmd_workspace_prune(int argc, char **argv) {
    int prune_done = 0;
    int prune_stale = 0;
    int dry_run = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--done") == 0) {
            prune_done = 1;
        } else if (strcmp(argv[i], "--stale") == 0) {
            prune_stale = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else {
            print_workspace_prune_usage();
            return 1;
        }
    }

    if (!prune_done && !prune_stale) {
        print_workspace_prune_usage();
        return 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    PruneCandidates candidates = {0};
    for (size_t i = 0; i < locked.board.workspaces.count; i++) {
        DispatchWorkspace *workspace = &locked.board.workspaces.items[i];
        if (!workspace_record_is_live(workspace))
            continue;

        if (prune_stale &&
            workspace->state == DISPATCH_WORKSPACE_CREATING) {
            prune_candidates_append(&candidates, workspace,
                                    PRUNE_CANDIDATE_STALE);
            continue;
        }

        if (prune_done && workspace_all_tasks_done(&locked.board, workspace)) {
            prune_candidates_append(&candidates, workspace,
                                    PRUNE_CANDIDATE_DONE);
        }
    }
    locked_board_close(&locked);

    DispatchStringList records_to_remove = {0};
    size_t pruned = 0;
    size_t skipped = 0;
    for (size_t i = 0; i < candidates.count; i++) {
        PruneCandidate *candidate = &candidates.items[i];
        int registered = git_worktree_is_registered(candidate->repo_path,
                                                    candidate->workspace_path);

        if (candidate->kind == PRUNE_CANDIDATE_STALE) {
            if (registered) {
                printf("Skipped stale workspace %s: git worktree exists\n",
                       candidate->task_id);
                skipped++;
                continue;
            }

            printf("%s stale workspace %s\n",
                   dry_run ? "Would prune" : "Pruned", candidate->task_id);
            pruned++;
            if (!dry_run)
                string_list_append_cli(&records_to_remove,
                                       candidate->task_id);
            continue;
        }

        if (!registered) {
            printf("Skipped done workspace %s: path is not a git worktree\n",
                   candidate->task_id);
            skipped++;
            continue;
        }

        if (git_worktree_is_dirty(candidate->workspace_path)) {
            printf("Skipped done workspace %s: workspace has uncommitted changes\n",
                   candidate->task_id);
            skipped++;
            continue;
        }

        if (dry_run) {
            printf("Would remove done workspace %s\n", candidate->task_id);
            pruned++;
            continue;
        }

        if (!git_worktree_remove(candidate->repo_path,
                                 candidate->workspace_path)) {
            printf("Skipped done workspace %s: could not remove git worktree\n",
                   candidate->task_id);
            skipped++;
            continue;
        }

        printf("Removed done workspace %s\n", candidate->task_id);
        pruned++;
        string_list_append_cli(&records_to_remove, candidate->task_id);
    }

    int ok = dry_run || remove_workspace_records_by_id(&records_to_remove);
    if (pruned == 0 && skipped == 0)
        printf("No workspaces pruned\n");

    string_list_free_cli(&records_to_remove);
    prune_candidates_free(&candidates);
    return ok ? 0 : 1;
}

static int cmd_workspace(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "create") == 0)
        return cmd_workspace_create(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "list") == 0)
        return cmd_workspace_list(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "show") == 0)
        return cmd_workspace_show(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "remove") == 0)
        return cmd_workspace_remove(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "prune") == 0)
        return cmd_workspace_prune(argc, argv);

    print_workspace_create_usage();
    fprintf(stderr, "       dispatch workspace list\n");
    fprintf(stderr, "       dispatch workspace show <task-id-or-workspace>\n");
    fprintf(stderr,
            "       dispatch workspace remove <task-id-or-workspace> [--force]\n");
    fprintf(stderr,
            "       dispatch workspace prune [--done] [--stale] [--dry-run]\n");
    return 1;
}

static int cmd_group_add(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: dispatch group add <name> [--prefix XX]\n");
        return 1;
    }

    const char *name = argv[3];
    const char *prefix = NULL;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--prefix") == 0 && (i + 1) < argc) {
            prefix = argv[++i];
        } else {
            fprintf(stderr, "Unknown group option: %s\n", argv[i]);
            return 1;
        }
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    if (!dispatch_board_add_group(board, name, prefix)) {
        locked_board_close(&locked);
        fprintf(stderr, "Could not add group '%s'\n", name);
        return 1;
    }

    DispatchGroup *group = dispatch_board_find_group(board, name);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    printf("Added group %s (%s)\n", group->name, group->prefix);
    locked_board_close(&locked);
    return 0;
}

static int cmd_group_ready(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr,
                "Usage: dispatch group ready <group> --actor <name> [--no-review]\n");
        return 1;
    }

    const char *group_id = argv[3];
    const char *actor = NULL;
    int no_review = 0;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
            actor = argv[++i];
        } else if (strcmp(argv[i], "--no-review") == 0) {
            no_review = 1;
        } else {
            fprintf(stderr, "Unknown group ready option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!actor || actor[0] == '\0') {
        fprintf(stderr,
                "Usage: dispatch group ready <group> --actor <name> [--no-review]\n");
        return 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchGroup *group = dispatch_board_find_group(board, group_id);
    if (!group) {
        locked_board_close(&locked);
        fprintf(stderr, "No group with id, prefix, or name %s\n", group_id);
        return 1;
    }

    int readied = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (strcmp(task->group, group->id) != 0)
            continue;
        if (task->state != DISPATCH_STATE_PROPOSED)
            continue;
        dispatch_task_mark_ready(board, task, actor);
        if (no_review)
            task->requires_review = 0;
        readied++;
    }

    dispatch_board_normalize_states(board);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    printf("Readied %d task%s in group %s\n", readied,
           readied == 1 ? "" : "s", group->prefix);
    locked_board_close(&locked);
    return 0;
}

static int cmd_group(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "add") == 0)
        return cmd_group_add(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "ready") == 0)
        return cmd_group_ready(argc, argv);

    fprintf(stderr, "Usage: dispatch group add <name> [--prefix XX]\n");
    fprintf(stderr,
            "       dispatch group ready <group> --actor <name> [--no-review]\n");
    return 1;
}

static int cmd_task(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[2], "delete") == 0) {
        const char *task_id = argv[3];
        int force = 0;
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--force") == 0) {
                force = 1;
            } else {
                fprintf(stderr, "Unknown task delete option: %s\n", argv[i]);
                return 1;
            }
        }

        LockedBoard locked;
        if (!locked_board_load_or_error(&locked))
            return 1;
        if (!dispatch_board_delete_task(&locked.board, task_id, force)) {
            locked_board_close(&locked);
            fprintf(stderr,
                    "Could not delete %s%s\n",
                    task_id,
                    force ? "" : " (task may have dependents; use --force)");
            return 1;
        }
        if (!locked_board_save_or_error(&locked)) {
            locked_board_close(&locked);
            return 1;
        }
        printf("Deleted task %s\n", task_id);
        locked_board_close(&locked);
        return 0;
    }

    if (argc < 5 || strcmp(argv[2], "add") != 0) {
        fprintf(stderr,
                "Usage: dispatch task add <group> <title> [--description "
                "text] [--no-review]\n");
        fprintf(stderr, "       dispatch task delete <id> [--force]\n");
        return 1;
    }

    const char *group = argv[3];
    const char *title = argv[4];
    const char *description = "";
    int requires_review = 1;

    if (title_starts_with_dispatch_id(title)) {
        fprintf(stderr,
                "Task titles should not include Dispatch IDs; use a human-readable title without a prefix like DE-01\n");
        return 1;
    }

    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--description") == 0 && (i + 1) < argc) {
            description = argv[++i];
        } else if (strcmp(argv[i], "--no-review") == 0) {
            requires_review = 0;
        } else {
            fprintf(stderr, "Unknown task option: %s\n", argv[i]);
            return 1;
        }
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchTask *task =
        dispatch_board_add_task(board, group, title, description);
    if (!task) {
        locked_board_close(&locked);
        fprintf(stderr, "Could not add task '%s' to group '%s'\n", title,
                group);
        return 1;
    }
    task->requires_review = requires_review;

    char *task_id = strdup(task->id);
    if (!locked_board_save_or_error(&locked)) {
        free(task_id);
        locked_board_close(&locked);
        return 1;
    }

    printf("Added task %s\n", task_id);
    free(task_id);
    locked_board_close(&locked);
    return 0;
}

static int cmd_dep(int argc, char **argv) {
    if (argc != 5 ||
        (strcmp(argv[2], "add") != 0 && strcmp(argv[2], "remove") != 0)) {
        fprintf(stderr,
                "Usage: dispatch dep add <dependency-id> <dependent-id>\n");
        fprintf(stderr,
                "       dispatch dep remove <dependency-id> <dependent-id>\n");
        fprintf(stderr,
                "Example: dispatch dep add DE-01 DE-02 means DE-02 depends "
                "on DE-01\n");
        return 1;
    }

    const char *from_id = argv[3];
    const char *to_id = argv[4];

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    int ok;
    if (strcmp(argv[2], "add") == 0) {
        ok = dispatch_task_add_dependency(board, from_id, to_id);
    } else {
        ok = dispatch_task_remove_dependency(board, from_id, to_id);
    }

    if (!ok) {
        locked_board_close(&locked);
        fprintf(stderr, "Could not %s dependency %s -> %s (%s depends on %s)\n",
                argv[2], from_id, to_id, to_id, from_id);
        return 1;
    }

    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    printf("%s dependency %s -> %s (%s depends on %s)\n",
           strcmp(argv[2], "add") == 0 ? "Added" : "Removed", from_id,
           to_id, to_id, from_id);
    locked_board_close(&locked);
    return 0;
}

static int cmd_normalize(void) {
    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    dispatch_board_normalize_states(&locked.board);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    printf("Normalized %s\n", DISPATCH_STORE_FILE);
    locked_board_close(&locked);
    return 0;
}

static int save_task_transition(LockedBoard *locked, const char *verb,
                                const char *task_id) {
    dispatch_board_normalize_states(&locked->board);
    if (!locked_board_save_or_error(locked))
        return 1;
    printf("%s %s\n", verb, task_id);
    return 0;
}

static int env_truthy(const char *name) {
    const char *value = getenv(name);
    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int cli_color_enabled(void) {
    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0] != '\0')
        return 0;
    if (env_truthy("FORCE_COLOR") || env_truthy("CLICOLOR_FORCE"))
        return 1;
    const char *clicolor = getenv("CLICOLOR");
    if (clicolor && strcmp(clicolor, "0") == 0)
        return 0;
    return isatty(fileno(stdout));
}

static const char *color_for_state(DispatchState state) {
    switch (state) {
    case DISPATCH_STATE_PROPOSED:
        return "\033[2;37m";
    case DISPATCH_STATE_READY:
        return "\033[1;32m";
    case DISPATCH_STATE_BLOCKED:
        return "\033[1;33m";
    case DISPATCH_STATE_DOING:
        return "\033[1;34m";
    case DISPATCH_STATE_REVIEW:
        return "\033[1;35m";
    case DISPATCH_STATE_DONE:
        return "\033[1;30m";
    case DISPATCH_STATE_PAUSED:
        return "\033[1;36m";
    }
    return "";
}

static DispatchWorkspace *task_workspace(const DispatchBoard *board,
                                         const char *task_id,
                                         int active_only) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (!workspace_covers_task(workspace, task_id))
            continue;
        if (active_only && workspace->state != DISPATCH_WORKSPACE_ACTIVE)
            continue;
        if (!active_only && workspace->state == DISPATCH_WORKSPACE_REMOVED)
            continue;
        return workspace;
    }
    return NULL;
}

static void print_task_line(const DispatchBoard *board, const DispatchTask *task,
                            const char *indent) {
    DispatchState state = dispatch_task_effective_state(board, task);
    int color = cli_color_enabled();
    const char *reset = color ? "\033[0m" : "";
    const char *id_color = color ? "\033[1;37m" : "";
    const char *state_color = color ? color_for_state(state) : "";
    const char *meta_color = color ? "\033[2;37m" : "";

    printf("%s%s%-8s%s %s%-10s%s %s", indent, id_color, task->id, reset,
           state_color, dispatch_state_name(state), reset,
           task_display_title(task));
    if (task->assigned_to)
        printf("  %sassigned:%s%s", meta_color, task->assigned_to, reset);
    if (task->depends_on.count > 0) {
        printf("  %sdepends_on:", meta_color);
        for (size_t i = 0; i < task->depends_on.count; i++)
            printf("%s%s", i == 0 ? "" : ",", task->depends_on.items[i]);
        printf("%s", reset);
    }
    printf("\n");

    DispatchWorkspace *workspace = task_workspace(board, task->id, 1);
    if (workspace) {
        printf("%s  workspace: %s\n", indent, workspace->path);
        printf("%s  branch: %s\n", indent, workspace->branch);
    }
}

static int print_ready_tasks_from_board(const DispatchBoard *board,
                                        const char *indent) {
    int count = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (dispatch_task_effective_state(board, task) != DISPATCH_STATE_READY)
            continue;
        print_task_line(board, task, indent);
        count++;
    }
    return count;
}

static int ready_task_count(const DispatchBoard *board) {
    int count = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (dispatch_task_effective_state(board, task) == DISPATCH_STATE_READY)
            count++;
    }
    return count;
}

static void print_list_for_group(const DispatchBoard *board,
                                 const DispatchGroup *group) {
    int color = cli_color_enabled();
    const char *group_color = color ? "\033[1;36m" : "";
    const char *reset = color ? "\033[0m" : "";

    printf("%s[%s] %s%s\n", group_color, group->prefix, group->name, reset);

    int any = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (strcmp(task->group, group->id) != 0)
            continue;
        print_task_line(board, task, "  ");
        any = 1;
    }

    if (!any)
        printf("  (no tasks)\n");
}

static int cmd_list(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: dispatch list [group]\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (argc == 3) {
        DispatchGroup *group = dispatch_board_find_group(&board, argv[2]);
        if (!group) {
            dispatch_board_free(&board);
            fprintf(stderr, "No group with id, prefix, or name %s\n", argv[2]);
            return 1;
        }
        print_list_for_group(&board, group);
        dispatch_board_free(&board);
        return 0;
    }

    if (board.groups.count == 0) {
        printf("(no groups)\n");
    } else {
        for (size_t g = 0; g < board.groups.count; g++)
            print_list_for_group(&board, &board.groups.items[g]);
    }

    dispatch_board_free(&board);
    return 0;
}

static int cmd_show(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: dispatch show <id>\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchTask *task = dispatch_board_find_task(&board, argv[2]);
    if (!task) {
        dispatch_board_free(&board);
        fprintf(stderr, "No task with id %s\n", argv[2]);
        return 1;
    }

    printf("ID: %s\n", task->id);
    printf("Title: %s\n", task_display_title(task));
    printf("Description: %s\n", task->description);
    printf("Group: %s\n", task->group);
    printf("State: %s\n",
           dispatch_state_name(dispatch_task_effective_state(&board, task)));
    printf("Requires review: %s\n", task->requires_review ? "yes" : "no");
    printf("Assigned to: %s\n", task->assigned_to ? task->assigned_to : "-");
    printf("Started by: %s\n", task->started_by ? task->started_by : "-");
    printf("Completed by: %s\n",
           task->completed_by ? task->completed_by : "-");
    DispatchWorkspace *workspace = task_workspace(&board, task->id, 1);
    if (workspace) {
        printf("Workspace actor: %s\n", workspace->actor);
        printf("Workspace path: %s\n", workspace->path);
        printf("Workspace branch: %s\n", workspace->branch);
    }

    printf("Depends on:");
    if (task->depends_on.count == 0) {
        printf(" -");
    } else {
        for (size_t i = 0; i < task->depends_on.count; i++)
            printf(" %s", task->depends_on.items[i]);
    }
    printf("\n");

    printf("Blocks:");
    int blocks = 0;
    for (size_t i = 0; i < board.tasks.count; i++) {
        DispatchTask *candidate = &board.tasks.items[i];
        for (size_t dep = 0; dep < candidate->depends_on.count; dep++) {
            if (strcmp(candidate->depends_on.items[dep], task->id) == 0) {
                printf(" %s", candidate->id);
                blocks = 1;
                break;
            }
        }
    }
    if (!blocks)
        printf(" -");
    printf("\n");

    printf("History:\n");
    if (task->history.count == 0) {
        printf("  -\n");
    } else {
        for (size_t i = 0; i < task->history.count; i++) {
            DispatchHistoryEntry *entry = &task->history.items[i];
            printf("  %s by %s", entry->action, entry->actor);
            if (entry->note && entry->note[0] != '\0')
                printf(": %s", entry->note);
            printf("\n");
        }
    }

    dispatch_board_free(&board);
    return 0;
}

static int cmd_ready_list(void) {
    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    print_ready_tasks_from_board(&board, "");

    dispatch_board_free(&board);
    return 0;
}

static int cmd_blocked(int argc, char **argv) {
    (void)argv;
    if (argc != 2) {
        fprintf(stderr, "Usage: dispatch blocked\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    for (size_t i = 0; i < board.tasks.count; i++) {
        DispatchTask *task = &board.tasks.items[i];
        if (dispatch_task_effective_state(&board, task) !=
            DISPATCH_STATE_BLOCKED) {
            continue;
        }
        printf("%-8s %s  blocked_by:", task->id, task_display_title(task));
        for (size_t dep = 0; dep < task->depends_on.count; dep++) {
            DispatchTask *blocker =
                dispatch_board_find_task(&board, task->depends_on.items[dep]);
            if (!blocker ||
                dispatch_task_effective_state(&board, blocker) !=
                    DISPATCH_STATE_DONE) {
                printf(" %s", task->depends_on.items[dep]);
            }
        }
        printf("\n");
    }

    dispatch_board_free(&board);
    return 0;
}

static int cmd_ready(int argc, char **argv) {
    if (argc == 2)
        return cmd_ready_list();
    if (argc < 5) {
        fprintf(stderr,
                "Usage: dispatch ready <id> --actor <name> [--no-review]\n");
        return 1;
    }

    const char *task_id = argv[2];
    const char *actor = NULL;
    int no_review = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
            actor = argv[++i];
        } else if (strcmp(argv[i], "--no-review") == 0) {
            no_review = 1;
        } else {
            fprintf(stderr, "Unknown ready option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!actor || actor[0] == '\0') {
        fprintf(stderr,
                "Usage: dispatch ready <id> --actor <name> [--no-review]\n");
        return 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchTask *task = dispatch_board_find_task(board, task_id);
    if (!task || !dispatch_task_mark_ready(board, task, actor)) {
        locked_board_close(&locked);
        fprintf(stderr, "Could not mark %s ready\n", task_id);
        return 1;
    }
    if (no_review)
        task->requires_review = 0;

    int result = save_task_transition(&locked, "Readied", task_id);
    locked_board_close(&locked);
    return result;
}

static int cmd_start(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[3], "--actor") != 0) {
        fprintf(stderr, "Usage: dispatch start <id> --actor <name>\n");
        return 1;
    }

    const char *task_id = argv[2];
    const char *actor = argv[4];

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchTask *task = dispatch_board_find_task(board, task_id);
    if (!task) {
        locked_board_close(&locked);
        fprintf(stderr, "Could not start %s\n", task_id);
        return 1;
    }
    DispatchWorkspace *workspace = task_workspace(board, task_id, 0);
    if (workspace && strcmp(workspace->actor, actor) != 0) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace for %s belongs to %s\n", task_id,
                workspace->actor);
        return 1;
    }
    if (!dispatch_task_start(board, task, actor)) {
        locked_board_close(&locked);
        fprintf(stderr, "Could not start %s\n", task_id);
        return 1;
    }

    int result = save_task_transition(&locked, "Started", task_id);
    locked_board_close(&locked);
    return result;
}

static int cmd_finish(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[3], "--actor") != 0) {
        fprintf(stderr, "Usage: dispatch finish <id> --actor <name>\n");
        return 1;
    }

    const char *task_id = argv[2];
    const char *actor = argv[4];

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchTask *task = dispatch_board_find_task(board, task_id);
    if (!task || !dispatch_task_finish(task, actor)) {
        locked_board_close(&locked);
        fprintf(stderr, "Could not finish %s\n", task_id);
        return 1;
    }

    DispatchState finished_state = task->state;
    dispatch_board_normalize_states(board);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    printf("Finished %s (%s)\n", task_id, dispatch_state_name(finished_state));
    if (finished_state == DISPATCH_STATE_REVIEW) {
        puts("Review required before continuing this sequence.");
    } else if (finished_state == DISPATCH_STATE_DONE &&
               ready_task_count(board) > 0) {
        puts("Next ready tasks:");
        print_ready_tasks_from_board(board, "  ");
    }

    locked_board_close(&locked);
    return 0;
}

static int cmd_review(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[3], "--actor") != 0) {
        fprintf(stderr, "Usage: dispatch review <id> --actor <name>\n");
        return 1;
    }

    const char *task_id = argv[2];
    const char *actor = argv[4];

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchTask *task = dispatch_board_find_task(board, task_id);
    if (!task || !dispatch_task_review(task, actor)) {
        locked_board_close(&locked);
        fprintf(stderr, "Could not review %s\n", task_id);
        return 1;
    }

    int result = save_task_transition(&locked, "Reviewed", task_id);
    locked_board_close(&locked);
    return result;
}

int dispatch_cli_dispatch(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
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
    if (strcmp(command->name, "normalize") == 0)
        return cmd_normalize();
    if (strcmp(command->name, "list") == 0)
        return cmd_list(argc, argv);
    if (strcmp(command->name, "show") == 0)
        return cmd_show(argc, argv);
    if (strcmp(command->name, "blocked") == 0)
        return cmd_blocked(argc, argv);
    if (strcmp(command->name, "ready") == 0)
        return cmd_ready(argc, argv);
    if (strcmp(command->name, "start") == 0)
        return cmd_start(argc, argv);
    if (strcmp(command->name, "finish") == 0)
        return cmd_finish(argc, argv);
    if (strcmp(command->name, "review") == 0)
        return cmd_review(argc, argv);

    fprintf(stderr, "Command '%s' is not implemented.\n", command->name);
    return 2;
}
