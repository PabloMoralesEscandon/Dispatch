/* TUI core: shared state, board loading, visibility, selection, and filters. */

#include "dispatch_tui_internal.h"

int tui_colors_enabled = 0;
volatile sig_atomic_t tui_quit_requested = 0;

static int board_file_changed(const DispatchTui *tui);
static int text_contains_casefold(const char *haystack, const char *needle);
static int log_record_matches_filter(json_t *record, const char *field,
                                     const char *value);
static void log_records_append(TuiLogRecords *records, json_t *record);
static int task_matches_search(const DispatchTask *task, const char *search);
static int task_matches_filter(const DispatchBoard *board,
                               const DispatchTask *task,
                               DispatchTuiFilter filter);
static int task_matches_secondary_filters(const DispatchTui *tui,
                                          const DispatchTask *task);
static void sync_index_scroll(int selected, int count, int visible_rows,
                              int *top);
static int board_selected_render_row_for_top(const DispatchTui *tui,
                                             int task_top);
static void restore_task_selection(DispatchTui *tui, const char *task_id);
static void restore_agent_selection(DispatchTui *tui, const char *name);
static void restore_workspace_selection(DispatchTui *tui, const char *target);

void tui_handle_sigint(int signal_number) {
    (void)signal_number;
    tui_quit_requested = 1;
}

void tui_style_title_on(void) {
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_TITLE) | A_BOLD);
    else
        attron(A_BOLD | A_REVERSE);
}

void tui_style_title_off(void) {
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_TITLE) | A_BOLD);
    else
        attroff(A_BOLD | A_REVERSE);
}

void tui_style_row_on(int row_index, int selected) {
    if (selected) {
        attron(A_REVERSE);
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_SELECTED));
    } else if ((row_index % 2) == 1 && tui_colors_enabled) {
        attron(COLOR_PAIR(TUI_COLOR_ALT_ROW));
    }
}

void tui_style_row_off(int row_index, int selected) {
    if (selected) {
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_SELECTED));
        attroff(A_REVERSE);
    } else if ((row_index % 2) == 1 && tui_colors_enabled) {
        attroff(COLOR_PAIR(TUI_COLOR_ALT_ROW));
    }
}

int title_starts_with_dispatch_id_like(const char *title) {
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

void tui_set_status(DispatchTui *tui, const char *message) {
    snprintf(tui->status, sizeof(tui->status), "%s", message ? message : "");
}

void tui_init(DispatchTui *tui) {
    memset(tui, 0, sizeof(*tui));
    tui->filter = TUI_FILTER_NOT_DONE;
    tui->group_filter = -1;
    tui->actor_filter = -1;
    const char *actor = getenv("DISPATCH_ACTOR");
    snprintf(tui->actor, sizeof(tui->actor), "%s",
             actor && actor[0] ? actor : "user");
    tui->running = 1;
}

void tui_free_board(DispatchTui *tui) {
    if (tui->board_loaded) {
        dispatch_board_free(&tui->board);
        tui->board_loaded = 0;
    }
}

int tui_load_board(DispatchTui *tui) {
    char error[256] = {0};
    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        tui_set_status(tui, error[0] ? error : "could not load board");
        return 0;
    }
    tui_free_board(tui);
    tui->board = board;
    tui->board_loaded = 1;
    struct stat info;
    if (stat(DISPATCH_STORE_FILE, &info) == 0) {
        tui->board_mtime = info.st_mtime;
        tui->board_size = info.st_size;
    }
    if (tui->group_filter >= 0 &&
        (size_t)tui->group_filter >= tui->board.groups.count)
        tui->group_filter = -1;
    if (tui->actor_filter >= 0 &&
        (size_t)tui->actor_filter >= tui->board.agents.count)
        tui->actor_filter = -1;
    tui_set_status(tui, "Ready");
    return 1;
}

static int board_file_changed(const DispatchTui *tui) {
    struct stat info;
    if (stat(DISPATCH_STORE_FILE, &info) != 0)
        return 0;
    return info.st_mtime != tui->board_mtime || info.st_size != tui->board_size;
}

