#include "dispatch_store.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *message) {
    if (error && error_size > 0)
        snprintf(error, error_size, "%s", message);
}

static void set_error_errno(char *error, size_t error_size,
                            const char *message) {
    if (error && error_size > 0)
        snprintf(error, error_size, "%s: %s", message, strerror(errno));
}

static char *store_strdup(const char *value) {
    return strdup(value ? value : "");
}

static void *store_realloc_array(void *items, size_t count, size_t item_size) {
    void *new_items = realloc(items, count * item_size);
    if (!new_items) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return new_items;
}

static int file_exists(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file)
        return 0;
    fclose(file);
    return 1;
}

static char *lock_path_for_store(const char *path) {
    size_t path_len = strlen(path);
    const char *suffix = ".lock";
    size_t suffix_len = strlen(suffix);
    char *lock_path = malloc(path_len + suffix_len + 1);
    if (!lock_path) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(lock_path, path, path_len);
    memcpy(lock_path + path_len, suffix, suffix_len + 1);
    return lock_path;
}

int dispatch_store_lock_acquire(DispatchStoreLock *lock, const char *path,
                                int timeout_ms, char *error,
                                size_t error_size) {
    if (!lock || !path || path[0] == '\0') {
        set_error(error, error_size, "invalid lock path");
        return 0;
    }

    memset(lock, 0, sizeof(*lock));
    lock->fd = -1;
    lock->path = lock_path_for_store(path);

    const int sleep_ms = 10;
    int waited_ms = 0;
    for (;;) {
        int fd = open(lock->path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0) {
            char buffer[64];
            int length = snprintf(buffer, sizeof(buffer), "%ld\n",
                                  (long)getpid());
            if (length > 0)
                (void)write(fd, buffer, (size_t)length);
            lock->fd = fd;
            lock->acquired = 1;
            return 1;
        }

        if (errno != EEXIST) {
            set_error_errno(error, error_size, "could not acquire board lock");
            free(lock->path);
            memset(lock, 0, sizeof(*lock));
            lock->fd = -1;
            return 0;
        }

        if (waited_ms >= timeout_ms) {
            set_error(error, error_size,
                      "Dispatch board is locked by another process; retry shortly.");
            free(lock->path);
            memset(lock, 0, sizeof(*lock));
            lock->fd = -1;
            return 0;
        }

        usleep((useconds_t)sleep_ms * 1000);
        waited_ms += sleep_ms;
    }
}

void dispatch_store_lock_release(DispatchStoreLock *lock) {
    if (!lock || !lock->acquired)
        return;

    if (lock->fd >= 0)
        close(lock->fd);
    if (lock->path)
        unlink(lock->path);
    free(lock->path);
    memset(lock, 0, sizeof(*lock));
    lock->fd = -1;
}

static json_t *log_fields_to_json(const DispatchLogField *fields,
                                  size_t field_count) {
    json_t *object = json_object();
    if (!object)
        return NULL;

    for (size_t i = 0; i < field_count; i++) {
        const DispatchLogField *field = &fields[i];
        if (!field->key || field->key[0] == '\0' || !field->value)
            continue;
        json_object_set_new(object, field->key, json_string(field->value));
    }
    return object;
}

static int utc_timestamp(char *buffer, size_t buffer_size) {
    time_t now = time(NULL);
    struct tm utc;
    if (gmtime_r(&now, &utc) == NULL)
        return 0;
    return strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%SZ", &utc) > 0;
}

