#include "dispatch_json.h"

#include <jansson.h>
#include <stdio.h>
#include <string.h>

static DispatchState presentation_state(const DispatchBoard *board,
                                        const DispatchTask *task) {
    if (task->state == DISPATCH_STATE_PROPOSED)
        return DISPATCH_STATE_PROPOSED;
    return dispatch_task_effective_state(board, task);
}

static json_t *optional_string(const char *value) {
    return value ? json_string(value) : json_null();
}

static json_t *time_or_null(time_t value) {
    return value ? json_integer((json_int_t)value) : json_null();
}

static json_t *string_list_json(const DispatchStringList *list) {
    json_t *array = json_array();
    for (size_t i = 0; i < list->count; i++)
        json_array_append_new(array, json_string(list->items[i]));
    return array;
}

static const DispatchTask *find_task(const DispatchBoard *board,
                                     const char *task_id) {
    for (size_t i = 0; i < board->tasks.count; i++) {
        if (strcmp(board->tasks.items[i].id, task_id) == 0)
            return &board->tasks.items[i];
    }
    return NULL;
}

static int workspace_covers_task(const DispatchWorkspace *workspace,
                                 const char *task_id) {
    if (strcmp(workspace->task_id, task_id) == 0)
        return 1;
    for (size_t i = 0; i < workspace->sequence_tasks.count; i++) {
        if (strcmp(workspace->sequence_tasks.items[i], task_id) == 0)
            return 1;
    }
    return 0;
}

static const DispatchWorkspace *
active_task_workspace(const DispatchBoard *board, const char *task_id) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        const DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_ACTIVE &&
            workspace_covers_task(workspace, task_id))
            return workspace;
    }
    return NULL;
}

static json_t *workspace_json(const DispatchWorkspace *workspace) {
    if (!workspace)
        return json_null();

    json_t *object = json_object();
    json_object_set_new(object, "id", json_string(workspace->id));
    json_object_set_new(object, "actor", json_string(workspace->actor));
    json_object_set_new(object, "path", json_string(workspace->path));
    json_object_set_new(object, "branch", json_string(workspace->branch));
    json_object_set_new(
        object, "state",
        json_string(dispatch_workspace_state_name(workspace->state)));
    return object;
}

static json_t *workspace_record_json(const DispatchBoard *board,
                                     const DispatchWorkspace *workspace) {
    json_t *object = json_object();
    json_object_set_new(object, "id", json_string(workspace->id));
    json_object_set_new(object, "task_id", json_string(workspace->task_id));
    const DispatchTask *task = find_task(board, workspace->task_id);
    json_object_set_new(
        object, "task_state",
        task ? json_string(dispatch_state_name(
                   dispatch_task_effective_state(board, task)))
             : json_null());
    json_object_set_new(object, "actor", json_string(workspace->actor));
    json_object_set_new(object, "path", json_string(workspace->path));
    json_object_set_new(object, "branch", json_string(workspace->branch));
    json_object_set_new(object, "repo_path",
                        json_string(workspace->repo_path));
    json_object_set_new(
        object, "state",
        json_string(dispatch_workspace_state_name(workspace->state)));
    json_object_set_new(object, "sequence_tasks",
                        string_list_json(&workspace->sequence_tasks));
    json_object_set_new(object, "review_gate",
                        optional_string(workspace->review_gate));
    json_object_set_new(object, "created_at",
                        time_or_null(workspace->created_at));
    json_object_set_new(object, "updated_at",
                        time_or_null(workspace->updated_at));
    return object;
}

