#include "dispatch_cli.h"

#include <stdio.h>
#include <string.h>

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

    fprintf(stderr,
            "Command '%s' is reserved for the Dispatch workflow but is not "
            "implemented yet.\n",
            command->name);
    return command->implemented ? 0 : 2;
}
