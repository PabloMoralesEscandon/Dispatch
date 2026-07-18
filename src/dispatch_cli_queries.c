#include "dispatch_cli_internal.h"

int save_task_transition(LockedBoard *locked, const char *verb,
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

static DispatchState task_presentation_state(const DispatchBoard *board,
                                             const DispatchTask *task) {
    if (task && task->state == DISPATCH_STATE_PROPOSED)
        return DISPATCH_STATE_PROPOSED;
    return dispatch_task_effective_state(board, task);
}

DispatchWorkspace *task_workspace(const DispatchBoard *board,
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
    DispatchState state = task_presentation_state(board, task);
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
    if (task->commits.count > 0)
        printf("  %scommits:%zu%s", meta_color, task->commits.count, reset);
    if (task->priority != 0)
        printf("  %spriority:%d%s", meta_color, task->priority, reset);
    printf("\n");

    DispatchWorkspace *workspace = task_workspace(board, task->id, 1);
    if (workspace) {
        printf("%s  workspace: %s\n", indent, workspace->path);
        printf("%s  branch: %s\n", indent, workspace->branch);
    }
}

/* Order ready tasks by descending priority; ties keep board order so the
 * listing stays stable for same-priority tasks. */
static int ready_task_order_compare(const void *left, const void *right) {
    const DispatchTask *a = *(const DispatchTask *const *)left;
    const DispatchTask *b = *(const DispatchTask *const *)right;
    if (a->priority != b->priority)
        return b->priority - a->priority;
    return a < b ? -1 : (a > b ? 1 : 0);
}

int print_ready_tasks_from_board(const DispatchBoard *board,
                                        const char *indent) {
    const DispatchTask **ready = NULL;
    int count = 0;
    if (board->tasks.count > 0) {
        ready = malloc(board->tasks.count * sizeof(*ready));
        if (!ready) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
    }
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (dispatch_task_effective_state(board, task) != DISPATCH_STATE_READY)
            continue;
        ready[count++] = task;
    }
    qsort(ready, (size_t)count, sizeof(*ready), ready_task_order_compare);
    for (int i = 0; i < count; i++)
        print_task_line(board, ready[i], indent);
    free(ready);
    return count;
}

int ready_task_count(const DispatchBoard *board) {
    int count = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (dispatch_task_effective_state(board, task) == DISPATCH_STATE_READY)
            count++;
    }
    return count;
}

static int task_count_by_presentation_state(const DispatchBoard *board,
                                            DispatchState state) {
    int count = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (task_presentation_state(board, task) == state)
            count++;
    }
    return count;
}

static int print_tasks_by_presentation_state(const DispatchBoard *board,
                                             DispatchState state,
                                             const char *indent) {
    int count = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (task_presentation_state(board, task) != state)
            continue;
        print_task_line(board, task, indent);
        count++;
    }
    return count;
}

static void print_list_for_group(const DispatchBoard *board,
                                 const DispatchGroup *group,
                                 int include_done) {
    int color = cli_color_enabled();
    const char *group_color = color ? "\033[1;36m" : "";
    const char *reset = color ? "\033[0m" : "";

    printf("%s[%s] %s%s\n", group_color, group->prefix, group->name, reset);

    int any_tasks = 0;
    int any_visible = 0;
    int all_done = 1;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (strcmp(task->group, group->id) != 0)
            continue;
        any_tasks = 1;
        DispatchState state = task_presentation_state(board, task);
        if (state != DISPATCH_STATE_DONE)
            all_done = 0;
        if (!include_done && state == DISPATCH_STATE_DONE)
            continue;
        print_task_line(board, task, "  ");
        any_visible = 1;
    }

    if (!any_tasks)
        printf("  (no tasks)\n");
    else if (!any_visible && all_done)
        printf("  (done)\n");
}

