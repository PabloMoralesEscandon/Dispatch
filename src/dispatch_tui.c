#include "dispatch_tui.h"

#include <ncurses.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <jansson.h>

#include "dispatch.h"
#include "dispatch_store.h"

typedef enum {
    TUI_SCREEN_BOARD,
    TUI_SCREEN_TASK_INSPECTOR,
    TUI_SCREEN_AGENTS,
    TUI_SCREEN_AGENT_INSPECTOR,
    TUI_SCREEN_TASK_FORM,
    TUI_SCREEN_GROUP_FORM,
    TUI_SCREEN_WORKSPACES,
    TUI_SCREEN_WORKSPACE_INSPECTOR,
    TUI_SCREEN_LOGS
} DispatchTuiScreen;

typedef enum {
    TUI_FILTER_NOT_DONE,
    TUI_FILTER_ALL,
    TUI_FILTER_READY,
    TUI_FILTER_BLOCKED,
    TUI_FILTER_REVIEW,
    TUI_FILTER_DOING,
    TUI_FILTER_DONE,
    TUI_FILTER_ATTENTION
} DispatchTuiFilter;

typedef struct {
    DispatchBoard board;
    int board_loaded;
    DispatchTuiScreen screen;
    DispatchTuiFilter filter;
    int group_filter;
    int actor_filter;
    int show_archived_agents;
    char actor[64];
    char status[256];
    char search[128];
    int search_active;
    int selected_task;
    int selected_agent;
    int selected_workspace;
    int selected_log;
    char log_filter_field[32];
    char log_filter_value[128];
    int show_help;
    int running;
} DispatchTui;

static DispatchTask *selected_visible_task(DispatchTui *tui);
static DispatchWorkspace *selected_visible_workspace(DispatchTui *tui);
static int parse_filter_name(const char *name, DispatchTuiFilter *filter);

static int title_starts_with_dispatch_id_like(const char *title) {
    if (!title)
        return 0;
    size_t i = 0;
    while (isalpha((unsigned char)title[i]) && i < 6)
        i++;
    if (i == 0 || title[i] != '-')
        return 0;
    i++;
    size_t digit_start = i;
    while (isdigit((unsigned char)title[i]))
        i++;
    return i > digit_start && isspace((unsigned char)title[i]);
}

static void tui_set_status(DispatchTui *tui, const char *message) {
    snprintf(tui->status, sizeof(tui->status), "%s", message ? message : "");
}

static void tui_init(DispatchTui *tui) {
    memset(tui, 0, sizeof(*tui));
    tui->filter = TUI_FILTER_NOT_DONE;
    tui->group_filter = -1;
    tui->actor_filter = -1;
    const char *actor = getenv("DISPATCH_ACTOR");
    snprintf(tui->actor, sizeof(tui->actor), "%s",
             actor && actor[0] ? actor : "user");
    tui->running = 1;
}

static void tui_free_board(DispatchTui *tui) {
    if (tui->board_loaded) {
        dispatch_board_free(&tui->board);
        tui->board_loaded = 0;
    }
}

static int tui_load_board(DispatchTui *tui) {
    char error[256] = {0};
    tui_free_board(tui);
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&tui->board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        tui_set_status(tui, error[0] ? error : "could not load board");
        return 0;
    }
    tui->board_loaded = 1;
    if (tui->group_filter >= 0 &&
        (size_t)tui->group_filter >= tui->board.groups.count)
        tui->group_filter = -1;
    if (tui->actor_filter >= 0 &&
        (size_t)tui->actor_filter >= tui->board.agents.count)
        tui->actor_filter = -1;
    tui_set_status(tui, "Ready");
    return 1;
}

static int task_count_for_state(const DispatchBoard *board,
                                DispatchState state) {
    int count = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        if (dispatch_task_effective_state(board, &board->tasks.items[i]) ==
            state) {
            count++;
        }
    }
    return count;
}

static const char *filter_name(DispatchTuiFilter filter) {
    switch (filter) {
    case TUI_FILTER_NOT_DONE:
        return "not-done";
    case TUI_FILTER_ALL:
        return "all";
    case TUI_FILTER_READY:
        return "ready";
    case TUI_FILTER_BLOCKED:
        return "blocked";
    case TUI_FILTER_REVIEW:
        return "review";
    case TUI_FILTER_DOING:
        return "doing";
    case TUI_FILTER_DONE:
        return "done";
    case TUI_FILTER_ATTENTION:
        return "attention";
    }
    return "not-done";
}

static int text_contains_casefold(const char *haystack, const char *needle) {
    if (!needle || needle[0] == '\0')
        return 1;
    if (!haystack)
        return 0;

    size_t needle_len = strlen(needle);
    for (size_t i = 0; haystack[i] != '\0'; i++) {
        size_t matched = 0;
        while (matched < needle_len && haystack[i + matched] != '\0' &&
               tolower((unsigned char)haystack[i + matched]) ==
                   tolower((unsigned char)needle[matched])) {
            matched++;
        }
        if (matched == needle_len)
            return 1;
    }
    return 0;
}

static const char *json_string_field(json_t *object, const char *name) {
    json_t *value = json_object_get(object, name);
    return json_is_string(value) ? json_string_value(value) : "";
}

static const char *json_nested_string_field(json_t *object, const char *parent,
                                            const char *name) {
    json_t *nested = json_object_get(object, parent);
    if (!json_is_object(nested))
        return "";
    return json_string_field(nested, name);
}

static int log_record_matches_filter(json_t *record, const char *field,
                                     const char *value) {
    if (!field || !field[0] || !value || !value[0])
        return 1;
    if (strcmp(field, "actor") == 0)
        return text_contains_casefold(json_string_field(record, "actor"), value);
    if (strcmp(field, "command") == 0)
        return text_contains_casefold(json_string_field(record, "command"), value);
    if (strcmp(field, "action") == 0)
        return text_contains_casefold(json_string_field(record, "action"), value);
    if (strcmp(field, "task") == 0)
        return text_contains_casefold(json_nested_string_field(record, "targets",
                                                              "task"),
                                      value);
    if (strcmp(field, "agent") == 0)
        return text_contains_casefold(json_nested_string_field(record, "targets",
                                                              "agent"),
                                      value) ||
               text_contains_casefold(json_string_field(record, "actor"), value);
    if (strcmp(field, "workspace") == 0)
        return text_contains_casefold(json_nested_string_field(record, "targets",
                                                              "workspace"),
                                      value);
    return 1;
}

static int task_matches_search(const DispatchTask *task, const char *search) {
    return text_contains_casefold(task->id, search) ||
           text_contains_casefold(task->title, search) ||
           text_contains_casefold(task->description, search) ||
           text_contains_casefold(task->assigned_to, search) ||
           text_contains_casefold(task->completed_by, search);
}

static int task_matches_filter(const DispatchBoard *board,
                               const DispatchTask *task,
                               DispatchTuiFilter filter) {
    DispatchState state = dispatch_task_effective_state(board, task);
    switch (filter) {
    case TUI_FILTER_ALL:
        return 1;
    case TUI_FILTER_NOT_DONE:
        return state != DISPATCH_STATE_DONE;
    case TUI_FILTER_READY:
        return state == DISPATCH_STATE_READY;
    case TUI_FILTER_BLOCKED:
        return state == DISPATCH_STATE_BLOCKED;
    case TUI_FILTER_REVIEW:
        return state == DISPATCH_STATE_REVIEW;
    case TUI_FILTER_DOING:
        return state == DISPATCH_STATE_DOING;
    case TUI_FILTER_DONE:
        return state == DISPATCH_STATE_DONE;
    case TUI_FILTER_ATTENTION:
        return state == DISPATCH_STATE_READY ||
               state == DISPATCH_STATE_REVIEW ||
               state == DISPATCH_STATE_PROPOSED;
    }
    return state != DISPATCH_STATE_DONE;
}

static const char *actor_filter_value(const DispatchBoard *board, int index) {
    if (index < 0 || (size_t)index >= board->agents.count)
        return NULL;
    return board->agents.items[index].name;
}

static int task_matches_secondary_filters(const DispatchTui *tui,
                                          const DispatchTask *task) {
    if (tui->group_filter >= 0) {
        if ((size_t)tui->group_filter >= tui->board.groups.count ||
            strcmp(task->group, tui->board.groups.items[tui->group_filter].id) !=
                0) {
            return 0;
        }
    }

    const char *actor = actor_filter_value(&tui->board, tui->actor_filter);
    if (actor) {
        int matches_actor =
            (task->assigned_to && strcmp(task->assigned_to, actor) == 0) ||
            (task->started_by && strcmp(task->started_by, actor) == 0) ||
            (task->completed_by && strcmp(task->completed_by, actor) == 0);
        if (!matches_actor)
            return 0;
    }
    return 1;
}

static int tui_task_is_visible(const DispatchTui *tui,
                               const DispatchTask *task) {
    return task_matches_filter(&tui->board, task, tui->filter) &&
           task_matches_secondary_filters(tui, task) &&
           task_matches_search(task, tui->search);
}

static int visible_task_count_for_tui(const DispatchTui *tui) {
    int count = 0;
    for (size_t i = 0; i < tui->board.tasks.count; i++) {
        if (tui_task_is_visible(tui, &tui->board.tasks.items[i]))
            count++;
    }
    return count;
}

static void clamp_selection(DispatchTui *tui) {
    int count = tui->board_loaded ? visible_task_count_for_tui(tui) : 0;
    if (count <= 0) {
        tui->selected_task = 0;
    } else if (tui->selected_task >= count) {
        tui->selected_task = count - 1;
    } else if (tui->selected_task < 0) {
        tui->selected_task = 0;
    }
}

static void clamp_agent_selection(DispatchTui *tui) {
    if (!tui->board_loaded || tui->board.agents.count == 0) {
        tui->selected_agent = 0;
    } else if ((size_t)tui->selected_agent >= tui->board.agents.count) {
        tui->selected_agent = (int)tui->board.agents.count - 1;
    } else if (tui->selected_agent < 0) {
        tui->selected_agent = 0;
    }
}

