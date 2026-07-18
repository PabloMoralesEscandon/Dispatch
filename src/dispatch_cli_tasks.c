#include "dispatch_cli_internal.h"

int cmd_repo(int argc, char **argv) {
    if (argc == 2) {
        DispatchBoard board;
        if (!load_board_or_error(&board))
            return 1;
        printf("Repo: %s\n", board.repo_path ? board.repo_path : ".");
        if (!board.repo_path ||
            !dispatch_path_is_git_repository(board.repo_path)) {
            printf("Warning: repo path is not a git repository\n");
            printf("Repair it with: dispatch repo set <path>\n");
        }
        dispatch_board_free(&board);
        return 0;
    }

    if (argc != 4 || strcmp(argv[2], "set") != 0) {
        fprintf(stderr, "Usage: dispatch repo [set <path>]\n");
        return 1;
    }

    const char *repo_arg = argv[3];
    char *repo_path = dispatch_resolve_path(".", repo_arg);
    if (!repo_path) {
        fprintf(stderr, "Repo path %s does not exist\n", repo_arg);
        return 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked)) {
        free(repo_path);
        return 1;
    }

    dispatch_board_set_repo_path(&locked.board, repo_path);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        free(repo_path);
        return 1;
    }

    printf("Set repo path to %s\n", repo_path);
    if (!dispatch_path_is_git_repository(repo_path))
        printf("Warning: %s is not a git repository\n", repo_path);
    DispatchLogField targets[] = {
        {"repo_path", repo_path},
    };
    append_dispatch_log("user", "repo", "set", targets, 1, NULL, 0,
                        "Set repo path");
    locked_board_close(&locked);
    free(repo_path);
    return 0;
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
    DispatchLogField targets[] = {
        {"group", group->id},
    };
    DispatchLogField context[] = {
        {"name", group->name},
        {"prefix", group->prefix},
    };
    char message[256];
    snprintf(message, sizeof(message), "Added group %s", group->name);
    append_dispatch_log("user", "group", "add", targets, 1, context, 2,
                        message);
    locked_board_close(&locked);
    return 0;
}

static int cmd_group_ready(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
                "Usage: dispatch group ready <group> [--actor <name>] [--no-review]\n");
        return 1;
    }

    const char *group_id = argv[3];
    const char *actor = "user";
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

        if (task->state == DISPATCH_STATE_PROPOSED) {
            dispatch_task_mark_ready(board, task, actor);
            if (no_review)
                task->requires_review = 0;
            readied++;
            continue;
        }

        if (no_review &&
            (task->state == DISPATCH_STATE_READY ||
             task->state == DISPATCH_STATE_BLOCKED) &&
            !task->assigned_to && !task->started_by) {
            task->requires_review = 0;
        }
    }

    dispatch_board_normalize_states(board);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    printf("Readied %d task%s in group %s\n", readied,
           readied == 1 ? "" : "s", group->prefix);
    DispatchLogField targets[] = {
        {"group", group->id},
    };
    char readied_value[32];
    snprintf(readied_value, sizeof(readied_value), "%d", readied);
    DispatchLogField context[] = {
        {"readied", readied_value},
        {"no_review", bool_string(no_review)},
    };
    char message[256];
    snprintf(message, sizeof(message), "Readied %d task%s in group %s",
             readied, readied == 1 ? "" : "s", group->prefix);
    append_dispatch_log(actor, "group", "ready", targets, 1, context, 2,
                        message);
    locked_board_close(&locked);
    return 0;
}

int cmd_group(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "add") == 0)
        return cmd_group_add(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "ready") == 0)
        return cmd_group_ready(argc, argv);

    fprintf(stderr, "Usage: dispatch group add <name> [--prefix XX]\n");
    fprintf(stderr,
            "       dispatch group ready <group> [--actor <name>] [--no-review]\n");
    return 1;
}

