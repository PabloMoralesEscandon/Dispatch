#include "dispatch_cli.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dispatch.h"
#include "dispatch_store.h"

typedef struct {
    const char *name;
    const char *summary;
} DispatchCliCommand;

static const DispatchCliCommand commands[] = {
    {"init", "Create dispatch.json for a target repository"},
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
    puts("  init, group add/ready, task add, dep add/remove, ready, start,");
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
        return 1;
    }

    if (existed) {
        printf("%s already exists\n", DISPATCH_STORE_FILE);
        return 0;
    }

    printf("Created %s for repo %s\n", DISPATCH_STORE_FILE, repo_path);
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

static int save_board_or_error(DispatchBoard *board) {
    char error[256] = {0};
    if (!dispatch_store_save(board, DISPATCH_STORE_FILE, error, sizeof(error))) {
        fprintf(stderr, "Could not save %s: %s\n", DISPATCH_STORE_FILE, error);
        return 0;
    }
    return 1;
}

static const char *parse_actor(int argc, char **argv, int start_index) {
    for (int i = start_index; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc)
            return argv[i + 1];
    }
    return NULL;
}

static int reject_unknown_actor_options(int argc, char **argv, int start_index) {
    for (int i = start_index; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
            i++;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return 1;
    }
    return 0;
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

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (!dispatch_board_add_group(&board, name, prefix)) {
        dispatch_board_free(&board);
        fprintf(stderr, "Could not add group '%s'\n", name);
        return 1;
    }

    DispatchGroup *group = dispatch_board_find_group(&board, name);
    if (!save_board_or_error(&board)) {
        dispatch_board_free(&board);
        return 1;
    }

    printf("Added group %s (%s)\n", group->name, group->prefix);
    dispatch_board_free(&board);
    return 0;
}

static int cmd_group_ready(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "Usage: dispatch group ready <group> --actor <name>\n");
        return 1;
    }
    if (reject_unknown_actor_options(argc, argv, 4))
        return 1;

    const char *group_id = argv[3];
    const char *actor = parse_actor(argc, argv, 4);

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchGroup *group = dispatch_board_find_group(&board, group_id);
    if (!group) {
        dispatch_board_free(&board);
        fprintf(stderr, "No group with id, prefix, or name %s\n", group_id);
        return 1;
    }

    int readied = 0;
    for (size_t i = 0; i < board.tasks.count; i++) {
        DispatchTask *task = &board.tasks.items[i];
        if (strcmp(task->group, group->id) != 0)
            continue;
        if (task->state != DISPATCH_STATE_PROPOSED)
            continue;
        dispatch_task_mark_ready(&board, task, actor);
        readied++;
    }

    dispatch_board_normalize_states(&board);
    if (!save_board_or_error(&board)) {
        dispatch_board_free(&board);
        return 1;
    }

    printf("Readied %d task%s in group %s\n", readied,
           readied == 1 ? "" : "s", group->prefix);
    dispatch_board_free(&board);
    return 0;
}

