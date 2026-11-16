#include "dispatch_cli_internal.h"

int cmd_ready(int argc, char **argv) {
    /* With task IDs, ready is a mutation and --json selects the result
     * document; without them it is the read-only queue listing. */
    int has_operand = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") != 0)
            has_operand = 1;
    }
    int json_output = 0;
    if (!dispatch_cli_extract_json_flag(&argc, argv, 2, &json_output)) {
        if (has_operand)
            cli_json_begin("ready");
        cli_error("usage", NULL,
                  "Usage: dispatch ready [--json] | dispatch ready <id> "
                  "[<id> ...] [--actor <name>] [--no-review]\n");
        return 1;
    }
    if (argc == 2)
        return cmd_ready_list(json_output);
    if (json_output)
        cli_json_begin("ready");

    /* Leading arguments are task IDs; options follow the last ID. */
    int ids_end = 2;
    while (ids_end < argc && strncmp(argv[ids_end], "--", 2) != 0)
        ids_end++;
    if (ids_end == 2) {
        cli_error("usage", NULL,
                  "Usage: dispatch ready <id> [<id> ...] "
                  "[--actor <name>] [--no-review]\n");
        return 1;
    }

    const char *actor = "user";
    int no_review = 0;
    for (int i = ids_end; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
            actor = argv[++i];
        } else if (strcmp(argv[i], "--no-review") == 0) {
            no_review = 1;
        } else {
            cli_error("usage", NULL, "Unknown ready option: %s\n", argv[i]);
            return 1;
        }
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchAgent *actor_agent = dispatch_board_find_agent(board, actor);
    if (actor_agent && actor_agent->archived) {
        locked_board_close(&locked);
        cli_error("agent_archived", NULL,
                  "Agent %s is archived; restore it first\n", actor);
        return 1;
    }

    /* Apply the whole batch in memory before saving so a bad ID leaves the
     * board untouched. */
    for (int i = 2; i < ids_end; i++) {
        DispatchTask *task = dispatch_board_find_task(board, argv[i]);
        if (!task || !dispatch_task_mark_ready(board, task, actor)) {
            locked_board_close(&locked);
            cli_error("task_not_found", argv[i], "Could not mark %s ready\n",
                      argv[i]);
            return 1;
        }
        if (no_review)
            task->requires_review = 0;
    }

    dispatch_board_normalize_states(board);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    DispatchJsonResult *json_result =
        cli_json() ? dispatch_json_result_new("ready") : NULL;
    for (int i = 2; i < ids_end; i++) {
        DispatchTask *task = dispatch_board_find_task(board, argv[i]);
        if (json_result)
            dispatch_json_result_add_target(json_result, argv[i]);
        else
            printf("Readied %s\n", argv[i]);
        DispatchLogField targets[] = {
            {"task", argv[i]},
        };
        DispatchLogField context[] = {
            {"no_review", bool_string(no_review)},
            {"new_state",
             dispatch_state_name(dispatch_task_effective_state(board, task))},
        };
        char message[256];
        snprintf(message, sizeof(message), "Readied %s", argv[i]);
        append_dispatch_log(actor, "ready", "ready", targets, 1, context, 2,
                            message);
    }
    int status = json_result ? cli_json_emit_tasks(board, NULL, json_result)
                             : 0;
    locked_board_close(&locked);
    return status;
}

