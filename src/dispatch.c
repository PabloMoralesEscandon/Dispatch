#include "dispatch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void replace_optional_string(char **target, const char *value) {
    free(*target);
    *target = value && value[0] ? dispatch_strdup(value) : NULL;
}

static char *trimmed_copy(const char *value) {
    const char *start = value ? value : "";
    while (isspace((unsigned char)*start))
        start++;
    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
        end--;

    size_t len = (size_t)(end - start);
    char *copy = malloc(len + 1);
    if (!copy) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static int string_list_contains(const DispatchStringList *list,
                                const char *value) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], value) == 0)
            return 1;
    }
    return 0;
}

static int string_list_remove(DispatchStringList *list, const char *value) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], value) != 0)
            continue;
        free(list->items[i]);
        for (size_t j = i + 1; j < list->count; j++)
            list->items[j - 1] = list->items[j];
        list->count--;
        return 1;
    }
    return 0;
}

static int ascii_is_alnum(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
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

static void task_free_fields(DispatchTask *task) {
    free(task->id);
    free(task->title);
    free(task->description);
    free(task->group);
    string_list_free(&task->depends_on);
    string_list_free(&task->commits);
    free(task->assigned_to);
    free(task->started_by);
    free(task->completed_by);
    history_free(&task->history);
}

static void agent_free_fields(DispatchAgent *agent) {
    free(agent->name);
    free(agent->runner);
    free(agent->model);
    free(agent->agent_dir);
    free(agent->prompt_path);
    free(agent->run_script_path);
    free(agent->session_id);
    free(agent->current_task);
    free(agent->last_workspace);
}

static void workspace_free_fields(DispatchWorkspace *workspace) {
    free(workspace->id);
    free(workspace->task_id);
    free(workspace->actor);
    free(workspace->path);
    free(workspace->branch);
    free(workspace->repo_path);
    string_list_free(&workspace->sequence_tasks);
    free(workspace->review_gate);
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
    board->repo_path = dispatch_strdup(".");
}

void dispatch_board_set_repo_path(DispatchBoard *board, const char *repo_path) {
    if (!board)
        return;
    replace_string(&board->repo_path,
                   repo_path && repo_path[0] ? repo_path : ".");
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
        task_free_fields(task);
    }
    free(board->tasks.items);

    for (size_t i = 0; i < board->agents.count; i++)
        agent_free_fields(&board->agents.items[i]);
    free(board->agents.items);

    for (size_t i = 0; i < board->workspaces.count; i++)
        workspace_free_fields(&board->workspaces.items[i]);
    free(board->workspaces.items);

    free(board->name);
    free(board->repo_path);
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

DispatchAgent *dispatch_board_find_agent(DispatchBoard *board,
                                         const char *name) {
    if (!board || !name)
        return NULL;
    for (size_t i = 0; i < board->agents.count; i++) {
        if (strcmp(board->agents.items[i].name, name) == 0)
            return &board->agents.items[i];
    }
    return NULL;
}

DispatchWorkspace *dispatch_board_find_workspace(DispatchBoard *board,
                                                 const char *id_or_task_id) {
    if (!board || !id_or_task_id)
        return NULL;
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (strcmp(workspace->id, id_or_task_id) == 0 ||
            strcmp(workspace->task_id, id_or_task_id) == 0) {
            return workspace;
        }
    }
    return NULL;
}