int dispatch_store_log_append(const char *path, const DispatchLogRecord *record,
                              char *error, size_t error_size) {
    if (!path || path[0] == '\0' || !record) {
        set_error(error, error_size, "invalid log record");
        return 0;
    }

    char timestamp[32] = {0};
    if (!utc_timestamp(timestamp, sizeof(timestamp))) {
        set_error(error, error_size, "could not format log timestamp");
        return 0;
    }

    json_t *entry = json_object();
    if (!entry) {
        set_error(error, error_size, "could not create log entry");
        return 0;
    }

    json_object_set_new(entry, "version", json_integer(1));
    json_object_set_new(entry, "timestamp", json_string(timestamp));
    json_object_set_new(entry, "actor",
                        json_string(record->actor ? record->actor : "unknown"));
    json_object_set_new(entry, "command",
                        json_string(record->command ? record->command : ""));
    json_object_set_new(entry, "action",
                        json_string(record->action ? record->action : ""));
    json_object_set_new(entry, "outcome",
                        json_string(record->outcome ? record->outcome : ""));
    json_object_set_new(entry, "message",
                        json_string(record->message ? record->message : ""));

    json_t *targets =
        log_fields_to_json(record->targets, record->target_count);
    if (!targets) {
        json_decref(entry);
        set_error(error, error_size, "could not create log targets");
        return 0;
    }
    json_object_set_new(entry, "targets", targets);

    if (record->context_count > 0) {
        json_t *context =
            log_fields_to_json(record->context, record->context_count);
        if (!context) {
            json_decref(entry);
            set_error(error, error_size, "could not create log context");
            return 0;
        }
        json_object_set_new(entry, "context", context);
    }

    FILE *file = fopen(path, "a");
    if (!file) {
        json_decref(entry);
        set_error_errno(error, error_size, "could not open dispatch log");
        return 0;
    }

    int ok = json_dumpf(entry, file, JSON_COMPACT) == 0 &&
             fputc('\n', file) != EOF && fclose(file) == 0;
    json_decref(entry);
    if (!ok) {
        set_error_errno(error, error_size, "could not append dispatch log");
        return 0;
    }
    return 1;
}

static time_t json_time_or_zero(json_t *object, const char *key) {
    json_t *value = json_object_get(object, key);
    if (json_is_integer(value))
        return (time_t)json_integer_value(value);
    return 0;
}

static json_t *json_time_or_null(time_t value) {
    if (value == (time_t)0)
        return json_null();
    return json_integer((json_int_t)value);
}

static json_t *json_optional_string(const char *value) {
    return value ? json_string(value) : json_null();
}

static const char *json_string_or(json_t *object, const char *key,
                                  const char *fallback) {
    json_t *value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : fallback;
}

static void string_list_append(DispatchStringList *list, const char *value) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = store_realloc_array(list->items, list->capacity,
                                          sizeof(*list->items));
    }
    list->items[list->count++] = store_strdup(value);
}

static void history_append_loaded(DispatchHistory *history, const char *actor,
                                  const char *action, const char *note,
                                  time_t timestamp) {
    if (history->count >= history->capacity) {
        history->capacity = history->capacity == 0 ? 4 : history->capacity * 2;
        history->items = store_realloc_array(history->items, history->capacity,
                                             sizeof(*history->items));
    }

    DispatchHistoryEntry *entry = &history->items[history->count++];
    entry->actor = store_strdup(actor);
    entry->action = store_strdup(action);
    entry->note = store_strdup(note);
    entry->timestamp = timestamp;
}

static void task_free_fields(DispatchTask *task) {
    free(task->id);
    free(task->title);
    free(task->description);
    free(task->group);
    for (size_t i = 0; i < task->depends_on.count; i++)
        free(task->depends_on.items[i]);
    free(task->depends_on.items);
    for (size_t i = 0; i < task->commits.count; i++)
        free(task->commits.items[i]);
    free(task->commits.items);
    free(task->assigned_to);
    free(task->started_by);
    free(task->completed_by);
    for (size_t i = 0; i < task->history.count; i++) {
        free(task->history.items[i].actor);
        free(task->history.items[i].action);
        free(task->history.items[i].note);
    }
    free(task->history.items);
}

static void workspace_free_fields(DispatchWorkspace *workspace) {
    free(workspace->id);
    free(workspace->task_id);
    free(workspace->actor);
    free(workspace->path);
    free(workspace->branch);
    free(workspace->repo_path);
    for (size_t i = 0; i < workspace->sequence_tasks.count; i++)
        free(workspace->sequence_tasks.items[i]);
    free(workspace->sequence_tasks.items);
    free(workspace->review_gate);
}