int cmd_start(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[3], "--actor") != 0) {
        cli_error("usage", NULL, "Usage: dispatch start <id> --actor <name>\n");
        return 1;
    }

    const char *task_id = argv[2];
    const char *actor = argv[4];

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchAgent *actor_agent = dispatch_board_find_agent(board, actor);
    if (actor_agent && actor_agent->archived) {
        locked_board_close(&locked);
        cli_error("agent_archived", NULL,
                  "Agent %s is archived; restore it first\n", actor);
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(board, task_id);
    if (!task) {
        locked_board_close(&locked);
        cli_error("task_not_found", task_id, "No task with id %s\n", task_id);
        return 1;
    }
    DispatchWorkspace *workspace = task_workspace(board, task_id, 0);
    if (workspace && strcmp(workspace->actor, actor) != 0) {
        cli_error("workspace_owner_mismatch", task_id,
                  "Workspace for %s belongs to %s\n", task_id,
                  workspace->actor);
        locked_board_close(&locked);
        return 1;
    }
    DispatchState effective = dispatch_task_effective_state(board, task);
    if (effective != DISPATCH_STATE_READY) {
        cli_error("invalid_state", task_id,
                  "Cannot start %s: task is %s, not ready\n", task_id,
                  dispatch_state_name(effective));
        locked_board_close(&locked);
        return 1;
    }
    if (task->assigned_to && task->assigned_to[0] != '\0') {
        cli_error("already_assigned", task_id,
                  "Cannot start %s: already assigned to %s (use 'dispatch "
                  "unassign %s --actor <name>' to clear it)\n",
                  task_id, task->assigned_to, task_id);
        locked_board_close(&locked);
        return 1;
    }
    if (!dispatch_task_start(board, task, actor)) {
        locked_board_close(&locked);
        cli_error("invalid_actor", task_id, "Could not start %s\n", task_id);
        return 1;
    }

    int result = save_task_transition(&locked, "Started", task_id);
    if (result == 0) {
        DispatchLogField targets[] = {
            {"task", task_id},
        };
        DispatchLogField context[] = {
            {"new_state", "doing"},
        };
        char message[256];
        snprintf(message, sizeof(message), "Started %s", task_id);
        append_dispatch_log(actor, "start", "start", targets, 1, context, 1,
                            message);
        if (cli_json()) {
            DispatchJsonResult *json_result = dispatch_json_result_new("start");
            dispatch_json_result_add_target(json_result, task_id);
            result = cli_json_emit_tasks(board, NULL, json_result);
        }
    }
    locked_board_close(&locked);
    return result;
}

int cmd_unassign(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[3], "--actor") != 0) {
        cli_error("usage", NULL,
                  "Usage: dispatch unassign <id> --actor <name>\n");
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
        cli_error("task_not_found", task_id, "No task with id %s\n", task_id);
        return 1;
    }
    if (task->state == DISPATCH_STATE_REVIEW ||
        task->state == DISPATCH_STATE_DONE) {
        const char *state_name = dispatch_state_name(task->state);
        locked_board_close(&locked);
        cli_error("invalid_state", task_id,
                  "Cannot unassign %s: task is %s and unassigning would "
                  "discard its completion\n",
                  task_id, state_name);
        return 1;
    }
    if (!task->assigned_to || task->assigned_to[0] == '\0') {
        locked_board_close(&locked);
        cli_error("not_assigned", task_id, "Task %s is not assigned\n",
                  task_id);
        return 1;
    }
    char previous[DISPATCH_AGENT_NAME_MAX + 1];
    snprintf(previous, sizeof(previous), "%s", task->assigned_to);
    if (!dispatch_task_unassign(board, task, actor)) {
        locked_board_close(&locked);
        cli_error("invalid_actor", task_id, "Could not unassign %s\n",
                  task_id);
        return 1;
    }

    DispatchState new_state = task->state;
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    if (!cli_json())
        printf("Unassigned %s from %s (%s)\n", task_id, previous,
               dispatch_state_name(new_state));
    DispatchLogField targets[] = {
        {"task", task_id},
    };
    DispatchLogField context[] = {
        {"previous_assignee", previous},
        {"new_state", dispatch_state_name(new_state)},
    };
    char message[256];
    snprintf(message, sizeof(message), "Unassigned %s", task_id);
    append_dispatch_log(actor, "unassign", "unassign", targets, 1, context, 2,
                        message);
    int status = 0;
    if (cli_json()) {
        DispatchJsonResult *json_result = dispatch_json_result_new("unassign");
        dispatch_json_result_add_target(json_result, task_id);
        dispatch_json_result_set_string(json_result, "previous_assignee",
                                        previous);
        status = cli_json_emit_tasks(board, NULL, json_result);
    }
    locked_board_close(&locked);
    return status;
}