int dispatch_board_add_group(DispatchBoard *board, const char *name,
                             const char *prefix) {
    if (!board || !name || name[0] == '\0' ||
        strlen(name) > DISPATCH_GROUP_NAME_MAX)
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

int dispatch_actor_label_is_valid(const char *actor) {
    if (!actor || actor[0] == '\0')
        return 0;

    size_t length = strlen(actor);
    if (length > 48)
        return 0;

    unsigned char first = (unsigned char)actor[0];
    if (!ascii_is_alnum(first))
        return 0;

    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)actor[i];
        if (!(ascii_is_alnum(c) || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

char *dispatch_actor_slug(const char *actor) {
    if (!dispatch_actor_label_is_valid(actor))
        return NULL;

    char *slug = dispatch_strdup(actor);
    for (size_t i = 0; slug[i] != '\0'; i++)
        slug[i] = (char)tolower((unsigned char)slug[i]);
    return slug;
}

char *dispatch_default_workspace_branch(const char *actor,
                                        const char *task_id) {
    if (!task_id || task_id[0] == '\0')
        return NULL;

    char *slug = dispatch_actor_slug(actor);
    if (!slug)
        return NULL;

    const char *prefix = "agent/";
    size_t size = strlen(prefix) + strlen(slug) + 1 + strlen(task_id) + 1;
    char *branch = malloc(size);
    if (!branch) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(branch, size, "%s%s/%s", prefix, slug, task_id);
    free(slug);
    return branch;
}

static char *path_without_trailing_slashes(const char *path) {
    if (!path || path[0] == '\0')
        return dispatch_strdup(".");

    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/')
        length--;

    char *copy = malloc(length + 1);
    if (!copy) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(copy, path, length);
    copy[length] = '\0';
    return copy;
}

static const char *path_basename_const(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return path;
    return slash[1] ? slash + 1 : path;
}

static char *path_parent(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return dispatch_strdup(".");
    if (slash == path)
        return dispatch_strdup("/");

    size_t length = (size_t)(slash - path);
    char *parent = malloc(length + 1);
    if (!parent) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(parent, path, length);
    parent[length] = '\0';
    return parent;
}

char *dispatch_default_workspace_path(const char *repo_path,
                                      const char *actor,
                                      const char *task_id) {
    if (!repo_path || repo_path[0] == '\0' || !task_id || task_id[0] == '\0')
        return NULL;

    char *slug = dispatch_actor_slug(actor);
    if (!slug)
        return NULL;

    char *repo = path_without_trailing_slashes(repo_path);
    char *parent = path_parent(repo);
    const char *repo_name = path_basename_const(repo);

    size_t leaf_size = strlen(repo_name) + strlen("-agent-") + strlen(slug) +
                       1 + strlen(task_id) + 1;
    char *leaf = malloc(leaf_size);
    if (!leaf) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(leaf, leaf_size, "%s-agent-%s-%s", repo_name, slug, task_id);

    size_t parent_len = strlen(parent);
    int needs_slash = parent_len > 0 && parent[parent_len - 1] != '/';
    size_t size = parent_len + (size_t)needs_slash + strlen(leaf) + 1;
    char *path = malloc(size);
    if (!path) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(path, size, "%s%s%s", parent, needs_slash ? "/" : "", leaf);

    free(slug);
    free(repo);
    free(parent);
    free(leaf);
    return path;
}

char *dispatch_resolve_path(const char *workflow_dir, const char *path) {
    if (!path || path[0] == '\0')
        return NULL;
    if (path[0] == '/')
        return realpath(path, NULL);

    const char *base = workflow_dir && workflow_dir[0] ? workflow_dir : ".";
    char *base_real = realpath(base, NULL);
    if (!base_real)
        return NULL;

    size_t base_len = strlen(base_real);
    int needs_slash = base_len > 0 && base_real[base_len - 1] != '/';
    size_t joined_size = base_len + (size_t)needs_slash + strlen(path) + 1;
    char *joined = malloc(joined_size);
    if (!joined) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(joined, joined_size, "%s%s%s", base_real,
             needs_slash ? "/" : "", path);
    free(base_real);

    char *resolved = realpath(joined, NULL);
    free(joined);
    return resolved;
}

int dispatch_path_is_git_repository(const char *path) {
    if (!path || path[0] == '\0')
        return 0;

    char *repo = path_without_trailing_slashes(path);
    size_t size = strlen(repo) + strlen("/.git") + 1;
    char *git_path = malloc(size);
    if (!git_path) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(git_path, size, "%s/.git", repo);

    struct stat info;
    int result = stat(git_path, &info) == 0 &&
                 (S_ISDIR(info.st_mode) || S_ISREG(info.st_mode));

    free(repo);
    free(git_path);
    return result;
}

int dispatch_workspace_path_conflicts(const char *repo_path,
                                      const char *workspace_path) {
    char *repo = dispatch_resolve_path(".", repo_path);
    char *workspace = dispatch_resolve_path(".", workspace_path);
    if (!repo || !workspace) {
        free(repo);
        free(workspace);
        return 0;
    }

    int conflicts = strcmp(repo, workspace) == 0;
    free(repo);
    free(workspace);
    return conflicts;
}

DispatchTask *dispatch_board_add_task(DispatchBoard *board,
                                      const char *group_id,
                                      const char *title,
                                      const char *description) {
    return dispatch_board_add_task_with_actor(board, group_id, title,
                                              description, "system");
}

DispatchTask *dispatch_board_add_task_with_actor(DispatchBoard *board,
                                                 const char *group_id,
                                                 const char *title,
                                                 const char *description,
                                                 const char *actor) {
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
    dispatch_task_append_history(task, actor && actor[0] ? actor : "system",
                                 "created", "");
    return task;
}

int dispatch_board_delete_task(DispatchBoard *board, const char *task_id,
                               int force) {
    if (!board || !task_id)
        return 0;

    size_t index = board->tasks.count;
    for (size_t i = 0; i < board->tasks.count; i++) {
        if (strcmp(board->tasks.items[i].id, task_id) == 0) {
            index = i;
            break;
        }
    }
    if (index == board->tasks.count)
        return 0;

    if (!force && dispatch_task_dependent_count(board, task_id) > 0)
        return 0;

    if (force) {
        for (size_t i = 0; i < board->tasks.count; i++)
            string_list_remove(&board->tasks.items[i].depends_on, task_id);
    }

    task_free_fields(&board->tasks.items[index]);
    for (size_t i = index + 1; i < board->tasks.count; i++)
        board->tasks.items[i - 1] = board->tasks.items[i];
    board->tasks.count--;
    dispatch_board_normalize_states(board);
    return 1;
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

int dispatch_task_move_to_group(DispatchBoard *board, DispatchTask *task,
                                const char *group_id, const char *actor) {
    DispatchGroup *group = dispatch_board_find_group(board, group_id);
    if (!board || !task || !group || strcmp(task->group, group->id) == 0)
        return 0;

    char note[64];
    snprintf(note, sizeof(note), "from %s to %s", task->group, group->id);
    replace_string(&task->group, group->id);
    return dispatch_task_append_history(task, actor, "moved", note);
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

const char *dispatch_workspace_state_name(DispatchWorkspaceState state) {
    switch (state) {
    case DISPATCH_WORKSPACE_CREATING:
        return "creating";
    case DISPATCH_WORKSPACE_ACTIVE:
        return "active";
    case DISPATCH_WORKSPACE_REMOVED:
        return "removed";
    }
    return "creating";
}

int dispatch_workspace_state_from_name(const char *name,
                                       DispatchWorkspaceState *state) {
    static const DispatchWorkspaceState states[] = {
        DISPATCH_WORKSPACE_CREATING,
        DISPATCH_WORKSPACE_ACTIVE,
        DISPATCH_WORKSPACE_REMOVED,
    };

    if (!name || !state)
        return 0;
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (strcmp(name, dispatch_workspace_state_name(states[i])) == 0) {
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

int dispatch_task_mark_ready(DispatchBoard *board, DispatchTask *task,
                             const char *actor) {
    if (!task)
        return 0;
    task->state = DISPATCH_STATE_READY;
    task->updated_at = time(NULL);
    dispatch_task_append_history(task, actor, "ready", "");
    dispatch_board_normalize_states(board);
    return 1;
}

int dispatch_task_start(DispatchBoard *board, DispatchTask *task,
                        const char *actor) {
    if (!board || !task || !actor || actor[0] == '\0')
        return 0;
    if (dispatch_task_effective_state(board, task) != DISPATCH_STATE_READY)
        return 0;
    if (task->assigned_to && task->assigned_to[0] != '\0')
        return 0;

    dispatch_task_assign(task, actor);
    replace_string(&task->started_by, actor);
    task->started_at = time(NULL);
    task->state = DISPATCH_STATE_DOING;
    task->updated_at = task->started_at;
    return dispatch_task_append_history(task, actor, "started", "");
}

int dispatch_task_finish(DispatchTask *task, const char *actor) {
    if (!task || !actor || actor[0] == '\0')
        return 0;
    if (task->state != DISPATCH_STATE_DOING)
        return 0;

    replace_string(&task->completed_by, actor);
    task->completed_at = time(NULL);
    dispatch_task_clear_assignment(task);
    task->state = task->requires_review ? DISPATCH_STATE_REVIEW
                                        : DISPATCH_STATE_DONE;
    task->updated_at = task->completed_at;
    return dispatch_task_append_history(task, actor, "finished", "");
}

int dispatch_task_review(DispatchTask *task, const char *actor) {
    if (!task || !actor || actor[0] == '\0')
        return 0;
    if (task->state != DISPATCH_STATE_REVIEW)
        return 0;

    if (!task->completed_by)
        replace_string(&task->completed_by, actor);
    if (task->completed_at == (time_t)0)
        task->completed_at = time(NULL);
    task->state = DISPATCH_STATE_DONE;
    task->updated_at = time(NULL);
    return dispatch_task_append_history(task, actor, "reviewed", "");
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

size_t dispatch_task_dependent_count(const DispatchBoard *board,
                                     const char *task_id) {
    size_t count = 0;
    if (!board || !task_id)
        return 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        if (string_list_contains(&board->tasks.items[i].depends_on, task_id))
            count++;
    }
    return count;
}

DispatchState dispatch_task_effective_state(const DispatchBoard *board,
                                            const DispatchTask *task) {
    if (!task)
        return DISPATCH_STATE_BLOCKED;
    if (task->state == DISPATCH_STATE_DONE ||
        task->state == DISPATCH_STATE_DOING ||
        task->state == DISPATCH_STATE_REVIEW ||
        task->state == DISPATCH_STATE_PROPOSED) {
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
    dispatch_board_normalize_states(board);
    return 1;
}

int dispatch_task_remove_dependency(DispatchBoard *board, const char *from_id,
                                    const char *to_id) {
    DispatchTask *to = dispatch_board_find_task(board, to_id);
    if (!to)
        return 0;
    int removed = string_list_remove(&to->depends_on, from_id);
    if (removed) {
        to->updated_at = time(NULL);
        dispatch_board_normalize_states(board);
    }
    return removed;
}

void dispatch_board_normalize_states(DispatchBoard *board) {
    if (!board)
        return;

    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (task->state == DISPATCH_STATE_DONE ||
            task->state == DISPATCH_STATE_DOING ||
            task->state == DISPATCH_STATE_REVIEW ||
            task->state == DISPATCH_STATE_PROPOSED) {
            continue;
        }

        if (dispatch_task_has_unmet_dependencies(board, task)) {
            task->state = DISPATCH_STATE_BLOCKED;
        } else if (task->state == DISPATCH_STATE_BLOCKED) {
            task->state = DISPATCH_STATE_READY;
        }
    }
}

int dispatch_board_normalize_agent_sessions(DispatchBoard *board) {
    if (!board)
        return 0;

    int changed = 0;
    for (size_t i = 0; i < board->agents.count; i++) {
        DispatchAgent *agent = &board->agents.items[i];
        if (!agent->session_id)
            continue;

        char *trimmed = trimmed_copy(agent->session_id);
        int differs = strcmp(agent->session_id, trimmed) != 0;
        if (differs) {
            replace_optional_string(&agent->session_id, trimmed);
            changed++;
        }
        free(trimmed);
    }
    return changed;
}