int dispatch_json_emit_workspaces(FILE *stream, const DispatchBoard *board,
                                  const char *command,
                                  const DispatchWorkspace *only) {
    json_t *workspaces = json_array();
    size_t returned = 0;
    if (only) {
        json_array_append_new(workspaces, workspace_record_json(board, only));
        returned = 1;
    } else {
        for (size_t i = 0; i < board->workspaces.count; i++) {
            const DispatchWorkspace *workspace = &board->workspaces.items[i];
            if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
                continue;
            json_array_append_new(workspaces,
                                  workspace_record_json(board, workspace));
            returned++;
        }
    }

    json_t *board_json = json_object();
    json_object_set_new(board_json, "name", json_string(board->name));
    json_object_set_new(board_json, "repo_path",
                        json_string(board->repo_path ? board->repo_path : "."));

    json_t *summary = json_object();
    json_object_set_new(summary, "returned",
                        json_integer((json_int_t)returned));

    json_t *root = json_object();
    json_object_set_new(root, "schema_version", json_integer(1));
    json_object_set_new(root, "command", json_string(command));
    json_object_set_new(root, "board", board_json);
    json_object_set_new(root, "summary", summary);
    json_object_set_new(root, "workspaces", workspaces);

    int result = json_dumpf(root, stream, JSON_INDENT(2));
    json_decref(root);
    if (result != 0 || fputc('\n', stream) == EOF)
        return 0;
    return 1;
}

static json_t *blocked_by_json(const DispatchBoard *board,
                               const DispatchTask *task) {
    json_t *array = json_array();
    for (size_t i = 0; i < task->depends_on.count; i++) {
        const char *dependency_id = task->depends_on.items[i];
        const DispatchTask *dependency = find_task(board, dependency_id);
        if (!dependency || dispatch_task_effective_state(board, dependency) !=
                               DISPATCH_STATE_DONE) {
            json_array_append_new(array, json_string(dependency_id));
        }
    }
    return array;
}

static json_t *blocks_json(const DispatchBoard *board,
                           const DispatchTask *task) {
    json_t *array = json_array();
    for (size_t i = 0; i < board->tasks.count; i++) {
        const DispatchTask *candidate = &board->tasks.items[i];
        for (size_t d = 0; d < candidate->depends_on.count; d++) {
            if (strcmp(candidate->depends_on.items[d], task->id) == 0) {
                json_array_append_new(array, json_string(candidate->id));
                break;
            }
        }
    }
    return array;
}

static json_t *history_json(const DispatchHistory *history) {
    json_t *array = json_array();
    for (size_t i = 0; i < history->count; i++) {
        const DispatchHistoryEntry *entry = &history->items[i];
        json_t *object = json_object();
        json_object_set_new(object, "actor", json_string(entry->actor));
        json_object_set_new(object, "action", json_string(entry->action));
        json_object_set_new(object, "note",
                            json_string(entry->note ? entry->note : ""));
        json_object_set_new(object, "timestamp",
                            time_or_null(entry->timestamp));
        json_array_append_new(array, object);
    }
    return array;
}

static json_t *task_json(const DispatchBoard *board, const DispatchTask *task) {
    json_t *object = json_object();
    json_object_set_new(object, "id", json_string(task->id));
    json_object_set_new(object, "title", json_string(task->title));
    json_object_set_new(object, "description", json_string(task->description));
    json_object_set_new(object, "group", json_string(task->group));
    json_object_set_new(
        object, "state",
        json_string(dispatch_state_name(presentation_state(board, task))));
    json_object_set_new(object, "stored_state",
                        json_string(dispatch_state_name(task->state)));
    json_object_set_new(object, "requires_review",
                        json_boolean(task->requires_review));
    json_object_set_new(object, "priority", json_integer(task->priority));
    json_object_set_new(object, "assigned_to",
                        optional_string(task->assigned_to));
    json_object_set_new(object, "started_by",
                        optional_string(task->started_by));
    json_object_set_new(object, "completed_by",
                        optional_string(task->completed_by));
    json_object_set_new(object, "depends_on",
                        string_list_json(&task->depends_on));
    json_object_set_new(object, "blocked_by", blocked_by_json(board, task));
    json_object_set_new(object, "blocks", blocks_json(board, task));
    json_object_set_new(object, "commits", string_list_json(&task->commits));
    json_object_set_new(object, "workspace",
                        workspace_json(active_task_workspace(board, task->id)));
    json_object_set_new(object, "created_at", time_or_null(task->created_at));
    json_object_set_new(object, "started_at", time_or_null(task->started_at));
    json_object_set_new(object, "completed_at",
                        time_or_null(task->completed_at));
    json_object_set_new(object, "updated_at", time_or_null(task->updated_at));
    json_object_set_new(object, "history", history_json(&task->history));
    return object;
}