int task_count_for_state(const DispatchBoard *board,
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

const char *filter_name(DispatchTuiFilter filter) {
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

const char *json_string_field(json_t *object, const char *name) {
    json_t *value = json_object_get(object, name);
    return json_is_string(value) ? json_string_value(value) : "";
}

const char *json_nested_string_field(json_t *object, const char *parent,
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

static void log_records_append(TuiLogRecords *records, json_t *record) {
    if (records->count >= records->capacity) {
        records->capacity = records->capacity == 0 ? 16 : records->capacity * 2;
        records->items = realloc(records->items,
                                 records->capacity * sizeof(*records->items));
        if (!records->items) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
    }
    records->items[records->count++] = record;
}

void log_records_free(TuiLogRecords *records) {
    for (size_t i = 0; i < records->count; i++)
        json_decref(records->items[i]);
    free(records->items);
    memset(records, 0, sizeof(*records));
}

int load_matching_log_records(const char *field, const char *value,
                                     TuiLogRecords *records) {
    memset(records, 0, sizeof(*records));
    FILE *file = fopen(DISPATCH_LOG_FILE, "r");
    if (!file)
        return 0;

    char line[8192];
    while (fgets(line, sizeof(line), file)) {
        json_error_t error;
        json_t *record = json_loads(line, 0, &error);
        if (!record)
            continue;
        if (log_record_matches_filter(record, field, value)) {
            log_records_append(records, record);
        } else {
            json_decref(record);
        }
    }
    fclose(file);
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

const char *actor_filter_value(const DispatchBoard *board, int index) {
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

int tui_task_is_visible(const DispatchTui *tui,
                               const DispatchTask *task) {
    return task_matches_filter(&tui->board, task, tui->filter) &&
           task_matches_secondary_filters(tui, task) &&
           task_matches_search(task, tui->search);
}

/* Canonical enumeration of visible tasks in grouped render order: the same
 * order draw_board_rows paints them (outer loop over groups, inner loop over
 * that group's tasks in storage order). Every selection index, visible count,
 * and highlight computation must walk tasks through this iterator so the
 * highlighted row and the acted-on task always refer to the same task, even
 * when tasks of different groups are interleaved in board.tasks storage
 * order. */

void visible_task_iter_init(const DispatchTui *tui,
                                   VisibleTaskIter *it) {
    it->tui = tui;
    it->group = 0;
    it->task = 0;
}

DispatchTask *visible_task_iter_next(VisibleTaskIter *it) {
    const DispatchBoard *board = &it->tui->board;
    while (it->group < board->groups.count) {
        const DispatchGroup *group = &board->groups.items[it->group];
        while (it->task < board->tasks.count) {
            DispatchTask *task =
                (DispatchTask *)&board->tasks.items[it->task++];
            if (strcmp(task->group, group->id) == 0 &&
                tui_task_is_visible(it->tui, task))
                return task;
        }
        it->group++;
        it->task = 0;
    }
    return NULL;
}

int visible_task_count_for_tui(const DispatchTui *tui) {
    VisibleTaskIter it;
    visible_task_iter_init(tui, &it);
    int count = 0;
    while (visible_task_iter_next(&it))
        count++;
    return count;
}

void clamp_selection(DispatchTui *tui) {
    int count = tui->board_loaded ? visible_task_count_for_tui(tui) : 0;
    if (count <= 0) {
        tui->selected_task = 0;
        tui->task_top = 0;
    } else if (tui->selected_task >= count) {
        tui->selected_task = count - 1;
    } else if (tui->selected_task < 0) {
        tui->selected_task = 0;
    }
    if (tui->task_top >= count)
        tui->task_top = count - 1;
    if (tui->task_top < 0)
        tui->task_top = 0;
}

int visible_agent_count(const DispatchTui *tui) {
    if (!tui->board_loaded)
        return 0;
    int count = 0;
    for (size_t i = 0; i < tui->board.agents.count; i++) {
        if (agent_is_visible(tui, &tui->board.agents.items[i]))
            count++;
    }
    return count;
}

void clamp_agent_selection(DispatchTui *tui) {
    int count = visible_agent_count(tui);
    if (count <= 0) {
        tui->selected_agent = 0;
        tui->agent_top = 0;
    } else if (tui->selected_agent >= count) {
        tui->selected_agent = count - 1;
    } else if (tui->selected_agent < 0) {
        tui->selected_agent = 0;
    }
    if (tui->agent_top >= count)
        tui->agent_top = count - 1;
    if (tui->agent_top < 0)
        tui->agent_top = 0;
}

int visible_workspace_count(const DispatchTui *tui) {
    if (!tui->board_loaded)
        return 0;
    int count = 0;
    for (size_t i = 0; i < tui->board.workspaces.count; i++) {
        if (tui->board.workspaces.items[i].state != DISPATCH_WORKSPACE_REMOVED)
            count++;
    }
    return count;
}

void clamp_workspace_selection(DispatchTui *tui) {
    int count = visible_workspace_count(tui);
    if (count <= 0) {
        tui->selected_workspace = 0;
        tui->workspace_top = 0;
    } else if (tui->selected_workspace >= count) {
        tui->selected_workspace = count - 1;
    } else if (tui->selected_workspace < 0) {
        tui->selected_workspace = 0;
    }
    if (tui->workspace_top >= count)
        tui->workspace_top = count - 1;
    if (tui->workspace_top < 0)
        tui->workspace_top = 0;
}

static void sync_index_scroll(int selected, int count, int visible_rows,
                              int *top) {
    if (visible_rows <= 0 || count <= 0) {
        *top = 0;
        return;
    }
    if (*top >= count)
        *top = count - 1;
    if (*top < 0)
        *top = 0;
    if (selected < *top)
        *top = selected;
    if (selected >= *top + visible_rows)
        *top = selected - visible_rows + 1;
    if (*top < 0)
        *top = 0;
}

void sync_agent_scroll(DispatchTui *tui, int visible_rows) {
    clamp_agent_selection(tui);
    sync_index_scroll(tui->selected_agent, visible_agent_count(tui),
                      visible_rows, &tui->agent_top);
}

void sync_workspace_scroll(DispatchTui *tui, int visible_rows) {
    clamp_workspace_selection(tui);
    sync_index_scroll(tui->selected_workspace, visible_workspace_count(tui),
                      visible_rows, &tui->workspace_top);
}

int visible_log_count(const DispatchTui *tui) {
    TuiLogRecords records;
    if (!load_matching_log_records(tui->log_filter_field, tui->log_filter_value,
                                   &records))
        return 0;
    int count = (int)records.count;
    log_records_free(&records);
    return count;
}

void clamp_log_selection(DispatchTui *tui) {
    int count = visible_log_count(tui);
    if (count <= 0) {
        tui->selected_log = 0;
        tui->log_top = 0;
    } else if (tui->selected_log >= count) {
        tui->selected_log = count - 1;
    } else if (tui->selected_log < 0) {
        tui->selected_log = 0;
    }
    if (tui->log_top >= count)
        tui->log_top = count - 1;
    if (tui->log_top < 0)
        tui->log_top = 0;
}

void sync_log_scroll(DispatchTui *tui, int visible_rows) {
    if (visible_rows <= 0) {
        tui->log_top = 0;
        return;
    }
    clamp_log_selection(tui);
    if (tui->selected_log < tui->log_top)
        tui->log_top = tui->selected_log;
    if (tui->selected_log >= tui->log_top + visible_rows)
        tui->log_top = tui->selected_log - visible_rows + 1;
    if (tui->log_top < 0)
        tui->log_top = 0;
}

static int board_selected_render_row_for_top(const DispatchTui *tui,
                                             int task_top) {
    VisibleTaskIter it;
    visible_task_iter_init(tui, &it);
    int visible_index = 0;
    int render_row = 0;
    size_t header_group = (size_t)-1;
    for (DispatchTask *task = visible_task_iter_next(&it); task;
         task = visible_task_iter_next(&it)) {
        if (visible_index < task_top) {
            visible_index++;
            continue;
        }
        if (it.group != header_group) {
            render_row++;
            header_group = it.group;
        }
        if (visible_index == tui->selected_task)
            return render_row;
        render_row++;
        visible_index++;
    }
    return -1;
}

void sync_task_scroll(DispatchTui *tui, int visible_rows) {
    clamp_selection(tui);
    int count = visible_task_count_for_tui(tui);
    if (visible_rows <= 0 || count <= 0) {
        tui->task_top = 0;
        return;
    }
    if (tui->selected_task < tui->task_top)
        tui->task_top = tui->selected_task;
    while (tui->task_top < tui->selected_task) {
        int selected_row = board_selected_render_row_for_top(tui, tui->task_top);
        if (selected_row >= 0 && selected_row < visible_rows)
            break;
        tui->task_top++;
    }
    if (tui->task_top < 0)
        tui->task_top = 0;
    if (tui->task_top >= count)
        tui->task_top = count - 1;
}

DispatchAgent *selected_agent(DispatchTui *tui) {
    if (!tui->board_loaded)
        return NULL;

    int visible_index = 0;
    for (size_t i = 0; i < tui->board.agents.count; i++) {
        DispatchAgent *agent = &tui->board.agents.items[i];
        if (!agent_is_visible(tui, agent))
            continue;
        if (visible_index == tui->selected_agent)
            return agent;
        visible_index++;
    }
    return NULL;
}

DispatchWorkspace *selected_visible_workspace(DispatchTui *tui) {
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

int select_task_by_id(DispatchTui *tui, const char *task_id) {
    DispatchTask *requested = dispatch_board_find_task(&tui->board, task_id);
    if (!requested)
        return 0;

    snprintf(tui->inspected_task_id, sizeof(tui->inspected_task_id), "%s",
             requested->id);
    tui->desc_top = 0;
    VisibleTaskIter it;
    visible_task_iter_init(tui, &it);
    int visible_index = 0;
    for (DispatchTask *task = visible_task_iter_next(&it); task;
         task = visible_task_iter_next(&it)) {
        if (strcmp(task->id, task_id) == 0) {
            tui->selected_task = visible_index;
            return 1;
        }
        visible_index++;
    }
    return 1;
}

int select_agent_by_name(DispatchTui *tui, const char *name) {
    tui->show_archived_agents = 1;
    int visible_index = 0;
    for (size_t i = 0; i < tui->board.agents.count; i++) {
        DispatchAgent *agent = &tui->board.agents.items[i];
        if (!agent_is_visible(tui, agent))
            continue;
        if (strcmp(agent->name, name) == 0) {
            tui->selected_agent = visible_index;
            return 1;
        }
        visible_index++;
    }
    return 0;
}

int select_workspace_by_id(DispatchTui *tui, const char *target) {
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

static void restore_task_selection(DispatchTui *tui, const char *task_id) {
    if (!task_id || !task_id[0]) {
        clamp_selection(tui);
        return;
    }

    VisibleTaskIter it;
    visible_task_iter_init(tui, &it);
    int visible_index = 0;
    for (DispatchTask *task = visible_task_iter_next(&it); task;
         task = visible_task_iter_next(&it)) {
        if (strcmp(task->id, task_id) == 0) {
            tui->selected_task = visible_index;
            return;
        }
        visible_index++;
    }
    clamp_selection(tui);
}

static void restore_agent_selection(DispatchTui *tui, const char *name) {
    if (!name || !name[0]) {
        clamp_agent_selection(tui);
        return;
    }

    int visible_index = 0;
    for (size_t i = 0; i < tui->board.agents.count; i++) {
        DispatchAgent *agent = &tui->board.agents.items[i];
        if (!agent_is_visible(tui, agent))
            continue;
        if (strcmp(agent->name, name) == 0) {
            tui->selected_agent = visible_index;
            return;
        }
        visible_index++;
    }
    clamp_agent_selection(tui);
}

static void restore_workspace_selection(DispatchTui *tui, const char *target) {
    if (!target || !target[0]) {
        clamp_workspace_selection(tui);
        return;
    }
    if (!select_workspace_by_id(tui, target))
        clamp_workspace_selection(tui);
}

int tui_reload_if_changed(DispatchTui *tui) {
    if (!tui->board_loaded || !board_file_changed(tui))
        return 0;

    char task_id[64] = "";
    char agent_name[128] = "";
    char workspace_id[64] = "";
    DispatchTask *task = selected_visible_task(tui);
    DispatchAgent *agent = selected_agent(tui);
    DispatchWorkspace *workspace = selected_visible_workspace(tui);
    if (task)
        snprintf(task_id, sizeof(task_id), "%s", task->id);
    if (agent)
        snprintf(agent_name, sizeof(agent_name), "%s", agent->name);
    if (workspace)
        snprintf(workspace_id, sizeof(workspace_id), "%s", workspace->task_id);

    if (!tui_load_board(tui))
        return 0;

    if (tui->inspected_task_id[0] &&
        !dispatch_board_find_task(&tui->board, tui->inspected_task_id)) {
        tui->inspected_task_id[0] = '\0';
        if (tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            tui->screen = TUI_SCREEN_BOARD;
    }
    restore_task_selection(tui, task_id);
    restore_agent_selection(tui, agent_name);
    restore_workspace_selection(tui, workspace_id);
    clamp_log_selection(tui);
    tui_set_status(tui, "Board reloaded");
    return 1;
}

int agent_is_visible(const DispatchTui *tui, const DispatchAgent *agent) {
    return tui->show_archived_agents || !agent->archived;
}

void set_filter(DispatchTui *tui, DispatchTuiFilter filter) {
    tui->filter = filter;
    tui->selected_task = 0;
    tui->task_top = 0;
    char message[128];
    snprintf(message, sizeof(message), "Filter: %s", filter_name(filter));
    tui_set_status(tui, message);
}

void cycle_group_filter(DispatchTui *tui) {
    if (!tui->board_loaded || tui->board.groups.count == 0) {
        tui->group_filter = -1;
        tui_set_status(tui, "No groups");
        return;
    }
    tui->group_filter++;
    if ((size_t)tui->group_filter >= tui->board.groups.count)
        tui->group_filter = -1;
    tui->selected_task = 0;
    tui->task_top = 0;
    tui_set_status(tui, tui->group_filter >= 0 ? "Group filter" : "All groups");
}

void cycle_actor_filter(DispatchTui *tui) {
    if (!tui->board_loaded || tui->board.agents.count == 0) {
        tui->actor_filter = -1;
        tui_set_status(tui, "No agents");
        return;
    }
    tui->actor_filter++;
    if ((size_t)tui->actor_filter >= tui->board.agents.count)
        tui->actor_filter = -1;
    tui->selected_task = 0;
    tui->task_top = 0;
    tui_set_status(tui, tui->actor_filter >= 0 ? "Actor filter" : "All actors");
}

void clear_secondary_filters(DispatchTui *tui) {
    tui->search[0] = '\0';
    tui->search_active = 0;
    tui->group_filter = -1;
    tui->actor_filter = -1;
    tui->selected_task = 0;
    tui->task_top = 0;
    tui_set_status(tui, "Filters cleared");
}

int handle_search_key(DispatchTui *tui, int ch) {
    if (!tui->search_active)
        return 0;

    if (ch == 27) {
        tui->search_active = 0;
        tui->search[0] = '\0';
        tui->selected_task = 0;
        tui->task_top = 0;
        tui_set_status(tui, "Search cleared");
        return 1;
    }
    if (ch == '\n' || ch == KEY_ENTER) {
        tui->search_active = 0;
        tui_set_status(tui, "Search applied");
        return 1;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        size_t len = strlen(tui->search);
        if (len > 0) {
            tui->search[len - 1] = '\0';
            tui->selected_task = 0;
            tui->task_top = 0;
        }
        return 1;
    }
    if (ch >= 0 && ch < 256 && isprint((unsigned char)ch)) {
        size_t len = strlen(tui->search);
        if (len + 1 < sizeof(tui->search)) {
            tui->search[len] = (char)ch;
            tui->search[len + 1] = '\0';
            tui->selected_task = 0;
            tui->task_top = 0;
        }
        return 1;
    }
    return 1;
}
