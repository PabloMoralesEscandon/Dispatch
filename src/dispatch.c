#include "dispatch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dispatch_strdup(const char *value) {
    return strdup(value ? value : "");
}

static void *dispatch_realloc_array(void *items, size_t count,
                                    size_t item_size) {
    void *new_items = realloc(items, count * item_size);
    if (!new_items) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return new_items;
}

static void string_list_append(DispatchStringList *list, const char *value) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = dispatch_realloc_array(list->items, list->capacity,
                                             sizeof(*list->items));
    }
    list->items[list->count++] = dispatch_strdup(value);
}

static void replace_string(char **target, const char *value) {
    char *copy = dispatch_strdup(value);
    free(*target);
    *target = copy;
}

static int string_list_contains(const DispatchStringList *list,
                                const char *value) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], value) == 0)
            return 1;
    }
    return 0;
}

static void string_list_free(DispatchStringList *list) {
    for (size_t i = 0; i < list->count; i++)
        free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void history_free(DispatchHistory *history) {
    for (size_t i = 0; i < history->count; i++) {
        free(history->items[i].actor);
        free(history->items[i].action);
        free(history->items[i].note);
    }
    free(history->items);
    history->items = NULL;
    history->count = 0;
    history->capacity = 0;
}

int dispatch_task_append_history(DispatchTask *task, const char *actor,
                                 const char *action, const char *note) {
    if (!task || !action || action[0] == '\0')
        return 0;

    if (task->history.count >= task->history.capacity) {
        task->history.capacity = task->history.capacity == 0
                                     ? 4
                                     : task->history.capacity * 2;
        task->history.items = dispatch_realloc_array(
            task->history.items, task->history.capacity,
            sizeof(*task->history.items));
    }

    DispatchHistoryEntry *entry = &task->history.items[task->history.count++];
    entry->actor = dispatch_strdup(actor && actor[0] ? actor : "unknown");
    entry->action = dispatch_strdup(action);
    entry->note = dispatch_strdup(note ? note : "");
    entry->timestamp = time(NULL);
    task->updated_at = entry->timestamp;
    return 1;
}

static char *prefix_from_name(const char *name) {
    char buffer[4] = {0};
    size_t out = 0;
    int at_word_start = 1;

    for (size_t i = 0; name && name[i] != '\0' && out < 3; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c)) {
            if (at_word_start || out == 0)
                buffer[out++] = (char)toupper(c);
            at_word_start = 0;
        } else {
            at_word_start = 1;
        }
    }

    if (out == 1 && name) {
        for (size_t i = 0; name[i] != '\0' && out < 2; i++) {
            unsigned char c = (unsigned char)name[i];
            if (isalnum(c) && (char)toupper(c) != buffer[0])
                buffer[out++] = (char)toupper(c);
        }
    }

    if (out == 0) {
        buffer[0] = 'G';
        buffer[1] = '\0';
    }

    return dispatch_strdup(buffer);
}

void dispatch_board_init(DispatchBoard *board, const char *name) {
    memset(board, 0, sizeof(*board));
    board->version = 1;
    board->name = dispatch_strdup(name && name[0] ? name : "Dispatch");
}

void dispatch_board_free(DispatchBoard *board) {
    if (!board)
        return;

    for (size_t i = 0; i < board->groups.count; i++) {
        free(board->groups.items[i].id);
        free(board->groups.items[i].name);
        free(board->groups.items[i].prefix);
    }
    free(board->groups.items);

    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        free(task->id);
        free(task->title);
        free(task->description);
        free(task->group);
        string_list_free(&task->depends_on);
        free(task->assigned_to);
        free(task->started_by);
        free(task->completed_by);
        history_free(&task->history);
    }
    free(board->tasks.items);
    free(board->name);
    memset(board, 0, sizeof(*board));
}

DispatchGroup *dispatch_board_find_group(DispatchBoard *board,
                                         const char *group_id) {
    if (!board || !group_id)
        return NULL;
    for (size_t i = 0; i < board->groups.count; i++) {
        DispatchGroup *group = &board->groups.items[i];
        if (strcmp(group->id, group_id) == 0 ||
            strcmp(group->prefix, group_id) == 0 ||
            strcmp(group->name, group_id) == 0) {
            return group;
        }
    }
    return NULL;
}