int cmd_task(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[2], "edit") == 0) {
        const char *task_id = argv[3];
        const char *title = NULL;
        const char *description = NULL;
        const char *actor = "user";

        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--title") == 0 && (i + 1) < argc) {
                title = argv[++i];
            } else if (strcmp(argv[i], "--description") == 0 &&
                       (i + 1) < argc) {
                description = argv[++i];
            } else if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
                actor = argv[++i];
            } else {
                fprintf(stderr, "Unknown task edit option: %s\n", argv[i]);
                return 1;
            }
        }

        if (!title && !description) {
            fprintf(stderr,
                    "Task edit requires --title, --description, or both\n");
            return 1;
        }
        if (title && title[0] == '\0') {
            fprintf(stderr, "Task title must not be empty\n");
            return 1;
        }
        if (title && title_starts_with_dispatch_id(title)) {
            fprintf(stderr,
                    "Task titles should not include Dispatch IDs; use a human-readable title without a prefix like DE-01\n");
            return 1;
        }
        if (!dispatch_actor_label_is_valid(actor)) {
            fprintf(stderr,
                    "Actor must start with an ASCII letter or digit and contain only letters, digits, '.', '_' or '-'\n");
            return 1;
        }

        LockedBoard locked;
        if (!locked_board_load_or_error(&locked))
            return 1;
        DispatchTask *task =
            dispatch_board_find_task(&locked.board, task_id);
        if (!task) {
            locked_board_close(&locked);
            fprintf(stderr, "No task with id %s\n", task_id);
            return 1;
        }

        if ((title && !dispatch_task_set_title(task, title)) ||
            (description &&
             !dispatch_task_set_description(task, description))) {
            locked_board_close(&locked);
            fprintf(stderr, "Could not edit task %s\n", task_id);
            return 1;
        }

        const char *note = title && description
                               ? "title and description"
                               : title ? "title" : "description";
        if (!dispatch_task_append_history(task, actor, "edited", note) ||
            !locked_board_save_or_error(&locked)) {
            locked_board_close(&locked);
            return 1;
        }

        printf("Edited task %s\n", task_id);
        DispatchLogField targets[] = {
            {"task", task_id},
        };
        DispatchLogField context[] = {
            {"title_changed", bool_string(title != NULL)},
            {"description_changed", bool_string(description != NULL)},
            {"title", title ? title : ""},
            {"description", description ? description : ""},
        };
        char message[256];
        snprintf(message, sizeof(message), "Edited task %s", task_id);
        append_dispatch_log(actor, "task", "edit", targets, 1, context, 4,
                            message);
        locked_board_close(&locked);
        return 0;
    }

    if (argc >= 5 && strcmp(argv[2], "move") == 0) {
        const char *task_id = argv[3];
        const char *group_id = argv[4];
        const char *actor = "user";

        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
                actor = argv[++i];
            } else {
                fprintf(stderr, "Unknown task move option: %s\n", argv[i]);
                return 1;
            }
        }
        if (!dispatch_actor_label_is_valid(actor)) {
            fprintf(stderr,
                    "Actor must start with an ASCII letter or digit and contain only letters, digits, '.', '_' or '-'\n");
            return 1;
        }

        LockedBoard locked;
        if (!locked_board_load_or_error(&locked))
            return 1;
        DispatchTask *task =
            dispatch_board_find_task(&locked.board, task_id);
        if (!task) {
            locked_board_close(&locked);
            fprintf(stderr, "No task with id %s\n", task_id);
            return 1;
        }
        DispatchGroup *group =
            dispatch_board_find_group(&locked.board, group_id);
        if (!group) {
            locked_board_close(&locked);
            fprintf(stderr, "No group with id, prefix, or name %s\n", group_id);
            return 1;
        }
        if (strcmp(task->group, group->id) == 0) {
            fprintf(stderr, "Task %s already belongs to group %s\n", task_id,
                    group->id);
            locked_board_close(&locked);
            return 1;
        }

        char *from_group = cli_strdup(task->group);
        char *to_group = cli_strdup(group->id);
        if (!dispatch_task_move_to_group(&locked.board, task, to_group, actor) ||
            !locked_board_save_or_error(&locked)) {
            free(from_group);
            free(to_group);
            locked_board_close(&locked);
            return 1;
        }

        printf("Moved task %s from %s to %s (ID unchanged)\n", task_id,
               from_group, to_group);
        DispatchLogField targets[] = {
            {"task", task_id},
            {"group", to_group},
        };
        DispatchLogField context[] = {
            {"from_group", from_group},
            {"id_changed", "false"},
        };
        char message[256];
        snprintf(message, sizeof(message), "Moved task %s from %s to %s",
                 task_id, from_group, to_group);
        append_dispatch_log(actor, "task", "move", targets, 2, context, 2,
                            message);
        free(from_group);
        free(to_group);
        locked_board_close(&locked);
        return 0;
    }

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
        DispatchLogField targets[] = {
            {"task", task_id},
        };
        DispatchLogField context[] = {
            {"force", bool_string(force)},
        };
        char message[256];
        snprintf(message, sizeof(message), "Deleted task %s", task_id);
        append_dispatch_log("user", "task", "delete", targets, 1, context, 1,
                            message);
        locked_board_close(&locked);
        return 0;
    }

    if (argc >= 5 && strcmp(argv[2], "priority") == 0) {
        const char *task_id = argv[3];
        char *end = NULL;
        long value = strtol(argv[4], &end, 10);
        if (!end || *end != '\0' || value < -99 || value > 99) {
            fprintf(stderr, "Priority must be an integer between -99 and 99\n");
            return 1;
        }
        const char *actor = "user";
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
                actor = argv[++i];
            } else {
                fprintf(stderr, "Unknown task priority option: %s\n", argv[i]);
                return 1;
            }
        }

        LockedBoard locked;
        if (!locked_board_load_or_error(&locked))
            return 1;
        DispatchTask *task = dispatch_board_find_task(&locked.board, task_id);
        if (!task || !dispatch_task_set_priority(task, (int)value, actor)) {
            locked_board_close(&locked);
            fprintf(stderr, "No task with id %s\n", task_id);
            return 1;
        }
        if (!locked_board_save_or_error(&locked)) {
            locked_board_close(&locked);
            return 1;
        }
        printf("Set priority of %s to %d\n", task_id, (int)value);
        DispatchLogField targets[] = {
            {"task", task_id},
        };
        DispatchLogField context[] = {
            {"priority", argv[4]},
        };
        char message[256];
        snprintf(message, sizeof(message), "Set priority of %s", task_id);
        append_dispatch_log(actor, "task", "priority", targets, 1, context, 1,
                            message);
        locked_board_close(&locked);
        return 0;
    }

    if (argc < 5 || strcmp(argv[2], "add") != 0) {
        fprintf(stderr,
                "Usage: dispatch task add <group> <title> [--description "
                "text] [--actor <name>] [--no-review]\n");
        fprintf(stderr,
                "       dispatch task edit <id> [--title <text>] [--description <text>] [--actor <name>]\n");
        fprintf(stderr,
                "       dispatch task move <id> <group> [--actor <name>]\n");
        fprintf(stderr, "       dispatch task delete <id> [--force]\n");
        fprintf(stderr,
                "       dispatch task priority <id> <value> [--actor <name>]\n");
        return 1;
    }

    const char *group = argv[3];
    const char *title = argv[4];
    const char *description = "";
    const char *actor = "user";
    int requires_review = 1;

    if (title_starts_with_dispatch_id(title)) {
        fprintf(stderr,
                "Task titles should not include Dispatch IDs; use a human-readable title without a prefix like DE-01\n");
        return 1;
    }

    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--description") == 0 && (i + 1) < argc) {
            description = argv[++i];
        } else if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
            actor = argv[++i];
        } else if (strcmp(argv[i], "--no-review") == 0) {
            requires_review = 0;
        } else {
            fprintf(stderr, "Unknown task option: %s\n", argv[i]);
            return 1;
        }
    }
    if (!dispatch_actor_label_is_valid(actor)) {
        fprintf(stderr,
                "Actor must start with an ASCII letter or digit and contain only letters, digits, '.', '_' or '-'\n");
        return 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchTask *task =
        dispatch_board_add_task_with_actor(board, group, title, description,
                                           actor);
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
    DispatchLogField targets[] = {
        {"task", task_id},
        {"group", group},
    };
    DispatchLogField context[] = {
        {"title", title},
        {"requires_review", bool_string(requires_review)},
    };
    char message[256];
    snprintf(message, sizeof(message), "Added task %s", task_id);
    append_dispatch_log(actor, "task", "add", targets, 2, context, 2,
                        message);
    free(task_id);
    locked_board_close(&locked);
    return 0;
}

int cmd_dep(int argc, char **argv) {
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
    DispatchLogField targets[] = {
        {"dependency", from_id},
        {"dependent", to_id},
    };
    char message[256];
    snprintf(message, sizeof(message), "%s dependency %s -> %s",
             strcmp(argv[2], "add") == 0 ? "Added" : "Removed", from_id,
             to_id);
    append_dispatch_log("user", "dep",
                        strcmp(argv[2], "add") == 0 ? "dependency_add"
                                                     : "dependency_remove",
                        targets, 2, NULL, 0, message);
    locked_board_close(&locked);
    return 0;
}