static int cmd_group(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "add") == 0)
        return cmd_group_add(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "ready") == 0)
        return cmd_group_ready(argc, argv);

    fprintf(stderr, "Usage: dispatch group add <name> [--prefix XX]\n");
    fprintf(stderr, "       dispatch group ready <group> --actor <name>\n");
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

        DispatchBoard board;
        if (!load_board_or_error(&board))
            return 1;
        if (!dispatch_board_delete_task(&board, task_id, force)) {
            dispatch_board_free(&board);
            fprintf(stderr,
                    "Could not delete %s%s\n",
                    task_id,
                    force ? "" : " (task may have dependents; use --force)");
            return 1;
        }
        if (!save_board_or_error(&board)) {
            dispatch_board_free(&board);
            return 1;
        }
        printf("Deleted task %s\n", task_id);
        dispatch_board_free(&board);
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

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchTask *task =
        dispatch_board_add_task(&board, group, title, description);
    if (!task) {
        dispatch_board_free(&board);
        fprintf(stderr, "Could not add task '%s' to group '%s'\n", title,
                group);
        return 1;
    }
    task->requires_review = requires_review;

    char *task_id = strdup(task->id);
    if (!save_board_or_error(&board)) {
        free(task_id);
        dispatch_board_free(&board);
        return 1;
    }

    printf("Added task %s\n", task_id);
    free(task_id);
    dispatch_board_free(&board);
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

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    int ok;
    if (strcmp(argv[2], "add") == 0) {
        ok = dispatch_task_add_dependency(&board, from_id, to_id);
    } else {
        ok = dispatch_task_remove_dependency(&board, from_id, to_id);
    }

    if (!ok) {
        dispatch_board_free(&board);
        fprintf(stderr, "Could not %s dependency %s -> %s (%s depends on %s)\n",
                argv[2], from_id, to_id, to_id, from_id);
        return 1;
    }

    if (!save_board_or_error(&board)) {
        dispatch_board_free(&board);
        return 1;
    }

    printf("%s dependency %s -> %s (%s depends on %s)\n",
           strcmp(argv[2], "add") == 0 ? "Added" : "Removed", from_id,
           to_id, to_id, from_id);
    dispatch_board_free(&board);
    return 0;
}

static int cmd_normalize(void) {
    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    dispatch_board_normalize_states(&board);
    if (!save_board_or_error(&board)) {
        dispatch_board_free(&board);
        return 1;
    }

    printf("Normalized %s\n", DISPATCH_STORE_FILE);
    dispatch_board_free(&board);
    return 0;
}

static int save_task_transition(DispatchBoard *board, const char *verb,
                                const char *task_id) {
    dispatch_board_normalize_states(board);
    if (!save_board_or_error(board))
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

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task || !dispatch_task_mark_ready(&board, task, actor)) {
        dispatch_board_free(&board);
        fprintf(stderr, "Could not mark %s ready\n", task_id);
        return 1;
    }
    if (no_review)
        task->requires_review = 0;

    int result = save_task_transition(&board, "Readied", task_id);
    dispatch_board_free(&board);
    return result;
}

static int cmd_start(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[3], "--actor") != 0) {
        fprintf(stderr, "Usage: dispatch start <id> --actor <name>\n");
        return 1;
    }

    const char *task_id = argv[2];
    const char *actor = argv[4];

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task || !dispatch_task_start(&board, task, actor)) {
        dispatch_board_free(&board);
        fprintf(stderr, "Could not start %s\n", task_id);
        return 1;
    }

    int result = save_task_transition(&board, "Started", task_id);
    dispatch_board_free(&board);
    return result;
}

static int cmd_finish(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[3], "--actor") != 0) {
        fprintf(stderr, "Usage: dispatch finish <id> --actor <name>\n");
        return 1;
    }

    const char *task_id = argv[2];
    const char *actor = argv[4];

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task || !dispatch_task_finish(task, actor)) {
        dispatch_board_free(&board);
        fprintf(stderr, "Could not finish %s\n", task_id);
        return 1;
    }

    DispatchState finished_state = task->state;
    dispatch_board_normalize_states(&board);
    if (!save_board_or_error(&board)) {
        dispatch_board_free(&board);
        return 1;
    }

    printf("Finished %s (%s)\n", task_id, dispatch_state_name(finished_state));
    if (finished_state == DISPATCH_STATE_REVIEW) {
        puts("Review required before continuing this sequence.");
    } else if (finished_state == DISPATCH_STATE_DONE &&
               ready_task_count(&board) > 0) {
        puts("Next ready tasks:");
        print_ready_tasks_from_board(&board, "  ");
    }

    dispatch_board_free(&board);
    return 0;
}

static int cmd_review(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[3], "--actor") != 0) {
        fprintf(stderr, "Usage: dispatch review <id> --actor <name>\n");
        return 1;
    }

    const char *task_id = argv[2];
    const char *actor = argv[4];

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task || !dispatch_task_review(task, actor)) {
        dispatch_board_free(&board);
        fprintf(stderr, "Could not review %s\n", task_id);
        return 1;
    }

    int result = save_task_transition(&board, "Reviewed", task_id);
    dispatch_board_free(&board);
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