static int visible_workspace_count(const DispatchTui *tui) {
    if (!tui->board_loaded)
        return 0;
    int count = 0;
    for (size_t i = 0; i < tui->board.workspaces.count; i++) {
        if (tui->board.workspaces.items[i].state != DISPATCH_WORKSPACE_REMOVED)
            count++;
    }
    return count;
}

static void clamp_workspace_selection(DispatchTui *tui) {
    int count = visible_workspace_count(tui);
    if (count <= 0) {
        tui->selected_workspace = 0;
    } else if (tui->selected_workspace >= count) {
        tui->selected_workspace = count - 1;
    } else if (tui->selected_workspace < 0) {
        tui->selected_workspace = 0;
    }
}

static int visible_log_count(const DispatchTui *tui) {
    FILE *file = fopen(DISPATCH_LOG_FILE, "r");
    if (!file)
        return 0;

    int count = 0;
    char line[8192];
    while (fgets(line, sizeof(line), file)) {
        json_error_t error;
        json_t *record = json_loads(line, 0, &error);
        if (record) {
            if (log_record_matches_filter(record, tui->log_filter_field,
                                          tui->log_filter_value))
                count++;
            json_decref(record);
        }
    }
    fclose(file);
    return count;
}

static void clamp_log_selection(DispatchTui *tui) {
    int count = visible_log_count(tui);
    if (count <= 0) {
        tui->selected_log = 0;
    } else if (tui->selected_log >= count) {
        tui->selected_log = count - 1;
    } else if (tui->selected_log < 0) {
        tui->selected_log = 0;
    }
}

static DispatchAgent *selected_agent(DispatchTui *tui) {
    if (!tui->board_loaded || tui->board.agents.count == 0 ||
        tui->selected_agent < 0 ||
        (size_t)tui->selected_agent >= tui->board.agents.count)
        return NULL;
    return &tui->board.agents.items[tui->selected_agent];
}

static DispatchWorkspace *selected_visible_workspace(DispatchTui *tui) {
    if (!tui->board_loaded)
        return NULL;

    int visible_index = 0;
    for (size_t i = 0; i < tui->board.workspaces.count; i++) {
        DispatchWorkspace *workspace = &tui->board.workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
            continue;
        if (visible_index == tui->selected_workspace)
            return workspace;
        visible_index++;
    }
    return NULL;
}

static int select_task_by_id(DispatchTui *tui, const char *task_id) {
    tui->filter = TUI_FILTER_ALL;
    tui->group_filter = -1;
    tui->actor_filter = -1;
    tui->search[0] = '\0';
    int visible_index = 0;
    for (size_t i = 0; i < tui->board.tasks.count; i++) {
        DispatchTask *task = &tui->board.tasks.items[i];
        if (!tui_task_is_visible(tui, task))
            continue;
        if (strcmp(task->id, task_id) == 0) {
            tui->selected_task = visible_index;
            return 1;
        }
        visible_index++;
    }
    return 0;
}

static int select_agent_by_name(DispatchTui *tui, const char *name) {
    for (size_t i = 0; i < tui->board.agents.count; i++) {
        if (strcmp(tui->board.agents.items[i].name, name) == 0) {
            tui->selected_agent = (int)i;
            tui->show_archived_agents = 1;
            return 1;
        }
    }
    return 0;
}

static int select_workspace_by_id(DispatchTui *tui, const char *target) {
    int visible_index = 0;
    for (size_t i = 0; i < tui->board.workspaces.count; i++) {
        DispatchWorkspace *workspace = &tui->board.workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
            continue;
        if (strcmp(workspace->id, target) == 0 ||
            strcmp(workspace->task_id, target) == 0) {
            tui->selected_workspace = visible_index;
            return 1;
        }
        visible_index++;
    }
    return 0;
}

static int agent_is_visible(const DispatchTui *tui, const DispatchAgent *agent) {
    return tui->show_archived_agents || !agent->archived;
}

static void set_filter(DispatchTui *tui, DispatchTuiFilter filter) {
    tui->filter = filter;
    tui->selected_task = 0;
    char message[128];
    snprintf(message, sizeof(message), "Filter: %s", filter_name(filter));
    tui_set_status(tui, message);
}

static void cycle_group_filter(DispatchTui *tui) {
    if (!tui->board_loaded || tui->board.groups.count == 0) {
        tui->group_filter = -1;
        tui_set_status(tui, "No groups");
        return;
    }
    tui->group_filter++;
    if ((size_t)tui->group_filter >= tui->board.groups.count)
        tui->group_filter = -1;
    tui->selected_task = 0;
    tui_set_status(tui, tui->group_filter >= 0 ? "Group filter" : "All groups");
}

static void cycle_actor_filter(DispatchTui *tui) {
    if (!tui->board_loaded || tui->board.agents.count == 0) {
        tui->actor_filter = -1;
        tui_set_status(tui, "No agents");
        return;
    }
    tui->actor_filter++;
    if ((size_t)tui->actor_filter >= tui->board.agents.count)
        tui->actor_filter = -1;
    tui->selected_task = 0;
    tui_set_status(tui, tui->actor_filter >= 0 ? "Actor filter" : "All actors");
}

static void clear_secondary_filters(DispatchTui *tui) {
    tui->search[0] = '\0';
    tui->search_active = 0;
    tui->group_filter = -1;
    tui->actor_filter = -1;
    tui->selected_task = 0;
    tui_set_status(tui, "Filters cleared");
}

static char *tui_shell_quote(const char *value) {
    size_t len = strlen(value ? value : "");
    size_t size = len * 4 + 3;
    char *quoted = malloc(size);
    if (!quoted) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    size_t out = 0;
    quoted[out++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (value[i] == '\'') {
            memcpy(quoted + out, "'\\''", 4);
            out += 4;
        } else {
            quoted[out++] = value[i];
        }
    }
    quoted[out++] = '\'';
    quoted[out] = '\0';
    return quoted;
}

static char *diff_command_for_task(const DispatchBoard *board,
                                   const DispatchTask *task) {
    if (!task || task->commits.count == 0)
        return NULL;

    char *repo_q = tui_shell_quote(board->repo_path ? board->repo_path : ".");
    char *commit_q = tui_shell_quote(task->commits.items[0]);
    size_t size = strlen("git -C  show ") + strlen(repo_q) + strlen(commit_q) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, "git -C %s show %s", repo_q, commit_q);
    free(repo_q);
    free(commit_q);
    return command;
}

static int path_has_git_metadata(const char *path) {
    if (!path || !path[0])
        return 0;
    char *git_path = malloc(strlen(path) + strlen("/.git") + 1);
    if (!git_path) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    sprintf(git_path, "%s/.git", path);
    int present = access(git_path, F_OK) == 0;
    free(git_path);
    return present;
}

static int workspace_is_dirty(const DispatchWorkspace *workspace) {
    if (!workspace || !workspace->path || !path_has_git_metadata(workspace->path))
        return 0;

    char *path_q = tui_shell_quote(workspace->path);
    size_t size = strlen("git -C  status --porcelain 2>/dev/null") +
                  strlen(path_q) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, "git -C %s status --porcelain 2>/dev/null",
             path_q);
    free(path_q);

    FILE *pipe = popen(command, "r");
    free(command);
    if (!pipe)
        return 1;
    int ch = fgetc(pipe);
    int status = pclose(pipe);
    return ch != EOF || status != 0;
}

static void run_selected_task_diff(DispatchTui *tui) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        tui_set_status(tui, "No selected task");
        return;
    }
    char *command = diff_command_for_task(&tui->board, task);
    if (!command) {
        tui_set_status(tui, "No commit metadata for selected task");
        return;
    }

    def_prog_mode();
    endwin();
    int result = system(command);
    reset_prog_mode();
    refresh();

    char message[256];
    snprintf(message, sizeof(message), "Diff exited with status %d", result);
    tui_set_status(tui, message);
    free(command);
}

static char *editor_command_for_path(const char *path) {
    const char *editor = getenv("EDITOR");
    if (!editor || !editor[0])
        editor = "vi";
    char *editor_q = tui_shell_quote(editor);
    char *path_q = tui_shell_quote(path);
    size_t size = strlen(editor_q) + strlen(path_q) + 2;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, "%s %s", editor_q, path_q);
    free(editor_q);
    free(path_q);
    return command;
}

static void edit_selected_agent_prompt(DispatchTui *tui) {
    DispatchAgent *agent = selected_agent(tui);
    if (!agent) {
        tui_set_status(tui, "No selected agent");
        return;
    }
    if (!agent->prompt_path || access(agent->prompt_path, R_OK) != 0) {
        tui_set_status(tui, "Prompt file missing");
        return;
    }

    char *command = editor_command_for_path(agent->prompt_path);
    def_prog_mode();
    endwin();
    int result = system(command);
    reset_prog_mode();
    refresh();

    char message[256];
    snprintf(message, sizeof(message), "Editor exited with status %d", result);
    tui_set_status(tui, message);
    free(command);
    tui_load_board(tui);
}

static DispatchTask *selected_visible_task(DispatchTui *tui) {
    if (!tui->board_loaded)
        return NULL;

    int visible_index = 0;
    for (size_t i = 0; i < tui->board.tasks.count; i++) {
        DispatchTask *task = &tui->board.tasks.items[i];
        if (!tui_task_is_visible(tui, task))
            continue;
        if (visible_index == tui->selected_task)
            return task;
        visible_index++;
    }
    return NULL;
}

static DispatchWorkspace *workspace_for_task(DispatchBoard *board,
                                             const char *task_id) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (strcmp(workspace->task_id, task_id) == 0 &&
            workspace->state != DISPATCH_WORKSPACE_REMOVED)
            return workspace;
    }
    return NULL;
}

typedef enum {
    TUI_ACTION_READY,
    TUI_ACTION_START,
    TUI_ACTION_FINISH,
    TUI_ACTION_REVIEW
} DispatchTuiAction;

static int parse_action_name(const char *name, DispatchTuiAction *action);

