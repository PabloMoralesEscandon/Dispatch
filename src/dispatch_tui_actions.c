/* Board mutations and agent/session operations invoked from the TUI. */

#include "dispatch_tui_internal.h"

static int add_dependencies_from_text(DispatchBoard *board, const char *task_id,
                                      const char *depends_text,
                                      char *message, size_t message_size);
static int string_starts_with(const char *value, const char *prefix);
static int agent_has_live_workspace(DispatchBoard *board, const char *name);

DispatchTask *selected_visible_task(DispatchTui *tui) {
    if (!tui->board_loaded)
        return NULL;

    if (tui->screen == TUI_SCREEN_TASK_INSPECTOR &&
        tui->inspected_task_id[0]) {
        return dispatch_board_find_task(&tui->board, tui->inspected_task_id);
    }

    VisibleTaskIter it;
    visible_task_iter_init(tui, &it);
    int visible_index = 0;
    for (DispatchTask *task = visible_task_iter_next(&it); task;
         task = visible_task_iter_next(&it)) {
        if (visible_index == tui->selected_task)
            return task;
        visible_index++;
    }
    return NULL;
}

DispatchWorkspace *workspace_for_task(DispatchBoard *board,
                                             const char *task_id) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (strcmp(workspace->task_id, task_id) == 0 &&
            workspace->state != DISPATCH_WORKSPACE_REMOVED)
            return workspace;
    }
    return NULL;
}