DispatchTask *dispatch_board_find_task(DispatchBoard *board,
                                       const char *task_id) {
    if (!board || !task_id)
        return NULL;
    for (size_t i = 0; i < board->tasks.count; i++) {
        if (strcmp(board->tasks.items[i].id, task_id) == 0)
            return &board->tasks.items[i];
    }
    return NULL;
}

int dispatch_board_add_group(DispatchBoard *board, const char *name,
                             const char *prefix) {
    if (!board || !name || name[0] == '\0')
        return 0;

    char *resolved_prefix = prefix && prefix[0] ? dispatch_strdup(prefix)
                                                : prefix_from_name(name);
    for (char *p = resolved_prefix; *p; p++)
        *p = (char)toupper((unsigned char)*p);

    if (!dispatch_group_prefix_is_valid(resolved_prefix) ||
        dispatch_board_find_group(board, resolved_prefix)) {
        free(resolved_prefix);
        return 0;
    }

    if (board->groups.count >= board->groups.capacity) {
        board->groups.capacity = board->groups.capacity == 0
                                     ? 4
                                     : board->groups.capacity * 2;
        board->groups.items = dispatch_realloc_array(
            board->groups.items, board->groups.capacity,
            sizeof(*board->groups.items));
    }

    DispatchGroup *group = &board->groups.items[board->groups.count++];
    group->id = dispatch_strdup(resolved_prefix);
    group->name = dispatch_strdup(name);
    group->prefix = resolved_prefix;
    return 1;
}

int dispatch_group_prefix_is_valid(const char *prefix) {
    size_t len = prefix ? strlen(prefix) : 0;
    if (len < 1 || len > 3)
        return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)prefix[i]))
            return 0;
    }
    return 1;
}

char *dispatch_next_task_id(const DispatchBoard *board, const char *group_id) {
    const char *prefix = group_id && group_id[0] ? group_id : "G";
    int next = 1;

    for (size_t i = 0; board && i < board->tasks.count; i++) {
        const char *id = board->tasks.items[i].id;
        size_t prefix_len = strlen(prefix);
        if (strncmp(id, prefix, prefix_len) == 0 && id[prefix_len] == '-') {
            int number = atoi(id + prefix_len + 1);
            if (number >= next)
                next = number + 1;
        }
    }

    size_t size = strlen(prefix) + 8;
    char *id = malloc(size);
    if (!id) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(id, size, "%s-%02d", prefix, next);
    return id;
}

DispatchTask *dispatch_board_add_task(DispatchBoard *board,
                                      const char *group_id,
                                      const char *title,
                                      const char *description) {
    DispatchGroup *group = dispatch_board_find_group(board, group_id);
    if (!board || !group || !title || title[0] == '\0')
        return NULL;

    char *task_id = dispatch_next_task_id(board, group->prefix);

    if (board->tasks.count >= board->tasks.capacity) {
        board->tasks.capacity = board->tasks.capacity == 0
                                    ? 8
                                    : board->tasks.capacity * 2;
        board->tasks.items = dispatch_realloc_array(board->tasks.items,
                                                    board->tasks.capacity,
                                                    sizeof(*board->tasks.items));
    }

    DispatchTask *task = &board->tasks.items[board->tasks.count++];
    memset(task, 0, sizeof(*task));
    task->id = task_id;
    task->title = dispatch_strdup(title);
    task->description = dispatch_strdup(description ? description : "");
    task->group = dispatch_strdup(group->id);
    task->state = DISPATCH_STATE_PROPOSED;
    task->requires_review = 1;
    task->created_at = time(NULL);
    task->updated_at = task->created_at;
    dispatch_task_append_history(task, "system", "created", "");
    return task;
}

int dispatch_task_set_title(DispatchTask *task, const char *title) {
    if (!task || !title || title[0] == '\0')
        return 0;
    replace_string(&task->title, title);
    task->updated_at = time(NULL);
    return 1;
}

int dispatch_task_set_description(DispatchTask *task, const char *description) {
    if (!task)
        return 0;
    replace_string(&task->description, description ? description : "");
    task->updated_at = time(NULL);
    return 1;
}