static int mutate_task(const char *task_id, const char *actor,
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
    case TUI_ACTION_START:
        ok = dispatch_task_start(&board, task, actor);
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

static int create_group(const char *name, const char *prefix, char *message,
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

static int create_task(const char *group, const char *title,
                       const char *description, int requires_review,
                       const char *depends_text, char *message,
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

    DispatchTask *task =
        dispatch_board_add_task(&board, group, title, description ? description : "");
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

    snprintf(message, message_size, "Added task %s", task_id);
    dispatch_board_free(&board);
    dispatch_store_lock_release(&lock);
    return 1;
}

static int mutate_dependency(const char *dependency_id, const char *dependent_id,
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

static void run_selected_task_action(DispatchTui *tui,
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

static int prompt_line(const char *label, char *buffer, size_t buffer_size,
                       const char *initial) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (initial)
        snprintf(buffer, buffer_size, "%s", initial);
    else
        buffer[0] = '\0';

    echo();
    curs_set(1);
    attron(A_REVERSE);
    mvhline(rows - 1, 0, ' ', cols);
    mvprintw(rows - 1, 0, "%s", label);
    attroff(A_REVERSE);
    move(rows - 1, (int)strlen(label));
    int result = getnstr(buffer, (int)buffer_size - 1);
    noecho();
    curs_set(0);
    return result == OK;
}

static int prompt_yes_no(const char *label, int default_yes) {
    char value[16];
    if (!prompt_line(label, value, sizeof(value), default_yes ? "y" : "n"))
        return default_yes;
    return value[0] == '\0' || value[0] == 'y' || value[0] == 'Y';
}

static const char *selected_task_group(DispatchTui *tui) {
    DispatchTask *task = selected_visible_task(tui);
    if (task)
        return task->group;
    if (tui->group_filter >= 0 &&
        (size_t)tui->group_filter < tui->board.groups.count)
        return tui->board.groups.items[tui->group_filter].id;
    if (tui->board.groups.count > 0)
        return tui->board.groups.items[0].id;
    return "";
}

static void run_group_form(DispatchTui *tui) {
    char name[128];
    char prefix[16];
    if (!prompt_line("New group name: ", name, sizeof(name), ""))
        return;
    if (!prompt_line("Prefix (blank auto): ", prefix, sizeof(prefix), ""))
        return;

    char message[256] = {0};
    create_group(name, prefix, message, sizeof(message));
    tui_load_board(tui);
    tui_set_status(tui, message);
}

static void run_task_form(DispatchTui *tui) {
    char group[32];
    char title[256];
    char description[512];
    char deps[256];

    if (!prompt_line("Task group: ", group, sizeof(group),
                     selected_task_group(tui)))
        return;
    if (!prompt_line("Task title: ", title, sizeof(title), ""))
        return;
    if (!prompt_line("Description: ", description, sizeof(description), ""))
        return;
    int requires_review = prompt_yes_no("Requires review? [Y/n]: ", 1);
    if (!prompt_line("Depends on (comma/space IDs, blank none): ", deps,
                     sizeof(deps), ""))
        return;

    char message[256] = {0};
    create_task(group, title, description, requires_review, deps, message,
                sizeof(message));
    tui_load_board(tui);
    tui_set_status(tui, message);
    clamp_selection(tui);
}

static void run_dependency_prompt(DispatchTui *tui, int add) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        tui_set_status(tui, "No selected task");
        return;
    }

    char task_id[64];
    char dependency_id[64];
    snprintf(task_id, sizeof(task_id), "%s", task->id);
    if (!prompt_line(add ? "Add dependency ID: " : "Remove dependency ID: ",
                     dependency_id, sizeof(dependency_id), ""))
        return;

    char message[256] = {0};
    mutate_dependency(dependency_id, task_id, add, message, sizeof(message));
    tui_load_board(tui);
    tui_set_status(tui, message);
}

static void run_external_command_in_terminal(DispatchTui *tui,
                                             const char *command,
                                             const char *label) {
    def_prog_mode();
    endwin();
    int result = system(command);
    reset_prog_mode();
    refresh();

    char message[256];
    snprintf(message, sizeof(message), "%s exited with status %d", label,
             result);
    tui_load_board(tui);
    tui_set_status(tui, message);
}

static void run_workspace_create_form(DispatchTui *tui) {
    char task_id[64];
    char actor[64];
    if (!prompt_line("Workspace task ID: ", task_id, sizeof(task_id), ""))
        return;
    if (!prompt_line("Actor: ", actor, sizeof(actor), tui->actor))
        return;
    int sequence = prompt_yes_no("Create sequence workspace? [y/N]: ", 0);

    char *task_q = tui_shell_quote(task_id);
    char *actor_q = tui_shell_quote(actor);
    size_t size = strlen("./dispatch workspace create  --actor  --sequence") +
                  strlen(task_q) + strlen(actor_q) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, "./dispatch workspace create %s --actor %s%s",
             task_q, actor_q, sequence ? " --sequence" : "");
    free(task_q);
    free(actor_q);

    run_external_command_in_terminal(tui, command, "Workspace create");
    free(command);
    clamp_workspace_selection(tui);
}

static void run_workspace_remove_form(DispatchTui *tui, int force) {
    DispatchWorkspace *workspace = selected_visible_workspace(tui);
    if (!workspace) {
        tui_set_status(tui, "No selected workspace");
        return;
    }

    char confirm[64];
    char label[128];
    snprintf(label, sizeof(label), "Type %s to remove: ", workspace->task_id);
    if (!prompt_line(label, confirm, sizeof(confirm), ""))
        return;
    if (strcmp(confirm, workspace->task_id) != 0) {
        tui_set_status(tui, "Workspace removal cancelled");
        return;
    }

    char *target_q = tui_shell_quote(workspace->task_id);
    size_t size = strlen("./dispatch workspace remove  --force") +
                  strlen(target_q) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, "./dispatch workspace remove %s%s", target_q,
             force ? " --force" : "");
    free(target_q);

    run_external_command_in_terminal(tui, command, "Workspace remove");
    free(command);
    clamp_workspace_selection(tui);
}

static void run_workspace_prune_form(DispatchTui *tui) {
    char confirm[32];
    if (!prompt_line("Type prune to remove done clean workspaces: ", confirm,
                     sizeof(confirm), ""))
        return;
    if (strcmp(confirm, "prune") != 0) {
        tui_set_status(tui, "Workspace prune cancelled");
        return;
    }
    run_external_command_in_terminal(
        tui, "./dispatch workspace prune --done", "Workspace prune");
    clamp_workspace_selection(tui);
}

static void set_log_filter(DispatchTui *tui, const char *field,
                           const char *value) {
    snprintf(tui->log_filter_field, sizeof(tui->log_filter_field), "%s",
             field ? field : "");
    snprintf(tui->log_filter_value, sizeof(tui->log_filter_value), "%s",
             value ? value : "");
    tui->selected_log = 0;
    char message[256];
    if (tui->log_filter_field[0]) {
        snprintf(message, sizeof(message), "Log filter %s=%s",
                 tui->log_filter_field, tui->log_filter_value);
    } else {
        snprintf(message, sizeof(message), "Log filters cleared");
    }
    tui_set_status(tui, message);
}

static void run_log_filter_form(DispatchTui *tui) {
    char field[32];
    char value[128];
    if (!prompt_line("Filter field (actor/command/action/task/agent/workspace): ",
                     field, sizeof(field), tui->log_filter_field))
        return;
    if (!prompt_line("Filter value: ", value, sizeof(value),
                     tui->log_filter_value))
        return;
    set_log_filter(tui, field, value);
}

static void show_selected_task_logs(DispatchTui *tui) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        tui_set_status(tui, "No selected task");
        return;
    }
    set_log_filter(tui, "task", task->id);
    tui->screen = TUI_SCREEN_LOGS;
}

static void show_selected_agent_logs(DispatchTui *tui) {
    DispatchAgent *agent = selected_agent(tui);
    if (!agent) {
        tui_set_status(tui, "No selected agent");
        return;
    }
    set_log_filter(tui, "agent", agent->name);
    tui->screen = TUI_SCREEN_LOGS;
}

static int string_starts_with(const char *value, const char *prefix) {
    return !prefix || !prefix[0] ||
           strncmp(value, prefix, strlen(prefix)) == 0;
}