int cmd_list(int argc, char **argv) {
    int json_output = 0;
    if (!dispatch_cli_extract_json_flag(&argc, argv, 2, &json_output)) {
        fprintf(stderr, "Usage: dispatch list [all] [group] [--json]\n");
        return 1;
    }
    if (argc != 2 && argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: dispatch list [all] [group] [--json]\n");
        return 1;
    }

    int include_done = 0;
    int group_arg = 2;
    if (argc >= 3 && strcmp(argv[2], "all") == 0) {
        include_done = 1;
        group_arg = 3;
    }
    if (argc == 4 && !include_done) {
        fprintf(stderr, "Usage: dispatch list [all] [group] [--json]\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (argc > group_arg) {
        DispatchGroup *group =
            dispatch_board_find_group(&board, argv[group_arg]);
        if (!group) {
            dispatch_board_free(&board);
            fprintf(stderr, "No group with id, prefix, or name %s\n",
                    argv[group_arg]);
            return 1;
        }
        if (json_output) {
            DispatchJsonRequest request = {
                .command = "list",
                .group = group->id,
                .include_done = include_done,
            };
            int result = dispatch_json_emit(stdout, &board, &request) ? 0 : 1;
            dispatch_board_free(&board);
            return result;
        }
        print_list_for_group(&board, group, include_done);
        dispatch_board_free(&board);
        return 0;
    }

    if (json_output) {
        DispatchJsonRequest request = {
            .command = "list",
            .include_done = include_done,
        };
        int result = dispatch_json_emit(stdout, &board, &request) ? 0 : 1;
        dispatch_board_free(&board);
        return result;
    }

    if (board.groups.count == 0) {
        printf("(no groups)\n");
    } else {
        for (size_t g = 0; g < board.groups.count; g++)
            print_list_for_group(&board, &board.groups.items[g], include_done);
    }

    dispatch_board_free(&board);
    return 0;
}

int cmd_show(int argc, char **argv) {
    int json_output = 0;
    if (!dispatch_cli_extract_json_flag(&argc, argv, 2, &json_output)) {
        fprintf(stderr, "Usage: dispatch show <id> [--json]\n");
        return 1;
    }
    if (argc != 3) {
        fprintf(stderr, "Usage: dispatch show <id> [--json]\n");
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

    if (json_output) {
        DispatchJsonRequest request = {
            .command = "show",
            .task_id = task->id,
            .include_done = -1,
        };
        int result = dispatch_json_emit(stdout, &board, &request) ? 0 : 1;
        dispatch_board_free(&board);
        return result;
    }

    printf("ID: %s\n", task->id);
    printf("Title: %s\n", task_display_title(task));
    printf("Description: %s\n", task->description);
    printf("Group: %s\n", task->group);
    printf("State: %s\n",
           dispatch_state_name(task_presentation_state(&board, task)));
    printf("Requires review: %s\n", task->requires_review ? "yes" : "no");
    printf("Priority: %d\n", task->priority);
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

    printf("Commits:");
    if (task->commits.count == 0) {
        printf(" -");
    } else {
        for (size_t i = 0; i < task->commits.count; i++)
            printf(" %s", task->commits.items[i]);
    }
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

static int commit_ref_is_valid(const char *ref) {
    size_t len = ref ? strlen(ref) : 0;
    if (len < 4 || len > 64)
        return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)ref[i]))
            return 0;
    }
    return 1;
}

static int task_commit_contains(const DispatchTask *task, const char *ref) {
    for (size_t i = 0; i < task->commits.count; i++) {
        if (strcmp(task->commits.items[i], ref) == 0)
            return 1;
    }
    return 0;
}

static void task_commit_append(DispatchTask *task, const char *ref) {
    if (task->commits.count >= task->commits.capacity) {
        task->commits.capacity =
            task->commits.capacity == 0 ? 4 : task->commits.capacity * 2;
        task->commits.items = cli_realloc_array(
            task->commits.items, task->commits.capacity,
            sizeof(*task->commits.items));
    }
    task->commits.items[task->commits.count++] = cli_strdup(ref);
    task->updated_at = time(NULL);
}

static void print_task_commits(const DispatchTask *task, int include_header) {
    if (include_header) {
        printf("Task: %s %s\n", task->id, task_display_title(task));
        printf("Commits:\n");
    }

    if (task->commits.count == 0) {
        printf("%s(no commits)\n", include_header ? "  " : "");
        return;
    }

    for (size_t i = 0; i < task->commits.count; i++)
        printf("%s%s\n", include_header ? "  " : "", task->commits.items[i]);
}

