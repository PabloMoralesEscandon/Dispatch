#include "dispatch_json.h"

#include "dispatch_issue.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
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

static json_t *issue_json(const char *issue) {
    DispatchIssueRef ref;
    if (!issue || !dispatch_issue_parse_canonical(issue, &ref))
        return json_null();

    char repo[DISPATCH_ISSUE_TEXT_MAX];
    char url[DISPATCH_ISSUE_TEXT_MAX];
    dispatch_issue_format_repo(&ref, repo, sizeof(repo));
    dispatch_issue_format_url(&ref, url, sizeof(url));
    json_t *object = json_object();
    json_object_set_new(object, "repo", json_string(repo));
    json_object_set_new(object, "number", json_integer((json_int_t)ref.number));
    json_object_set_new(object, "url", json_string(url));
    return object;
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
    json_object_set_new(object, "adopted", json_boolean(workspace->adopted));
    json_object_set_new(object, "external_id",
                        optional_string(workspace->external_id));
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
    json_object_set_new(object, "adopted", json_boolean(workspace->adopted));
    json_object_set_new(object, "external_id",
                        optional_string(workspace->external_id));
    json_object_set_new(object, "created_at",
                        time_or_null(workspace->created_at));
    json_object_set_new(object, "updated_at",
                        time_or_null(workspace->updated_at));
    return object;
}

/* The board object of the workspace, agent, and log envelopes. */
static json_t *board_ref_json(const DispatchBoard *board) {
    json_t *board_json = json_object();
    json_object_set_new(board_json, "name", json_string(board->name));
    json_object_set_new(board_json, "repo_path",
                        json_string(board->repo_path ? board->repo_path : "."));
    return board_json;
}

static json_t *envelope_json(const DispatchBoard *board, const char *command,
                             json_t *query, json_t *summary) {
    json_t *root = json_object();
    json_object_set_new(root, "schema_version", json_integer(1));
    json_object_set_new(root, "command", json_string(command));
    json_object_set_new(root, "board", board_ref_json(board));
    json_object_set_new(root, "query", query);
    json_object_set_new(root, "summary", summary);
    return root;
}

/* The workspace envelope up to its workspaces array. query.workspace is
 * the single record's ID when exactly one is returned. */
static json_t *workspace_envelope_json(const DispatchBoard *board,
                                       const char *command,
                                       json_t *workspaces,
                                       const char *query_workspace) {
    size_t active = 0;
    size_t removed = 0;
    for (size_t i = 0; i < board->workspaces.count; i++) {
        if (board->workspaces.items[i].state == DISPATCH_WORKSPACE_REMOVED)
            removed++;
        else
            active++;
    }

    json_t *query = json_object();
    json_object_set_new(query, "workspace", optional_string(query_workspace));

    json_t *summary = json_object();
    json_object_set_new(summary, "total",
                        json_integer((json_int_t)board->workspaces.count));
    json_object_set_new(summary, "returned",
                        json_integer((json_int_t)json_array_size(workspaces)));
    json_object_set_new(summary, "active", json_integer((json_int_t)active));
    json_object_set_new(summary, "removed", json_integer((json_int_t)removed));

    json_t *root = envelope_json(board, command, query, summary);
    json_object_set_new(root, "workspaces", workspaces);
    return root;
}

static int dump_document(FILE *stream, json_t *root);

int dispatch_json_emit_workspaces(FILE *stream, const DispatchBoard *board,
                                  const char *command,
                                  const DispatchWorkspace *only) {
    json_t *workspaces = json_array();
    if (only) {
        json_array_append_new(workspaces, workspace_record_json(board, only));
    } else {
        for (size_t i = 0; i < board->workspaces.count; i++) {
            const DispatchWorkspace *workspace = &board->workspaces.items[i];
            if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
                continue;
            json_array_append_new(workspaces,
                                  workspace_record_json(board, workspace));
        }
    }
    return dump_document(stream,
                         workspace_envelope_json(board, command, workspaces,
                                                 only ? only->id : NULL));
}