static void execute_palette_command(DispatchTui *tui, const char *command) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", command ? command : "");
    char *verb = strtok(copy, " \t");
    if (!verb) {
        tui_set_status(tui, "No command");
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

static void run_command_palette(DispatchTui *tui) {
    char command[512];
    if (!prompt_line(": ", command, sizeof(command), ""))
        return;
    execute_palette_command(tui, command);
}

static void print_palette_completion(const DispatchBoard *board,
                                     const char *input) {
    const char *commands[] = {"board",     "agents", "workspaces", "logs",
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

static int update_agent_session_metadata(const char *name,
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

static void clear_selected_agent_session(DispatchTui *tui) {
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

static int agent_has_live_workspace(DispatchBoard *board, const char *name) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace->state != DISPATCH_WORKSPACE_REMOVED &&
            strcmp(workspace->actor, name) == 0)
            return 1;
    }
    return 0;
}

static int set_agent_archived_state(const char *name, int archived,
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

static void toggle_selected_agent_archived(DispatchTui *tui) {
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

static void draw_truncated(int y, int x, int width, const char *text) {
    if (width <= 0)
        return;
    mvaddnstr(y, x, text ? text : "", width);
}

static void tui_render_help(void) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);

    const char *lines[] = {
        "Dispatch TUI Help",
        "",
        "q        quit",
        "?        toggle help",
        "u        reload board",
        "r/s/f/v  ready/start/finish/review selected task",
        "n/+      create task/group",
        ">/<      add/remove dependency in task inspector",
        "d        open selected task commit diff",
        "e        edit selected agent prompt",
        "Enter/i  inspect selected task",
        "Esc/q    close inspector",
        "Tab/a    switch to agents",
        "w        switch to workspaces",
        "l/L      logs / selected task or agent logs",
        ":        command palette",
        "/        search tasks",
        "Esc      clear search",
        "arrows   move selection",
        "j/k      move selection",
        "1-7/R    filter presets",
        "G/A      cycle group/actor filters",
        "c        clear search/group/actor filters",
        "",
        "This foundation screen validates terminal setup, refresh, resize, and",
        "clean shutdown. Board and agent views are implemented in follow-up tasks.",
        NULL,
    };

    int width = cols > 72 ? 72 : cols - 4;
    if (width < 20)
        width = cols;
    int height = 13;
    int top = rows > height ? (rows - height) / 2 : 0;
    int left = cols > width ? (cols - width) / 2 : 0;

    attron(A_REVERSE);
    for (int y = 0; y < height && top + y < rows; y++)
        mvhline(top + y, left, ' ', width);
    attroff(A_REVERSE);

    for (int i = 0; lines[i] && top + i + 1 < rows; i++) {
        attron(A_REVERSE);
        draw_truncated(top + i + 1, left + 2, width - 4, lines[i]);
        attroff(A_REVERSE);
    }
}

static void draw_agent_rows(DispatchTui *tui, int start_y, int rows, int cols) {
    int y = start_y;
    if (tui->board.agents.count == 0) {
        draw_truncated(y, 0, cols, "(no agents)");
        return;
    }

    draw_truncated(y++, 0, cols,
                   "Name             Runner   Status    Session  Current task  Last workspace");
    for (size_t i = 0; i < tui->board.agents.count && y < rows - 1; i++) {
        DispatchAgent *agent = &tui->board.agents.items[i];
        if (!agent_is_visible(tui, agent))
            continue;
        char line[1024];
        snprintf(line, sizeof(line), "%-16s %-8s %-9s %-8s %-13s %s",
                 agent->name, agent->runner,
                 agent->archived ? "archived" : "enabled",
                 agent->session_id ? "yes" : "no",
                 agent->current_task ? agent->current_task : "-",
                 agent->last_workspace ? agent->last_workspace : "-");
        if ((int)i == tui->selected_agent)
            attron(A_REVERSE);
        draw_truncated(y++, 0, cols, line);
        if ((int)i == tui->selected_agent)
            attroff(A_REVERSE);
    }
}

static void draw_workspace_rows(DispatchTui *tui, int start_y, int rows,
                                int cols) {
    int y = start_y;
    int visible_index = 0;
    if (visible_workspace_count(tui) == 0) {
        draw_truncated(y, 0, cols, "(no active workspaces)");
        return;
    }

    draw_truncated(y++, 0, cols,
                   "Task     Task state  Workspace  Actor       Dirty  Git      Branch / Path");
    for (size_t i = 0; i < tui->board.workspaces.count && y < rows - 1; i++) {
        DispatchWorkspace *workspace = &tui->board.workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
            continue;
        DispatchTask *task =
            dispatch_board_find_task(&tui->board, workspace->task_id);
        const char *task_state =
            task ? dispatch_state_name(dispatch_task_effective_state(&tui->board,
                                                                     task))
                 : "missing";
        int git_present = path_has_git_metadata(workspace->path);
        int dirty = workspace_is_dirty(workspace);
        char line[1200];
        snprintf(line, sizeof(line), "%-8s %-11s %-10s %-11s %-6s %-8s %s  %s",
                 workspace->task_id, task_state,
                 dispatch_workspace_state_name(workspace->state),
                 workspace->actor, dirty ? "yes" : "no",
                 git_present ? "present" : "missing", workspace->branch,
                 workspace->path);
        if (visible_index == tui->selected_workspace)
            attron(A_REVERSE);
        draw_truncated(y++, 0, cols, line);
        if (visible_index == tui->selected_workspace)
            attroff(A_REVERSE);
        visible_index++;
    }
}

static void draw_log_rows(DispatchTui *tui, int start_y, int rows, int cols) {
    int y = start_y;
    FILE *file = fopen(DISPATCH_LOG_FILE, "r");
    if (!file) {
        draw_truncated(y, 0, cols, "(no dispatch.log)");
        return;
    }

    int visible_index = 0;
    int any_visible = 0;
    char line[8192];
    while (fgets(line, sizeof(line), file) && y < rows - 1) {
        json_error_t error;
        json_t *record = json_loads(line, 0, &error);
        if (!record)
            continue;
        if (!log_record_matches_filter(record, tui->log_filter_field,
                                       tui->log_filter_value)) {
            json_decref(record);
            continue;
        }

        const char *time = json_string_field(record, "timestamp");
        const char *actor = json_string_field(record, "actor");
        const char *command = json_string_field(record, "command");
        const char *action = json_string_field(record, "action");
        const char *message = json_string_field(record, "message");
        const char *task = json_nested_string_field(record, "targets", "task");
        const char *agent = json_nested_string_field(record, "targets", "agent");
        const char *workspace =
            json_nested_string_field(record, "targets", "workspace");
        char target[256] = "";
        if (task[0])
            snprintf(target, sizeof(target), " task:%s", task);
        else if (agent[0])
            snprintf(target, sizeof(target), " agent:%s", agent);
        else if (workspace[0])
            snprintf(target, sizeof(target), " workspace:%s", workspace);

        char row[1400];
        snprintf(row, sizeof(row), "%s %-12s %-10s %-18s%s  %s", time,
                 actor, command, action, target, message);
        if (visible_index == tui->selected_log)
            attron(A_REVERSE);
        draw_truncated(y++, 0, cols, row);
        if (visible_index == tui->selected_log)
            attroff(A_REVERSE);
        visible_index++;
        any_visible = 1;
        json_decref(record);
    }
    fclose(file);
    if (!any_visible)
        draw_truncated(y, 0, cols, "(no matching log records)");
}

static int draw_line(int y, int rows, int cols, const char *label,
                     const char *value) {
    if (y >= rows - 1)
        return y;
    char line[1024];
    snprintf(line, sizeof(line), "%s%s%s", label ? label : "",
             label && label[0] ? " " : "", value ? value : "-");
    draw_truncated(y, 0, cols, line);
    return y + 1;
}

static int draw_string_list(int y, int rows, int cols, const char *label,
                            const DispatchStringList *list) {
    if (list->count == 0)
        return draw_line(y, rows, cols, label, "-");

    char line[1024];
    snprintf(line, sizeof(line), "%s", label);
    size_t used = strlen(line);
    for (size_t i = 0; i < list->count && used + 2 < sizeof(line); i++) {
        int written = snprintf(line + used, sizeof(line) - used, "%s%s",
                               i == 0 ? " " : ",", list->items[i]);
        if (written < 0)
            break;
        used += (size_t)written;
    }
    return draw_line(y, rows, cols, "", line);
}

static int draw_blocked_by(int y, int rows, int cols, DispatchBoard *board,
                           const char *task_id) {
    char line[1024] = "Blocks:";
    size_t used = strlen(line);
    int count = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *candidate = &board->tasks.items[i];
        for (size_t dep = 0; dep < candidate->depends_on.count; dep++) {
            if (strcmp(candidate->depends_on.items[dep], task_id) != 0)
                continue;
            int written = snprintf(line + used, sizeof(line) - used, " %s",
                                   candidate->id);
            if (written > 0)
                used += (size_t)written;
            count++;
            break;
        }
    }
    if (count == 0)
        snprintf(line, sizeof(line), "Blocks: -");
    return draw_line(y, rows, cols, "", line);
}

static void draw_task_inspector(DispatchTui *tui, int rows, int cols) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        draw_truncated(2, 0, cols, "No selected task.");
        return;
    }

    char line[1024];
    int y = 2;
    snprintf(line, sizeof(line), "%s  %s", task->id, task->title);
    attron(A_BOLD);
    y = draw_line(y, rows, cols, "", line);
    attroff(A_BOLD);

    y = draw_line(y + 1, rows, cols, "Description:", task->description);
    y = draw_line(y, rows, cols, "Group:", task->group);
    y = draw_line(y, rows, cols, "State:",
                  dispatch_state_name(dispatch_task_effective_state(
                      &tui->board, task)));
    y = draw_line(y, rows, cols, "Requires review:",
                  task->requires_review ? "yes" : "no");
    y = draw_line(y, rows, cols, "Assigned to:",
                  task->assigned_to ? task->assigned_to : "-");
    y = draw_line(y, rows, cols, "Started by:",
                  task->started_by ? task->started_by : "-");
    y = draw_line(y, rows, cols, "Completed by:",
                  task->completed_by ? task->completed_by : "-");
    y = draw_string_list(y, rows, cols, "Depends on:", &task->depends_on);
    y = draw_blocked_by(y, rows, cols, &tui->board, task->id);
    y = draw_string_list(y, rows, cols, "Commits:", &task->commits);

    DispatchWorkspace *workspace = workspace_for_task(&tui->board, task->id);
    if (workspace) {
        y = draw_line(y + 1, rows, cols, "Workspace actor:", workspace->actor);
        y = draw_line(y, rows, cols, "Workspace path:", workspace->path);
        y = draw_line(y, rows, cols, "Workspace branch:", workspace->branch);
    }

    y = draw_line(y + 1, rows, cols, "History:", "");
    if (task->history.count == 0) {
        y = draw_line(y, rows, cols, "  -", "");
    } else {
        for (size_t i = 0; i < task->history.count && y < rows - 1; i++) {
            DispatchHistoryEntry *entry = &task->history.items[i];
            snprintf(line, sizeof(line), "  %s by %s%s%s", entry->action,
                     entry->actor,
                     entry->note && entry->note[0] ? ": " : "",
                     entry->note && entry->note[0] ? entry->note : "");
            y = draw_line(y, rows, cols, "", line);
        }
    }
}