int cmd_commit(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
                "Usage: dispatch commit add <task-id> <sha> [--actor <name>]\n");
        fprintf(stderr, "       dispatch commit list <task-id>\n");
        fprintf(stderr, "       dispatch commit show <task-id>\n");
        return 1;
    }

    const char *subcommand = argv[2];
    const char *task_id = argv[3];

    if (strcmp(subcommand, "list") == 0 || strcmp(subcommand, "show") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: dispatch commit %s <task-id>\n",
                    subcommand);
            return 1;
        }

        DispatchBoard board;
        if (!load_board_or_error(&board))
            return 1;

        DispatchTask *task = dispatch_board_find_task(&board, task_id);
        if (!task) {
            dispatch_board_free(&board);
            fprintf(stderr, "No task with id %s\n", task_id);
            return 1;
        }

        print_task_commits(task, strcmp(subcommand, "show") == 0);
        dispatch_board_free(&board);
        return 0;
    }

    if (strcmp(subcommand, "add") != 0) {
        fprintf(stderr,
                "Usage: dispatch commit add <task-id> <sha> [--actor <name>]\n");
        fprintf(stderr, "       dispatch commit list <task-id>\n");
        fprintf(stderr, "       dispatch commit show <task-id>\n");
        return 1;
    }

    if (argc != 5 && argc != 7) {
        fprintf(stderr,
                "Usage: dispatch commit add <task-id> <sha> [--actor <name>]\n");
        return 1;
    }

    const char *sha = argv[4];
    const char *actor = "user";
    if (argc == 7) {
        if (strcmp(argv[5], "--actor") != 0 || argv[6][0] == '\0') {
            fprintf(stderr,
                    "Usage: dispatch commit add <task-id> <sha> [--actor <name>]\n");
            return 1;
        }
        actor = argv[6];
    }

    if (!commit_ref_is_valid(sha)) {
        fprintf(stderr, "Commit reference must be a 4-64 character hex SHA\n");
        return 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    DispatchTask *task = dispatch_board_find_task(&locked.board, task_id);
    if (!task) {
        locked_board_close(&locked);
        fprintf(stderr, "No task with id %s\n", task_id);
        return 1;
    }

    if (task_commit_contains(task, sha)) {
        locked_board_close(&locked);
        printf("Commit %s already recorded for %s\n", sha, task_id);
        return 0;
    }

    task_commit_append(task, sha);
    dispatch_task_append_history(task, actor, "commit", sha);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    printf("Added commit %s to %s\n", sha, task_id);
    DispatchLogField targets[] = {
        {"task", task_id},
    };
    DispatchLogField context[] = {
        {"commit", sha},
    };
    char message[256];
    snprintf(message, sizeof(message), "Added commit %s to %s", sha, task_id);
    append_dispatch_log(actor, "commit", "commit_add", targets, 1, context, 1,
                        message);
    locked_board_close(&locked);
    return 0;
}

int cmd_ready_list(int json_output) {
    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (json_output) {
        DispatchJsonRequest request = {
            .command = "ready",
            .include_done = -1,
            .state_mask = DISPATCH_JSON_STATE(DISPATCH_STATE_READY),
        };
        int result = dispatch_json_emit(stdout, &board, &request) ? 0 : 1;
        dispatch_board_free(&board);
        return result;
    }

    int count = print_ready_tasks_from_board(&board, "");
    if (count == 0) {
        int review_count =
            task_count_by_presentation_state(&board, DISPATCH_STATE_REVIEW);
        int proposed_count =
            task_count_by_presentation_state(&board, DISPATCH_STATE_PROPOSED);
        int blocked_count =
            task_count_by_presentation_state(&board, DISPATCH_STATE_BLOCKED);
        printf("No ready tasks.\n");
        if (review_count > 0)
            printf("Reviews waiting: %d (run: dispatch reviews)\n",
                   review_count);
        if (proposed_count > 0)
            printf("Proposed tasks: %d (run: dispatch proposed)\n",
                   proposed_count);
        if (blocked_count > 0)
            printf("Blocked tasks: %d (run: dispatch blocked)\n",
                   blocked_count);
    }

    dispatch_board_free(&board);
    return 0;
}