static int append_loaded_task(DispatchBoard *board, const DispatchTask *task) {
    if (board->tasks.count >= board->tasks.capacity) {
        board->tasks.capacity = board->tasks.capacity == 0
                                    ? 8
                                    : board->tasks.capacity * 2;
        board->tasks.items = store_realloc_array(board->tasks.items,
                                                 board->tasks.capacity,
                                                 sizeof(*board->tasks.items));
    }
    board->tasks.items[board->tasks.count++] = *task;
    return 1;
}

static int append_loaded_agent(DispatchBoard *board,
                               const DispatchAgent *agent) {
    if (board->agents.count >= board->agents.capacity) {
        board->agents.capacity = board->agents.capacity == 0
                                     ? 4
                                     : board->agents.capacity * 2;
        board->agents.items = store_realloc_array(
            board->agents.items, board->agents.capacity,
            sizeof(*board->agents.items));
    }
    board->agents.items[board->agents.count++] = *agent;
    return 1;
}

static int append_loaded_workspace(DispatchBoard *board,
                                   const DispatchWorkspace *workspace) {
    if (board->workspaces.count >= board->workspaces.capacity) {
        board->workspaces.capacity = board->workspaces.capacity == 0
                                         ? 4
                                         : board->workspaces.capacity * 2;
        board->workspaces.items = store_realloc_array(
            board->workspaces.items, board->workspaces.capacity,
            sizeof(*board->workspaces.items));
    }
    board->workspaces.items[board->workspaces.count++] = *workspace;
    return 1;
}

static json_t *group_to_json(const DispatchGroup *group) {
    json_t *object = json_object();
    json_object_set_new(object, "id", json_string(group->id));
    json_object_set_new(object, "name", json_string(group->name));
    json_object_set_new(object, "prefix", json_string(group->prefix));
    return object;
}

static json_t *history_to_json(const DispatchHistory *history) {
    json_t *array = json_array();
    for (size_t i = 0; i < history->count; i++) {
        const DispatchHistoryEntry *entry = &history->items[i];
        json_t *object = json_object();
        json_object_set_new(object, "actor", json_string(entry->actor));
        json_object_set_new(object, "action", json_string(entry->action));
        json_object_set_new(object, "note", json_string(entry->note));
        json_object_set_new(object, "timestamp",
                            json_time_or_null(entry->timestamp));
        json_array_append_new(array, object);
    }
    return array;
}

static json_t *string_list_to_json(const DispatchStringList *list) {
    json_t *array = json_array();
    for (size_t i = 0; i < list->count; i++)
        json_array_append_new(array, json_string(list->items[i]));
    return array;
}

static json_t *task_to_json(const DispatchTask *task) {
    json_t *object = json_object();
    json_object_set_new(object, "id", json_string(task->id));
    json_object_set_new(object, "title", json_string(task->title));
    json_object_set_new(object, "description", json_string(task->description));
    json_object_set_new(object, "group", json_string(task->group));
    json_object_set_new(object, "state",
                        json_string(dispatch_state_name(task->state)));

    json_object_set_new(object, "depends_on",
                        string_list_to_json(&task->depends_on));
    json_object_set_new(object, "commits", string_list_to_json(&task->commits));

    json_object_set_new(object, "requires_review",
                        json_boolean(task->requires_review));
    json_object_set_new(object, "assigned_to",
                        task->assigned_to ? json_string(task->assigned_to)
                                          : json_null());
    json_object_set_new(object, "started_by",
                        task->started_by ? json_string(task->started_by)
                                         : json_null());
    json_object_set_new(object, "completed_by",
                        task->completed_by ? json_string(task->completed_by)
                                           : json_null());
    json_object_set_new(object, "created_at", json_time_or_null(task->created_at));
    json_object_set_new(object, "started_at", json_time_or_null(task->started_at));
    json_object_set_new(object, "completed_at",
                        json_time_or_null(task->completed_at));
    json_object_set_new(object, "updated_at", json_time_or_null(task->updated_at));
    json_object_set_new(object, "history", history_to_json(&task->history));
    return object;
}