const char *dispatch_state_name(DispatchState state) {
    switch (state) {
    case DISPATCH_STATE_PROPOSED:
        return "proposed";
    case DISPATCH_STATE_READY:
        return "ready";
    case DISPATCH_STATE_BLOCKED:
        return "blocked";
    case DISPATCH_STATE_DOING:
        return "doing";
    case DISPATCH_STATE_REVIEW:
        return "review";
    case DISPATCH_STATE_DONE:
        return "done";
    case DISPATCH_STATE_PAUSED:
        return "paused";
    }
    return "unknown";
}

int dispatch_state_from_name(const char *name, DispatchState *state) {
    static const DispatchState states[] = {
        DISPATCH_STATE_PROPOSED, DISPATCH_STATE_READY,
        DISPATCH_STATE_BLOCKED,  DISPATCH_STATE_DOING,
        DISPATCH_STATE_REVIEW,   DISPATCH_STATE_DONE,
        DISPATCH_STATE_PAUSED,
    };

    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (strcmp(name, dispatch_state_name(states[i])) == 0) {
            *state = states[i];
            return 1;
        }
    }
    return 0;
}

int dispatch_task_set_state(DispatchTask *task, DispatchState state,
                            const char *actor, const char *note) {
    if (!task)
        return 0;

    task->state = state;
    task->updated_at = time(NULL);
    return dispatch_task_append_history(task, actor, dispatch_state_name(state),
                                        note ? note : "");
}

int dispatch_task_assign(DispatchTask *task, const char *actor) {
    if (!task || !actor || actor[0] == '\0')
        return 0;

    replace_string(&task->assigned_to, actor);
    task->updated_at = time(NULL);
    return dispatch_task_append_history(task, actor, "assigned", "");
}

void dispatch_task_clear_assignment(DispatchTask *task) {
    if (!task)
        return;

    free(task->assigned_to);
    task->assigned_to = NULL;
    task->updated_at = time(NULL);
}

int dispatch_task_has_unmet_dependencies(const DispatchBoard *board,
                                         const DispatchTask *task) {
    if (!board || !task)
        return 0;

    for (size_t i = 0; i < task->depends_on.count; i++) {
        DispatchTask *dependency =
            dispatch_board_find_task((DispatchBoard *)board,
                                     task->depends_on.items[i]);
        if (!dependency || dependency->state != DISPATCH_STATE_DONE)
            return 1;
    }
    return 0;
}

DispatchState dispatch_task_effective_state(const DispatchBoard *board,
                                            const DispatchTask *task) {
    if (!task)
        return DISPATCH_STATE_BLOCKED;
    if (task->state == DISPATCH_STATE_DONE ||
        task->state == DISPATCH_STATE_DOING ||
        task->state == DISPATCH_STATE_REVIEW) {
        return task->state;
    }
    if (dispatch_task_has_unmet_dependencies(board, task))
        return DISPATCH_STATE_BLOCKED;
    return task->state;
}

static int has_path_recursive(const DispatchBoard *board, const char *current,
                              const char *target, DispatchStringList *seen) {
    if (strcmp(current, target) == 0)
        return 1;
    if (string_list_contains(seen, current))
        return 0;
    string_list_append(seen, current);

    for (size_t i = 0; i < board->tasks.count; i++) {
        const DispatchTask *task = &board->tasks.items[i];
        if (!string_list_contains(&task->depends_on, current))
            continue;
        if (has_path_recursive(board, task->id, target, seen))
            return 1;
    }
    return 0;
}

int dispatch_board_has_dependency_path(const DispatchBoard *board,
                                       const char *from_id,
                                       const char *to_id) {
    DispatchStringList seen = {0};
    int result = 0;

    if (board && from_id && to_id)
        result = has_path_recursive(board, from_id, to_id, &seen);

    string_list_free(&seen);
    return result;
}

int dispatch_task_add_dependency(DispatchBoard *board, const char *from_id,
                                 const char *to_id) {
    DispatchTask *from = dispatch_board_find_task(board, from_id);
    DispatchTask *to = dispatch_board_find_task(board, to_id);

    if (!from || !to || strcmp(from_id, to_id) == 0)
        return 0;
    if (string_list_contains(&to->depends_on, from_id))
        return 1;
    if (dispatch_board_has_dependency_path(board, to_id, from_id))
        return 0;

    string_list_append(&to->depends_on, from_id);
    to->updated_at = time(NULL);
    return 1;
}