int cmd_queue_list(int argc, char **argv, DispatchState state,
                          const char *label) {
    int json_output = 0;
    if (!dispatch_cli_extract_json_flag(&argc, argv, 2, &json_output)) {
        fprintf(stderr, "Usage: dispatch %s [--json]\n", label);
        return 1;
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: dispatch %s [--json]\n", label);
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (json_output) {
        DispatchJsonRequest request = {
            .command = label,
            .include_done = -1,
            .state_mask = DISPATCH_JSON_STATE(state),
        };
        int result = dispatch_json_emit(stdout, &board, &request) ? 0 : 1;
        dispatch_board_free(&board);
        return result;
    }

    int count = print_tasks_by_presentation_state(&board, state, "");
    if (count == 0)
        printf("(no %s tasks)\n", label);

    dispatch_board_free(&board);
    return 0;
}

int cmd_blocked(int argc, char **argv) {
    int json_output = 0;
    if (!dispatch_cli_extract_json_flag(&argc, argv, 2, &json_output)) {
        fprintf(stderr, "Usage: dispatch blocked [--json]\n");
        return 1;
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: dispatch blocked [--json]\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (json_output) {
        DispatchJsonRequest request = {
            .command = "blocked",
            .include_done = -1,
            .state_mask = DISPATCH_JSON_STATE(DISPATCH_STATE_BLOCKED),
        };
        int result = dispatch_json_emit(stdout, &board, &request) ? 0 : 1;
        dispatch_board_free(&board);
        return result;
    }

    for (size_t i = 0; i < board.tasks.count; i++) {
        DispatchTask *task = &board.tasks.items[i];
        if (task_presentation_state(&board, task) != DISPATCH_STATE_BLOCKED) {
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

int cmd_status(int argc, char **argv) {
    int json_output = 0;
    if (!dispatch_cli_extract_json_flag(&argc, argv, 2, &json_output)) {
        fprintf(stderr, "Usage: dispatch status [--json]\n");
        return 1;
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: dispatch status [--json]\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (json_output) {
        DispatchJsonRequest request = {
            .command = "status",
            .include_done = -1,
            .state_mask = DISPATCH_JSON_STATE(DISPATCH_STATE_READY) |
                          DISPATCH_JSON_STATE(DISPATCH_STATE_REVIEW),
            .include_warnings = 1,
        };
        int result = dispatch_json_emit(stdout, &board, &request) ? 0 : 1;
        dispatch_board_free(&board);
        return result;
    }

    size_t state_counts[DISPATCH_STATE_PAUSED + 1] = {0};
    size_t enabled_agents = 0;
    size_t archived_agents = 0;
    size_t active_agent_sessions = 0;
    size_t active_workspaces = 0;
    size_t removed_workspaces = 0;
    size_t warnings = 0;

    for (size_t i = 0; i < board.tasks.count; i++) {
        DispatchState state = task_presentation_state(&board, &board.tasks.items[i]);
        state_counts[state]++;
    }
    for (size_t i = 0; i < board.agents.count; i++) {
        DispatchAgent *agent = &board.agents.items[i];
        if (agent->archived)
            archived_agents++;
        else
            enabled_agents++;
        if (agent->current_task && agent->current_task[0])
            active_agent_sessions++;
    }
    for (size_t i = 0; i < board.workspaces.count; i++) {
        if (board.workspaces.items[i].state == DISPATCH_WORKSPACE_REMOVED)
            removed_workspaces++;
        else
            active_workspaces++;
    }

    printf("Dispatch status\n");
    printf("Board: %s\n", board.name);
    printf("Repo: %s\n", board.repo_path ? board.repo_path : ".");
    printf("Tasks: %zu total", board.tasks.count);
    for (int state = DISPATCH_STATE_PROPOSED; state <= DISPATCH_STATE_PAUSED;
         state++) {
        printf("  %s:%zu", dispatch_state_name((DispatchState)state),
               state_counts[state]);
    }
    printf("\n");

    printf("Ready:\n");
    if (print_ready_tasks_from_board(&board, "  ") == 0)
        printf("  -\n");

    printf("Review:\n");
    int review_count = 0;
    for (size_t i = 0; i < board.tasks.count; i++) {
        DispatchTask *task = &board.tasks.items[i];
        if (task_presentation_state(&board, task) != DISPATCH_STATE_REVIEW)
            continue;
        print_task_line(&board, task, "  ");
        review_count++;
    }
    if (review_count == 0)
        printf("  -\n");

    printf("Blocked: %zu\n", state_counts[DISPATCH_STATE_BLOCKED]);
    printf("Agents: %zu enabled, %zu archived", enabled_agents,
           archived_agents);
    if (active_agent_sessions > 0)
        printf(", %zu with current task", active_agent_sessions);
    printf("\n");
    printf("Workspaces: %zu active, %zu removed\n", active_workspaces,
           removed_workspaces);

    printf("Warnings:\n");
    for (size_t i = 0; i < board.tasks.count; i++) {
        DispatchTask *task = &board.tasks.items[i];
        DispatchState state = task_presentation_state(&board, task);
        if ((state == DISPATCH_STATE_DONE || state == DISPATCH_STATE_REVIEW) &&
            task->commits.count == 0) {
            printf("  %s has no recorded commits\n", task->id);
            warnings++;
        }
        if (task->assigned_to &&
            state != DISPATCH_STATE_DOING && state != DISPATCH_STATE_REVIEW) {
            printf("  %s is assigned to %s but state is %s\n", task->id,
                   task->assigned_to, dispatch_state_name(state));
            warnings++;
        }
    }
    for (size_t i = 0; i < board.agents.count; i++) {
        DispatchAgent *agent = &board.agents.items[i];
        if (!agent->current_task || !agent->current_task[0])
            continue;
        if (!dispatch_board_find_task(&board, agent->current_task)) {
            printf("  agent %s references missing current task %s\n",
                   agent->name, agent->current_task);
            warnings++;
        }
    }
    if (warnings == 0)
        printf("  -\n");

    dispatch_board_free(&board);
    return 0;
}
