#include "dispatch_store.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *message) {
    if (error && error_size > 0)
        snprintf(error, error_size, "%s", message);
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

static json_t *task_to_json(const DispatchTask *task) {
    json_t *object = json_object();
    json_object_set_new(object, "id", json_string(task->id));
    json_object_set_new(object, "title", json_string(task->title));
    json_object_set_new(object, "description", json_string(task->description));
    json_object_set_new(object, "group", json_string(task->group));
    json_object_set_new(object, "state",
                        json_string(dispatch_state_name(task->state)));

    json_t *depends_on = json_array();
    for (size_t i = 0; i < task->depends_on.count; i++)
        json_array_append_new(depends_on,
                              json_string(task->depends_on.items[i]));
    json_object_set_new(object, "depends_on", depends_on);

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