int cmd_finish(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[3], "--actor") != 0) {
        cli_error("usage", NULL, "Usage: dispatch finish <id> --actor <name>\n");
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
        const char *code = !task ? "task_not_found"
                           : task->state != DISPATCH_STATE_DOING
                               ? "invalid_state"
                               : "invalid_actor";
        locked_board_close(&locked);
        cli_error(code, task_id, "Could not finish %s\n", task_id);
        return 1;
    }

    DispatchState finished_state = task->state;
    dispatch_board_normalize_states(board);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    if (!cli_json())
        printf("Finished %s (%s)\n", task_id,
               dispatch_state_name(finished_state));
    DispatchLogField targets[] = {
        {"task", task_id},
    };
    DispatchLogField context[] = {
        {"new_state", dispatch_state_name(finished_state)},
    };
    char message[256];
    snprintf(message, sizeof(message), "Finished %s (%s)", task_id,
             dispatch_state_name(finished_state));
    append_dispatch_log(actor, "finish", "finish", targets, 1, context, 1,
                        message);
    int status = 0;
    if (cli_json()) {
        DispatchJsonResult *json_result = dispatch_json_result_new("finish");
        dispatch_json_result_add_target(json_result, task_id);
        dispatch_json_result_set_bool(json_result, "review_required",
                                      finished_state == DISPATCH_STATE_REVIEW);
        status = cli_json_emit_tasks(board, NULL, json_result);
    } else if (finished_state == DISPATCH_STATE_REVIEW) {
        puts("Review required before continuing this sequence.");
    } else if (finished_state == DISPATCH_STATE_DONE &&
               ready_task_count(board) > 0) {
        puts("Next ready tasks:");
        print_ready_tasks_from_board(board, "  ");
    }

    locked_board_close(&locked);
    return status;
}

int cmd_review(int argc, char **argv) {
    if (argc < 3) {
        cli_error("usage", NULL,
                  "Usage: dispatch review <id> [<id> ...] [--actor <name>]\n");
        return 1;
    }

    /* Leading arguments are task IDs; options follow the last ID. */
    int ids_end = 2;
    while (ids_end < argc && strncmp(argv[ids_end], "--", 2) != 0)
        ids_end++;
    if (ids_end == 2) {
        cli_error("usage", NULL,
                  "Usage: dispatch review <id> [<id> ...] [--actor <name>]\n");
        return 1;
    }

    const char *actor = "user";
    for (int i = ids_end; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc &&
            argv[i + 1][0] != '\0') {
            actor = argv[++i];
        } else {
            cli_error("usage", NULL,
                      "Usage: dispatch review <id> [<id> ...] "
                      "[--actor <name>]\n");
            return 1;
        }
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    /* Apply the whole batch in memory before saving so a bad ID leaves the
     * board untouched. */
    for (int i = 2; i < ids_end; i++) {
        DispatchTask *task = dispatch_board_find_task(board, argv[i]);
        if (!task || !dispatch_task_review(task, actor)) {
            const char *code = !task ? "task_not_found" : "invalid_state";
            locked_board_close(&locked);
            cli_error(code, argv[i], "Could not review %s\n", argv[i]);
            return 1;
        }
    }

    dispatch_board_normalize_states(board);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    DispatchJsonResult *json_result =
        cli_json() ? dispatch_json_result_new("review") : NULL;
    for (int i = 2; i < ids_end; i++) {
        if (json_result)
            dispatch_json_result_add_target(json_result, argv[i]);
        else
            printf("Reviewed %s\n", argv[i]);
        DispatchLogField targets[] = {
            {"task", argv[i]},
        };
        DispatchLogField context[] = {
            {"new_state", "done"},
        };
        char message[256];
        snprintf(message, sizeof(message), "Reviewed %s", argv[i]);
        append_dispatch_log(actor, "review", "review", targets, 1, context, 1,
                            message);
    }
    int status = json_result ? cli_json_emit_tasks(board, NULL, json_result)
                             : 0;
    locked_board_close(&locked);
    return status;
}
