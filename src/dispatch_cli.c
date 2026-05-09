#include "dispatch_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "dispatch_store.h"

typedef struct {
    const char *name;
    const char *summary;
} DispatchCliCommand;

static const DispatchCliCommand commands[] = {
    {"init", "Create dispatch.json if it does not exist"},
    {"group", "Manage groups"},
    {"task", "Manage tasks"},
    {"dep", "Manage dependencies"},
    {"ready", "List ready work or mark a task ready"},
    {"blocked", "List blocked work and blockers"},
    {"show", "Show one task"},
    {"list", "List tasks by group and workflow order"},
    {"start", "Start and assign a ready task"},
    {"pause", "Pause an in-progress task"},
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
    puts("Usage: dispatch <command> [args] [--json]");
    puts("");
    puts("Workflow commands:");
    for (int i = 0; commands[i].name != NULL; i++)
        printf("  %-10s %s\n", commands[i].name, commands[i].summary);
    puts("");
    puts("Implemented now:");
    puts("  init, group add, task add, dep add/remove, ready, start, pause,");
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

static int cmd_init(void) {
    char error[256] = {0};
    int existed = 0;
    FILE *file = fopen(DISPATCH_STORE_FILE, "r");
    if (file) {
        existed = 1;
        fclose(file);
    }

    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, error, sizeof(error))) {
        fprintf(stderr, "Could not initialize %s: %s\n", DISPATCH_STORE_FILE,
                error);
        return 1;
    }

    if (existed) {
        printf("%s already exists\n", DISPATCH_STORE_FILE);
        return 0;
    }

    printf("Created %s\n", DISPATCH_STORE_FILE);
    return 0;
}

static int load_board_or_error(DispatchBoard *board) {
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, error, sizeof(error))) {
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

static int cmd_group(int argc, char **argv) {
    if (argc < 4 || strcmp(argv[2], "add") != 0) {
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
        fprintf(stderr, "Usage: dispatch dep add <from-id> <to-id>\n");
        fprintf(stderr, "       dispatch dep remove <from-id> <to-id>\n");
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
        fprintf(stderr, "Could not %s dependency %s -> %s\n", argv[2],
                from_id, to_id);
        return 1;
    }

    if (!save_board_or_error(&board)) {
        dispatch_board_free(&board);
        return 1;
    }

    printf("%s dependency %s -> %s\n",
           strcmp(argv[2], "add") == 0 ? "Added" : "Removed", from_id,
           to_id);
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

static void print_task_line(const DispatchBoard *board, const DispatchTask *task,
                            const char *indent) {
    DispatchState state = dispatch_task_effective_state(board, task);
    printf("%s%-8s %-10s %s", indent, task->id, dispatch_state_name(state),
           task->title);
    if (task->assigned_to)
        printf("  assigned:%s", task->assigned_to);
    if (task->depends_on.count > 0) {
        printf("  depends_on:");
        for (size_t i = 0; i < task->depends_on.count; i++)
            printf("%s%s", i == 0 ? "" : ",", task->depends_on.items[i]);
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

static int cmd_list(int argc, char **argv) {
    (void)argv;
    if (argc != 2) {
        fprintf(stderr, "Usage: dispatch list\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    for (size_t g = 0; g < board.groups.count; g++) {
        DispatchGroup *group = &board.groups.items[g];
        printf("[%s] %s\n", group->prefix, group->name);
        int any = 0;
        for (size_t t = 0; t < board.tasks.count; t++) {
            DispatchTask *task = &board.tasks.items[t];
            if (strcmp(task->group, group->id) != 0)
                continue;
            print_task_line(&board, task, "  ");
            any = 1;
        }
        if (!any)
            printf("  (no tasks)\n");
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
    printf("Title: %s\n", task->title);
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
        printf("%-8s %s  blocked_by:", task->id, task->title);
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
    if (argc != 3 && argc != 5) {
        fprintf(stderr, "Usage: dispatch ready <id> [--actor name]\n");
        return 1;
    }
    if (reject_unknown_actor_options(argc, argv, 3))
        return 1;

    const char *task_id = argv[2];
    const char *actor = parse_actor(argc, argv, 3);

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task || !dispatch_task_mark_ready(&board, task, actor)) {
        dispatch_board_free(&board);
        fprintf(stderr, "Could not mark %s ready\n", task_id);
        return 1;
    }

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

static int cmd_pause(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[3], "--actor") != 0) {
        fprintf(stderr, "Usage: dispatch pause <id> --actor <name>\n");
        return 1;
    }

    const char *task_id = argv[2];
    const char *actor = argv[4];

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task || !dispatch_task_pause(task, actor)) {
        dispatch_board_free(&board);
        fprintf(stderr, "Could not pause %s\n", task_id);
        return 1;
    }

    int result = save_task_transition(&board, "Paused", task_id);
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
        return cmd_init();
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
    if (strcmp(command->name, "pause") == 0)
        return cmd_pause(argc, argv);
    if (strcmp(command->name, "finish") == 0)
        return cmd_finish(argc, argv);
    if (strcmp(command->name, "review") == 0)
        return cmd_review(argc, argv);

    fprintf(stderr, "Command '%s' is not implemented.\n", command->name);
    return 2;
}