static void draw_agent_inspector(DispatchTui *tui, int rows, int cols) {
    DispatchAgent *agent = selected_agent(tui);
    if (!agent) {
        draw_truncated(2, 0, cols, "No selected agent.");
        return;
    }

    char line[1024];
    int y = 2;
    snprintf(line, sizeof(line), "%s  %s", agent->name, agent->runner);
    attron(A_BOLD);
    y = draw_line(y, rows, cols, "", line);
    attroff(A_BOLD);

    y = draw_line(y + 1, rows, cols, "Status:",
                  agent->archived ? "archived" : "enabled");
    y = draw_line(y, rows, cols, "Model:", agent->model ? agent->model : "-");
    y = draw_line(y, rows, cols, "Prompt:", agent->prompt_path);
    y = draw_line(y, rows, cols, "Run script:",
                  agent->run_script_path ? agent->run_script_path : "-");
    y = draw_line(y, rows, cols, "Session ID:",
                  agent->session_id ? agent->session_id : "-");
    y = draw_line(y, rows, cols, "Current task:",
                  agent->current_task ? agent->current_task : "-");
    y = draw_line(y, rows, cols, "Last workspace:",
                  agent->last_workspace ? agent->last_workspace : "-");
    if (strcmp(agent->runner, "codex") == 0) {
        y = draw_line(y, rows, cols, "Codex session:",
                      "manual metadata; use dispatch agent session");
    }

    y = draw_line(y + 1, rows, cols, "Recent completed tasks:", "");
    int shown = 0;
    for (size_t i = tui->board.tasks.count; i > 0 && y < rows - 1; i--) {
        DispatchTask *task = &tui->board.tasks.items[i - 1];
        if (!task->completed_by || strcmp(task->completed_by, agent->name) != 0)
            continue;
        snprintf(line, sizeof(line), "  %s %s", task->id, task->title);
        y = draw_line(y, rows, cols, "", line);
        shown++;
        if (shown == 5)
            break;
    }
    if (shown == 0)
        draw_line(y, rows, cols, "  -", "");
}

static void draw_workspace_inspector(DispatchTui *tui, int rows, int cols) {
    DispatchWorkspace *workspace = selected_visible_workspace(tui);
    if (!workspace) {
        draw_truncated(2, 0, cols, "No selected workspace.");
        return;
    }

    DispatchTask *task = dispatch_board_find_task(&tui->board,
                                                  workspace->task_id);
    const char *task_state =
        task ? dispatch_state_name(dispatch_task_effective_state(&tui->board,
                                                                 task))
             : "missing";
    int y = 2;
    char line[1024];
    snprintf(line, sizeof(line), "%s  %s", workspace->task_id,
             workspace->branch);
    attron(A_BOLD);
    y = draw_line(y, rows, cols, "", line);
    attroff(A_BOLD);

    y = draw_line(y + 1, rows, cols, "Task state:", task_state);
    y = draw_line(y, rows, cols, "Workspace state:",
                  dispatch_workspace_state_name(workspace->state));
    y = draw_line(y, rows, cols, "Actor:", workspace->actor);
    y = draw_line(y, rows, cols, "Path:", workspace->path);
    y = draw_line(y, rows, cols, "Repo:", workspace->repo_path);
    y = draw_line(y, rows, cols, "Branch:", workspace->branch);
    y = draw_line(y, rows, cols, "Git worktree:",
                  path_has_git_metadata(workspace->path) ? "present" : "missing");
    y = draw_line(y, rows, cols, "Dirty:",
                  workspace_is_dirty(workspace) ? "yes" : "no");
    y = draw_string_list(y + 1, rows, cols, "Sequence tasks:",
                         &workspace->sequence_tasks);
    y = draw_line(y, rows, cols, "Review gate:",
                  workspace->review_gate ? workspace->review_gate : "-");
}

static void draw_board_rows(DispatchTui *tui, int start_y, int rows, int cols) {
    int y = start_y;
    int visible_index = 0;
    int any_visible = 0;

    for (size_t g = 0; g < tui->board.groups.count && y < rows - 1; g++) {
        DispatchGroup *group = &tui->board.groups.items[g];
        int group_has_visible = 0;
        for (size_t i = 0; i < tui->board.tasks.count; i++) {
            DispatchTask *task = &tui->board.tasks.items[i];
            if (strcmp(task->group, group->id) == 0 &&
                tui_task_is_visible(tui, task)) {
                group_has_visible = 1;
                break;
            }
        }
        if (!group_has_visible)
            continue;

        char heading[256];
        snprintf(heading, sizeof(heading), "[%s] %s", group->prefix,
                 group->name);
        attron(A_BOLD);
        draw_truncated(y++, 0, cols, heading);
        attroff(A_BOLD);

        for (size_t i = 0; i < tui->board.tasks.count && y < rows - 1; i++) {
            DispatchTask *task = &tui->board.tasks.items[i];
            if (strcmp(task->group, group->id) != 0 ||
                !tui_task_is_visible(tui, task)) {
                continue;
            }

            DispatchState state = dispatch_task_effective_state(&tui->board, task);
            char meta[256] = {0};
            if (task->assigned_to) {
                snprintf(meta, sizeof(meta), " actor:%s", task->assigned_to);
            } else if (task->completed_by) {
                snprintf(meta, sizeof(meta), " by:%s", task->completed_by);
            }
            char line[1024];
            snprintf(line, sizeof(line), "  %-8s %-8s %s%s%s%s",
                     task->id, dispatch_state_name(state), task->title,
                     task->requires_review ? " review:yes" : "",
                     task->commits.count > 0 ? " commits" : "", meta);

            if (visible_index == tui->selected_task)
                attron(A_REVERSE);
            draw_truncated(y++, 0, cols, line);
            if (visible_index == tui->selected_task)
                attroff(A_REVERSE);
            visible_index++;
            any_visible = 1;
        }
    }

    if (!any_visible && y < rows - 1) {
        draw_truncated(y, 0, cols,
                       tui->search[0] ? "(no matching not-done tasks)"
                                      : "(no not-done tasks)");
    }
}

static void tui_render(DispatchTui *tui) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    erase();
    clamp_selection(tui);

    attron(A_BOLD);
    draw_truncated(0, 0, cols,
                   tui->screen == TUI_SCREEN_TASK_INSPECTOR
                       ? "Dispatch TUI - Task"
                       : tui->screen == TUI_SCREEN_AGENT_INSPECTOR
                             ? "Dispatch TUI - Agent"
                       : tui->screen == TUI_SCREEN_TASK_FORM
                             ? "Dispatch TUI - New Task"
                       : tui->screen == TUI_SCREEN_GROUP_FORM
                             ? "Dispatch TUI - New Group"
                       : tui->screen == TUI_SCREEN_WORKSPACE_INSPECTOR
                             ? "Dispatch TUI - Workspace"
                       : tui->screen == TUI_SCREEN_WORKSPACES
                             ? "Dispatch TUI - Workspaces"
                       : tui->screen == TUI_SCREEN_LOGS
                             ? "Dispatch TUI - Logs"
                       : tui->screen == TUI_SCREEN_AGENTS
                             ? "Dispatch TUI - Agents"
                             : "Dispatch TUI - Board");
    attroff(A_BOLD);

    if (!tui->board_loaded) {
        draw_truncated(2, 0, cols, "Board not loaded.");
    } else if (tui->screen == TUI_SCREEN_TASK_INSPECTOR) {
        draw_task_inspector(tui, rows, cols);
    } else if (tui->screen == TUI_SCREEN_AGENT_INSPECTOR) {
        draw_agent_inspector(tui, rows, cols);
    } else if (tui->screen == TUI_SCREEN_WORKSPACE_INSPECTOR) {
        draw_workspace_inspector(tui, rows, cols);
    } else if (tui->screen == TUI_SCREEN_WORKSPACES) {
        char line[512];
        snprintf(line, sizeof(line),
                 "Workspaces: %d active    n create    x remove    X force remove    P prune done",
                 visible_workspace_count(tui));
        draw_truncated(2, 0, cols, line);
        draw_workspace_rows(tui, 4, rows, cols);
    } else if (tui->screen == TUI_SCREEN_LOGS) {
        char line[512];
        snprintf(line, sizeof(line), "Logs: %d visible%s%s%s    F filter    C clear",
                 visible_log_count(tui),
                 tui->log_filter_field[0] ? "  " : "",
                 tui->log_filter_field[0] ? tui->log_filter_field : "",
                 tui->log_filter_field[0] ? "=" : "");
        size_t used = strlen(line);
        if (tui->log_filter_field[0])
            snprintf(line + used, sizeof(line) - used, "%s",
                     tui->log_filter_value);
        draw_truncated(2, 0, cols, line);
        draw_log_rows(tui, 4, rows, cols);
    } else if (tui->screen == TUI_SCREEN_AGENTS) {
        char line[512];
        snprintf(line, sizeof(line),
                 "Agents: %zu    view:%s    A toggle all    z archive/restore",
                 tui->board.agents.count,
                 tui->show_archived_agents ? "all" : "enabled");
        draw_truncated(2, 0, cols, line);
        draw_agent_rows(tui, 4, rows, cols);
    } else {
        char line[512];
        snprintf(line, sizeof(line), "Board: %s    Repo: %s", tui->board.name,
                 tui->board.repo_path ? tui->board.repo_path : ".");
        draw_truncated(2, 0, cols, line);

        int visible = visible_task_count_for_tui(tui);
        snprintf(line, sizeof(line),
                 "Tasks: %zu total  visible:%d  ready:%d  doing:%d  review:%d  blocked:%d  done:%d",
                 tui->board.tasks.count,
                 visible,
                 task_count_for_state(&tui->board, DISPATCH_STATE_READY),
                 task_count_for_state(&tui->board, DISPATCH_STATE_DOING),
                 task_count_for_state(&tui->board, DISPATCH_STATE_REVIEW),
                 task_count_for_state(&tui->board, DISPATCH_STATE_BLOCKED),
                 task_count_for_state(&tui->board, DISPATCH_STATE_DONE));
        draw_truncated(4, 0, cols, line);

        snprintf(line, sizeof(line),
                 "Filter: %s%s%s%s%s%s%s%s    Groups: %zu    Agents: %zu    Workspaces: %zu",
                 filter_name(tui->filter),
                 tui->group_filter >= 0 ? " group:" : "",
                 tui->group_filter >= 0
                     ? tui->board.groups.items[tui->group_filter].prefix
                     : "",
                 actor_filter_value(&tui->board, tui->actor_filter) ? " actor:" : "",
                 actor_filter_value(&tui->board, tui->actor_filter)
                     ? actor_filter_value(&tui->board, tui->actor_filter)
                     : "",
                 tui->search[0] ? " search:" : "",
                 tui->search[0] ? tui->search : "",
                 tui->search_active ? "_" : "", tui->board.groups.count,
                 tui->board.agents.count, tui->board.workspaces.count);
        draw_truncated(5, 0, cols, line);

        draw_board_rows(tui, 7, rows, cols);
    }

    if (rows > 1) {
        attron(A_REVERSE);
        mvhline(rows - 1, 0, ' ', cols);
        char status[512];
        snprintf(status, sizeof(status),
                tui->screen == TUI_SCREEN_TASK_INSPECTOR
                     ? " %s | q/Esc back | > add dep | < remove dep | d diff"
                     : tui->screen == TUI_SCREEN_AGENT_INSPECTOR
                           ? " %s | q/Esc back | e edit prompt | x clear session"
                     : tui->screen == TUI_SCREEN_WORKSPACE_INSPECTOR
                           ? " %s | q/Esc back | x remove | X force remove"
                     : tui->screen == TUI_SCREEN_WORKSPACES
                           ? " %s | b board | a agents | n create | x/X remove | P prune"
                     : tui->screen == TUI_SCREEN_LOGS
                           ? " %s | b board | F filter | C clear | j/k move"
                     : tui->screen == TUI_SCREEN_AGENTS
                           ? " %s | A all/enabled | z archive/restore | Enter/i inspect"
                           : " %s | : palette | Tab/a agents | w workspaces | n task | + group",
                 tui->status[0] ? tui->status : "Ready");
        draw_truncated(rows - 1, 0, cols, status);
        attroff(A_REVERSE);
    }

    if (tui->show_help)
        tui_render_help();

    refresh();
}