static json_t *groups_json(const DispatchBoard *board) {
    json_t *array = json_array();
    for (size_t i = 0; i < board->groups.count; i++) {
        const DispatchGroup *group = &board->groups.items[i];
        json_t *object = json_object();
        json_object_set_new(object, "id", json_string(group->id));
        json_object_set_new(object, "name", json_string(group->name));
        json_object_set_new(object, "prefix", json_string(group->prefix));
        json_object_set_new(
            object, "description",
            json_string(group->description ? group->description : ""));
        json_array_append_new(array, object);
    }
    return array;
}

static int task_matches(const DispatchBoard *board, const DispatchTask *task,
                        const DispatchJsonRequest *request) {
    DispatchState state = presentation_state(board, task);
    if (request->task_id && strcmp(task->id, request->task_id) != 0)
        return 0;
    if (request->group && strcmp(task->group, request->group) != 0)
        return 0;
    if (request->state_mask &&
        !(request->state_mask & DISPATCH_JSON_STATE(state)))
        return 0;
    if (request->include_done == 0 && state == DISPATCH_STATE_DONE)
        return 0;
    return 1;
}

static json_t *query_json(const DispatchJsonRequest *request) {
    json_t *object = json_object();
    json_object_set_new(object, "task_id", optional_string(request->task_id));
    json_object_set_new(object, "group", optional_string(request->group));
    json_object_set_new(object, "include_done",
                        request->include_done < 0
                            ? json_null()
                            : json_boolean(request->include_done));

    json_t *states = json_array();
    for (int state = DISPATCH_STATE_PROPOSED; state <= DISPATCH_STATE_PAUSED;
         state++) {
        if (request->state_mask & DISPATCH_JSON_STATE(state)) {
            json_array_append_new(
                states, json_string(dispatch_state_name((DispatchState)state)));
        }
    }
    json_object_set_new(object, "states", states);
    return object;
}

static json_t *summary_json(const DispatchBoard *board, size_t returned) {
    size_t state_counts[DISPATCH_STATE_PAUSED + 1] = {0};
    size_t enabled_agents = 0;
    size_t archived_agents = 0;
    size_t active_agent_sessions = 0;
    size_t active_workspaces = 0;
    size_t removed_workspaces = 0;

    for (size_t i = 0; i < board->tasks.count; i++)
        state_counts[presentation_state(board, &board->tasks.items[i])]++;
    for (size_t i = 0; i < board->agents.count; i++) {
        const DispatchAgent *agent = &board->agents.items[i];
        if (agent->archived)
            archived_agents++;
        else
            enabled_agents++;
        if (agent->current_task && agent->current_task[0])
            active_agent_sessions++;
    }
    for (size_t i = 0; i < board->workspaces.count; i++) {
        if (board->workspaces.items[i].state == DISPATCH_WORKSPACE_REMOVED)
            removed_workspaces++;
        else
            active_workspaces++;
    }

    json_t *states = json_object();
    for (int state = DISPATCH_STATE_PROPOSED; state <= DISPATCH_STATE_PAUSED;
         state++) {
        json_object_set_new(states, dispatch_state_name((DispatchState)state),
                            json_integer((json_int_t)state_counts[state]));
    }

    json_t *agents = json_object();
    json_object_set_new(agents, "enabled", json_integer(enabled_agents));
    json_object_set_new(agents, "archived", json_integer(archived_agents));
    json_object_set_new(agents, "with_current_task",
                        json_integer(active_agent_sessions));

    json_t *workspaces = json_object();
    json_object_set_new(workspaces, "active", json_integer(active_workspaces));
    json_object_set_new(workspaces, "removed",
                        json_integer(removed_workspaces));

    json_t *summary = json_object();
    json_object_set_new(summary, "total",
                        json_integer((json_int_t)board->tasks.count));
    json_object_set_new(summary, "returned",
                        json_integer((json_int_t)returned));
    json_object_set_new(summary, "states", states);
    json_object_set_new(summary, "agents", agents);
    json_object_set_new(summary, "workspaces", workspaces);
    return summary;
}