static json_t *agent_json(const DispatchBoard *board,
                          const DispatchAgent *agent) {
    json_t *assigned = json_array();
    for (size_t i = 0; i < board->tasks.count; i++) {
        const DispatchTask *task = &board->tasks.items[i];
        if (task->assigned_to && strcmp(task->assigned_to, agent->name) == 0)
            json_array_append_new(assigned, json_string(task->id));
    }
    json_t *workspaces = json_array();
    for (size_t i = 0; i < board->workspaces.count; i++) {
        const DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace->state != DISPATCH_WORKSPACE_REMOVED &&
            strcmp(workspace->actor, agent->name) == 0)
            json_array_append_new(workspaces, json_string(workspace->id));
    }

    json_t *object = json_object();
    json_object_set_new(object, "name", json_string(agent->name));
    json_object_set_new(object, "runner", json_string(agent->runner));
    json_object_set_new(object, "model", optional_string(agent->model));
    json_object_set_new(object, "archived", json_boolean(agent->archived));
    json_object_set_new(object, "agent_dir", optional_string(agent->agent_dir));
    json_object_set_new(object, "prompt_path",
                        optional_string(agent->prompt_path));
    json_object_set_new(object, "run_script_path",
                        optional_string(agent->run_script_path));
    json_object_set_new(object, "session_id",
                        optional_string(agent->session_id));
    json_object_set_new(object, "current_task",
                        optional_string(agent->current_task));
    json_object_set_new(object, "last_workspace",
                        optional_string(agent->last_workspace));
    json_object_set_new(object, "assigned_tasks", assigned);
    json_object_set_new(object, "workspaces", workspaces);
    json_object_set_new(object, "created_at", time_or_null(agent->created_at));
    json_object_set_new(object, "updated_at", time_or_null(agent->updated_at));
    return object;
}

static json_t *agent_envelope_json(const DispatchBoard *board,
                                   const char *command,
                                   const DispatchAgent *only,
                                   int include_archived) {
    json_t *agents = json_array();
    size_t enabled = 0;
    size_t archived = 0;
    size_t with_current_task = 0;
    for (size_t i = 0; i < board->agents.count; i++) {
        const DispatchAgent *agent = &board->agents.items[i];
        if (agent->archived)
            archived++;
        else
            enabled++;
        if (agent->current_task && agent->current_task[0])
            with_current_task++;
        if (only ? agent == only : (include_archived || !agent->archived))
            json_array_append_new(agents, agent_json(board, agent));
    }

    json_t *query = json_object();
    json_object_set_new(query, "name", optional_string(only ? only->name : NULL));
    json_object_set_new(query, "include_archived",
                        only ? json_null() : json_boolean(include_archived));

    json_t *summary = json_object();
    json_object_set_new(summary, "total",
                        json_integer((json_int_t)board->agents.count));
    json_object_set_new(summary, "returned",
                        json_integer((json_int_t)json_array_size(agents)));
    json_object_set_new(summary, "enabled", json_integer((json_int_t)enabled));
    json_object_set_new(summary, "archived",
                        json_integer((json_int_t)archived));
    json_object_set_new(summary, "with_current_task",
                        json_integer((json_int_t)with_current_task));

    json_t *root = envelope_json(board, command, query, summary);
    json_object_set_new(root, "agents", agents);
    return root;
}

int dispatch_json_emit_agents(FILE *stream, const DispatchBoard *board,
                              const char *command, const DispatchAgent *only,
                              int include_archived) {
    return dump_document(
        stream, agent_envelope_json(board, command, only, include_archived));
}