static int tui_run(void) {
    DispatchTui tui;
    tui_init(&tui);
    tui_load_board(&tui);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
    }

    while (tui.running) {
        tui_render(&tui);
        int ch = getch();
        switch (ch) {
        case 'q':
            if (tui.screen == TUI_SCREEN_TASK_INSPECTOR)
                tui.screen = TUI_SCREEN_BOARD;
            else if (tui.screen == TUI_SCREEN_AGENT_INSPECTOR)
                tui.screen = TUI_SCREEN_AGENTS;
            else if (tui.screen == TUI_SCREEN_WORKSPACE_INSPECTOR)
                tui.screen = TUI_SCREEN_WORKSPACES;
            else
                tui.running = 0;
            break;
        case '\t':
            tui.screen = tui.screen == TUI_SCREEN_AGENTS ? TUI_SCREEN_BOARD
                                                         : TUI_SCREEN_AGENTS;
            break;
        case 'a':
            tui.screen = TUI_SCREEN_AGENTS;
            break;
        case 'b':
            tui.screen = TUI_SCREEN_BOARD;
            break;
        case 'w':
            tui.screen = TUI_SCREEN_WORKSPACES;
            break;
        case 'l':
            tui.screen = TUI_SCREEN_LOGS;
            break;
        case 'L':
            if (tui.screen == TUI_SCREEN_TASK_INSPECTOR)
                show_selected_task_logs(&tui);
            else if (tui.screen == TUI_SCREEN_AGENT_INSPECTOR)
                show_selected_agent_logs(&tui);
            else
                tui.screen = TUI_SCREEN_LOGS;
            break;
        case '\n':
        case KEY_ENTER:
        case 'i':
            if (tui.screen == TUI_SCREEN_BOARD && selected_visible_task(&tui)) {
                tui.screen = TUI_SCREEN_TASK_INSPECTOR;
                tui_set_status(&tui, "Inspecting task");
            } else if (tui.screen == TUI_SCREEN_AGENTS && selected_agent(&tui)) {
                tui.screen = TUI_SCREEN_AGENT_INSPECTOR;
                tui_set_status(&tui, "Inspecting agent");
            } else if (tui.screen == TUI_SCREEN_WORKSPACES &&
                       selected_visible_workspace(&tui)) {
                tui.screen = TUI_SCREEN_WORKSPACE_INSPECTOR;
                tui_set_status(&tui, "Inspecting workspace");
            }
            break;
        case '?':
            tui.show_help = !tui.show_help;
            break;
        case ':':
            run_command_palette(&tui);
            break;
        case '1':
            set_filter(&tui, TUI_FILTER_NOT_DONE);
            break;
        case '2':
            set_filter(&tui, TUI_FILTER_ALL);
            break;
        case '3':
            set_filter(&tui, TUI_FILTER_READY);
            break;
        case '4':
            set_filter(&tui, TUI_FILTER_BLOCKED);
            break;
        case '5':
            set_filter(&tui, TUI_FILTER_REVIEW);
            break;
        case '6':
            set_filter(&tui, TUI_FILTER_DOING);
            break;
        case '7':
            set_filter(&tui, TUI_FILTER_DONE);
            break;
        case 'R':
            set_filter(&tui, TUI_FILTER_ATTENTION);
            break;
        case 'r':
            run_selected_task_action(&tui, TUI_ACTION_READY);
            break;
        case 'n':
            if (tui.screen == TUI_SCREEN_BOARD) {
                tui.screen = TUI_SCREEN_TASK_FORM;
                run_task_form(&tui);
                tui.screen = TUI_SCREEN_BOARD;
            } else if (tui.screen == TUI_SCREEN_WORKSPACES) {
                run_workspace_create_form(&tui);
            }
            break;
        case '+':
            if (tui.screen == TUI_SCREEN_BOARD) {
                tui.screen = TUI_SCREEN_GROUP_FORM;
                run_group_form(&tui);
                tui.screen = TUI_SCREEN_BOARD;
            }
            break;
        case '>':
            if (tui.screen == TUI_SCREEN_TASK_INSPECTOR)
                run_dependency_prompt(&tui, 1);
            break;
        case '<':
            if (tui.screen == TUI_SCREEN_TASK_INSPECTOR)
                run_dependency_prompt(&tui, 0);
            break;
        case 's':
            run_selected_task_action(&tui, TUI_ACTION_START);
            break;
        case 'f':
            run_selected_task_action(&tui, TUI_ACTION_FINISH);
            break;
        case 'v':
            run_selected_task_action(&tui, TUI_ACTION_REVIEW);
            break;
        case 'd':
            run_selected_task_diff(&tui);
            break;
        case 'x':
            if (tui.screen == TUI_SCREEN_AGENT_INSPECTOR)
                clear_selected_agent_session(&tui);
            else if (tui.screen == TUI_SCREEN_WORKSPACES ||
                     tui.screen == TUI_SCREEN_WORKSPACE_INSPECTOR)
                run_workspace_remove_form(&tui, 0);
            break;
        case 'X':
            if (tui.screen == TUI_SCREEN_WORKSPACES ||
                tui.screen == TUI_SCREEN_WORKSPACE_INSPECTOR)
                run_workspace_remove_form(&tui, 1);
            break;
        case 'P':
            if (tui.screen == TUI_SCREEN_WORKSPACES)
                run_workspace_prune_form(&tui);
            break;
        case 'e':
            if (tui.screen == TUI_SCREEN_AGENT_INSPECTOR)
                edit_selected_agent_prompt(&tui);
            break;
        case 'z':
            if (tui.screen == TUI_SCREEN_AGENTS ||
                tui.screen == TUI_SCREEN_AGENT_INSPECTOR)
                toggle_selected_agent_archived(&tui);
            break;
        case 'G':
            cycle_group_filter(&tui);
            break;
        case 'F':
            if (tui.screen == TUI_SCREEN_LOGS)
                run_log_filter_form(&tui);
            break;
        case 'A':
            if (tui.screen == TUI_SCREEN_AGENTS) {
                tui.show_archived_agents = !tui.show_archived_agents;
                tui_set_status(&tui, tui.show_archived_agents
                                        ? "Showing all agents"
                                        : "Showing enabled agents");
            } else {
                cycle_actor_filter(&tui);
            }
            break;
        case 'c':
            clear_secondary_filters(&tui);
            break;
        case 'C':
            if (tui.screen == TUI_SCREEN_LOGS)
                set_log_filter(&tui, "", "");
            break;
        case '/':
            tui.search_active = 1;
            tui_set_status(&tui, "Search");
            break;
        case 27:
            if (tui.screen == TUI_SCREEN_TASK_INSPECTOR) {
                tui.screen = TUI_SCREEN_BOARD;
                tui_set_status(&tui, "Board");
            } else if (tui.screen == TUI_SCREEN_AGENT_INSPECTOR) {
                tui.screen = TUI_SCREEN_AGENTS;
                tui_set_status(&tui, "Agents");
            } else if (tui.screen == TUI_SCREEN_WORKSPACE_INSPECTOR) {
                tui.screen = TUI_SCREEN_WORKSPACES;
                tui_set_status(&tui, "Workspaces");
            } else {
                tui.search_active = 0;
                tui.search[0] = '\0';
                tui.selected_task = 0;
                tui_set_status(&tui, "Search cleared");
            }
            break;
        case KEY_UP:
        case 'k':
            if (tui.screen == TUI_SCREEN_AGENTS) {
                tui.selected_agent--;
                clamp_agent_selection(&tui);
            } else if (tui.screen == TUI_SCREEN_WORKSPACES) {
                tui.selected_workspace--;
                clamp_workspace_selection(&tui);
            } else if (tui.screen == TUI_SCREEN_LOGS) {
                tui.selected_log--;
                clamp_log_selection(&tui);
            } else {
                tui.selected_task--;
                clamp_selection(&tui);
            }
            break;
        case KEY_DOWN:
        case 'j':
            if (tui.screen == TUI_SCREEN_AGENTS) {
                tui.selected_agent++;
                clamp_agent_selection(&tui);
            } else if (tui.screen == TUI_SCREEN_WORKSPACES) {
                tui.selected_workspace++;
                clamp_workspace_selection(&tui);
            } else if (tui.screen == TUI_SCREEN_LOGS) {
                tui.selected_log++;
                clamp_log_selection(&tui);
            } else {
                tui.selected_task++;
                clamp_selection(&tui);
            }
            break;
        case 'u':
            tui_load_board(&tui);
            clamp_selection(&tui);
            clamp_agent_selection(&tui);
            clamp_workspace_selection(&tui);
            clamp_log_selection(&tui);
            break;
        case KEY_RESIZE:
            tui_set_status(&tui, "Resized");
            break;
        case KEY_BACKSPACE:
        case 127:
        case '\b':
            if (tui.search_active && strlen(tui.search) > 0) {
                tui.search[strlen(tui.search) - 1] = '\0';
                tui.selected_task = 0;
            }
            break;
        default:
            if (tui.search_active && ch >= 0 && ch < 256 &&
                isprint((unsigned char)ch)) {
                size_t len = strlen(tui.search);
                if (len + 1 < sizeof(tui.search)) {
                    tui.search[len] = (char)ch;
                    tui.search[len + 1] = '\0';
                    tui.selected_task = 0;
                }
            }
            break;
        }
    }

    endwin();
    tui_free_board(&tui);
    return 0;
}