static json_t *agent_to_json(const DispatchAgent *agent) {
    json_t *object = json_object();
    json_object_set_new(object, "name", json_string(agent->name));
    json_object_set_new(object, "runner", json_string(agent->runner));
    json_object_set_new(object, "model", json_optional_string(agent->model));
    json_object_set_new(object, "agent_dir", json_string(agent->agent_dir));
    json_object_set_new(object, "prompt_path", json_string(agent->prompt_path));
    json_object_set_new(object, "run_script_path",
                        json_optional_string(agent->run_script_path));
    json_object_set_new(object, "session_id",
                        json_optional_string(agent->session_id));
    json_object_set_new(object, "current_task",
                        json_optional_string(agent->current_task));
    json_object_set_new(object, "last_workspace",
                        json_optional_string(agent->last_workspace));
    json_object_set_new(object, "created_at",
                        json_time_or_null(agent->created_at));
    json_object_set_new(object, "updated_at",
                        json_time_or_null(agent->updated_at));
    return object;
}

static json_t *workspace_to_json(const DispatchWorkspace *workspace) {
    json_t *object = json_object();
    json_object_set_new(object, "id", json_string(workspace->id));
    json_object_set_new(object, "task_id", json_string(workspace->task_id));
    json_object_set_new(object, "actor", json_string(workspace->actor));
    json_object_set_new(object, "path", json_string(workspace->path));
    json_object_set_new(object, "branch", json_string(workspace->branch));
    json_object_set_new(object, "repo_path", json_string(workspace->repo_path));
    json_object_set_new(
        object, "state",
        json_string(dispatch_workspace_state_name(workspace->state)));
    json_object_set_new(object, "sequence_tasks",
                        string_list_to_json(&workspace->sequence_tasks));
    json_object_set_new(object, "review_gate",
                        json_optional_string(workspace->review_gate));
    json_object_set_new(object, "created_at",
                        json_time_or_null(workspace->created_at));
    json_object_set_new(object, "updated_at",
                        json_time_or_null(workspace->updated_at));
    return object;
}

int dispatch_store_save(const DispatchBoard *board, const char *path,
                        char *error, size_t error_size) {
    json_t *root = json_object();
    json_object_set_new(root, "version", json_integer(board->version));

    json_t *workspace = json_object();
    json_object_set_new(workspace, "repo_path",
                        json_string(board->repo_path ? board->repo_path : "."));
    json_object_set_new(root, "workspace", workspace);

    json_t *json_board = json_object();
    json_object_set_new(json_board, "name", json_string(board->name));

    json_t *groups = json_array();
    for (size_t i = 0; i < board->groups.count; i++)
        json_array_append_new(groups, group_to_json(&board->groups.items[i]));
    json_object_set_new(json_board, "groups", groups);

    json_t *tasks = json_array();
    for (size_t i = 0; i < board->tasks.count; i++)
        json_array_append_new(tasks, task_to_json(&board->tasks.items[i]));
    json_object_set_new(json_board, "tasks", tasks);

    json_t *agents = json_array();
    for (size_t i = 0; i < board->agents.count; i++)
        json_array_append_new(agents, agent_to_json(&board->agents.items[i]));
    json_object_set_new(json_board, "agents", agents);

    json_t *workspaces = json_array();
    for (size_t i = 0; i < board->workspaces.count; i++) {
        json_array_append_new(
            workspaces, workspace_to_json(&board->workspaces.items[i]));
    }
    json_object_set_new(json_board, "workspaces", workspaces);

    json_object_set_new(root, "board", json_board);

    if (json_dump_file(root, path, JSON_INDENT(2)) != 0) {
        json_decref(root);
        set_error(error, error_size, "could not write Dispatch storage");
        return 0;
    }

    json_decref(root);
    return 1;
}

