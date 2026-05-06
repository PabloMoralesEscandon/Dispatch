#include "dispatch_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "dispatch_store.h"

typedef struct {
    const char *name;
    const char *summary;
    int implemented;
} DispatchCliCommand;

static const DispatchCliCommand commands[] = {
    {"init", "Create dispatch.json if it does not exist", 1},
    {"group", "Manage groups", 0},
    {"task", "Manage tasks", 0},
    {"dep", "Manage dependencies", 0},
    {"ready", "List ready work or mark a task ready", 0},
    {"blocked", "List blocked work and blockers", 0},
    {"show", "Show one task", 0},
    {"list", "List tasks by group and workflow order", 0},
    {"start", "Start and assign a ready task", 0},
    {"pause", "Pause an in-progress task", 0},
    {"finish", "Finish a task", 0},
    {"review", "Accept a task in review", 0},
    {"normalize", "Repair IDs and derived state", 0},
    {NULL, NULL, 0},
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
    puts("  init");
    puts("");
    puts("The remaining workflow commands are reserved and will fail clearly until");
    puts("their persistence and lifecycle implementations are complete.");
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
    if (argc < 5 || strcmp(argv[2], "add") != 0) {
        fprintf(stderr,
                "Usage: dispatch task add <group> <title> [--description "
                "text] [--no-review]\n");
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

    fprintf(stderr,
            "Command '%s' is reserved for the Dispatch workflow but is not "
            "implemented yet.\n",
            command->name);
    return command->implemented ? 0 : 2;
}