int dispatch_json_emit_log(FILE *stream, const DispatchBoard *board,
                           const char *log_path, const DispatchLogQuery *query,
                           const DispatchLogRead *read) {
    json_t *query_json_value = json_object();
    for (int i = 0; i < DISPATCH_LOG_FILTER_COUNT; i++)
        json_object_set_new(query_json_value, dispatch_log_filter_fields[i],
                            optional_string(query->filters[i]));
    json_object_set_new(query_json_value, "limit",
                        query->limit ? json_integer((json_int_t)query->limit)
                                     : json_null());

    json_t *summary = json_object();
    json_object_set_new(summary, "total", json_integer((json_int_t)read->total));
    json_object_set_new(summary, "matched",
                        json_integer((json_int_t)read->matched));
    json_object_set_new(summary, "returned",
                        json_integer((json_int_t)read->count));
    json_object_set_new(
        summary, "skipped",
        json_integer((json_int_t)(read->malformed + read->unsupported)));

    json_t *records = json_array();
    for (size_t i = 0; i < read->count; i++)
        json_array_append(records, read->items[i].record);

    json_t *warnings = json_array();
    DispatchLogWarning found[2];
    size_t warning_count = dispatch_log_read_warnings(read, log_path, found);
    for (size_t i = 0; i < warning_count; i++) {
        json_t *warning = json_object();
        json_object_set_new(warning, "code", json_string(found[i].code));
        json_object_set_new(warning, "message", json_string(found[i].message));
        json_object_set_new(warning, "task_id", json_null());
        json_object_set_new(warning, "agent", json_null());
        json_object_set_new(warning, "count",
                            json_integer((json_int_t)found[i].count));
        json_object_set_new(warning, "first_line",
                            json_integer((json_int_t)found[i].first_line));
        json_array_append_new(warnings, warning);
    }

    json_t *root = envelope_json(board, "log", query_json_value, summary);
    json_object_set_new(root, "records", records);
    json_object_set_new(root, "warnings", warnings);
    return dump_document(stream, root);
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
    json_object_set_new(object, "issue", issue_json(task->issue));
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
    json_t *message_json = json_string(message ? message : "");
    json_object_set_new(
        warning, "message",
        message_json ? message_json : json_string("(message is not valid UTF-8)"));
    json_object_set_new(warning, "task_id", optional_string(task_id));
    json_object_set_new(warning, "agent", optional_string(agent));
    json_array_append_new(warnings, warning);
}