static int load_groups(DispatchBoard *board, json_t *groups, char *error,
                       size_t error_size) {
    if (!json_is_array(groups)) {
        set_error(error, error_size, "board.groups must be an array");
        return 0;
    }

    size_t index;
    json_t *value;
    json_array_foreach(groups, index, value) {
        if (!json_is_object(value)) {
            set_error(error, error_size, "group entries must be objects");
            return 0;
        }
        const char *name = json_string_or(value, "name", NULL);
        const char *prefix = json_string_or(value, "prefix", NULL);
        if (!name || !prefix || !dispatch_board_add_group(board, name, prefix)) {
            set_error(error, error_size, "invalid or duplicate group");
            return 0;
        }
    }
    return 1;
}

static int load_history(DispatchHistory *history, json_t *array, char *error,
                        size_t error_size) {
    if (!json_is_array(array))
        return 1;

    size_t index;
    json_t *value;
    json_array_foreach(array, index, value) {
        if (!json_is_object(value)) {
            set_error(error, error_size, "history entries must be objects");
            return 0;
        }
        history_append_loaded(history, json_string_or(value, "actor", "unknown"),
                              json_string_or(value, "action", "unknown"),
                              json_string_or(value, "note", ""),
                              json_time_or_zero(value, "timestamp"));
    }
    return 1;
}

static int load_tasks(DispatchBoard *board, json_t *tasks, char *error,
                      size_t error_size) {
    if (!json_is_array(tasks)) {
        set_error(error, error_size, "board.tasks must be an array");
        return 0;
    }

    size_t index;
    json_t *value;
    json_array_foreach(tasks, index, value) {
        if (!json_is_object(value)) {
            set_error(error, error_size, "task entries must be objects");
            return 0;
        }

        const char *id = json_string_or(value, "id", NULL);
        const char *title = json_string_or(value, "title", NULL);
        const char *group = json_string_or(value, "group", NULL);
        if (!id || !title || !group || dispatch_board_find_task(board, id) ||
            !dispatch_board_find_group(board, group)) {
            set_error(error, error_size, "invalid task identity");
            return 0;
        }

        DispatchTask task = {0};
        task.id = store_strdup(id);
        task.title = store_strdup(title);
        task.description =
            store_strdup(json_string_or(value, "description", ""));
        task.group = store_strdup(group);

        DispatchState state;
        if (!dispatch_state_from_name(json_string_or(value, "state", "ready"),
                                      &state)) {
            set_error(error, error_size, "invalid task state");
            task_free_fields(&task);
            return 0;
        }
        task.state = state;

        json_t *depends_on = json_object_get(value, "depends_on");
        if (json_is_array(depends_on)) {
            size_t dep_index;
            json_t *dep;
            json_array_foreach(depends_on, dep_index, dep) {
                if (!json_is_string(dep)) {
                    set_error(error, error_size,
                              "depends_on entries must be task IDs");
                    task_free_fields(&task);
                    return 0;
                }
                string_list_append(&task.depends_on, json_string_value(dep));
            }
        }

        json_t *commits = json_object_get(value, "commits");
        if (json_is_array(commits)) {
            size_t commit_index;
            json_t *commit;
            json_array_foreach(commits, commit_index, commit) {
                if (!json_is_string(commit)) {
                    set_error(error, error_size,
                              "commit entries must be strings");
                    task_free_fields(&task);
                    return 0;
                }
                string_list_append(&task.commits, json_string_value(commit));
            }
        }

        json_t *requires_review = json_object_get(value, "requires_review");
        task.requires_review = json_is_boolean(requires_review)
                                   ? json_boolean_value(requires_review)
                                   : 1;

        const char *assigned_to = json_string_or(value, "assigned_to", NULL);
        const char *started_by = json_string_or(value, "started_by", NULL);
        const char *completed_by = json_string_or(value, "completed_by", NULL);
        task.assigned_to = assigned_to ? store_strdup(assigned_to) : NULL;
        task.started_by = started_by ? store_strdup(started_by) : NULL;
        task.completed_by = completed_by ? store_strdup(completed_by) : NULL;
        task.created_at = json_time_or_zero(value, "created_at");
        task.started_at = json_time_or_zero(value, "started_at");
        task.completed_at = json_time_or_zero(value, "completed_at");
        task.updated_at = json_time_or_zero(value, "updated_at");

        if (!load_history(&task.history, json_object_get(value, "history"),
                          error, error_size)) {
            task_free_fields(&task);
            return 0;
        }

        append_loaded_task(board, &task);
    }

    return 1;
}