static void append_warning(json_t *warnings, const char *code,
                           const char *message, const char *task_id,
                           const char *agent) {
    json_t *warning = json_object();
    json_object_set_new(warning, "code", json_string(code));
    json_object_set_new(warning, "message", json_string(message));
    json_object_set_new(warning, "task_id", optional_string(task_id));
    json_object_set_new(warning, "agent", optional_string(agent));
    json_array_append_new(warnings, warning);
}

static int board_has_task(const DispatchBoard *board, const char *task_id) {
    return find_task(board, task_id) != NULL;
}

static json_t *warnings_json(const DispatchBoard *board, int include_warnings) {
    json_t *warnings = json_array();
    char message[512];
    if (!include_warnings)
        return warnings;

    for (size_t i = 0; i < board->tasks.count; i++) {
        const DispatchTask *task = &board->tasks.items[i];
        DispatchState state = presentation_state(board, task);
        if ((state == DISPATCH_STATE_DONE || state == DISPATCH_STATE_REVIEW) &&
            task->commits.count == 0) {
            snprintf(message, sizeof(message), "%s has no recorded commits",
                     task->id);
            append_warning(warnings, "missing_commits", message, task->id,
                           NULL);
        }
        if (task->assigned_to && state != DISPATCH_STATE_DOING &&
            state != DISPATCH_STATE_REVIEW) {
            snprintf(message, sizeof(message),
                     "%s is assigned to %s but state is %s", task->id,
                     task->assigned_to, dispatch_state_name(state));
            append_warning(warnings, "assignment_state_mismatch", message,
                           task->id, NULL);
        }
    }
    for (size_t i = 0; i < board->agents.count; i++) {
        const DispatchAgent *agent = &board->agents.items[i];
        if (!agent->current_task || !agent->current_task[0] ||
            board_has_task(board, agent->current_task))
            continue;
        snprintf(message, sizeof(message),
                 "agent %s references missing current task %s", agent->name,
                 agent->current_task);
        append_warning(warnings, "missing_current_task", message, NULL,
                       agent->name);
    }
    return warnings;
}

int dispatch_cli_extract_json_flag(int *argc, char **argv, int first_arg,
                                   int *json_output) {
    int write_index = first_arg;
    *json_output = 0;
    for (int read_index = first_arg; read_index < *argc; read_index++) {
        if (strcmp(argv[read_index], "--json") == 0) {
            if (*json_output)
                return 0;
            *json_output = 1;
            continue;
        }
        argv[write_index++] = argv[read_index];
    }
    *argc = write_index;
    argv[write_index] = NULL;
    return 1;
}

int dispatch_json_emit(FILE *stream, const DispatchBoard *board,
                       const DispatchJsonRequest *request) {
    json_t *tasks = json_array();
    size_t returned = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        const DispatchTask *task = &board->tasks.items[i];
        if (!task_matches(board, task, request))
            continue;
        json_array_append_new(tasks, task_json(board, task));
        returned++;
    }

    json_t *board_json = json_object();
    json_object_set_new(board_json, "name", json_string(board->name));
    json_object_set_new(board_json, "repo_path",
                        json_string(board->repo_path ? board->repo_path : "."));
    json_object_set_new(board_json, "groups", groups_json(board));

    json_t *root = json_object();
    json_object_set_new(root, "schema_version", json_integer(1));
    json_object_set_new(root, "command", json_string(request->command));
    json_object_set_new(root, "board", board_json);
    json_object_set_new(root, "query", query_json(request));
    json_object_set_new(root, "summary", summary_json(board, returned));
    json_object_set_new(root, "tasks", tasks);
    json_object_set_new(root, "warnings",
                        warnings_json(board, request->include_warnings));

    int result = json_dumpf(root, stream, JSON_INDENT(2));
    json_decref(root);
    if (result != 0 || fputc('\n', stream) == EOF)
        return 0;
    return 1;
}