static int tui_smoke(void) {
    DispatchTui tui;
    tui_init(&tui);
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&tui.board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }
    tui.board_loaded = 1;

    printf("dispatch tui smoke ok: %zu tasks, %d visible, %zu groups, %zu agents, %zu workspaces\n",
           tui.board.tasks.count, visible_task_count_for_tui(&tui),
           tui.board.groups.count, tui.board.agents.count,
           tui.board.workspaces.count);
    tui_free_board(&tui);
    return 0;
}

static int tui_inspect_smoke(const char *task_id) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui inspect smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task) {
        fprintf(stderr, "No task with id %s\n", task_id);
        dispatch_board_free(&board);
        return 1;
    }

    printf("Task: %s\n", task->id);
    printf("Title: %s\n", task->title);
    printf("State: %s\n",
           dispatch_state_name(dispatch_task_effective_state(&board, task)));
    printf("Requires review: %s\n", task->requires_review ? "yes" : "no");
    printf("Depends on: %zu\n", task->depends_on.count);
    printf("Commits: %zu\n", task->commits.count);
    for (size_t i = 0; i < task->commits.count; i++)
        printf("Commit: %s\n", task->commits.items[i]);
    printf("History: %zu\n", task->history.count);
    DispatchWorkspace *workspace = workspace_for_task(&board, task->id);
    printf("Workspace: %s\n", workspace ? workspace->id : "-");

    dispatch_board_free(&board);
    return 0;
}

static int parse_filter_name(const char *name, DispatchTuiFilter *filter) {
    if (strcmp(name, "not-done") == 0) {
        *filter = TUI_FILTER_NOT_DONE;
    } else if (strcmp(name, "all") == 0) {
        *filter = TUI_FILTER_ALL;
    } else if (strcmp(name, "ready") == 0) {
        *filter = TUI_FILTER_READY;
    } else if (strcmp(name, "blocked") == 0) {
        *filter = TUI_FILTER_BLOCKED;
    } else if (strcmp(name, "review") == 0) {
        *filter = TUI_FILTER_REVIEW;
    } else if (strcmp(name, "doing") == 0) {
        *filter = TUI_FILTER_DOING;
    } else if (strcmp(name, "done") == 0) {
        *filter = TUI_FILTER_DONE;
    } else if (strcmp(name, "attention") == 0) {
        *filter = TUI_FILTER_ATTENTION;
    } else {
        return 0;
    }
    return 1;
}

static int tui_filter_smoke(const char *filter_name_arg) {
    DispatchTui tui;
    tui_init(&tui);
    if (!parse_filter_name(filter_name_arg, &tui.filter)) {
        fprintf(stderr, "Unknown TUI filter %s\n", filter_name_arg);
        return 1;
    }

    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&tui.board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui filter smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }
    tui.board_loaded = 1;

    printf("Filter: %s\n", filter_name(tui.filter));
    printf("Visible: %d\n", visible_task_count_for_tui(&tui));
    tui_free_board(&tui);
    return 0;
}

static int parse_action_name(const char *name, DispatchTuiAction *action) {
    if (strcmp(name, "ready") == 0) {
        *action = TUI_ACTION_READY;
    } else if (strcmp(name, "start") == 0) {
        *action = TUI_ACTION_START;
    } else if (strcmp(name, "finish") == 0) {
        *action = TUI_ACTION_FINISH;
    } else if (strcmp(name, "review") == 0) {
        *action = TUI_ACTION_REVIEW;
    } else {
        return 0;
    }
    return 1;
}

static int tui_action_smoke(const char *action_name, const char *task_id,
                            const char *actor) {
    DispatchTuiAction action;
    if (!parse_action_name(action_name, &action)) {
        fprintf(stderr, "Unknown TUI action %s\n", action_name);
        return 1;
    }

    char message[256] = {0};
    if (!mutate_task(task_id, actor && actor[0] ? actor : "user", action,
                     message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int tui_diff_smoke(const char *task_id) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui diff smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task) {
        fprintf(stderr, "No task with id %s\n", task_id);
        dispatch_board_free(&board);
        return 1;
    }

    char *command = diff_command_for_task(&board, task);
    if (!command) {
        fprintf(stderr, "No commit metadata for %s\n", task_id);
        dispatch_board_free(&board);
        return 1;
    }

    printf("%s\n", command);
    free(command);
    dispatch_board_free(&board);
    return 0;
}

static int tui_agents_smoke(void) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui agents smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    printf("Agents: %zu\n", board.agents.count);
    for (size_t i = 0; i < board.agents.count; i++) {
        DispatchAgent *agent = &board.agents.items[i];
        printf("%s %s %s session:%s current:%s workspace:%s\n", agent->name,
               agent->runner, agent->archived ? "archived" : "enabled",
               agent->session_id ? "yes" : "no",
               agent->current_task ? agent->current_task : "-",
               agent->last_workspace ? agent->last_workspace : "-");
    }
    dispatch_board_free(&board);
    return 0;
}

static int tui_agent_inspect_smoke(const char *name) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui agent inspect smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchAgent *agent = dispatch_board_find_agent(&board, name);
    if (!agent) {
        fprintf(stderr, "No agent named %s\n", name);
        dispatch_board_free(&board);
        return 1;
    }

    printf("Agent: %s\n", agent->name);
    printf("Runner: %s\n", agent->runner);
    printf("Status: %s\n", agent->archived ? "archived" : "enabled");
    printf("Prompt: %s\n", agent->prompt_path);
    printf("Run script: %s\n", agent->run_script_path ? agent->run_script_path : "-");
    printf("Session ID: %s\n", agent->session_id ? agent->session_id : "-");
    printf("Current task: %s\n", agent->current_task ? agent->current_task : "-");
    printf("Last workspace: %s\n",
           agent->last_workspace ? agent->last_workspace : "-");
    if (strcmp(agent->runner, "codex") == 0)
        printf("Codex session: manual metadata\n");

    dispatch_board_free(&board);
    return 0;
}

static int tui_agent_session_smoke(const char *name, const char *session_id,
                                   const char *current_task,
                                   const char *last_workspace) {
    char message[256] = {0};
    int ok = update_agent_session_metadata(
        name, strcmp(session_id, "-") == 0 ? NULL : session_id,
        strcmp(current_task, "-") == 0 ? NULL : current_task,
        strcmp(last_workspace, "-") == 0 ? NULL : last_workspace,
        strcmp(session_id, "-") == 0, strcmp(current_task, "-") == 0,
        strcmp(last_workspace, "-") == 0, message, sizeof(message));
    if (!ok) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int tui_prompt_edit_smoke(const char *name) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui prompt edit smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchAgent *agent = dispatch_board_find_agent(&board, name);
    if (!agent) {
        fprintf(stderr, "No agent named %s\n", name);
        dispatch_board_free(&board);
        return 1;
    }
    if (!agent->prompt_path || access(agent->prompt_path, R_OK) != 0) {
        fprintf(stderr, "Prompt file missing for %s\n", name);
        dispatch_board_free(&board);
        return 1;
    }

    char *command = editor_command_for_path(agent->prompt_path);
    printf("%s\n", command);
    free(command);
    dispatch_board_free(&board);
    return 0;
}