static int load_agents(DispatchBoard *board, json_t *agents, char *error,
                       size_t error_size) {
    if (!agents)
        return 1;
    if (!json_is_array(agents)) {
        set_error(error, error_size, "board.agents must be an array");
        return 0;
    }

    size_t index;
    json_t *value;
    json_array_foreach(agents, index, value) {
        if (!json_is_object(value)) {
            set_error(error, error_size, "agent entries must be objects");
            return 0;
        }

        const char *name = json_string_or(value, "name", NULL);
        const char *runner = json_string_or(value, "runner", NULL);
        const char *agent_dir = json_string_or(value, "agent_dir", NULL);
        const char *prompt_path = json_string_or(value, "prompt_path", NULL);
        if (!name || !runner || !agent_dir || !prompt_path ||
            dispatch_board_find_agent(board, name)) {
            set_error(error, error_size, "invalid or duplicate agent record");
            return 0;
        }

        const char *model = json_string_or(value, "model", NULL);
        const char *run_script_path =
            json_string_or(value, "run_script_path", NULL);
        const char *session_id = json_string_or(value, "session_id", NULL);
        const char *current_task = json_string_or(value, "current_task", NULL);
        const char *last_workspace =
            json_string_or(value, "last_workspace", NULL);

        DispatchAgent agent = {0};
        agent.name = store_strdup(name);
        agent.runner = store_strdup(runner);
        agent.model = model ? store_strdup(model) : NULL;
        agent.agent_dir = store_strdup(agent_dir);
        agent.prompt_path = store_strdup(prompt_path);
        agent.run_script_path =
            run_script_path ? store_strdup(run_script_path) : NULL;
        agent.session_id = session_id ? store_strdup(session_id) : NULL;
        agent.current_task = current_task ? store_strdup(current_task) : NULL;
        agent.last_workspace =
            last_workspace ? store_strdup(last_workspace) : NULL;
        agent.created_at = json_time_or_zero(value, "created_at");
        agent.updated_at = json_time_or_zero(value, "updated_at");

        append_loaded_agent(board, &agent);
    }

    return 1;
}

static int load_workspace_sequence(DispatchWorkspace *workspace,
                                   DispatchBoard *board, json_t *array,
                                   char *error, size_t error_size) {
    if (!array)
        return 1;
    if (!json_is_array(array)) {
        set_error(error, error_size, "workspace.sequence_tasks must be an array");
        return 0;
    }

    size_t index;
    json_t *value;
    json_array_foreach(array, index, value) {
        if (!json_is_string(value)) {
            set_error(error, error_size,
                      "workspace.sequence_tasks entries must be task IDs");
            return 0;
        }
        const char *task_id = json_string_value(value);
        if (!dispatch_board_find_task(board, task_id)) {
            set_error(error, error_size,
                      "workspace.sequence_tasks references a missing task");
            return 0;
        }
        string_list_append(&workspace->sequence_tasks, task_id);
    }
    return 1;
}