int mutate_task(const char *task_id, const char *actor,
                       DispatchTuiAction action, char *message,
                       size_t message_size) {
    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return 0;
    }

    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not load board");
        dispatch_store_lock_release(&lock);
        return 0;
    }

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task) {
        snprintf(message, message_size, "No task with id %s", task_id);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    int ok = 0;
    DispatchState finished_state = DISPATCH_STATE_PROPOSED;
    switch (action) {
    case TUI_ACTION_READY:
        ok = dispatch_task_mark_ready(&board, task, actor);
        break;
    case TUI_ACTION_READY_NO_REVIEW:
        /* Mirrors the CLI `dispatch ready <id> --no-review`. */
        ok = dispatch_task_mark_ready(&board, task, actor);
        if (ok)
            task->requires_review = 0;
        break;
    case TUI_ACTION_START:
        ok = dispatch_task_start(&board, task, actor);
        if (!ok) {
            /* Mirror the CLI's specific start refusal reasons. */
            DispatchState effective =
                dispatch_task_effective_state(&board, task);
            if (effective != DISPATCH_STATE_READY)
                snprintf(message, message_size,
                         "Cannot start %s: task is %s, not ready", task_id,
                         dispatch_state_name(effective));
            else
                snprintf(message, message_size,
                         "Cannot start %s: already assigned to %s", task_id,
                         task->assigned_to ? task->assigned_to : "?");
            dispatch_board_free(&board);
            dispatch_store_lock_release(&lock);
            return 0;
        }
        break;
    case TUI_ACTION_FINISH:
        ok = dispatch_task_finish(task, actor);
        finished_state = task->state;
        break;
    case TUI_ACTION_REVIEW:
        ok = dispatch_task_review(task, actor);
        break;
    }

    if (!ok) {
        snprintf(message, message_size, "Could not update %s", task_id);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    dispatch_board_normalize_states(&board);
    if (!dispatch_store_save(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not save board");
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    switch (action) {
    case TUI_ACTION_READY:
        snprintf(message, message_size, "Readied %s", task_id);
        break;
    case TUI_ACTION_READY_NO_REVIEW:
        snprintf(message, message_size, "Readied %s (no review)", task_id);
        break;
    case TUI_ACTION_START:
        snprintf(message, message_size, "Started %s as %s", task_id, actor);
        break;
    case TUI_ACTION_FINISH:
        snprintf(message, message_size, "Finished %s (%s)", task_id,
                 dispatch_state_name(finished_state));
        break;
    case TUI_ACTION_REVIEW:
        snprintf(message, message_size, "Reviewed %s", task_id);
        break;
    }

    dispatch_board_free(&board);
    dispatch_store_lock_release(&lock);
    return 1;
}

int mutate_task_content(const char *task_id, const char *title,
                               const char *description, const char *actor,
                               char *message, size_t message_size) {
    if (!title || !title[0]) {
        snprintf(message, message_size, "Task title is required");
        return 0;
    }
    if (title_starts_with_dispatch_id_like(title)) {
        snprintf(message, message_size, "Task title should not include an ID");
        return 0;
    }

    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return 0;
    }

    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not load board");
        dispatch_store_lock_release(&lock);
        return 0;
    }

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task) {
        snprintf(message, message_size, "No task with id %s", task_id);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    if (!dispatch_task_set_title(task, title) ||
        !dispatch_task_set_description(task, description) ||
        !dispatch_task_append_history(task, actor, "edited",
                                      "title and description") ||
        !dispatch_store_save(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not edit task");
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    snprintf(message, message_size, "Edited task %s", task_id);
    dispatch_board_free(&board);
    dispatch_store_lock_release(&lock);
    return 1;
}

int mutate_task_group(const char *task_id, const char *group_id,
                             const char *actor, char *message,
                             size_t message_size) {
    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return 0;
    }

    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not load board");
        dispatch_store_lock_release(&lock);
        return 0;
    }

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    DispatchGroup *group = dispatch_board_find_group(&board, group_id);
    if (!task || !group) {
        snprintf(message, message_size, "%s",
                 !task ? "Task not found" : "Group not found");
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }
    if (strcmp(task->group, group->id) == 0) {
        snprintf(message, message_size, "Task %s already belongs to group %s",
                 task_id, group->prefix);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    char from_group[32];
    snprintf(from_group, sizeof(from_group), "%s", task->group);
    if (!dispatch_task_move_to_group(&board, task, group->id, actor) ||
        !dispatch_store_save(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not move task");
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    snprintf(message, message_size, "Moved task %s from %s to %s", task_id,
             from_group, group->id);
    dispatch_board_free(&board);
    dispatch_store_lock_release(&lock);
    return 1;
}

/* Delete a task in place, mirroring the CLI
 * `dispatch task delete <id> [--force]`: without force the delete is
 * refused while other tasks depend on the task, and the failure message
 * surfaces which tasks those are. */
int mutate_task_delete(const char *task_id, int force, char *message,
                              size_t message_size) {
    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return 0;
    }

    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not load board");
        dispatch_store_lock_release(&lock);
        return 0;
    }

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task) {
        snprintf(message, message_size, "No task with id %s", task_id);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    if (!force && dispatch_task_dependent_count(&board, task_id) > 0) {
        char blocks[256];
        blocks_text(&board, task_id, blocks, sizeof(blocks));
        snprintf(message, message_size,
                 "Could not delete %s (blocks: %s; use X to force)", task_id,
                 blocks);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    if (!dispatch_board_delete_task(&board, task_id, force) ||
        !dispatch_store_save(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not delete task");
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    snprintf(message, message_size, "Deleted task %s%s", task_id,
             force ? " (forced)" : "");
    dispatch_board_free(&board);
    dispatch_store_lock_release(&lock);
    return 1;
}

int create_group(const char *name, const char *prefix, char *message,
                        size_t message_size) {
    if (!name || !name[0]) {
        snprintf(message, message_size, "Group name is required");
        return 0;
    }

    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return 0;
    }

    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not load board");
        dispatch_store_lock_release(&lock);
        return 0;
    }

    if (!dispatch_board_add_group(&board, name, prefix && prefix[0] ? prefix : NULL)) {
        snprintf(message, message_size, "Could not add group %s", name);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    DispatchGroup *group = dispatch_board_find_group(&board, name);
    char prefix_copy[16];
    snprintf(prefix_copy, sizeof(prefix_copy), "%s", group ? group->prefix : "");
    if (!dispatch_store_save(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not save board");
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    snprintf(message, message_size, "Added group %s (%s)", name, prefix_copy);
    dispatch_board_free(&board);
    dispatch_store_lock_release(&lock);
    return 1;
}

static int add_dependencies_from_text(DispatchBoard *board, const char *task_id,
                                      const char *depends_text,
                                      char *message, size_t message_size) {
    if (!depends_text || !depends_text[0] || strcmp(depends_text, "-") == 0)
        return 1;

    char *copy = strdup(depends_text);
    if (!copy) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    for (char *token = strtok(copy, ", \t"); token;
         token = strtok(NULL, ", \t")) {
        if (!dispatch_task_add_dependency(board, token, task_id)) {
            snprintf(message, message_size, "Could not add dependency %s -> %s",
                     token, task_id);
            free(copy);
            return 0;
        }
    }

    free(copy);
    return 1;
}

int create_task(const char *group, const char *title,
                       const char *description, int requires_review,
                       const char *depends_text, const char *actor, char *message,
                       size_t message_size) {
    if (!group || !group[0]) {
        snprintf(message, message_size, "Group is required");
        return 0;
    }
    if (!title || !title[0]) {
        snprintf(message, message_size, "Task title is required");
        return 0;
    }
    if (title_starts_with_dispatch_id_like(title)) {
        snprintf(message, message_size, "Task title should not include an ID");
        return 0;
    }

    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return 0;
    }

    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not load board");
        dispatch_store_lock_release(&lock);
        return 0;
    }

    int created_group = 0;
    char created_group_prefix[16] = {0};
    if (!dispatch_board_find_group(&board, group)) {
        if (!dispatch_board_add_group(&board, group, NULL)) {
            snprintf(message, message_size, "Could not create group %s", group);
            dispatch_board_free(&board);
            dispatch_store_lock_release(&lock);
            return 0;
        }
        created_group = 1;
        DispatchGroup *new_group = dispatch_board_find_group(&board, group);
        if (new_group)
            snprintf(created_group_prefix, sizeof(created_group_prefix), "%s",
                     new_group->prefix);
    }

    DispatchTask *task =
        dispatch_board_add_task_with_actor(&board, group, title,
                                           description ? description : "",
                                           actor && actor[0] ? actor : "user");
    if (!task) {
        snprintf(message, message_size, "Could not add task %s", title);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }
    task->requires_review = requires_review;

    char task_id[64];
    snprintf(task_id, sizeof(task_id), "%s", task->id);
    if (!add_dependencies_from_text(&board, task_id, depends_text, message,
                                    message_size)) {
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }
    dispatch_board_normalize_states(&board);

    if (!dispatch_store_save(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not save board");
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    if (created_group)
        snprintf(message, message_size, "Added group %s (%s); added task %s",
                 group, created_group_prefix, task_id);
    else
        snprintf(message, message_size, "Added task %s", task_id);
    dispatch_board_free(&board);
    dispatch_store_lock_release(&lock);
    return 1;
}

int mutate_dependency(const char *dependency_id, const char *dependent_id,
                             int add, char *message, size_t message_size) {
    if (!dependency_id || !dependency_id[0] || !dependent_id || !dependent_id[0]) {
        snprintf(message, message_size, "Dependency and dependent are required");
        return 0;
    }

    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return 0;
    }

    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not load board");
        dispatch_store_lock_release(&lock);
        return 0;
    }

    int ok = add ? dispatch_task_add_dependency(&board, dependency_id, dependent_id)
                 : dispatch_task_remove_dependency(&board, dependency_id,
                                                   dependent_id);
    if (!ok) {
        snprintf(message, message_size, "Could not %s dependency %s -> %s",
                 add ? "add" : "remove", dependency_id, dependent_id);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }
    dispatch_board_normalize_states(&board);

    if (!dispatch_store_save(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not save board");
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    snprintf(message, message_size, "%s dependency %s -> %s",
             add ? "Added" : "Removed", dependency_id, dependent_id);
    dispatch_board_free(&board);
    dispatch_store_lock_release(&lock);
    return 1;
}

void run_selected_task_action(DispatchTui *tui,
                                     DispatchTuiAction action) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        tui_set_status(tui, "No selected task");
        return;
    }

    char task_id[64];
    snprintf(task_id, sizeof(task_id), "%s", task->id);
    char message[256] = {0};
    mutate_task(task_id, tui->actor, action, message, sizeof(message));
    tui_load_board(tui);
    tui_set_status(tui, message);
    clamp_selection(tui);
}

static int string_starts_with(const char *value, const char *prefix) {
    return !prefix || !prefix[0] ||
           strncmp(value, prefix, strlen(prefix)) == 0;
}

void execute_palette_command(DispatchTui *tui, const char *command) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", command ? command : "");
    char *verb = strtok(copy, " \t");
    if (!verb) {
        tui_set_status(tui, "No command");
        return;
    }

    if (strcmp(verb, "q") == 0 || strcmp(verb, "quit") == 0) {
        tui->running = 0;
        tui_set_status(tui, "Quit");
        return;
    }
    if (strcmp(verb, "board") == 0 || strcmp(verb, "b") == 0) {
        tui->screen = TUI_SCREEN_BOARD;
        tui_set_status(tui, "Board");
        return;
    }
    if (strcmp(verb, "agents") == 0 || strcmp(verb, "a") == 0) {
        tui->screen = TUI_SCREEN_AGENTS;
        tui_set_status(tui, "Agents");
        return;
    }
    if (strcmp(verb, "workspaces") == 0 || strcmp(verb, "w") == 0) {
        tui->screen = TUI_SCREEN_WORKSPACES;
        tui_set_status(tui, "Workspaces");
        return;
    }
    if (strcmp(verb, "logs") == 0 || strcmp(verb, "l") == 0) {
        tui->screen = TUI_SCREEN_LOGS;
        tui_set_status(tui, "Logs");
        return;
    }
    if (strcmp(verb, "filter") == 0) {
        char *name = strtok(NULL, " \t");
        DispatchTuiFilter filter;
        if (name && parse_filter_name(name, &filter)) {
            set_filter(tui, filter);
            tui->screen = TUI_SCREEN_BOARD;
        } else {
            tui_set_status(tui, "Unknown filter");
        }
        return;
    }
    if (strcmp(verb, "group") == 0) {
        char *group = strtok(NULL, " \t");
        tui->group_filter = -1;
        if (group && strcmp(group, "all") != 0) {
            for (size_t i = 0; i < tui->board.groups.count; i++) {
                DispatchGroup *candidate = &tui->board.groups.items[i];
                if (strcmp(candidate->id, group) == 0 ||
                    strcmp(candidate->prefix, group) == 0 ||
                    strcmp(candidate->name, group) == 0) {
                    tui->group_filter = (int)i;
                    break;
                }
            }
            if (tui->group_filter < 0) {
                tui_set_status(tui, "No matching group");
                return;
            }
        }
        tui->screen = TUI_SCREEN_BOARD;
        tui->selected_task = 0;
        tui->task_top = 0;
        tui_set_status(tui, "Group filter updated");
        return;
    }
    if (strcmp(verb, "actor") == 0) {
        char *actor = strtok(NULL, " \t");
        tui->actor_filter = -1;
        if (actor && strcmp(actor, "all") != 0) {
            for (size_t i = 0; i < tui->board.agents.count; i++) {
                if (strcmp(tui->board.agents.items[i].name, actor) == 0) {
                    tui->actor_filter = (int)i;
                    break;
                }
            }
            if (tui->actor_filter < 0) {
                tui_set_status(tui, "No matching actor");
                return;
            }
        }
        tui->screen = TUI_SCREEN_BOARD;
        tui->selected_task = 0;
        tui->task_top = 0;
        tui_set_status(tui, "Actor filter updated");
        return;
    }
    if (strcmp(verb, "log") == 0) {
        char *field = strtok(NULL, " \t");
        char *value = strtok(NULL, "");
        set_log_filter(tui, field ? field : "", value ? value : "");
        tui->screen = TUI_SCREEN_LOGS;
        return;
    }
    if (strcmp(verb, "task") == 0) {
        char *task_id = strtok(NULL, " \t");
        if (task_id && select_task_by_id(tui, task_id)) {
            tui->screen = TUI_SCREEN_TASK_INSPECTOR;
            tui_set_status(tui, "Inspecting task");
        } else {
            tui_set_status(tui, "No matching task");
        }
        return;
    }
    if (strcmp(verb, "agent") == 0) {
        char *name = strtok(NULL, " \t");
        if (name && select_agent_by_name(tui, name)) {
            tui->screen = TUI_SCREEN_AGENT_INSPECTOR;
            tui_set_status(tui, "Inspecting agent");
        } else {
            tui_set_status(tui, "No matching agent");
        }
        return;
    }
    if (strcmp(verb, "workspace") == 0) {
        char *target = strtok(NULL, " \t");
        if (target && select_workspace_by_id(tui, target)) {
            tui->screen = TUI_SCREEN_WORKSPACE_INSPECTOR;
            tui_set_status(tui, "Inspecting workspace");
        } else {
            tui_set_status(tui, "No matching workspace");
        }
        return;
    }

    DispatchTuiAction action;
    if (parse_action_name(verb, &action)) {
        char *task_id = strtok(NULL, " \t");
        if (!task_id) {
            tui_set_status(tui, "Task ID required");
            return;
        }
        char message[256] = {0};
        mutate_task(task_id, tui->actor, action, message, sizeof(message));
        tui_load_board(tui);
        tui_set_status(tui, message);
        return;
    }

    tui_set_status(tui, "Unknown palette command");
}

void run_command_palette(DispatchTui *tui) {
    char command[512];
    if (!prompt_line(": ", command, sizeof(command), ""))
        return;
    execute_palette_command(tui, command);
}

void print_palette_completion(const DispatchBoard *board,
                                     const char *input) {
    const char *commands[] = {"q",         "quit",   "board",      "agents",
                              "workspaces", "logs",
                              "filter",    "group",  "actor",      "log",
                              "task",      "agent",  "workspace",  "ready",
                              "start",     "finish", "review",     NULL};
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", input ? input : "");
    char *space = strchr(copy, ' ');
    if (!space) {
        for (int i = 0; commands[i]; i++) {
            if (string_starts_with(commands[i], copy))
                puts(commands[i]);
        }
        return;
    }

    *space = '\0';
    char *verb = copy;
    char *prefix = space + 1;
    while (*prefix == ' ')
        prefix++;
    if (strcmp(verb, "filter") == 0) {
        const char *filters[] = {"not-done", "all",   "ready", "blocked",
                                 "review",   "doing", "done",  "attention",
                                 NULL};
        for (int i = 0; filters[i]; i++) {
            if (string_starts_with(filters[i], prefix))
                puts(filters[i]);
        }
    } else if (strcmp(verb, "group") == 0) {
        puts("all");
        for (size_t i = 0; i < board->groups.count; i++) {
            if (string_starts_with(board->groups.items[i].prefix, prefix))
                puts(board->groups.items[i].prefix);
        }
    } else if (strcmp(verb, "actor") == 0 || strcmp(verb, "agent") == 0) {
        if (strcmp(verb, "actor") == 0)
            puts("all");
        for (size_t i = 0; i < board->agents.count; i++) {
            if (string_starts_with(board->agents.items[i].name, prefix))
                puts(board->agents.items[i].name);
        }
    } else if (strcmp(verb, "workspace") == 0) {
        for (size_t i = 0; i < board->workspaces.count; i++) {
            DispatchWorkspace *workspace = &board->workspaces.items[i];
            if (workspace->state != DISPATCH_WORKSPACE_REMOVED &&
                string_starts_with(workspace->task_id, prefix))
                puts(workspace->task_id);
        }
    } else if (strcmp(verb, "log") == 0) {
        const char *fields[] = {"actor", "command", "action", "task",
                                "agent", "workspace", NULL};
        for (int i = 0; fields[i]; i++) {
            if (string_starts_with(fields[i], prefix))
                puts(fields[i]);
        }
    } else {
        for (size_t i = 0; i < board->tasks.count; i++) {
            if (string_starts_with(board->tasks.items[i].id, prefix))
                puts(board->tasks.items[i].id);
        }
    }
}

int update_agent_session_metadata(const char *name,
                                         const char *session_id,
                                         const char *current_task,
                                         const char *last_workspace,
                                         int clear_session,
                                         int clear_current_task,
                                         int clear_last_workspace,
                                         char *message,
                                         size_t message_size) {
    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return 0;
    }

    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not load board");
        dispatch_store_lock_release(&lock);
        return 0;
    }

    DispatchAgent *agent = dispatch_board_find_agent(&board, name);
    if (!agent) {
        snprintf(message, message_size, "No agent named %s", name);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }
    if (current_task && !dispatch_board_find_task(&board, current_task)) {
        snprintf(message, message_size, "No task with id %s", current_task);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }
    if (last_workspace && !dispatch_board_find_workspace(&board, last_workspace)) {
        snprintf(message, message_size, "No workspace for %s", last_workspace);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    if (session_id || clear_session) {
        free(agent->session_id);
        agent->session_id = session_id && session_id[0] ? strdup(session_id) : NULL;
    }
    if (current_task || clear_current_task) {
        free(agent->current_task);
        agent->current_task =
            current_task && current_task[0] ? strdup(current_task) : NULL;
    }
    if (last_workspace || clear_last_workspace) {
        free(agent->last_workspace);
        agent->last_workspace =
            last_workspace && last_workspace[0] ? strdup(last_workspace) : NULL;
    }
    agent->updated_at = time(NULL);

    if (!dispatch_store_save(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not save board");
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    snprintf(message, message_size, "Updated agent session %s", name);
    dispatch_board_free(&board);
    dispatch_store_lock_release(&lock);
    return 1;
}

void clear_selected_agent_session(DispatchTui *tui) {
    DispatchAgent *agent = selected_agent(tui);
    if (!agent) {
        tui_set_status(tui, "No selected agent");
        return;
    }
    char name[128];
    snprintf(name, sizeof(name), "%s", agent->name);
    char message[256] = {0};
    update_agent_session_metadata(name, NULL, NULL, NULL, 1, 0, 0, message,
                                  sizeof(message));
    tui_load_board(tui);
    tui_set_status(tui, message);
}

int set_agent_session_id(const char *name, const char *session_id,
                                char *message, size_t message_size) {
    char *trimmed = tui_trimmed_copy(session_id);
    int ok = update_agent_session_metadata(
        name, trimmed[0] ? trimmed : NULL, NULL, NULL, !trimmed[0], 0, 0,
        message, message_size);
    free(trimmed);
    return ok;
}

void set_selected_agent_session(DispatchTui *tui) {
    DispatchAgent *agent = selected_agent(tui);
    if (!agent) {
        tui_set_status(tui, "No selected agent");
        return;
    }

    char name[128];
    snprintf(name, sizeof(name), "%s", agent->name);
    char label[256];
    snprintf(label, sizeof(label), "Session ID for %s (blank clears): ", name);
    char session_id[256];
    if (!prompt_line(label, session_id, sizeof(session_id), ""))
        return;

    char message[256] = {0};
    set_agent_session_id(name, session_id, message, sizeof(message));
    tui_load_board(tui);
    tui_set_status(tui, message);
}

static int agent_has_live_workspace(DispatchBoard *board, const char *name) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace->state != DISPATCH_WORKSPACE_REMOVED &&
            strcmp(workspace->actor, name) == 0)
            return 1;
    }
    return 0;
}

int set_agent_archived_state(const char *name, int archived,
                                    char *message, size_t message_size) {
    char error[256] = {0};
    DispatchStoreLock lock = {0};
    if (!dispatch_store_lock_acquire(&lock, DISPATCH_STORE_FILE, 1000, error,
                                     sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return 0;
    }

    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not load board");
        dispatch_store_lock_release(&lock);
        return 0;
    }

    DispatchAgent *agent = dispatch_board_find_agent(&board, name);
    if (!agent) {
        snprintf(message, message_size, "No agent named %s", name);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }
    if (archived && ((agent->current_task && agent->current_task[0]) ||
                     agent_has_live_workspace(&board, name))) {
        snprintf(message, message_size, "Agent %s owns active work", name);
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    agent->archived = archived;
    agent->updated_at = time(NULL);
    if (!dispatch_store_save(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "could not save board");
        dispatch_board_free(&board);
        dispatch_store_lock_release(&lock);
        return 0;
    }

    snprintf(message, message_size, "%s agent %s",
             archived ? "Archived" : "Restored", name);
    dispatch_board_free(&board);
    dispatch_store_lock_release(&lock);
    return 1;
}

void toggle_selected_agent_archived(DispatchTui *tui) {
    DispatchAgent *agent = selected_agent(tui);
    if (!agent) {
        tui_set_status(tui, "No selected agent");
        return;
    }
    char name[128];
    int archived = !agent->archived;
    snprintf(name, sizeof(name), "%s", agent->name);
    char message[256] = {0};
    set_agent_archived_state(name, archived, message, sizeof(message));
    tui_load_board(tui);
    tui_set_status(tui, message);
    clamp_agent_selection(tui);
}
