#include "dispatch_cli_internal.h"

#include "dispatch_gh.h"
#include "dispatch_issue.h"

#include <strings.h>

int cmd_ready(int argc, char **argv) {
    /* With task IDs, ready is a mutation and --json selects the result
     * document; without them it is the read-only queue listing. */
    if (argc == 2)
        return cmd_ready_list(cli_json());

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

static char *format_review_close_comment(const DispatchTask *task,
                                         const char *actor) {
    const char *title = task_display_title(task);
    size_t commits_len = 0;
    for (size_t i = 0; i < task->commits.count; i++)
        commits_len += strlen(task->commits.items[i]) + 2;
    size_t size =
        strlen(task->id) + strlen(title) + strlen(actor) + commits_len + 96;
    char *comment = malloc(size);
    if (!comment) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    int pos = snprintf(comment, size,
                       "Closed via Dispatch review of %s (%s) by %s.",
                       task->id, title, actor);
    if (pos > 0 && (size_t)pos < size && task->commits.count > 0) {
        size_t used = (size_t)pos;
        used += (size_t)snprintf(comment + used, size - used, " Commits: ");
        for (size_t i = 0; i < task->commits.count && used < size; i++) {
            used += (size_t)snprintf(comment + used, size - used, "%s%s",
                                     i == 0 ? "" : ", ",
                                     task->commits.items[i]);
        }
        if (used < size)
            snprintf(comment + used, size - used, ".");
    }
    return comment;
}

static int issue_already_closed_in_batch(const char *const *closed_refs,
                                         size_t count, const char *issue) {
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(closed_refs[i], issue) == 0)
            return 1;
    }
    return 0;
}

static void format_gh_close_detail(DispatchGhStatus status,
                                   const DispatchGhResult *gh_result,
                                   char *out, size_t out_size) {
    if (status == DISPATCH_GH_MISSING) {
        snprintf(out, out_size, "gh is not installed or not on PATH");
    } else if (status == DISPATCH_GH_NOT_AUTHENTICATED) {
        snprintf(out, out_size, "gh is not logged in (run: gh auth login)");
    } else if (gh_result && gh_result->detail[0]) {
        snprintf(out, out_size, "%s", gh_result->detail);
    } else {
        snprintf(out, out_size, "gh failed");
    }
}

static void close_reviewed_issues(DispatchBoard *board, int ids_end,
                                  char **argv, const char *actor,
                                  DispatchJsonResult *json_result) {
    if (json_result)
        dispatch_json_result_init_close_issues(json_result);

    const char **closed_refs = NULL;
    size_t closed_count = 0;
    size_t closed_capacity = 0;
    DispatchGhStatus fatal_status = DISPATCH_GH_OK;
    char fatal_detail[256] = {0};

    for (int i = 2; i < ids_end; i++) {
        DispatchTask *task = dispatch_board_find_task(board, argv[i]);
        if (!task || !task->issue || !task->issue[0]) {
            if (!json_result)
                printf("No linked issue on %s\n", argv[i]);
            continue;
        }
        if (issue_already_closed_in_batch(closed_refs, closed_count,
                                          task->issue)) {
            if (!json_result) {
                printf("Issue %s already closed during this review\n",
                       task->issue);
            }
            continue;
        }

        DispatchIssueRef ref;
        if (!dispatch_issue_parse_canonical(task->issue, &ref))
            continue;

        char detail[256] = {0};
        DispatchGhStatus status;
        if (fatal_status != DISPATCH_GH_OK) {
            status = fatal_status;
            snprintf(detail, sizeof(detail), "%s", fatal_detail);
        } else {
            char *comment = format_review_close_comment(task, actor);
            DispatchGhResult gh_result = {0};
            status = dispatch_gh_issue_close(&ref, comment, &gh_result);
            free(comment);
            if (status != DISPATCH_GH_OK)
                format_gh_close_detail(status, &gh_result, detail,
                                       sizeof(detail));
            dispatch_gh_result_free(&gh_result);
            if (status == DISPATCH_GH_MISSING ||
                status == DISPATCH_GH_NOT_AUTHENTICATED) {
                fatal_status = status;
                snprintf(fatal_detail, sizeof(fatal_detail), "%s", detail);
            }
        }

        if (status == DISPATCH_GH_OK) {
            if (closed_count >= closed_capacity) {
                size_t next = closed_capacity ? closed_capacity * 2 : 4;
                closed_refs =
                    cli_realloc_array(closed_refs, next, sizeof(*closed_refs));
                closed_capacity = next;
            }
            closed_refs[closed_count++] = task->issue;

            DispatchLogField targets[] = {
                {"task", task->id},
            };
            DispatchLogField context[] = {
                {"issue", task->issue},
            };
            char message[256];
            snprintf(message, sizeof(message), "Closed %s for %s", task->issue,
                     task->id);
            append_dispatch_log(actor, "review", "issue_close", targets, 1,
                                context, 1, message);
            if (json_result)
                dispatch_json_result_add_closed_issue(json_result, task->id,
                                                      task->issue);
            else
                printf("Closed %s for %s\n", task->issue, task->id);
        } else {
            const char *code = dispatch_gh_error_code(status);
            if (json_result) {
                dispatch_json_result_add_issue_close_error(
                    json_result, task->id, task->issue, code, detail);
            } else {
                fprintf(stderr, "Warning: could not close %s for %s: %s\n",
                        task->issue, task->id, detail);
            }
        }
    }

    free(closed_refs);
}

int cmd_review(int argc, char **argv) {
    if (argc < 3) {
        cli_error("usage", NULL,
                  "Usage: dispatch review <id> [<id> ...] [--actor <name>] "
                  "[--close-issue]\n");
        return 1;
    }

    /* Leading arguments are task IDs; options follow the last ID. */
    int ids_end = 2;
    while (ids_end < argc && strncmp(argv[ids_end], "--", 2) != 0)
        ids_end++;
    if (ids_end == 2) {
        cli_error("usage", NULL,
                  "Usage: dispatch review <id> [<id> ...] [--actor <name>] "
                  "[--close-issue]\n");
        return 1;
    }

    const char *actor = "user";
    int close_issue = 0;
    for (int i = ids_end; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc &&
            argv[i + 1][0] != '\0') {
            actor = argv[++i];
        } else if (strcmp(argv[i], "--close-issue") == 0) {
            close_issue = 1;
        } else {
            cli_error("usage", NULL,
                      "Usage: dispatch review <id> [<id> ...] "
                      "[--actor <name>] [--close-issue]\n");
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
    /* Release the board lock before any network calls for --close-issue. */
    dispatch_store_lock_release(&locked.lock);

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
    if (close_issue)
        close_reviewed_issues(board, ids_end, argv, actor, json_result);

    int status = json_result ? cli_json_emit_tasks(board, NULL, json_result)
                             : 0;
    locked_board_close(&locked);
    return status;
}