static int tui_agent_archive_smoke(const char *name, const char *action) {
    int archived;
    if (strcmp(action, "archive") == 0) {
        archived = 1;
    } else if (strcmp(action, "restore") == 0) {
        archived = 0;
    } else {
        fprintf(stderr, "Unknown archive action %s\n", action);
        return 1;
    }

    char message[256] = {0};
    if (!set_agent_archived_state(name, archived, message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int tui_create_group_smoke(const char *name, const char *prefix) {
    char message[256] = {0};
    if (!create_group(name, strcmp(prefix, "-") == 0 ? "" : prefix, message,
                      sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int tui_create_task_smoke(const char *group, const char *title,
                                 const char *description,
                                 const char *review_mode,
                                 const char *depends_text) {
    int requires_review;
    if (strcmp(review_mode, "review") == 0) {
        requires_review = 1;
    } else if (strcmp(review_mode, "no-review") == 0) {
        requires_review = 0;
    } else {
        fprintf(stderr, "Review mode must be review or no-review\n");
        return 1;
    }

    char message[256] = {0};
    if (!create_task(group, title, strcmp(description, "-") == 0 ? "" : description,
                     requires_review, depends_text, message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int tui_dependency_smoke(const char *action, const char *dependency_id,
                                const char *dependent_id) {
    int add;
    if (strcmp(action, "add") == 0) {
        add = 1;
    } else if (strcmp(action, "remove") == 0) {
        add = 0;
    } else {
        fprintf(stderr, "Dependency action must be add or remove\n");
        return 1;
    }

    char message[256] = {0};
    if (!mutate_dependency(dependency_id, dependent_id, add, message,
                           sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int tui_workspaces_smoke(void) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui workspaces smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    int live = 0;
    for (size_t i = 0; i < board.workspaces.count; i++) {
        DispatchWorkspace *workspace = &board.workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
            continue;
        live++;
    }
    printf("Workspaces: %d\n", live);
    for (size_t i = 0; i < board.workspaces.count; i++) {
        DispatchWorkspace *workspace = &board.workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
            continue;
        DispatchTask *task = dispatch_board_find_task(&board,
                                                      workspace->task_id);
        const char *task_state =
            task ? dispatch_state_name(dispatch_task_effective_state(&board,
                                                                     task))
                 : "missing";
        printf("%s %s %s actor:%s branch:%s git:%s dirty:%s path:%s\n",
               workspace->task_id, task_state,
               dispatch_workspace_state_name(workspace->state),
               workspace->actor, workspace->branch,
               path_has_git_metadata(workspace->path) ? "present" : "missing",
               workspace_is_dirty(workspace) ? "yes" : "no",
               workspace->path);
    }
    dispatch_board_free(&board);
    return 0;
}

static int tui_workspace_inspect_smoke(const char *target) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui workspace inspect smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchWorkspace *workspace = dispatch_board_find_workspace(&board, target);
    if (!workspace || workspace->state == DISPATCH_WORKSPACE_REMOVED) {
        fprintf(stderr, "No workspace for %s\n", target);
        dispatch_board_free(&board);
        return 1;
    }
    DispatchTask *task = dispatch_board_find_task(&board, workspace->task_id);
    printf("Task: %s\n", workspace->task_id);
    printf("Task state: %s\n",
           task ? dispatch_state_name(dispatch_task_effective_state(&board,
                                                                    task))
                : "missing");
    printf("Workspace state: %s\n",
           dispatch_workspace_state_name(workspace->state));
    printf("Actor: %s\n", workspace->actor);
    printf("Branch: %s\n", workspace->branch);
    printf("Path: %s\n", workspace->path);
    printf("Git worktree: %s\n",
           path_has_git_metadata(workspace->path) ? "present" : "missing");
    printf("Dirty: %s\n", workspace_is_dirty(workspace) ? "yes" : "no");
    printf("Sequence tasks: %zu\n", workspace->sequence_tasks.count);
    printf("Review gate: %s\n",
           workspace->review_gate ? workspace->review_gate : "-");
    dispatch_board_free(&board);
    return 0;
}

static int tui_logs_smoke(const char *field, const char *value) {
    FILE *file = fopen(DISPATCH_LOG_FILE, "r");
    if (!file) {
        fprintf(stderr, "No %s\n", DISPATCH_LOG_FILE);
        return 1;
    }

    int count = 0;
    char line[8192];
    while (fgets(line, sizeof(line), file)) {
        json_error_t error;
        json_t *record = json_loads(line, 0, &error);
        if (!record)
            continue;
        if (!log_record_matches_filter(record, field, value)) {
            json_decref(record);
            continue;
        }
        const char *actor = json_string_field(record, "actor");
        const char *command = json_string_field(record, "command");
        const char *action = json_string_field(record, "action");
        const char *task = json_nested_string_field(record, "targets", "task");
        const char *agent = json_nested_string_field(record, "targets", "agent");
        const char *workspace =
            json_nested_string_field(record, "targets", "workspace");
        printf("%s %s %s task:%s agent:%s workspace:%s\n", actor, command,
               action, task[0] ? task : "-", agent[0] ? agent : "-",
               workspace[0] ? workspace : "-");
        count++;
        json_decref(record);
    }
    fclose(file);
    printf("Logs: %d\n", count);
    return 0;
}

static const char *screen_name(DispatchTuiScreen screen) {
    switch (screen) {
    case TUI_SCREEN_BOARD:
        return "board";
    case TUI_SCREEN_TASK_INSPECTOR:
        return "task";
    case TUI_SCREEN_AGENTS:
        return "agents";
    case TUI_SCREEN_AGENT_INSPECTOR:
        return "agent";
    case TUI_SCREEN_TASK_FORM:
        return "task-form";
    case TUI_SCREEN_GROUP_FORM:
        return "group-form";
    case TUI_SCREEN_WORKSPACES:
        return "workspaces";
    case TUI_SCREEN_WORKSPACE_INSPECTOR:
        return "workspace";
    case TUI_SCREEN_LOGS:
        return "logs";
    }
    return "board";
}

static int tui_palette_smoke(const char *command) {
    DispatchTui tui;
    tui_init(&tui);
    if (!tui_load_board(&tui)) {
        fprintf(stderr, "%s\n", tui.status);
        return 1;
    }
    execute_palette_command(&tui, command);
    printf("Screen: %s\n", screen_name(tui.screen));
    printf("Status: %s\n", tui.status);
    printf("Filter: %s\n", filter_name(tui.filter));
    if (tui.log_filter_field[0])
        printf("Log filter: %s=%s\n", tui.log_filter_field,
               tui.log_filter_value);
    DispatchTask *task = selected_visible_task(&tui);
    if (task)
        printf("Selected task: %s\n", task->id);
    DispatchAgent *agent = selected_agent(&tui);
    if (agent)
        printf("Selected agent: %s\n", agent->name);
    DispatchWorkspace *workspace = selected_visible_workspace(&tui);
    if (workspace)
        printf("Selected workspace: %s\n", workspace->task_id);
    tui_free_board(&tui);
    return 0;
}

static int tui_palette_complete_smoke(const char *input) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui palette complete failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }
    print_palette_completion(&board, input);
    dispatch_board_free(&board);
    return 0;
}

static void print_tui_help(void) {
    puts("Usage: dispatch tui [--smoke]");
    puts("");
    puts("Open the ncurses Dispatch terminal UI.");
    puts("");
    puts("Interactive keys:");
    puts("  b board, a/Tab agents, w workspaces, l logs, : command palette");
    puts("  j/k or arrows move, Enter/i inspect, q/Esc backs out");
    puts("  r/s/f/v ready/start/finish/review selected task");
    puts("  tmux: no control-prefix bindings; run alongside tmux panes");
    puts("");
    puts("  --smoke   load the board and exit without initializing ncurses");
    puts("  --inspect-smoke <task-id>  print task inspector data and exit");
    puts("  --filter-smoke <filter>    print visible row count and exit");
    puts("  --action-smoke <action> <task-id> [actor]  run lifecycle action and exit");
    puts("  --diff-smoke <task-id>     print external diff command and exit");
    puts("  --agents-smoke             print agent dashboard data and exit");
    puts("  --agent-inspect-smoke <name>  print agent inspector data and exit");
    puts("  --agent-session-smoke <name> <session|- > <task|- > <workspace|- >");
    puts("  --prompt-edit-smoke <name>    print prompt editor command and exit");
    puts("  --agent-archive-smoke <name> archive|restore");
    puts("  --create-group-smoke <name> <prefix|->");
    puts("  --create-task-smoke <group> <title> <description|-> review|no-review <deps|->");
    puts("  --dependency-smoke add|remove <dependency-id> <dependent-id>");
    puts("  --workspaces-smoke          print workspace dashboard data and exit");
    puts("  --workspace-inspect-smoke <task-id-or-workspace>");
    puts("  --logs-smoke [actor|command|action|task|agent|workspace <value>]");
    puts("  --palette-smoke <command>");
    puts("  --palette-complete-smoke <prefix>");
}

int dispatch_tui_main(int argc, char **argv) {
    if (argc == 3 &&
        (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        print_tui_help();
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "--smoke") == 0)
        return tui_smoke();
    if (argc == 4 && strcmp(argv[2], "--inspect-smoke") == 0)
        return tui_inspect_smoke(argv[3]);
    if (argc == 4 && strcmp(argv[2], "--filter-smoke") == 0)
        return tui_filter_smoke(argv[3]);
    if ((argc == 5 || argc == 6) && strcmp(argv[2], "--action-smoke") == 0)
        return tui_action_smoke(argv[3], argv[4], argc == 6 ? argv[5] : "user");
    if (argc == 4 && strcmp(argv[2], "--diff-smoke") == 0)
        return tui_diff_smoke(argv[3]);
    if (argc == 3 && strcmp(argv[2], "--agents-smoke") == 0)
        return tui_agents_smoke();
    if (argc == 4 && strcmp(argv[2], "--agent-inspect-smoke") == 0)
        return tui_agent_inspect_smoke(argv[3]);
    if (argc == 7 && strcmp(argv[2], "--agent-session-smoke") == 0)
        return tui_agent_session_smoke(argv[3], argv[4], argv[5], argv[6]);
    if (argc == 4 && strcmp(argv[2], "--prompt-edit-smoke") == 0)
        return tui_prompt_edit_smoke(argv[3]);
    if (argc == 5 && strcmp(argv[2], "--agent-archive-smoke") == 0)
        return tui_agent_archive_smoke(argv[3], argv[4]);
    if (argc == 5 && strcmp(argv[2], "--create-group-smoke") == 0)
        return tui_create_group_smoke(argv[3], argv[4]);
    if (argc == 8 && strcmp(argv[2], "--create-task-smoke") == 0)
        return tui_create_task_smoke(argv[3], argv[4], argv[5], argv[6],
                                    argv[7]);
    if (argc == 6 && strcmp(argv[2], "--dependency-smoke") == 0)
        return tui_dependency_smoke(argv[3], argv[4], argv[5]);
    if (argc == 3 && strcmp(argv[2], "--workspaces-smoke") == 0)
        return tui_workspaces_smoke();
    if (argc == 4 && strcmp(argv[2], "--workspace-inspect-smoke") == 0)
        return tui_workspace_inspect_smoke(argv[3]);
    if ((argc == 3 || argc == 5) && strcmp(argv[2], "--logs-smoke") == 0)
        return tui_logs_smoke(argc == 5 ? argv[3] : "", argc == 5 ? argv[4] : "");
    if (argc == 4 && strcmp(argv[2], "--palette-smoke") == 0)
        return tui_palette_smoke(argv[3]);
    if (argc == 4 && strcmp(argv[2], "--palette-complete-smoke") == 0)
        return tui_palette_complete_smoke(argv[3]);
    if (argc != 2) {
        print_tui_help();
        return 1;
    }

    return tui_run();
}