static json_t *extra_warnings_json(const DispatchJsonWarning *items,
                                   size_t count) {
    json_t *warnings = json_array();
    for (size_t i = 0; i < count; i++) {
        append_warning(warnings, items[i].code, items[i].message,
                       items[i].task_id, items[i].agent);
    }
    return warnings;
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
        const DispatchTask *duplicate =
            dispatch_board_earlier_task_with_issue(board, task);
        if (duplicate) {
            snprintf(message, sizeof(message), "%s links %s, also linked to %s",
                     task->id, task->issue, duplicate->id);
            append_warning(warnings, "duplicate_issue", message, task->id,
                           NULL);
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

    return dump_document(stream, root);
}

static int dump_document(FILE *stream, json_t *root) {
    int result = json_dumpf(root, stream, JSON_INDENT(2));
    json_decref(root);
    if (result != 0 || fputc('\n', stream) == EOF)
        return 0;
    return fflush(stream) == 0;
}

void dispatch_json_snapshot_take(DispatchJsonSnapshot *snapshot,
                                 const DispatchBoard *board) {
    memset(snapshot, 0, sizeof(*snapshot));
    if (!board || board->tasks.count == 0)
        return;

    snapshot->ids = calloc(board->tasks.count, sizeof(*snapshot->ids));
    snapshot->states = calloc(board->tasks.count, sizeof(*snapshot->states));
    if (!snapshot->ids || !snapshot->states) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    for (size_t i = 0; i < board->tasks.count; i++) {
        const DispatchTask *task = &board->tasks.items[i];
        snapshot->ids[i] = strdup(task->id);
        if (!snapshot->ids[i]) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        snapshot->states[i] = presentation_state(board, task);
        snapshot->count++;
    }
}

void dispatch_json_snapshot_free(DispatchJsonSnapshot *snapshot) {
    if (!snapshot)
        return;
    for (size_t i = 0; i < snapshot->count; i++)
        free(snapshot->ids[i]);
    free(snapshot->ids);
    free(snapshot->states);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int snapshot_state(const DispatchJsonSnapshot *snapshot,
                          const char *task_id, DispatchState *state) {
    for (size_t i = 0; snapshot && i < snapshot->count; i++) {
        if (strcmp(snapshot->ids[i], task_id) == 0) {
            *state = snapshot->states[i];
            return 1;
        }
    }
    return 0;
}

struct DispatchJsonResult {
    char *action;
    int changed;
    json_t *targets; /* array of task ID strings */
    json_t *extras;  /* action-specific fields */
};

DispatchJsonResult *dispatch_json_result_new(const char *action) {
    DispatchJsonResult *result = calloc(1, sizeof(*result));
    if (!result || !(result->action = strdup(action))) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    result->changed = 1;
    result->targets = json_array();
    result->extras = json_object();
    /* Prune always reports its skipped list, even when empty. */
    if (strcmp(action, "workspace_prune") == 0)
        json_object_set_new(result->extras, "skipped", json_array());
    return result;
}

void dispatch_json_result_free(DispatchJsonResult *result) {
    if (!result)
        return;
    free(result->action);
    json_decref(result->targets);
    json_decref(result->extras);
    free(result);
}

void dispatch_json_result_add_target(DispatchJsonResult *result,
                                     const char *task_id) {
    json_array_append_new(result->targets, json_string(task_id));
}

void dispatch_json_result_set_changed(DispatchJsonResult *result,
                                      int changed) {
    result->changed = changed ? 1 : 0;
}

void dispatch_json_result_set_string(DispatchJsonResult *result,
                                     const char *key, const char *value) {
    json_object_set_new(result->extras, key, optional_string(value));
}

void dispatch_json_result_set_bool(DispatchJsonResult *result,
                                   const char *key, int value) {
    json_object_set_new(result->extras, key, json_boolean(value));
}

void dispatch_json_result_set_int(DispatchJsonResult *result,
                                  const char *key, long long value) {
    json_object_set_new(result->extras, key, json_integer((json_int_t)value));
}

void dispatch_json_result_add_skipped(DispatchJsonResult *result,
                                      const char *workspace_id,
                                      const char *reason) {
    json_t *skipped = json_object_get(result->extras, "skipped");
    if (!skipped) {
        skipped = json_array();
        json_object_set_new(result->extras, "skipped", skipped);
    }
    json_t *entry = json_object();
    json_object_set_new(entry, "workspace", json_string(workspace_id));
    json_object_set_new(entry, "reason", json_string(reason));
    json_array_append_new(skipped, entry);
}

static json_t *state_or_null(int known, DispatchState state) {
    return known ? json_string(dispatch_state_name(state)) : json_null();
}

/* The single target task ID, or NULL when the command acted on zero or
 * several tasks. */
static const char *single_target(const DispatchJsonResult *result) {
    if (json_array_size(result->targets) != 1)
        return NULL;
    return json_string_value(json_array_get(result->targets, 0));
}

static json_t *result_json(const DispatchBoard *board,
                           const DispatchJsonSnapshot *before,
                           const DispatchJsonResult *result) {
    json_t *object = json_object();
    json_object_set_new(object, "action", json_string(result->action));
    json_object_set_new(object, "changed", json_boolean(result->changed));

    json_t *targets = json_array();
    size_t index;
    json_t *value;
    json_array_foreach(result->targets, index, value) {
        const char *task_id = json_string_value(value);
        DispatchState previous = DISPATCH_STATE_PROPOSED;
        int had_previous = snapshot_state(before, task_id, &previous);
        const DispatchTask *task = find_task(board, task_id);
        json_t *target = json_object();
        json_object_set_new(target, "task", json_string(task_id));
        json_object_set_new(target, "previous_state",
                            state_or_null(had_previous, previous));
        json_object_set_new(
            target, "new_state",
            state_or_null(task != NULL,
                          task ? presentation_state(board, task)
                               : DISPATCH_STATE_PROPOSED));
        json_array_append_new(targets, target);
    }

    const char *task_id = single_target(result);
    json_t *single = task_id ? json_array_get(targets, 0) : NULL;
    json_object_set_new(object, "task", optional_string(task_id));
    json_object_set(object, "previous_state",
                    single ? json_object_get(single, "previous_state")
                           : json_null());
    json_object_set(object, "new_state",
                    single ? json_object_get(single, "new_state")
                           : json_null());
    json_object_set_new(object, "targets", targets);

    json_t *newly_ready = json_array();
    for (size_t i = 0; i < board->tasks.count; i++) {
        const DispatchTask *task = &board->tasks.items[i];
        if (presentation_state(board, task) != DISPATCH_STATE_READY)
            continue;
        DispatchState previous = DISPATCH_STATE_PROPOSED;
        if (snapshot_state(before, task->id, &previous) &&
            previous == DISPATCH_STATE_READY)
            continue;
        json_array_append_new(newly_ready, json_string(task->id));
    }
    json_object_set_new(object, "newly_ready", newly_ready);

    const char *key;
    json_object_foreach(result->extras, key, value)
        json_object_set(object, key, value);
    return object;
}

int dispatch_json_emit_mutation(FILE *stream, const DispatchBoard *board,
                                const char *command, const char *group,
                                const DispatchJsonSnapshot *before,
                                const DispatchJsonResult *result,
                                const DispatchJsonWarning *warnings,
                                size_t warning_count) {
    json_t *tasks = json_array();
    size_t returned = 0;
    size_t index;
    json_t *value;
    json_array_foreach(result->targets, index, value) {
        const DispatchTask *task = find_task(board, json_string_value(value));
        if (!task)
            continue;
        json_array_append_new(tasks, task_json(board, task));
        returned++;
    }

    DispatchJsonRequest request = {
        .command = command,
        .task_id = single_target(result),
        .group = group,
        .include_done = -1,
    };

    json_t *board_json = json_object();
    json_object_set_new(board_json, "name", json_string(board->name));
    json_object_set_new(board_json, "repo_path",
                        json_string(board->repo_path ? board->repo_path : "."));
    json_object_set_new(board_json, "groups", groups_json(board));

    json_t *root = json_object();
    json_object_set_new(root, "schema_version", json_integer(1));
    json_object_set_new(root, "command", json_string(command));
    json_object_set_new(root, "board", board_json);
    json_object_set_new(root, "query", query_json(&request));
    json_object_set_new(root, "summary", summary_json(board, returned));
    json_object_set_new(root, "tasks", tasks);
    json_object_set_new(root, "warnings",
                        extra_warnings_json(warnings, warning_count));
    json_object_set_new(root, "result", result_json(board, before, result));
    return dump_document(stream, root);
}

int dispatch_json_emit_workspace_mutation(
    FILE *stream, const DispatchBoard *board, const char *command,
    const DispatchWorkspace *records, size_t record_count,
    const DispatchJsonSnapshot *before, const DispatchJsonResult *result,
    const DispatchJsonWarning *warnings, size_t warning_count) {
    json_t *workspaces = json_array();
    for (size_t i = 0; i < record_count; i++)
        json_array_append_new(workspaces,
                              workspace_record_json(board, &records[i]));

    json_t *root = workspace_envelope_json(
        board, command, workspaces, record_count == 1 ? records[0].id : NULL);
    json_object_set_new(root, "warnings",
                        extra_warnings_json(warnings, warning_count));
    json_object_set_new(root, "result", result_json(board, before, result));
    return dump_document(stream, root);
}

int dispatch_json_emit_agent_mutation(
    FILE *stream, const DispatchBoard *board, const char *command,
    const DispatchAgent *agent, const DispatchJsonSnapshot *before,
    const DispatchJsonResult *result, const DispatchJsonWarning *warnings,
    size_t warning_count) {
    json_t *root = agent_envelope_json(board, command, agent, 0);
    json_object_set_new(root, "warnings",
                        extra_warnings_json(warnings, warning_count));
    json_object_set_new(root, "result", result_json(board, before, result));
    return dump_document(stream, root);
}

int dispatch_json_emit_error(FILE *stream, const char *command,
                             const char *code, const char *message,
                             const char *task_id) {
    json_t *error = json_object();
    json_object_set_new(error, "code", json_string(code));
    /* Messages echo user input, which may not be valid UTF-8. */
    json_t *message_json = json_string(message ? message : "");
    json_object_set_new(error, "message",
                        message_json ? message_json
                                     : json_string("(message is not valid UTF-8)"));
    json_t *task_json_value = task_id ? json_string(task_id) : NULL;
    json_object_set_new(error, "task_id",
                        task_json_value ? task_json_value : json_null());

    json_t *root = json_object();
    json_object_set_new(root, "schema_version", json_integer(1));
    json_object_set_new(root, "command", json_string(command));
    json_object_set_new(root, "error", error);
    return dump_document(stream, root);
}