static int load_workspaces(DispatchBoard *board, json_t *workspaces,
                           char *error, size_t error_size) {
    if (!workspaces)
        return 1;
    if (!json_is_array(workspaces)) {
        set_error(error, error_size, "board.workspaces must be an array");
        return 0;
    }

    size_t index;
    json_t *value;
    json_array_foreach(workspaces, index, value) {
        if (!json_is_object(value)) {
            set_error(error, error_size, "workspace entries must be objects");
            return 0;
        }

        const char *id = json_string_or(value, "id", NULL);
        const char *task_id = json_string_or(value, "task_id", NULL);
        const char *actor = json_string_or(value, "actor", NULL);
        const char *path = json_string_or(value, "path", NULL);
        const char *branch = json_string_or(value, "branch", NULL);
        const char *repo_path = json_string_or(value, "repo_path", NULL);
        if (!id || !task_id || !actor || !path || !branch || !repo_path ||
            !dispatch_board_find_task(board, task_id) ||
            dispatch_board_find_workspace(board, id) ||
            dispatch_board_find_workspace(board, task_id)) {
            set_error(error, error_size, "invalid or duplicate workspace record");
            return 0;
        }

        DispatchWorkspace workspace = {0};
        workspace.id = store_strdup(id);
        workspace.task_id = store_strdup(task_id);
        workspace.actor = store_strdup(actor);
        workspace.path = store_strdup(path);
        workspace.branch = store_strdup(branch);
        workspace.repo_path = store_strdup(repo_path);

        if (!dispatch_workspace_state_from_name(
                json_string_or(value, "state", "creating"),
                &workspace.state)) {
            set_error(error, error_size, "invalid workspace state");
            workspace_free_fields(&workspace);
            return 0;
        }

        if (!load_workspace_sequence(
                &workspace, board, json_object_get(value, "sequence_tasks"),
                error, error_size)) {
            workspace_free_fields(&workspace);
            return 0;
        }

        const char *review_gate = json_string_or(value, "review_gate", NULL);
        workspace.review_gate = review_gate ? store_strdup(review_gate) : NULL;
        workspace.created_at = json_time_or_zero(value, "created_at");
        workspace.updated_at = json_time_or_zero(value, "updated_at");

        append_loaded_workspace(board, &workspace);
    }

    return 1;
}

static int validate_dependencies(DispatchBoard *board, char *error,
                                 size_t error_size) {
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        for (size_t dep = 0; dep < task->depends_on.count; dep++) {
            if (!dispatch_board_find_task(board, task->depends_on.items[dep])) {
                set_error(error, error_size,
                          "task depends_on references a missing task");
                return 0;
            }
        }
    }
    return 1;
}

int dispatch_store_load(DispatchBoard *board, const char *path, char *error,
                        size_t error_size) {
    json_error_t json_error;
    json_t *root = json_load_file(path, 0, &json_error);
    if (!root) {
        set_error(error, error_size, json_error.text);
        return 0;
    }

    json_t *version = json_object_get(root, "version");
    if (!json_is_object(root) || !json_is_integer(version) ||
        json_integer_value(version) != 1) {
        json_decref(root);
        set_error(error, error_size, "unsupported Dispatch storage version");
        return 0;
    }

    json_t *json_board = json_object_get(root, "board");
    if (!json_is_object(json_board)) {
        json_decref(root);
        set_error(error, error_size, "board must be an object");
        return 0;
    }

    dispatch_board_init(board, json_string_or(json_board, "name", "Dispatch"));

    json_t *workspace = json_object_get(root, "workspace");
    if (json_is_object(workspace)) {
        dispatch_board_set_repo_path(
            board, json_string_or(workspace, "repo_path", "."));
    }

    if (!load_groups(board, json_object_get(json_board, "groups"), error,
                     error_size) ||
        !load_tasks(board, json_object_get(json_board, "tasks"), error,
                    error_size) ||
        !load_agents(board, json_object_get(json_board, "agents"), error,
                     error_size) ||
        !load_workspaces(board, json_object_get(json_board, "workspaces"),
                         error, error_size) ||
        !validate_dependencies(board, error, error_size)) {
        dispatch_board_free(board);
        json_decref(root);
        return 0;
    }

    json_decref(root);
    dispatch_board_normalize_states(board);
    return 1;
}

int dispatch_store_init_file(const char *path, const char *repo_path,
                             char *error, size_t error_size) {
    if (file_exists(path)) {
        DispatchBoard board;
        if (!dispatch_store_load(&board, path, error, error_size))
            return 0;
        dispatch_board_free(&board);
        return 1;
    }

    DispatchBoard board;
    dispatch_board_init(&board, "Dispatch");
    dispatch_board_set_repo_path(&board, repo_path);
    int saved = dispatch_store_save(&board, path, error, error_size);
    dispatch_board_free(&board);
    return saved;
}
