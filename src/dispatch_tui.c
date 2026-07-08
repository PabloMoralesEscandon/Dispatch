#include "dispatch_tui.h"

#include <ncurses.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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
    TUI_SCREEN_AGENT_FORM,
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
    char inspected_task_id[64];
    int search_active;
    int selected_task;
    int task_top;
    int selected_agent;
    int agent_top;
    int selected_workspace;
    int workspace_top;
    int selected_log;
    int log_top;
    char log_filter_field[32];
    char log_filter_value[128];
    time_t board_mtime;
    off_t board_size;
    int show_help;
    int running;
} DispatchTui;

typedef struct {
    json_t **items;
    size_t count;
    size_t capacity;
} TuiLogRecords;

typedef struct {
    char group[32];
    char title[256];
    char description[512];
    char deps[256];
    int requires_review;
    int active_field;
    char status[256];
} TuiTaskForm;

enum {
    TUI_TASK_FORM_GROUP = 0,
    TUI_TASK_FORM_TITLE = 1,
    TUI_TASK_FORM_DESCRIPTION = 2,
    TUI_TASK_FORM_DEPS = 3,
    TUI_TASK_FORM_REVIEW = 4,
    TUI_TASK_FORM_FIELD_COUNT = 5,
};

typedef struct {
    char name[128];
    char runner[16];
    char model[128];
    int no_run_script;
    int active_field;
    char status[256];
} TuiAgentForm;

enum {
    TUI_AGENT_FORM_NAME = 0,
    TUI_AGENT_FORM_RUNNER = 1,
    TUI_AGENT_FORM_MODEL = 2,
    TUI_AGENT_FORM_NO_RUN_SCRIPT = 3,
    TUI_AGENT_FORM_FIELD_COUNT = 4,
};

static DispatchTask *selected_visible_task(DispatchTui *tui);
static DispatchWorkspace *selected_visible_workspace(DispatchTui *tui);
static int parse_filter_name(const char *name, DispatchTuiFilter *filter);
static int agent_is_visible(const DispatchTui *tui, const DispatchAgent *agent);
static int prompt_line(const char *label, char *buffer, size_t buffer_size,
                       const char *initial);
static int handle_prompt_line_key(char *buffer, size_t buffer_size, int ch,
                                  int *done, int *cancelled);
static void draw_truncated(int y, int x, int width, const char *text);
static void draw_padded(int y, int x, int width, const char *text);
static void draw_title_bar(DispatchTui *tui, const char *view);
static void draw_footer(const char *message, const char *hints);
static const char *tui_footer_hints(DispatchTuiScreen screen);

enum {
    TUI_COLOR_HEADER = 1,
    TUI_COLOR_ALT_ROW = 2,
    TUI_COLOR_SELECTED = 3,
    TUI_COLOR_TITLE = 4,
    TUI_COLOR_BRAND = 5,
    TUI_COLOR_MUTED = 6,
    TUI_COLOR_ACCENT = 7,
    TUI_COLOR_STATE_READY = 8,
    TUI_COLOR_STATE_DOING = 9,
    TUI_COLOR_STATE_REVIEW = 10,
    TUI_COLOR_STATE_BLOCKED = 11,
    TUI_COLOR_STATE_DONE = 12,
    TUI_COLOR_STATE_PROPOSED = 13,
};

static int tui_colors_enabled = 0;
static volatile sig_atomic_t tui_quit_requested = 0;

enum {
    TUI_ESCAPE_DELAY_MS = 25,
};

static void tui_handle_sigint(int signal_number) {
    (void)signal_number;
    tui_quit_requested = 1;
}

static void tui_style_title_on(void) {
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_TITLE) | A_BOLD);
    else
        attron(A_BOLD | A_REVERSE);
}

static void tui_style_title_off(void) {
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_TITLE) | A_BOLD);
    else
        attroff(A_BOLD | A_REVERSE);
}

static void tui_style_row_on(int row_index, int selected) {
    if (selected) {
        attron(A_REVERSE);
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_SELECTED));
    } else if ((row_index % 2) == 1 && tui_colors_enabled) {
        attron(COLOR_PAIR(TUI_COLOR_ALT_ROW));
    }
}

static void tui_style_row_off(int row_index, int selected) {
    if (selected) {
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_SELECTED));
        attroff(A_REVERSE);
    } else if ((row_index % 2) == 1 && tui_colors_enabled) {
        attroff(COLOR_PAIR(TUI_COLOR_ALT_ROW));
    }
}

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

static void log_records_free(TuiLogRecords *records) {
    for (size_t i = 0; i < records->count; i++)
        json_decref(records->items[i]);
    free(records->items);
    memset(records, 0, sizeof(*records));
}

static int load_matching_log_records(const char *field, const char *value,
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

static int visible_agent_count(const DispatchTui *tui) {
    if (!tui->board_loaded)
        return 0;
    int count = 0;
    for (size_t i = 0; i < tui->board.agents.count; i++) {
        if (agent_is_visible(tui, &tui->board.agents.items[i]))
            count++;
    }
    return count;
}

static void clamp_agent_selection(DispatchTui *tui) {
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

static void sync_agent_scroll(DispatchTui *tui, int visible_rows) {
    clamp_agent_selection(tui);
    sync_index_scroll(tui->selected_agent, visible_agent_count(tui),
                      visible_rows, &tui->agent_top);
}

static void sync_workspace_scroll(DispatchTui *tui, int visible_rows) {
    clamp_workspace_selection(tui);
    sync_index_scroll(tui->selected_workspace, visible_workspace_count(tui),
                      visible_rows, &tui->workspace_top);
}

static int visible_log_count(const DispatchTui *tui) {
    TuiLogRecords records;
    if (!load_matching_log_records(tui->log_filter_field, tui->log_filter_value,
                                   &records))
        return 0;
    int count = (int)records.count;
    log_records_free(&records);
    return count;
}

static void clamp_log_selection(DispatchTui *tui) {
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

static void sync_log_scroll(DispatchTui *tui, int visible_rows) {
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
    int visible_index = 0;
    int render_row = 0;
    for (size_t g = 0; g < tui->board.groups.count; g++) {
        DispatchGroup *group = &tui->board.groups.items[g];
        int group_has_drawn_task = 0;
        int group_header_row = render_row;
        for (size_t i = 0; i < tui->board.tasks.count; i++) {
            DispatchTask *task = &tui->board.tasks.items[i];
            if (strcmp(task->group, group->id) != 0 ||
                !tui_task_is_visible(tui, task)) {
                continue;
            }
            if (visible_index < task_top) {
                visible_index++;
                continue;
            }
            if (!group_has_drawn_task) {
                render_row++;
                group_has_drawn_task = 1;
            }
            if (visible_index == tui->selected_task)
                return render_row;
            render_row++;
            visible_index++;
        }
        if (!group_has_drawn_task)
            render_row = group_header_row;
    }
    return -1;
}

static void sync_task_scroll(DispatchTui *tui, int visible_rows) {
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

static DispatchAgent *selected_agent(DispatchTui *tui) {
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
    DispatchTask *requested = dispatch_board_find_task(&tui->board, task_id);
    if (!requested)
        return 0;

    snprintf(tui->inspected_task_id, sizeof(tui->inspected_task_id), "%s",
             requested->id);
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
    return 1;
}

static int select_agent_by_name(DispatchTui *tui, const char *name) {
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

static void restore_task_selection(DispatchTui *tui, const char *task_id) {
    if (!task_id || !task_id[0]) {
        clamp_selection(tui);
        return;
    }

    int visible_index = 0;
    for (size_t i = 0; i < tui->board.tasks.count; i++) {
        DispatchTask *task = &tui->board.tasks.items[i];
        if (!tui_task_is_visible(tui, task))
            continue;
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

static int tui_reload_if_changed(DispatchTui *tui) {
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

static int agent_is_visible(const DispatchTui *tui, const DispatchAgent *agent) {
    return tui->show_archived_agents || !agent->archived;
}

static void set_filter(DispatchTui *tui, DispatchTuiFilter filter) {
    tui->filter = filter;
    tui->selected_task = 0;
    tui->task_top = 0;
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
    tui->task_top = 0;
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
    tui->task_top = 0;
    tui_set_status(tui, tui->actor_filter >= 0 ? "Actor filter" : "All actors");
}

static void clear_secondary_filters(DispatchTui *tui) {
    tui->search[0] = '\0';
    tui->search_active = 0;
    tui->group_filter = -1;
    tui->actor_filter = -1;
    tui->selected_task = 0;
    tui->task_top = 0;
    tui_set_status(tui, "Filters cleared");
}

static int handle_search_key(DispatchTui *tui, int ch) {
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

static char *tui_trimmed_copy(const char *value) {
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

static char *tui_base64_encode(const unsigned char *data, size_t len) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((len + 2) / 3) * 4;
    char *encoded = malloc(out_len + 1);
    if (!encoded) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    size_t in = 0;
    size_t out = 0;
    while (in < len) {
        size_t remaining = len - in;
        unsigned int a = data[in++];
        unsigned int b = remaining > 1 ? data[in++] : 0;
        unsigned int c = remaining > 2 ? data[in++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;

        encoded[out++] = alphabet[(triple >> 18) & 0x3f];
        encoded[out++] = alphabet[(triple >> 12) & 0x3f];
        encoded[out++] = remaining > 1 ? alphabet[(triple >> 6) & 0x3f] : '=';
        encoded[out++] = remaining > 2 ? alphabet[triple & 0x3f] : '=';
    }

    encoded[out_len] = '\0';
    return encoded;
}

static char *osc52_sequence_for_text(const char *text) {
    const char *value = text ? text : "";
    char *payload =
        tui_base64_encode((const unsigned char *)value, strlen(value));
    size_t size = strlen("\033]52;c;\a") + strlen(payload) + 1;
    char *sequence = malloc(size);
    if (!sequence) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(sequence, size, "\033]52;c;%s\a", payload);
    free(payload);
    return sequence;
}

static char *diff_command_for_task(const DispatchBoard *board,
                                   const DispatchTask *task) {
    if (!task || task->commits.count == 0)
        return NULL;

    char *repo_q = tui_shell_quote(board->repo_path ? board->repo_path : ".");

    /* Quote every recorded commit so the diff covers the full task change set,
     * not just the first commit. */
    char **commit_q = calloc(task->commits.count, sizeof(char *));
    if (!commit_q) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    size_t size = strlen("git -C  show") + strlen(repo_q) + 1;
    for (size_t i = 0; i < task->commits.count; i++) {
        commit_q[i] = tui_shell_quote(task->commits.items[i]);
        size += strlen(" ") + strlen(commit_q[i]);
    }

    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    size_t out = 0;
    out += snprintf(command + out, size - out, "git -C %s show", repo_q);
    for (size_t i = 0; i < task->commits.count; i++)
        out += snprintf(command + out, size - out, " %s", commit_q[i]);

    for (size_t i = 0; i < task->commits.count; i++)
        free(commit_q[i]);
    free(commit_q);
    free(repo_q);
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

static int command_available(const char *command) {
    char *command_q = tui_shell_quote(command);
    size_t size = strlen("command -v  >/dev/null 2>&1") + strlen(command_q) + 1;
    char *check = malloc(size);
    if (!check) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(check, size, "command -v %s >/dev/null 2>&1", command_q);
    int ok = system(check) == 0;
    free(command_q);
    free(check);
    return ok;
}

static const char *fallback_editor(void) {
    const char *editors[] = {"nano", "vim", "vi", "sensible-editor", NULL};
    for (int i = 0; editors[i]; i++) {
        if (command_available(editors[i]))
            return editors[i];
    }
    return NULL;
}

static const char *configured_editor(void) {
    const char *visual = getenv("VISUAL");
    if (visual && visual[0])
        return visual;
    const char *editor = getenv("EDITOR");
    if (editor && editor[0])
        return editor;
    return fallback_editor();
}

static char *editor_command_for_path(const char *path, char *error,
                                     size_t error_size) {
    const char *editor = configured_editor();
    if (!editor || !editor[0]) {
        snprintf(error, error_size,
                 "No editor configured; set VISUAL or EDITOR to edit %s",
                 path ? path : "the prompt");
        return NULL;
    }
    char *path_q = tui_shell_quote(path);
    size_t size = strlen(editor) + strlen(path_q) + 2;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, "%s %s", editor, path_q);
    free(path_q);
    return command;
}

static void edit_selected_agent_prompt(DispatchTui *tui) {
    DispatchAgent *agent = selected_agent(tui);
    if (!agent) {
        tui_set_status(tui, "No selected agent");
        return;
    }
    if (!agent->prompt_path || access(agent->prompt_path, F_OK) != 0) {
        tui_set_status(tui, "Prompt file missing");
        return;
    }

    char error[256] = {0};
    char *command = editor_command_for_path(agent->prompt_path, error,
                                           sizeof(error));
    if (!command) {
        tui_set_status(tui, error);
        return;
    }
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

static char *agent_run_command_text(DispatchBoard *board,
                                    const DispatchAgent *agent) {
    if (!agent)
        return NULL;

    if (agent->session_id && agent->session_id[0]) {
        const DispatchWorkspace *workspace = NULL;
        if (board && agent->last_workspace) {
            DispatchWorkspace *found =
                dispatch_board_find_workspace(board, agent->last_workspace);
            if (found && found->state != DISPATCH_WORKSPACE_REMOVED)
                workspace = found;
        }
        if (strcmp(agent->runner, "codex") == 0)
            return codex_agent_resume_command_for(agent, workspace);
        if (strcmp(agent->runner, "claude") == 0)
            return claude_agent_resume_command_for(agent, workspace, 0);
    }

    if (agent->run_script_path && agent->run_script_path[0])
        return strdup(agent->run_script_path);

    char *prompt_q = tui_shell_quote(agent->prompt_path ? agent->prompt_path : "");
    char *model_q = agent->model && agent->model[0]
                        ? tui_shell_quote(agent->model)
                        : NULL;
    const char *format;
    if (strcmp(agent->runner, "codex") == 0) {
        format = model_q ? "codex --model %s \"$(cat %s)\""
                         : "codex \"$(cat %s)\"";
    } else if (strcmp(agent->runner, "claude") == 0) {
        format = "claude \"$(cat %s)\"";
    } else {
        format = "%s \"$(cat %s)\"";
    }

    size_t size = strlen(format) + strlen(prompt_q) +
                  (model_q ? strlen(model_q) : strlen(agent->runner)) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    if (strcmp(agent->runner, "codex") == 0 && model_q)
        snprintf(command, size, format, model_q, prompt_q);
    else if (strcmp(agent->runner, "codex") == 0)
        snprintf(command, size, format, prompt_q);
    else if (strcmp(agent->runner, "claude") == 0)
        snprintf(command, size, format, prompt_q);
    else
        snprintf(command, size, format, agent->runner, prompt_q);

    free(prompt_q);
    free(model_q);
    return command;
}

static int copy_command_to_tmux_buffer(const char *command) {
    if (!getenv("TMUX") || !command_available("tmux"))
        return 0;

    char *command_q = tui_shell_quote(command);
    size_t size = strlen("printf %s  | tmux load-buffer -") +
                  strlen(command_q) + 1;
    char *copy = malloc(size);
    if (!copy) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(copy, size, "printf %%s %s | tmux load-buffer -", command_q);
    int ok = system(copy) == 0;
    free(command_q);
    free(copy);
    return ok;
}

static int send_command_to_osc52_clipboard(const char *command) {
    if (!command)
        return 0;

    char *sequence = osc52_sequence_for_text(command);
    def_prog_mode();
    endwin();
    int ok = fputs(sequence, stdout) != EOF && fflush(stdout) == 0;
    reset_prog_mode();
    refresh();
    free(sequence);
    return ok;
}

static void copy_selected_agent_run_command(DispatchTui *tui) {
    DispatchAgent *agent = selected_agent(tui);
    if (!agent) {
        tui_set_status(tui, "No selected agent");
        return;
    }

    char *command = agent_run_command_text(&tui->board, agent);
    if (!command) {
        tui_set_status(tui, "Could not build agent command");
        return;
    }

    char message[512];
    int sent_osc52 = send_command_to_osc52_clipboard(command);
    int copied_tmux = copy_command_to_tmux_buffer(command);
    if (sent_osc52 && copied_tmux) {
        snprintf(message, sizeof(message),
                 "Sent OSC 52 copy and tmux buffer for %s", agent->name);
    } else if (sent_osc52) {
        snprintf(message, sizeof(message), "Sent OSC 52 copy for %s",
                 agent->name);
    } else if (copied_tmux) {
        snprintf(message, sizeof(message), "Copied run command for %s to tmux buffer",
                 agent->name);
    } else {
        snprintf(message, sizeof(message), "Run command: %s", command);
    }
    tui_set_status(tui, message);
    free(command);
}

static DispatchTask *selected_visible_task(DispatchTui *tui) {
    if (!tui->board_loaded)
        return NULL;

    if (tui->screen == TUI_SCREEN_TASK_INSPECTOR &&
        tui->inspected_task_id[0]) {
        return dispatch_board_find_task(&tui->board, tui->inspected_task_id);
    }

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

static int handle_prompt_line_key(char *buffer, size_t buffer_size, int ch,
                                  int *done, int *cancelled) {
    *done = 0;
    *cancelled = 0;
    if (ch == 27) {
        *cancelled = 1;
        return 1;
    }
    if (ch == '\n' || ch == KEY_ENTER) {
        *done = 1;
        return 1;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        size_t len = strlen(buffer);
        if (len > 0)
            buffer[len - 1] = '\0';
        return 1;
    }
    if (ch == 21) {
        buffer[0] = '\0';
        return 1;
    }
    if (ch >= 0 && ch < 256 && isprint((unsigned char)ch)) {
        size_t len = strlen(buffer);
        if (len + 1 < buffer_size) {
            buffer[len] = (char)ch;
            buffer[len + 1] = '\0';
        }
        return 1;
    }
    return 1;
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

    curs_set(1);
    noecho();
    timeout(-1);
    int done = 0;
    int cancelled = 0;
    while (!done && !cancelled) {
        attron(A_REVERSE);
        mvhline(rows - 1, 0, ' ', cols);
        mvprintw(rows - 1, 0, "%s%s", label, buffer);
        attroff(A_REVERSE);
        int cursor_x = (int)(strlen(label) + strlen(buffer));
        if (cursor_x >= cols)
            cursor_x = cols - 1;
        move(rows - 1, cursor_x);
        refresh();
        handle_prompt_line_key(buffer, buffer_size, getch(), &done,
                               &cancelled);
    }
    timeout(1000);
    curs_set(0);
    return !cancelled;
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

static void run_group_form(DispatchTui *tui);

static void task_form_buffer(TuiTaskForm *form, int field, char **buffer,
                             size_t *buffer_size) {
    *buffer = NULL;
    *buffer_size = 0;
    switch (field) {
    case TUI_TASK_FORM_GROUP:
        *buffer = form->group;
        *buffer_size = sizeof(form->group);
        break;
    case TUI_TASK_FORM_TITLE:
        *buffer = form->title;
        *buffer_size = sizeof(form->title);
        break;
    case TUI_TASK_FORM_DESCRIPTION:
        *buffer = form->description;
        *buffer_size = sizeof(form->description);
        break;
    case TUI_TASK_FORM_DEPS:
        *buffer = form->deps;
        *buffer_size = sizeof(form->deps);
        break;
    default:
        break;
    }
}

/* A single selectable entry in a task-form option picker. `value` is written
 * into the form field; `label` is what the picker shows. */
typedef struct {
    char value[64];
    char label[160];
} TuiTaskFormOption;

/* Returns 1 when id already appears as a comma/whitespace separated token in
 * deps_text. */
static int task_form_deps_contains(const char *deps_text, const char *id) {
    if (!deps_text || !id || !id[0])
        return 0;
    size_t id_len = strlen(id);
    const char *p = deps_text;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t')
            p++;
        size_t tok_len = (size_t)(p - start);
        if (tok_len == id_len && strncmp(start, id, id_len) == 0)
            return 1;
    }
    return 0;
}

/* Appends id to deps_text as a comma-separated token when it is not already
 * present. Treats "-" or empty as no existing dependencies. Returns 1 when the
 * text changed. */
static int task_form_deps_append(char *deps_text, size_t size, const char *id) {
    if (!deps_text || size == 0 || !id || !id[0])
        return 0;
    if (deps_text[0] == '\0' || strcmp(deps_text, "-") == 0)
        deps_text[0] = '\0';
    if (task_form_deps_contains(deps_text, id))
        return 0;
    size_t len = strlen(deps_text);
    const char *sep = len > 0 ? ", " : "";
    if (len + strlen(sep) + strlen(id) + 1 > size)
        return 0;
    snprintf(deps_text + len, size - len, "%s%s", sep, id);
    return 1;
}

/* Fills out[] with the existing groups as pickable options. Free-text entry for
 * new groups is still supported by the form itself. Returns the option count. */
static size_t task_form_group_options(const DispatchBoard *board,
                                      TuiTaskFormOption *out, size_t max) {
    size_t count = 0;
    for (size_t i = 0; board && i < board->groups.count && count < max; i++) {
        const DispatchGroup *group = &board->groups.items[i];
        snprintf(out[count].value, sizeof(out[count].value), "%s",
                 group->prefix ? group->prefix : "");
        snprintf(out[count].label, sizeof(out[count].label), "%-4s %s",
                 group->prefix ? group->prefix : "",
                 group->name ? group->name : "");
        count++;
    }
    return count;
}

/* Fills out[] with tasks that can be selected as dependencies: not-done tasks
 * that are not already present in deps_text. Returns the option count. */
static size_t task_form_dep_options(const DispatchBoard *board,
                                    const char *deps_text, TuiTaskFormOption *out,
                                    size_t max) {
    size_t count = 0;
    const char *deps = deps_text ? deps_text : "";
    for (size_t i = 0; board && i < board->tasks.count && count < max; i++) {
        const DispatchTask *task = &board->tasks.items[i];
        if (task->state == DISPATCH_STATE_DONE)
            continue;
        if (task_form_deps_contains(deps, task->id))
            continue;
        snprintf(out[count].value, sizeof(out[count].value), "%s", task->id);
        snprintf(out[count].label, sizeof(out[count].label), "%-8s [%s] %s",
                 task->id, dispatch_state_name(task->state),
                 task->title ? task->title : "");
        count++;
    }
    return count;
}

static void draw_task_form_box(int y, int x, int width, int height,
                               const char *label, const char *value,
                               int active) {
    if (width < 8 || height < 3)
        return;

    int border_attr = 0;
    int label_attr = 0;
    if (active) {
        border_attr = (tui_colors_enabled ? COLOR_PAIR(TUI_COLOR_ACCENT) : 0) |
                      A_BOLD;
        label_attr = border_attr;
    } else {
        border_attr = (tui_colors_enabled ? COLOR_PAIR(TUI_COLOR_MUTED) : 0) |
                      A_DIM;
        label_attr = border_attr;
    }

    char lab[128];
    snprintf(lab, sizeof(lab), "%s %s", active ? ">" : " ", label);
    attron(label_attr);
    mvaddnstr(y, x, lab, width);
    attroff(label_attr);

    int box_y = y + 1;
    attron(border_attr);
    mvaddch(box_y, x, ACS_ULCORNER);
    mvhline(box_y, x + 1, ACS_HLINE, width - 2);
    mvaddch(box_y, x + width - 1, ACS_URCORNER);
    for (int row = 1; row < height - 1; row++) {
        mvaddch(box_y + row, x, ACS_VLINE);
        mvaddch(box_y + row, x + width - 1, ACS_VLINE);
    }
    mvaddch(box_y + height - 1, x, ACS_LLCORNER);
    mvhline(box_y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(box_y + height - 1, x + width - 1, ACS_LRCORNER);
    attroff(border_attr);

    /* Interior and value text are drawn in the normal attribute so typed
     * content and the cursor stay legible. */
    for (int row = 1; row < height - 1; row++)
        mvhline(box_y + row, x + 1, ' ', width - 2);

    int inner_width = width - 4;
    if (inner_width <= 0)
        return;
    const char *text = value ? value : "";
    for (int row = 0; row < height - 2; row++) {
        size_t offset = (size_t)row * (size_t)inner_width;
        if (offset >= strlen(text))
            break;
        mvaddnstr(box_y + 1 + row, x + 2, text + offset, inner_width);
    }
}

static void draw_task_form_review_box(int y, int x, int width,
                                      const TuiTaskForm *form) {
    draw_task_form_box(y, x, width, 3, "Requires review",
                       form->requires_review ? "yes" : "no",
                       form->active_field == TUI_TASK_FORM_REVIEW);
}

static void task_form_move_cursor(const TuiTaskForm *form, int rows, int cols,
                                  int left, int width, int desc_height) {
    char *buffer = NULL;
    size_t buffer_size = 0;
    TuiTaskForm mutable_form = *form;
    task_form_buffer(&mutable_form, form->active_field, &buffer, &buffer_size);
    (void)buffer_size;
    if (!buffer)
        return;

    int y = 3;
    int height = 3;
    if (form->active_field == TUI_TASK_FORM_TITLE) {
        y += 4;
    } else if (form->active_field == TUI_TASK_FORM_DESCRIPTION) {
        y += 8;
        height = desc_height;
    } else if (form->active_field == TUI_TASK_FORM_DEPS) {
        y += 9 + desc_height;
    }

    int inner_width = width - 4;
    if (inner_width <= 0)
        return;
    size_t len = strlen(buffer);
    int max_pos = inner_width * (height - 2) - 1;
    int pos = (int)(len > (size_t)max_pos ? (size_t)max_pos : len);
    int cursor_y = y + 2 + pos / inner_width;
    int cursor_x = left + 2 + pos % inner_width;
    if (cursor_y < rows - 1 && cursor_x < cols)
        move(cursor_y, cursor_x);
}

static void render_task_form_screen(DispatchTui *tui, const TuiTaskForm *form) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    erase();

    draw_title_bar(tui, "New Task");

    if (rows < 24 || cols < 50) {
        draw_truncated(2, 0, cols, "Terminal too small for task form.");
        draw_truncated(3, 0, cols, "Resize to at least 50x24.");
        refresh();
        return;
    }

    int width = cols - 4;
    if (width > 76)
        width = 76;
    int left = (cols - width) / 2;
    int desc_height = rows >= 30 ? 5 : 3;

    char heading[256];
    snprintf(heading, sizeof(heading), "Creating task as %s", tui->actor);
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    draw_truncated(1, left, width, heading);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);

    int y = 3;
    draw_task_form_box(y, left, width, 3, "Group", form->group,
                       form->active_field == TUI_TASK_FORM_GROUP);
    y += 4;
    draw_task_form_box(y, left, width, 3, "Title", form->title,
                       form->active_field == TUI_TASK_FORM_TITLE);
    y += 4;
    draw_task_form_box(y, left, width, desc_height, "Description",
                       form->description,
                       form->active_field == TUI_TASK_FORM_DESCRIPTION);
    y += desc_height + 1;
    draw_task_form_box(y, left, width, 3, "Depends on", form->deps,
                       form->active_field == TUI_TASK_FORM_DEPS);
    y += 4;
    draw_task_form_review_box(y, left, width, form);

    draw_footer(form->status[0] ? form->status : "New task",
                tui_footer_hints(TUI_SCREEN_TASK_FORM));

    task_form_move_cursor(form, rows, cols, left, width, desc_height);
    refresh();
}

static int task_form_submit(const TuiTaskForm *form, const char *actor,
                            char *message, size_t message_size) {
    return create_task(form->group, form->title, form->description,
                       form->requires_review, form->deps, actor, message,
                       message_size);
}

static int handle_task_form_key(TuiTaskForm *form, int ch, int *submit,
                                int *cancel) {
    *submit = 0;
    *cancel = 0;
    if (ch == 27) {
        *cancel = 1;
        return 1;
    }
    if (ch == '\t' || ch == KEY_DOWN) {
        form->active_field = (form->active_field + 1) % TUI_TASK_FORM_FIELD_COUNT;
        return 1;
    }
    if (ch == KEY_UP) {
        form->active_field =
            (form->active_field + TUI_TASK_FORM_FIELD_COUNT - 1) %
            TUI_TASK_FORM_FIELD_COUNT;
        return 1;
    }
    if (ch == '\n' || ch == KEY_ENTER) {
        if (form->active_field == TUI_TASK_FORM_REVIEW) {
            *submit = 1;
        } else {
            form->active_field++;
        }
        return 1;
    }
    if (form->active_field == TUI_TASK_FORM_REVIEW) {
        if (ch == ' ' || ch == 'y' || ch == 'Y' || ch == 'n' || ch == 'N') {
            if (ch == 'y' || ch == 'Y')
                form->requires_review = 1;
            else if (ch == 'n' || ch == 'N')
                form->requires_review = 0;
            else
                form->requires_review = !form->requires_review;
            return 1;
        }
        return 1;
    }

    char *buffer = NULL;
    size_t buffer_size = 0;
    task_form_buffer(form, form->active_field, &buffer, &buffer_size);
    if (!buffer || buffer_size == 0)
        return 1;
    size_t len = strlen(buffer);
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (len > 0)
            buffer[len - 1] = '\0';
        return 1;
    }
    if (ch == 21) {
        buffer[0] = '\0';
        return 1;
    }
    if (ch >= 0 && ch < 256 && isprint((unsigned char)ch) &&
        len + 1 < buffer_size) {
        buffer[len] = (char)ch;
        buffer[len + 1] = '\0';
    }
    return 1;
}

/* Modal option picker used by the task form. Renders a centered scrollable list
 * and returns the selected index, or -1 when the user cancels. */
static int run_task_form_picker(const char *title,
                                const TuiTaskFormOption *options, size_t count) {
    if (count == 0)
        return -1;
    int selected = 0;
    int top = 0;
    for (;;) {
        int rows = 0;
        int cols = 0;
        getmaxyx(stdscr, rows, cols);

        int width = cols - 8;
        if (width > 64)
            width = 64;
        if (width < 16)
            width = cols > 16 ? 16 : cols;
        int max_visible = rows - 8;
        if (max_visible < 1)
            max_visible = 1;
        int visible = (int)count < max_visible ? (int)count : max_visible;
        int height = visible + 2;
        int left = (cols - width) / 2;
        if (left < 0)
            left = 0;
        int y = (rows - height) / 2;
        if (y < 2)
            y = 2;

        if (selected < top)
            top = selected;
        if (selected >= top + visible)
            top = selected - visible + 1;

        erase();
        int accent = tui_colors_enabled ? COLOR_PAIR(TUI_COLOR_ACCENT) : 0;
        int muted = tui_colors_enabled ? COLOR_PAIR(TUI_COLOR_MUTED) : 0;

        attron(accent | A_BOLD);
        mvaddnstr(y - 1, left, title, width);
        attroff(accent | A_BOLD);

        attron(muted | A_DIM);
        mvaddch(y, left, ACS_ULCORNER);
        mvhline(y, left + 1, ACS_HLINE, width - 2);
        mvaddch(y, left + width - 1, ACS_URCORNER);
        for (int r = 1; r < height - 1; r++) {
            mvaddch(y + r, left, ACS_VLINE);
            mvaddch(y + r, left + width - 1, ACS_VLINE);
        }
        mvaddch(y + height - 1, left, ACS_LLCORNER);
        mvhline(y + height - 1, left + 1, ACS_HLINE, width - 2);
        mvaddch(y + height - 1, left + width - 1, ACS_LRCORNER);
        attroff(muted | A_DIM);

        for (int r = 0; r < visible; r++) {
            size_t idx = (size_t)(top + r);
            if (idx >= count)
                break;
            int row_y = y + 1 + r;
            int is_selected = (int)idx == selected;
            if (is_selected)
                attron(accent | A_REVERSE);
            mvhline(row_y, left + 1, ' ', width - 2);
            char line[192];
            snprintf(line, sizeof(line), " %s", options[idx].label);
            mvaddnstr(row_y, left + 1, line, width - 2);
            if (is_selected)
                attroff(accent | A_REVERSE);
        }

        draw_footer("Select option", "Enter select   Up/Down move   Esc cancel");
        refresh();

        int ch = getch();
        if (ch == KEY_RESIZE)
            continue;
        if (ch == 27 || ch == 'q')
            return -1;
        if (ch == KEY_UP || ch == 'k') {
            if (selected > 0)
                selected--;
            continue;
        }
        if (ch == KEY_DOWN || ch == 'j') {
            if (selected < (int)count - 1)
                selected++;
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER)
            return selected;
    }
}

/* Opens the option picker appropriate for the form's active field. Returns 1
 * when the picker key was handled for this field. */
static int task_form_open_picker(DispatchTui *tui, TuiTaskForm *form) {
    if (form->active_field == TUI_TASK_FORM_GROUP) {
        TuiTaskFormOption opts[64];
        size_t n = task_form_group_options(&tui->board, opts, 64);
        if (n == 0) {
            snprintf(form->status, sizeof(form->status),
                     "No existing groups yet; type a new group name");
            return 1;
        }
        int sel = run_task_form_picker("Select group", opts, n);
        if (sel >= 0)
            snprintf(form->group, sizeof(form->group), "%s", opts[sel].value);
        return 1;
    }
    if (form->active_field == TUI_TASK_FORM_DEPS) {
        TuiTaskFormOption opts[256];
        size_t n = task_form_dep_options(&tui->board, form->deps, opts, 256);
        if (n == 0) {
            snprintf(form->status, sizeof(form->status),
                     "No selectable tasks to depend on");
            return 1;
        }
        int sel = run_task_form_picker("Select dependency", opts, n);
        if (sel >= 0)
            task_form_deps_append(form->deps, sizeof(form->deps), opts[sel].value);
        return 1;
    }
    snprintf(form->status, sizeof(form->status),
             "Options available on the Group and Depends fields");
    return 1;
}

static void run_task_form(DispatchTui *tui) {
    TuiTaskForm form;
    memset(&form, 0, sizeof(form));
    snprintf(form.group, sizeof(form.group), "%s", selected_task_group(tui));
    form.requires_review = 1;
    form.active_field = TUI_TASK_FORM_GROUP;
    snprintf(form.status, sizeof(form.status), "Fill task fields");

    timeout(-1);
    curs_set(1);
    int running = 1;
    while (running) {
        render_task_form_screen(tui, &form);
        int ch = getch();
        if (ch == KEY_RESIZE)
            continue;
        if (ch == 15) { /* Ctrl-O: open the option picker for this field */
            task_form_open_picker(tui, &form);
            continue;
        }
        int submit = 0;
        int cancel = 0;
        handle_task_form_key(&form, ch, &submit, &cancel);
        if (cancel) {
            tui_set_status(tui, "Task creation cancelled");
            break;
        }
        if (submit) {
            char message[256] = {0};
            if (task_form_submit(&form, tui->actor, message, sizeof(message))) {
                tui_load_board(tui);
                tui_set_status(tui, message);
                clamp_selection(tui);
                running = 0;
            } else {
                snprintf(form.status, sizeof(form.status), "%s", message);
            }
        }
    }
    curs_set(0);
    timeout(1000);
}

/* New agent form ----------------------------------------------------------- */

static void agent_form_ensure_runner(TuiAgentForm *form) {
    if (!form->runner[0])
        snprintf(form->runner, sizeof(form->runner), "codex");
}

static void agent_form_toggle_runner(TuiAgentForm *form) {
    agent_form_ensure_runner(form);
    snprintf(form->runner, sizeof(form->runner), "%s",
             strcmp(form->runner, "codex") == 0 ? "claude" : "codex");
}

static void agent_form_buffer(TuiAgentForm *form, int field, char **buffer,
                              size_t *buffer_size) {
    if (field == TUI_AGENT_FORM_MODEL) {
        *buffer = form->model;
        *buffer_size = sizeof(form->model);
    } else if (field == TUI_AGENT_FORM_NAME) {
        *buffer = form->name;
        *buffer_size = sizeof(form->name);
    } else {
        *buffer = NULL;
        *buffer_size = 0;
    }
}

static void draw_agent_form_no_run_box(int y, int x, int width,
                                       const TuiAgentForm *form) {
    draw_task_form_box(y, x, width, 3, "No run script",
                       form->no_run_script ? "yes" : "no",
                       form->active_field == TUI_AGENT_FORM_NO_RUN_SCRIPT);
}

static void agent_form_move_cursor(const TuiAgentForm *form, int rows, int cols,
                                   int left, int width) {
    if (form->active_field != TUI_AGENT_FORM_NAME &&
        form->active_field != TUI_AGENT_FORM_MODEL) {
        return;
    }

    char *buffer = NULL;
    size_t buffer_size = 0;
    TuiAgentForm mutable_form = *form;
    agent_form_buffer(&mutable_form, form->active_field, &buffer, &buffer_size);
    (void)buffer_size;
    if (!buffer)
        return;

    int y = form->active_field == TUI_AGENT_FORM_NAME ? 3 : 11;
    int inner_width = width - 4;
    if (inner_width <= 0)
        return;
    int len = (int)strlen(buffer);
    if (len > inner_width - 1)
        len = inner_width - 1;
    int cursor_y = y + 2;
    int cursor_x = left + 2 + len;
    if (cursor_y < rows - 1 && cursor_x < cols)
        move(cursor_y, cursor_x);
}

void render_agent_form_screen(DispatchTui *tui, const TuiAgentForm *form) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    erase();

    draw_title_bar(tui, "New Agent");

    if (rows < 22 || cols < 50) {
        draw_truncated(2, 0, cols, "Terminal too small for agent form.");
        draw_truncated(3, 0, cols, "Resize to at least 50x22.");
        refresh();
        return;
    }

    int width = cols - 4;
    if (width > 70)
        width = 70;
    int left = (cols - width) / 2;

    char heading[256];
    snprintf(heading, sizeof(heading), "Creating agent as %s", tui->actor);
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    draw_truncated(1, left, width, heading);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);

    char runner[16];
    snprintf(runner, sizeof(runner), "%s",
             form->runner[0] ? form->runner : "codex");

    draw_task_form_box(3, left, width, 3, "Name", form->name,
                       form->active_field == TUI_AGENT_FORM_NAME);
    draw_task_form_box(7, left, width, 3, "Runner", runner,
                       form->active_field == TUI_AGENT_FORM_RUNNER);
    draw_task_form_box(11, left, width, 3, "Model (blank = runner default)",
                       form->model,
                       form->active_field == TUI_AGENT_FORM_MODEL);
    draw_agent_form_no_run_box(15, left, width, form);

    draw_footer(form->status[0] ? form->status : "New agent",
                tui_footer_hints(TUI_SCREEN_AGENT_FORM));

    agent_form_move_cursor(form, rows, cols, left, width);
    refresh();
}

static int handle_agent_form_key(TuiAgentForm *form, int ch, int *submit,
                                 int *cancel) {
    *submit = 0;
    *cancel = 0;
    agent_form_ensure_runner(form);

    if (ch == 27) {
        *cancel = 1;
        return 1;
    }
    if (ch == '\t' || ch == KEY_DOWN) {
        form->active_field =
            (form->active_field + 1) % TUI_AGENT_FORM_FIELD_COUNT;
        return 1;
    }
    if (ch == KEY_UP) {
        form->active_field =
            (form->active_field + TUI_AGENT_FORM_FIELD_COUNT - 1) %
            TUI_AGENT_FORM_FIELD_COUNT;
        return 1;
    }
    if (ch == '\n' || ch == KEY_ENTER) {
        if (form->active_field == TUI_AGENT_FORM_NO_RUN_SCRIPT)
            *submit = 1;
        else
            form->active_field++;
        return 1;
    }
    if (form->active_field == TUI_AGENT_FORM_RUNNER) {
        if (ch == 'c' || ch == 'C') {
            snprintf(form->runner, sizeof(form->runner), "codex");
        } else if (ch == 'l' || ch == 'L') {
            snprintf(form->runner, sizeof(form->runner), "claude");
        } else if (ch == ' ') {
            agent_form_toggle_runner(form);
        }
        return 1;
    }
    if (form->active_field == TUI_AGENT_FORM_NO_RUN_SCRIPT) {
        if (ch == 'y' || ch == 'Y')
            form->no_run_script = 1;
        else if (ch == 'n' || ch == 'N')
            form->no_run_script = 0;
        else if (ch == ' ')
            form->no_run_script = !form->no_run_script;
        return 1;
    }

    char *buffer = NULL;
    size_t buffer_size = 0;
    agent_form_buffer(form, form->active_field, &buffer, &buffer_size);
    if (!buffer || buffer_size == 0)
        return 1;
    size_t len = strlen(buffer);
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (len > 0)
            buffer[len - 1] = '\0';
        return 1;
    }
    if (ch == 21) {
        buffer[0] = '\0';
        return 1;
    }
    if (ch >= 0 && ch < 256 && isprint((unsigned char)ch) &&
        len + 1 < buffer_size) {
        buffer[len] = (char)ch;
        buffer[len + 1] = '\0';
    }
    return 1;
}

/* New group form ----------------------------------------------------------- */

typedef struct {
    char name[128];
    char prefix[16];
    int active_field;
    char status[256];
} TuiGroupForm;

enum {
    TUI_GROUP_FORM_NAME = 0,
    TUI_GROUP_FORM_PREFIX = 1,
    TUI_GROUP_FORM_FIELD_COUNT = 2,
};

static void group_form_buffer(TuiGroupForm *form, int field, char **buffer,
                              size_t *buffer_size) {
    if (field == TUI_GROUP_FORM_PREFIX) {
        *buffer = form->prefix;
        *buffer_size = sizeof(form->prefix);
    } else {
        *buffer = form->name;
        *buffer_size = sizeof(form->name);
    }
}

static void render_group_form_screen(DispatchTui *tui,
                                     const TuiGroupForm *form) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    erase();

    draw_title_bar(tui, "New Group");

    if (rows < 14 || cols < 40) {
        draw_truncated(2, 0, cols, "Terminal too small for group form.");
        draw_truncated(3, 0, cols, "Resize to at least 40x14.");
        refresh();
        return;
    }

    int width = cols - 4;
    if (width > 60)
        width = 60;
    int left = (cols - width) / 2;

    char heading[256];
    snprintf(heading, sizeof(heading), "Creating group as %s", tui->actor);
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    draw_truncated(1, left, width, heading);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);

    draw_task_form_box(3, left, width, 3, "Name", form->name,
                       form->active_field == TUI_GROUP_FORM_NAME);
    draw_task_form_box(7, left, width, 3, "Prefix (blank = auto)", form->prefix,
                       form->active_field == TUI_GROUP_FORM_PREFIX);

    draw_footer(form->status[0] ? form->status : "New group",
                tui_footer_hints(TUI_SCREEN_GROUP_FORM));

    int box_y = form->active_field == TUI_GROUP_FORM_NAME ? 3 : 7;
    char *buffer = NULL;
    size_t buffer_size = 0;
    TuiGroupForm mutable_form = *form;
    group_form_buffer(&mutable_form, form->active_field, &buffer, &buffer_size);
    int inner_width = width - 4;
    if (buffer && inner_width > 0) {
        int len = (int)strlen(buffer);
        if (len > inner_width - 1)
            len = inner_width - 1;
        int cursor_y = box_y + 2;
        int cursor_x = left + 2 + len;
        if (cursor_y < rows - 1 && cursor_x < cols)
            move(cursor_y, cursor_x);
    }
    refresh();
}

static int handle_group_form_key(TuiGroupForm *form, int ch, int *submit,
                                 int *cancel) {
    *submit = 0;
    *cancel = 0;
    if (ch == 27) {
        *cancel = 1;
        return 1;
    }
    if (ch == '\t' || ch == KEY_DOWN) {
        form->active_field =
            (form->active_field + 1) % TUI_GROUP_FORM_FIELD_COUNT;
        return 1;
    }
    if (ch == KEY_UP) {
        form->active_field =
            (form->active_field + TUI_GROUP_FORM_FIELD_COUNT - 1) %
            TUI_GROUP_FORM_FIELD_COUNT;
        return 1;
    }
    if (ch == '\n' || ch == KEY_ENTER) {
        if (form->active_field == TUI_GROUP_FORM_PREFIX)
            *submit = 1;
        else
            form->active_field++;
        return 1;
    }

    char *buffer = NULL;
    size_t buffer_size = 0;
    group_form_buffer(form, form->active_field, &buffer, &buffer_size);
    if (!buffer || buffer_size == 0)
        return 1;
    size_t len = strlen(buffer);
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (len > 0)
            buffer[len - 1] = '\0';
        return 1;
    }
    if (ch == 21) {
        buffer[0] = '\0';
        return 1;
    }
    if (ch >= 0 && ch < 256 && isprint((unsigned char)ch) &&
        len + 1 < buffer_size) {
        buffer[len] = (char)ch;
        buffer[len + 1] = '\0';
    }
    return 1;
}

static void run_group_form(DispatchTui *tui) {
    TuiGroupForm form;
    memset(&form, 0, sizeof(form));
    form.active_field = TUI_GROUP_FORM_NAME;
    snprintf(form.status, sizeof(form.status), "Fill group fields");

    timeout(-1);
    curs_set(1);
    int running = 1;
    while (running) {
        render_group_form_screen(tui, &form);
        int ch = getch();
        if (ch == KEY_RESIZE)
            continue;
        int submit = 0;
        int cancel = 0;
        handle_group_form_key(&form, ch, &submit, &cancel);
        if (cancel) {
            tui_set_status(tui, "Group creation cancelled");
            break;
        }
        if (submit) {
            char message[256] = {0};
            if (create_group(form.name, form.prefix, message,
                             sizeof(message))) {
                tui_load_board(tui);
                tui_set_status(tui, message);
                running = 0;
            } else {
                snprintf(form.status, sizeof(form.status), "%s", message);
            }
        }
    }
    curs_set(0);
    timeout(1000);
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
    tui->log_top = 0;
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

static void run_command_palette(DispatchTui *tui) {
    char command[512];
    if (!prompt_line(": ", command, sizeof(command), ""))
        return;
    execute_palette_command(tui, command);
}

static void print_palette_completion(const DispatchBoard *board,
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

static int set_agent_session_id(const char *name, const char *session_id,
                                char *message, size_t message_size) {
    char *trimmed = tui_trimmed_copy(session_id);
    int ok = update_agent_session_metadata(
        name, trimmed[0] ? trimmed : NULL, NULL, NULL, !trimmed[0], 0, 0,
        message, message_size);
    free(trimmed);
    return ok;
}

static void set_selected_agent_session(DispatchTui *tui) {
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

static void draw_padded(int y, int x, int width, const char *text) {
    if (width <= 0)
        return;
    mvhline(y, x, ' ', width);
    mvaddnstr(y, x, text ? text : "", width);
}

static const char *tui_view_name(DispatchTuiScreen screen) {
    switch (screen) {
    case TUI_SCREEN_BOARD:
        return "Board";
    case TUI_SCREEN_TASK_INSPECTOR:
        return "Task";
    case TUI_SCREEN_AGENTS:
        return "Agents";
    case TUI_SCREEN_AGENT_INSPECTOR:
        return "Agent";
    case TUI_SCREEN_AGENT_FORM:
        return "New Agent";
    case TUI_SCREEN_TASK_FORM:
        return "New Task";
    case TUI_SCREEN_GROUP_FORM:
        return "New Group";
    case TUI_SCREEN_WORKSPACES:
        return "Workspaces";
    case TUI_SCREEN_WORKSPACE_INSPECTOR:
        return "Workspace";
    case TUI_SCREEN_LOGS:
        return "Logs";
    }
    return "Board";
}

/* Persistent branded title bar drawn at row 0 of every screen: a green
 * "DISPATCH" chip, the current view name, and right-aligned actor/board
 * context. */
static void draw_title_bar(DispatchTui *tui, const char *view) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    (void)rows;
    if (cols <= 0)
        return;

    tui_style_title_on();
    mvhline(0, 0, ' ', cols);

    const char *brand = " DISPATCH ";
    int x = 0;
    tui_style_title_off();
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_BRAND) | A_BOLD);
    else
        attron(A_REVERSE | A_BOLD);
    mvaddnstr(0, x, brand, cols);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_BRAND) | A_BOLD);
    else
        attroff(A_REVERSE | A_BOLD);
    x += (int)strlen(brand);

    tui_style_title_on();
    if (x < cols) {
        char label[128];
        snprintf(label, sizeof(label), "  %s", view ? view : "");
        mvaddnstr(0, x, label, cols - x);
    }

    char context[256];
    snprintf(context, sizeof(context), "actor:%s  board:%s ", tui->actor,
             tui->board_loaded && tui->board.name ? tui->board.name : "-");
    int ctx_len = (int)strlen(context);
    int ctx_x = cols - ctx_len;
    int label_end = x + (int)strlen(view ? view : "") + 2;
    if (ctx_x > label_end + 1)
        mvaddnstr(0, ctx_x, context, cols - ctx_x);
    tui_style_title_off();
}

/* Persistent footer at the last row: a bold status message on the left and
 * right-aligned, recessed key hints on the right. */
static void draw_footer(const char *message, const char *hints) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows < 1 || cols <= 0)
        return;
    int y = rows - 1;

    attron(A_REVERSE);
    mvhline(y, 0, ' ', cols);

    char left[256];
    snprintf(left, sizeof(left), " %s ",
             message && message[0] ? message : "Ready");
    attron(A_BOLD);
    mvaddnstr(y, 0, left, cols);
    attroff(A_BOLD);
    int left_len = (int)strlen(left);

    if (hints && hints[0]) {
        int hlen = (int)strlen(hints);
        int hx = cols - hlen - 1;
        if (hx > left_len + 2) {
            mvaddnstr(y, hx, hints, cols - hx);
        } else if (left_len + 1 < cols) {
            mvaddnstr(y, left_len + 1, hints, cols - left_len - 1);
        }
    }
    attroff(A_REVERSE);
}

static const char *tui_footer_hints(DispatchTuiScreen screen) {
    switch (screen) {
    case TUI_SCREEN_TASK_INSPECTOR:
        return "q/Esc back   > add dep   < remove dep   d diff   L logs";
    case TUI_SCREEN_AGENT_INSPECTOR:
        return "q/Esc back   y run   e prompt   S session   x clear   z archive";
    case TUI_SCREEN_WORKSPACE_INSPECTOR:
        return "q/Esc back   x remove   X force remove";
    case TUI_SCREEN_WORKSPACES:
        return "b board   a agents   n create   x/X remove   P prune";
    case TUI_SCREEN_LOGS:
        return "b board   F filter   C clear   j/k move";
    case TUI_SCREEN_AGENTS:
        return "Enter/i inspect   y run   A all/enabled   z archive   Tab board";
    case TUI_SCREEN_AGENT_FORM:
        return "Enter next/save   Tab move   Space toggle   Esc cancel";
    case TUI_SCREEN_TASK_FORM:
        return "Enter next/save   Tab move   ^O options   Space review   Esc cancel";
    case TUI_SCREEN_GROUP_FORM:
        return "Enter next/save   Tab move   Esc cancel";
    case TUI_SCREEN_BOARD:
    default:
        return ": palette   Tab/a agents   w workspaces   n task   + group   ? help";
    }
}

static int tui_state_color(DispatchState state) {
    switch (state) {
    case DISPATCH_STATE_READY:
        return TUI_COLOR_STATE_READY;
    case DISPATCH_STATE_DOING:
        return TUI_COLOR_STATE_DOING;
    case DISPATCH_STATE_REVIEW:
        return TUI_COLOR_STATE_REVIEW;
    case DISPATCH_STATE_BLOCKED:
        return TUI_COLOR_STATE_BLOCKED;
    case DISPATCH_STATE_DONE:
        return TUI_COLOR_STATE_DONE;
    case DISPATCH_STATE_PROPOSED:
        return TUI_COLOR_STATE_PROPOSED;
    case DISPATCH_STATE_PAUSED:
        return TUI_COLOR_MUTED;
    }
    return TUI_COLOR_MUTED;
}

/* Draw a colored "<count> <label>" chip and return the next x position. */
static int draw_count_chip(int y, int x, int cols, int count, const char *label,
                           int color, int dim) {
    if (x >= cols)
        return x;
    char chip[64];
    snprintf(chip, sizeof(chip), "%d %s", count, label);
    int attr = dim ? A_DIM : A_BOLD;
    if (tui_colors_enabled)
        attron(COLOR_PAIR(color) | attr);
    else
        attron(attr);
    mvaddnstr(y, x, chip, cols - x);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(color) | attr);
    else
        attroff(attr);
    return x + (int)strlen(chip) + 3;
}

/* Board header: a colored strip of live task-state counts. */
static void draw_board_stats(DispatchTui *tui, int y, int cols) {
    int x = 0;
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    mvaddnstr(y, x, "Tasks  ", cols);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    x += 7;

    int proposed = task_count_for_state(&tui->board, DISPATCH_STATE_PROPOSED);
    x = draw_count_chip(y, x, cols,
                        task_count_for_state(&tui->board, DISPATCH_STATE_READY),
                        "ready", tui_state_color(DISPATCH_STATE_READY), 0);
    x = draw_count_chip(y, x, cols,
                        task_count_for_state(&tui->board, DISPATCH_STATE_DOING),
                        "doing", tui_state_color(DISPATCH_STATE_DOING), 0);
    x = draw_count_chip(y, x, cols,
                        task_count_for_state(&tui->board, DISPATCH_STATE_REVIEW),
                        "review", tui_state_color(DISPATCH_STATE_REVIEW), 0);
    x = draw_count_chip(y, x, cols,
                        task_count_for_state(&tui->board, DISPATCH_STATE_BLOCKED),
                        "blocked", tui_state_color(DISPATCH_STATE_BLOCKED), 0);
    if (proposed > 0)
        x = draw_count_chip(y, x, cols, proposed, "proposed",
                            tui_state_color(DISPATCH_STATE_PROPOSED), 0);
    x = draw_count_chip(y, x, cols,
                        task_count_for_state(&tui->board, DISPATCH_STATE_DONE),
                        "done", tui_state_color(DISPATCH_STATE_DONE), 1);
    (void)x;
}

/* Draw a small "<text>" chip with a leading/trailing space and return next x. */
static int draw_tag(int y, int x, int cols, const char *text, int color,
                    int reverse) {
    if (x >= cols)
        return x;
    int attr = A_BOLD | (reverse ? A_REVERSE : 0);
    char buf[128];
    snprintf(buf, sizeof(buf), " %s ", text);
    if (tui_colors_enabled)
        attron(COLOR_PAIR(color) | attr);
    else
        attron(attr);
    mvaddnstr(y, x, buf, cols - x);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(color) | attr);
    else
        attroff(attr);
    return x + (int)strlen(buf) + 1;
}

/* Board filter line: the active view filter highlighted as a chip, followed by
 * chips for any active group / actor / search refinements. */
static void draw_filter_line(DispatchTui *tui, int y, int cols) {
    int x = 0;
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    mvaddnstr(y, x, "View  ", cols);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    x += 6;

    x = draw_tag(y, x, cols, filter_name(tui->filter), TUI_COLOR_ACCENT, 1);

    int has_refine = 0;
    char tag[160];
    if (tui->group_filter >= 0 &&
        (size_t)tui->group_filter < tui->board.groups.count) {
        snprintf(tag, sizeof(tag), "group %s",
                 tui->board.groups.items[tui->group_filter].prefix);
        x = draw_tag(y, x, cols, tag, TUI_COLOR_HEADER, 0);
        has_refine = 1;
    }
    const char *actor = actor_filter_value(&tui->board, tui->actor_filter);
    if (actor) {
        snprintf(tag, sizeof(tag), "actor %s", actor);
        x = draw_tag(y, x, cols, tag, TUI_COLOR_HEADER, 0);
        has_refine = 1;
    }
    if (tui->search[0] || tui->search_active) {
        snprintf(tag, sizeof(tag), "search %s%s", tui->search,
                 tui->search_active ? "_" : "");
        x = draw_tag(y, x, cols, tag, TUI_COLOR_STATE_DOING,
                     tui->search_active);
        has_refine = 1;
    }

    if (!has_refine && x + 4 < cols) {
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        draw_truncated(y, x + 2, cols - x - 2,
                       "1-7/R filter   G group   A actor   / search   c clear");
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    }
}

typedef struct {
    const char *key;
    const char *desc;
} HelpItem;

/* Per-view control lists. A NULL key marks a section heading. HL-04 renders the
 * help overlay from the list for the current screen so each view shows only and
 * all of its own controls. */
static const HelpItem help_board_items[] = {
    {NULL, "Navigation"},
    {"j / k", "move selection"},
    {"Enter / i", "inspect task"},
    {"Tab", "switch to agents"},
    {NULL, "Views"},
    {"b a w l", "board agents ws logs"},
    {":", "command palette"},
    {NULL, "Tasks"},
    {"r s f v", "ready start finish review"},
    {"n  +", "new task / group"},
    {"d", "diff selected"},
    {NULL, "Filter & search"},
    {"1-7  R", "filter presets"},
    {"G  A", "cycle group / actor"},
    {"/  c", "search / clear refine"},
    {NULL, "General"},
    {"u", "reload board"},
    {"h / ?", "toggle help"},
    {"^C  :q", "quit"},
};

static const HelpItem help_task_inspector_items[] = {
    {NULL, "Task inspector"},
    {"q / Esc", "back to board"},
    {"r s f v", "ready start finish review"},
    {">", "add dependency"},
    {"<", "remove dependency"},
    {"d", "diff"},
    {"L", "task logs"},
    {"h / ?", "toggle help"},
};

static const HelpItem help_agents_items[] = {
    {NULL, "Agents"},
    {"j / k", "move selection"},
    {"Enter / i", "inspect agent"},
    {"y", "run command"},
    {"z", "archive / restore"},
    {"A", "all / enabled"},
    {"Tab / b", "back to board"},
    {"h / ?", "toggle help"},
};

static const HelpItem help_agent_inspector_items[] = {
    {NULL, "Agent inspector"},
    {"q / Esc", "back to agents"},
    {"y", "run command"},
    {"e", "edit prompt"},
    {"S", "set session"},
    {"x", "clear session"},
    {"z", "archive / restore"},
    {"L", "agent logs"},
    {"h / ?", "toggle help"},
};

static const HelpItem help_workspaces_items[] = {
    {NULL, "Workspaces"},
    {"j / k", "move selection"},
    {"Enter / i", "inspect workspace"},
    {"n", "create workspace"},
    {"x", "remove"},
    {"X", "force remove"},
    {"P", "prune"},
    {"b  a", "board / agents"},
    {"h / ?", "toggle help"},
};

static const HelpItem help_workspace_inspector_items[] = {
    {NULL, "Workspace inspector"},
    {"q / Esc", "back to workspaces"},
    {"x", "remove"},
    {"X", "force remove"},
    {"h / ?", "toggle help"},
};

static const HelpItem help_logs_items[] = {
    {NULL, "Logs"},
    {"j / k", "move selection"},
    {"F", "filter logs"},
    {"C", "clear filter"},
    {"b", "back to board"},
    {"h / ?", "toggle help"},
};

static const HelpItem help_task_form_items[] = {
    {NULL, "New task"},
    {"Enter", "next / save"},
    {"Tab", "move field"},
    {"^O", "options"},
    {"Space", "toggle review"},
    {"Esc", "cancel"},
};

static const HelpItem help_group_form_items[] = {
    {NULL, "New group"},
    {"Enter", "next / save"},
    {"Tab", "move field"},
    {"Esc", "cancel"},
};

static const HelpItem help_agent_form_items[] = {
    {NULL, "New agent"},
    {"Enter", "next / save"},
    {"Tab", "move field"},
    {"Space", "toggle no-run-script"},
    {"Esc", "cancel"},
};

/* Return the control list for a screen, writing its length to *count. */
static const HelpItem *help_controls_for_screen(DispatchTuiScreen screen,
                                                int *count) {
#define HELP_VIEW(items)                                                       \
    do {                                                                       \
        *count = (int)(sizeof(items) / sizeof((items)[0]));                    \
        return (items);                                                        \
    } while (0)

    switch (screen) {
    case TUI_SCREEN_TASK_INSPECTOR:
        HELP_VIEW(help_task_inspector_items);
    case TUI_SCREEN_AGENTS:
        HELP_VIEW(help_agents_items);
    case TUI_SCREEN_AGENT_INSPECTOR:
        HELP_VIEW(help_agent_inspector_items);
    case TUI_SCREEN_AGENT_FORM:
        HELP_VIEW(help_agent_form_items);
    case TUI_SCREEN_TASK_FORM:
        HELP_VIEW(help_task_form_items);
    case TUI_SCREEN_GROUP_FORM:
        HELP_VIEW(help_group_form_items);
    case TUI_SCREEN_WORKSPACES:
        HELP_VIEW(help_workspaces_items);
    case TUI_SCREEN_WORKSPACE_INSPECTOR:
        HELP_VIEW(help_workspace_inspector_items);
    case TUI_SCREEN_LOGS:
        HELP_VIEW(help_logs_items);
    case TUI_SCREEN_BOARD:
    default:
        HELP_VIEW(help_board_items);
    }

#undef HELP_VIEW
}

/* Draw one column of help items: NULL key means a section heading. */
static void help_draw_column(int top_y, int x, int colw, const HelpItem *items,
                             int n, int max_y) {
    int y = top_y;
    for (int i = 0; i < n && y < max_y; i++, y++) {
        if (!items[i].key) {
            if (tui_colors_enabled)
                attron(COLOR_PAIR(TUI_COLOR_ACCENT) | A_BOLD);
            else
                attron(A_BOLD | A_UNDERLINE);
            mvaddnstr(y, x, items[i].desc, colw);
            if (tui_colors_enabled)
                attroff(COLOR_PAIR(TUI_COLOR_ACCENT) | A_BOLD);
            else
                attroff(A_BOLD | A_UNDERLINE);
        } else {
            attron(A_BOLD);
            mvaddnstr(y, x, items[i].key, 11);
            attroff(A_BOLD);
            if (tui_colors_enabled)
                attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
            if (x + 12 < x + colw)
                mvaddnstr(y, x + 12, items[i].desc, colw - 12);
            if (tui_colors_enabled)
                attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        }
    }
}

static void tui_render_help(DispatchTuiScreen screen) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);

    int count = 0;
    const HelpItem *items = help_controls_for_screen(screen, &count);

    /* Lay the current view's controls out in a single column, splitting long
     * lists into two columns at a section heading near the midpoint so every
     * control stays visible and whole sections stay together. */
    int rows_for_list = rows - 8;
    if (rows_for_list < 1)
        rows_for_list = 1;
    int split = count;
    if (count > rows_for_list) {
        split = (count + 1) / 2;
        for (int i = split; i < count; i++) {
            if (!items[i].key) {
                split = i;
                break;
            }
        }
    }
    const HelpItem *left_items = items;
    int left_n = split;
    const HelpItem *right_items = items + split;
    int right_n = count - split;
    int two_col = right_n > 0;
    int content = left_n > right_n ? left_n : right_n;

    int width = two_col ? 72 : 44;
    if (width > cols - 2)
        width = cols - 2;
    if (width < 24)
        width = cols;
    int height = content + 6;
    if (height > rows - 2)
        height = rows - 2;
    int top = rows > height ? (rows - height) / 2 : 0;
    int left = cols > width ? (cols - width) / 2 : 0;

    for (int yy = 0; yy < height && top + yy < rows; yy++)
        mvhline(top + yy, left, ' ', width);

    int border = (tui_colors_enabled ? COLOR_PAIR(TUI_COLOR_ACCENT) : 0) | A_BOLD;
    attron(border);
    mvaddch(top, left, ACS_ULCORNER);
    mvhline(top, left + 1, ACS_HLINE, width - 2);
    mvaddch(top, left + width - 1, ACS_URCORNER);
    for (int yy = 1; yy < height - 1; yy++) {
        mvaddch(top + yy, left, ACS_VLINE);
        mvaddch(top + yy, left + width - 1, ACS_VLINE);
    }
    mvaddch(top + height - 1, left, ACS_LLCORNER);
    mvhline(top + height - 1, left + 1, ACS_HLINE, width - 2);
    mvaddch(top + height - 1, left + width - 1, ACS_LRCORNER);
    attroff(border);

    const char *title = "Dispatch  Keyboard Shortcuts";
    attron(A_BOLD);
    mvaddnstr(top + 1, left + (width - (int)strlen(title)) / 2, title,
              width - 2);
    attroff(A_BOLD);

    int max_y = top + height - 2;
    if (two_col) {
        int colw = width / 2 - 4;
        help_draw_column(top + 3, left + 3, colw, left_items, left_n, max_y);
        help_draw_column(top + 3, left + width / 2 + 1, colw, right_items,
                         right_n, max_y);
    } else {
        help_draw_column(top + 3, left + 3, width - 6, left_items, left_n,
                         max_y);
    }

    const char *hint = "press h, ?, or Esc to close";
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    mvaddnstr(top + height - 2, left + (width - (int)strlen(hint)) / 2, hint,
              width - 2);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
}

/* Column header for the list views: accent, bold, drawn over a full-width row
 * so it reads as a header band. */
static void draw_list_header(int y, int cols, const char *text) {
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_ACCENT) | A_BOLD);
    else
        attron(A_BOLD | A_UNDERLINE);
    draw_padded(y, 0, cols, text);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_ACCENT) | A_BOLD);
    else
        attroff(A_BOLD | A_UNDERLINE);
}

static void draw_agent_rows(DispatchTui *tui, int start_y, int rows, int cols) {
    int y = start_y;
    if (visible_agent_count(tui) == 0) {
        draw_padded(y, 0, cols, "(no agents)");
        return;
    }

    int visible_rows = rows - start_y - 2;
    sync_agent_scroll(tui, visible_rows);
    char header[256];
    snprintf(header, sizeof(header), "  %-16s %-8s %-9s %-7s %-12s %s", "Name",
             "Runner", "Status", "Session", "Task", "Workspace");
    draw_list_header(y++, cols, header);

    int visible_index = 0;
    for (size_t i = 0; i < tui->board.agents.count && y < rows - 1; i++) {
        DispatchAgent *agent = &tui->board.agents.items[i];
        if (!agent_is_visible(tui, agent))
            continue;
        if (visible_index < tui->agent_top) {
            visible_index++;
            continue;
        }
        int selected = visible_index == tui->selected_agent;
        const char *status = agent->archived ? "archived" : "enabled";
        char line[1024];
        snprintf(line, sizeof(line), "%s%-16s %-8s %-9s %-7s %-12s %s",
                 selected ? " >" : "  ", agent->name, agent->runner, status,
                 agent->session_id ? "yes" : "no",
                 agent->current_task ? agent->current_task : "-",
                 agent->last_workspace ? agent->last_workspace : "-");
        tui_style_row_on(visible_index, selected);
        draw_padded(y, 0, cols, line);
        tui_style_row_off(visible_index, selected);

        if (!selected && tui_colors_enabled) {
            int color = agent->archived ? TUI_COLOR_MUTED : TUI_COLOR_STATE_READY;
            attron(COLOR_PAIR(color) | A_BOLD);
            mvaddnstr(y, 28, status, 9);
            attroff(COLOR_PAIR(color) | A_BOLD);
        }
        y++;
        visible_index++;
    }
}

static void draw_workspace_rows(DispatchTui *tui, int start_y, int rows,
                                int cols) {
    int y = start_y;
    int visible_index = 0;
    if (visible_workspace_count(tui) == 0) {
        draw_padded(y, 0, cols, "(no active workspaces)");
        return;
    }

    int visible_rows = rows - start_y - 2;
    sync_workspace_scroll(tui, visible_rows);
    char header[256];
    snprintf(header, sizeof(header), "  %-8s %-9s %-9s %-16s %-5s %-7s %s",
             "Task", "State", "WS", "Actor", "Dirty", "Git", "Branch / Path");
    draw_list_header(y++, cols, header);

    for (size_t i = 0; i < tui->board.workspaces.count && y < rows - 1; i++) {
        DispatchWorkspace *workspace = &tui->board.workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
            continue;
        if (visible_index < tui->workspace_top) {
            visible_index++;
            continue;
        }
        DispatchTask *task =
            dispatch_board_find_task(&tui->board, workspace->task_id);
        int has_state = task != NULL;
        DispatchState wstate =
            has_state ? dispatch_task_effective_state(&tui->board, task)
                      : DISPATCH_STATE_PROPOSED;
        const char *task_state = has_state ? dispatch_state_name(wstate)
                                           : "missing";
        int git_present = path_has_git_metadata(workspace->path);
        int dirty = workspace_is_dirty(workspace);
        int selected = visible_index == tui->selected_workspace;

        char branchpath[1100];
        snprintf(branchpath, sizeof(branchpath), "%s  %s", workspace->branch,
                 workspace->path);
        char line[1200];
        snprintf(line, sizeof(line),
                 "%s%-8.8s %-9.9s %-9.9s %-16.16s %-5s %-7s %s",
                 selected ? " >" : "  ", workspace->task_id, task_state,
                 dispatch_workspace_state_name(workspace->state),
                 workspace->actor, dirty ? "dirty" : "clean",
                 git_present ? "git" : "no-git", branchpath);
        tui_style_row_on(visible_index, selected);
        draw_padded(y, 0, cols, line);
        tui_style_row_off(visible_index, selected);

        if (!selected && tui_colors_enabled) {
            int sc = has_state ? tui_state_color(wstate) : TUI_COLOR_MUTED;
            attron(COLOR_PAIR(sc) | A_BOLD);
            mvaddnstr(y, 11, task_state, 9);
            attroff(COLOR_PAIR(sc) | A_BOLD);
            int dc = dirty ? TUI_COLOR_STATE_DOING : TUI_COLOR_MUTED;
            attron(COLOR_PAIR(dc) | (dirty ? A_BOLD : A_DIM));
            mvaddnstr(y, 48, dirty ? "dirty" : "clean", 5);
            attroff(COLOR_PAIR(dc) | (dirty ? A_BOLD : A_DIM));
        }
        y++;
        visible_index++;
    }
}

static int log_action_color(const char *action) {
    if (!action || !action[0])
        return TUI_COLOR_MUTED;
    if (strstr(action, "ready"))
        return TUI_COLOR_STATE_READY;
    if (strstr(action, "start"))
        return TUI_COLOR_STATE_DOING;
    if (strstr(action, "finish") || strstr(action, "done") ||
        strstr(action, "complete"))
        return TUI_COLOR_STATE_DONE;
    if (strstr(action, "review"))
        return TUI_COLOR_STATE_REVIEW;
    if (strstr(action, "creat") || strstr(action, "add"))
        return TUI_COLOR_STATE_PROPOSED;
    if (strstr(action, "remov") || strstr(action, "delet") ||
        strstr(action, "archiv") || strstr(action, "block"))
        return TUI_COLOR_STATE_BLOCKED;
    return TUI_COLOR_MUTED;
}

static void draw_log_rows(DispatchTui *tui, int start_y, int rows, int cols) {
    int y = start_y;
    TuiLogRecords records;
    if (!load_matching_log_records(tui->log_filter_field, tui->log_filter_value,
                                   &records)) {
        draw_padded(y, 0, cols, "(no dispatch.log)");
        return;
    }

    int visible_rows = rows - start_y - 2;
    sync_log_scroll(tui, visible_rows);
    int visible_index = 0;
    int any_visible = 0;
    char header[256];
    snprintf(header, sizeof(header), "  %-20s %-14s %-10s %-12s %s", "Time",
             "Actor", "Command", "Action", "Target / Message");
    draw_list_header(y++, cols, header);

    for (size_t i = records.count; i > 0 && y < rows - 1; i--) {
        json_t *record = records.items[i - 1];
        if (visible_index < tui->log_top) {
            visible_index++;
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
            snprintf(target, sizeof(target), "task:%s", task);
        else if (agent[0])
            snprintf(target, sizeof(target), "agent:%s", agent);
        else if (workspace[0])
            snprintf(target, sizeof(target), "workspace:%s", workspace);

        char rest[1400];
        snprintf(rest, sizeof(rest), "%s%s%s", target, target[0] ? "  " : "",
                 message);
        int selected = visible_index == tui->selected_log;
        char row[1600];
        snprintf(row, sizeof(row), "%s%-20.20s %-14.14s %-10.10s %-12.12s %s",
                 selected ? " >" : "  ", time, actor, command, action, rest);
        tui_style_row_on(visible_index, selected);
        draw_padded(y, 0, cols, row);
        tui_style_row_off(visible_index, selected);

        if (!selected && tui_colors_enabled) {
            int color = log_action_color(action);
            attron(COLOR_PAIR(color) | A_BOLD);
            mvaddnstr(y, 49, action, 12);
            attroff(COLOR_PAIR(color) | A_BOLD);
        }
        y++;
        visible_index++;
        any_visible = 1;
    }
    log_records_free(&records);
    if (!any_visible)
        draw_padded(y, 0, cols, "(no matching log records)");
}

/* Shared inspector chrome --------------------------------------------------*/

/* Inspector title: an accent ID, the title, and (optionally) a colored state
 * badge on the right, with a dim rule underneath. Returns the next free row. */
static int draw_inspector_header(int y, int rows, int cols, const char *id,
                                 const char *title, const char *badge,
                                 int badge_color) {
    if (y >= rows - 1)
        return y;
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_ACCENT) | A_BOLD);
    else
        attron(A_BOLD);
    mvaddnstr(y, 2, id, cols - 2);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_ACCENT) | A_BOLD);
    else
        attroff(A_BOLD);

    int x = 2 + (int)strlen(id) + 2;
    if (title && title[0] && x < cols) {
        attron(A_BOLD);
        mvaddnstr(y, x, title, cols - x);
        attroff(A_BOLD);
    }
    if (badge && badge[0]) {
        int bx = cols - (int)strlen(badge) - 3;
        if (bx > x + (int)strlen(title ? title : "") + 1)
            draw_tag(y, bx, cols, badge, badge_color, 1);
    }
    if (y + 1 < rows - 1) {
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        mvhline(y + 1, 2, ACS_HLINE, cols - 4);
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    }
    return y + 2;
}

/* A "Label   value" row with a dim fixed-width label. */
static int draw_field(int y, int rows, int cols, const char *label,
                      const char *value) {
    if (y >= rows - 1)
        return y;
    char lab[24];
    snprintf(lab, sizeof(lab), "%-13s", label ? label : "");
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    mvaddnstr(y, 2, lab, cols - 2);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    if (2 + 15 < cols)
        draw_truncated(y, 2 + 15, cols - 2 - 15, value && value[0] ? value : "-");
    return y + 1;
}

/* A bold, accented section heading. */
static int draw_section(int y, int rows, int cols, const char *title) {
    if (y >= rows - 1)
        return y;
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_ACCENT) | A_BOLD);
    else
        attron(A_BOLD);
    mvaddnstr(y, 0, title, cols);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_ACCENT) | A_BOLD);
    else
        attroff(A_BOLD);
    return y + 1;
}

static void join_string_list(const DispatchStringList *list, char *buf,
                             size_t size) {
    if (size == 0)
        return;
    buf[0] = '\0';
    if (list->count == 0) {
        snprintf(buf, size, "-");
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < list->count && used + 2 < size; i++) {
        int written = snprintf(buf + used, size - used, "%s%s",
                               i == 0 ? "" : "  ", list->items[i]);
        if (written < 0)
            break;
        used += (size_t)written;
    }
}

static void blocks_text(DispatchBoard *board, const char *task_id, char *buf,
                        size_t size) {
    if (size == 0)
        return;
    buf[0] = '\0';
    size_t used = 0;
    int count = 0;
    for (size_t i = 0; i < board->tasks.count && used + 2 < size; i++) {
        DispatchTask *candidate = &board->tasks.items[i];
        for (size_t dep = 0; dep < candidate->depends_on.count; dep++) {
            if (strcmp(candidate->depends_on.items[dep], task_id) != 0)
                continue;
            int written = snprintf(buf + used, size - used, "%s%s",
                                   count == 0 ? "" : "  ", candidate->id);
            if (written > 0)
                used += (size_t)written;
            count++;
            break;
        }
    }
    if (count == 0)
        snprintf(buf, size, "-");
}

static void draw_task_inspector(DispatchTui *tui, int rows, int cols) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        draw_truncated(2, 0, cols, "No selected task.");
        return;
    }

    char buf[1024];
    DispatchState state = dispatch_task_effective_state(&tui->board, task);
    int y = draw_inspector_header(2, rows, cols, task->id, task->title,
                                  dispatch_state_name(state),
                                  tui_state_color(state));
    y++;

    y = draw_field(y, rows, cols, "Description",
                   task->description[0] ? task->description : "-");
    y++;

    y = draw_section(y, rows, cols, "Details");
    y = draw_field(y, rows, cols, "Group", task->group);
    y = draw_field(y, rows, cols, "Review gate",
                   task->requires_review ? "required" : "none");
    y = draw_field(y, rows, cols, "Assigned", task->assigned_to);
    y = draw_field(y, rows, cols, "Started by", task->started_by);
    y = draw_field(y, rows, cols, "Completed by", task->completed_by);
    y++;

    y = draw_section(y, rows, cols, "Dependencies");
    join_string_list(&task->depends_on, buf, sizeof(buf));
    y = draw_field(y, rows, cols, "Depends on", buf);
    blocks_text(&tui->board, task->id, buf, sizeof(buf));
    y = draw_field(y, rows, cols, "Blocks", buf);
    join_string_list(&task->commits, buf, sizeof(buf));
    y = draw_field(y, rows, cols, "Commits", buf);

    DispatchWorkspace *workspace = workspace_for_task(&tui->board, task->id);
    if (workspace) {
        y++;
        y = draw_section(y, rows, cols, "Workspace");
        y = draw_field(y, rows, cols, "Actor", workspace->actor);
        y = draw_field(y, rows, cols, "Branch", workspace->branch);
        y = draw_field(y, rows, cols, "Path", workspace->path);
    }

    y++;
    y = draw_section(y, rows, cols, "History");
    if (task->history.count == 0) {
        draw_field(y, rows, cols, "", "-");
    } else {
        for (size_t i = 0; i < task->history.count && y < rows - 1; i++) {
            DispatchHistoryEntry *entry = &task->history.items[i];
            snprintf(buf, sizeof(buf), "%s by %s%s%s", entry->action,
                     entry->actor, entry->note && entry->note[0] ? ": " : "",
                     entry->note && entry->note[0] ? entry->note : "");
            if (tui_colors_enabled)
                attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
            mvaddnstr(y, 2, "-", cols - 2);
            if (tui_colors_enabled)
                attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
            draw_truncated(y, 4, cols - 4, buf);
            y++;
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
    const char *status = agent->archived ? "archived" : "enabled";
    int status_color = agent->archived ? TUI_COLOR_MUTED : TUI_COLOR_STATE_READY;
    int y = draw_inspector_header(2, rows, cols, agent->name, agent->runner,
                                  status, status_color);
    y++;

    y = draw_section(y, rows, cols, "Runner");
    y = draw_field(y, rows, cols, "Model", agent->model);
    y = draw_field(y, rows, cols, "Prompt", agent->prompt_path);
    y = draw_field(y, rows, cols, "Run script", agent->run_script_path);
    y++;

    y = draw_section(y, rows, cols, "Session");
    y = draw_field(y, rows, cols, "Session ID", agent->session_id);
    y = draw_field(y, rows, cols, "Current task", agent->current_task);
    y = draw_field(y, rows, cols, "Last workspace", agent->last_workspace);
    if (strcmp(agent->runner, "codex") == 0)
        y = draw_field(y, rows, cols, "Codex", "manual; use dispatch agent session");
    y++;

    y = draw_section(y, rows, cols, "Recent completed tasks");
    int shown = 0;
    for (size_t i = tui->board.tasks.count; i > 0 && y < rows - 1; i--) {
        DispatchTask *task = &tui->board.tasks.items[i - 1];
        if (!task->completed_by || strcmp(task->completed_by, agent->name) != 0)
            continue;
        snprintf(line, sizeof(line), "%s  %s", task->id, task->title);
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        mvaddnstr(y, 2, "-", cols - 2);
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        draw_truncated(y, 4, cols - 4, line);
        y++;
        shown++;
        if (shown == 5)
            break;
    }
    if (shown == 0)
        draw_field(y, rows, cols, "", "-");
}

static void draw_workspace_inspector(DispatchTui *tui, int rows, int cols) {
    DispatchWorkspace *workspace = selected_visible_workspace(tui);
    if (!workspace) {
        draw_truncated(2, 0, cols, "No selected workspace.");
        return;
    }

    DispatchTask *task = dispatch_board_find_task(&tui->board,
                                                  workspace->task_id);
    int has_state = task != NULL;
    DispatchState wstate =
        has_state ? dispatch_task_effective_state(&tui->board, task)
                  : DISPATCH_STATE_PROPOSED;
    const char *task_state = has_state ? dispatch_state_name(wstate) : "missing";
    int badge_color = has_state ? tui_state_color(wstate) : TUI_COLOR_MUTED;

    char buf[1024];
    int y = draw_inspector_header(2, rows, cols, workspace->task_id,
                                  workspace->branch, task_state, badge_color);
    y++;

    y = draw_section(y, rows, cols, "Location");
    y = draw_field(y, rows, cols, "Actor", workspace->actor);
    y = draw_field(y, rows, cols, "Repo", workspace->repo_path);
    y = draw_field(y, rows, cols, "Path", workspace->path);
    y = draw_field(y, rows, cols, "Branch", workspace->branch);
    y++;

    y = draw_section(y, rows, cols, "Git");
    y = draw_field(y, rows, cols, "WS state",
                   dispatch_workspace_state_name(workspace->state));
    y = draw_field(y, rows, cols, "Worktree",
                   path_has_git_metadata(workspace->path) ? "present"
                                                          : "missing");
    y = draw_field(y, rows, cols, "Dirty",
                   workspace_is_dirty(workspace) ? "yes" : "no");
    y++;

    y = draw_section(y, rows, cols, "Sequence");
    join_string_list(&workspace->sequence_tasks, buf, sizeof(buf));
    y = draw_field(y, rows, cols, "Tasks", buf);
    y = draw_field(y, rows, cols, "Review gate", workspace->review_gate);
}

static int group_visible_task_count(DispatchTui *tui,
                                    const DispatchGroup *group) {
    int n = 0;
    for (size_t i = 0; i < tui->board.tasks.count; i++) {
        DispatchTask *task = &tui->board.tasks.items[i];
        if (strcmp(task->group, group->id) == 0 && tui_task_is_visible(tui, task))
            n++;
    }
    return n;
}

/* Group divider: a reverse prefix chip, the group name in accent, and a dim
 * count of how many tasks are visible under it. */
static void draw_group_header(DispatchTui *tui, int y, int cols,
                              const DispatchGroup *group) {
    mvhline(y, 0, ' ', cols);
    int x = draw_tag(y, 2, cols, group->prefix, TUI_COLOR_ACCENT, 1);
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_HEADER) | A_BOLD);
    else
        attron(A_BOLD);
    mvaddnstr(y, x, group->name, cols - x);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_HEADER) | A_BOLD);
    else
        attroff(A_BOLD);
    int cx = x + (int)strlen(group->name) + 2;
    char count[32];
    snprintf(count, sizeof(count), "%d", group_visible_task_count(tui, group));
    if (cx < cols) {
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        mvaddnstr(y, cx, count, cols - cx);
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    }
}

static void draw_task_row(DispatchTui *tui, int y, int cols,
                          const DispatchTask *task, int visible_index) {
    DispatchState state = dispatch_task_effective_state(&tui->board, task);
    int selected = visible_index == tui->selected_task;

    char left[1024];
    snprintf(left, sizeof(left), "%s %-7s %-8s %s", selected ? " >" : "  ",
             task->id, dispatch_state_name(state), task->title);

    tui_style_row_on(visible_index, selected);
    draw_padded(y, 0, cols, left);

    char tags[256] = {0};
    size_t tl = 0;
    if (task->requires_review && tl < sizeof(tags))
        tl += (size_t)snprintf(tags + tl, sizeof(tags) - tl, "review  ");
    if (task->commits.count > 0 && tl < sizeof(tags))
        tl += (size_t)snprintf(tags + tl, sizeof(tags) - tl, "%zu commit  ",
                               task->commits.count);
    if (task->assigned_to && task->assigned_to[0] && tl < sizeof(tags))
        tl += (size_t)snprintf(tags + tl, sizeof(tags) - tl, "@%s",
                               task->assigned_to);
    else if (task->completed_by && task->completed_by[0] && tl < sizeof(tags))
        tl += (size_t)snprintf(tags + tl, sizeof(tags) - tl, "by %s",
                               task->completed_by);
    if (tags[0]) {
        int tx = cols - (int)strlen(tags) - 1;
        if (tx > (int)strlen(left) + 2)
            mvaddnstr(y, tx, tags, cols - tx);
    }
    tui_style_row_off(visible_index, selected);

    /* Colored state badge sits at column 11 (after marker + id). */
    if (!selected && tui_colors_enabled) {
        attron(COLOR_PAIR(tui_state_color(state)) | A_BOLD);
        mvaddnstr(y, 11, dispatch_state_name(state), 8);
        attroff(COLOR_PAIR(tui_state_color(state)) | A_BOLD);
    }
}

static void draw_board_rows(DispatchTui *tui, int start_y, int rows, int cols) {
    int y = start_y;
    int visible_index = 0;
    int any_visible = 0;
    int visible_rows = rows - start_y - 1;
    sync_task_scroll(tui, visible_rows);

    for (size_t g = 0; g < tui->board.groups.count && y < rows - 1; g++) {
        DispatchGroup *group = &tui->board.groups.items[g];
        int group_header_drawn = 0;
        for (size_t i = 0; i < tui->board.tasks.count; i++) {
            DispatchTask *task = &tui->board.tasks.items[i];
            if (strcmp(task->group, group->id) != 0 ||
                !tui_task_is_visible(tui, task)) {
                continue;
            }
            if (visible_index < tui->task_top) {
                visible_index++;
                continue;
            }
            if (!group_header_drawn) {
                draw_group_header(tui, y++, cols, group);
                group_header_drawn = 1;
                if (y >= rows - 1)
                    break;
            }

            draw_task_row(tui, y++, cols, task, visible_index);
            visible_index++;
            any_visible = 1;
        }
    }

    if (!any_visible && y < rows - 1) {
        draw_padded(y, 0, cols,
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

    draw_title_bar(tui, tui_view_name(tui->screen));

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
        snprintf(line, sizeof(line), "%d active workspaces",
                 visible_workspace_count(tui));
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        draw_truncated(2, 0, cols, line);
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        draw_workspace_rows(tui, 4, rows, cols);
    } else if (tui->screen == TUI_SCREEN_LOGS) {
        char line[512];
        if (tui->log_filter_field[0])
            snprintf(line, sizeof(line), "%d entries   filter %s=%s",
                     visible_log_count(tui), tui->log_filter_field,
                     tui->log_filter_value);
        else
            snprintf(line, sizeof(line), "%d entries   no filter",
                     visible_log_count(tui));
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        draw_truncated(2, 0, cols, line);
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        draw_log_rows(tui, 4, rows, cols);
    } else if (tui->screen == TUI_SCREEN_AGENTS) {
        char line[512];
        int enabled = 0;
        for (size_t i = 0; i < tui->board.agents.count; i++)
            if (!tui->board.agents.items[i].archived)
                enabled++;
        snprintf(line, sizeof(line),
                 "%zu agents   %d enabled   %zu archived   showing %s",
                 tui->board.agents.count, enabled,
                 tui->board.agents.count - (size_t)enabled,
                 tui->show_archived_agents ? "all" : "enabled");
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        draw_truncated(2, 0, cols, line);
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        draw_agent_rows(tui, 4, rows, cols);
    } else {
        char line[512];
        draw_board_stats(tui, 2, cols);

        int visible = visible_task_count_for_tui(tui);
        snprintf(line, sizeof(line),
                 "Repo %s    %zu tasks (%d shown)    %zu groups    %zu agents    %zu workspaces",
                 tui->board.repo_path ? tui->board.repo_path : ".",
                 tui->board.tasks.count, visible, tui->board.groups.count,
                 tui->board.agents.count, tui->board.workspaces.count);
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        draw_truncated(4, 0, cols, line);
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);

        draw_filter_line(tui, 5, cols);

        draw_board_rows(tui, 7, rows, cols);
    }

    if (rows > 1)
        draw_footer(tui->status, tui_footer_hints(tui->screen));

    if (tui->show_help)
        tui_render_help(tui->screen);

    refresh();
}

static int tui_run(void) {
    DispatchTui tui;
    tui_init(&tui);
    tui_load_board(&tui);

    initscr();
    set_escdelay(TUI_ESCAPE_DELAY_MS);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    timeout(1000);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(TUI_COLOR_HEADER, COLOR_CYAN, -1);
        init_pair(TUI_COLOR_ALT_ROW, COLOR_WHITE, COLOR_BLACK);
        init_pair(TUI_COLOR_SELECTED, COLOR_BLACK, COLOR_CYAN);
        init_pair(TUI_COLOR_TITLE, COLOR_BLACK, COLOR_CYAN);
        init_pair(TUI_COLOR_BRAND, COLOR_BLACK, COLOR_GREEN);
        init_pair(TUI_COLOR_MUTED, COLOR_WHITE, -1);
        init_pair(TUI_COLOR_ACCENT, COLOR_CYAN, -1);
        init_pair(TUI_COLOR_STATE_READY, COLOR_GREEN, -1);
        init_pair(TUI_COLOR_STATE_DOING, COLOR_YELLOW, -1);
        init_pair(TUI_COLOR_STATE_REVIEW, COLOR_MAGENTA, -1);
        init_pair(TUI_COLOR_STATE_BLOCKED, COLOR_RED, -1);
        init_pair(TUI_COLOR_STATE_DONE, COLOR_BLUE, -1);
        init_pair(TUI_COLOR_STATE_PROPOSED, COLOR_CYAN, -1);
        tui_colors_enabled = 1;
    }
    signal(SIGINT, tui_handle_sigint);

    while (tui.running) {
        if (tui_quit_requested) {
            tui.running = 0;
            break;
        }
        tui_reload_if_changed(&tui);
        tui_render(&tui);
        int ch = getch();
        if (tui_quit_requested) {
            tui.running = 0;
            break;
        }
        if (ch == ERR)
            continue;
        if (tui.show_help && (ch == 27 || ch == 'q')) {
            tui.show_help = 0;
            continue;
        }
        if (handle_search_key(&tui, ch))
            continue;
        switch (ch) {
        case 'q':
            if (tui.screen == TUI_SCREEN_TASK_INSPECTOR) {
                tui.inspected_task_id[0] = '\0';
                tui.screen = TUI_SCREEN_BOARD;
            } else if (tui.screen == TUI_SCREEN_AGENT_INSPECTOR)
                tui.screen = TUI_SCREEN_AGENTS;
            else if (tui.screen == TUI_SCREEN_WORKSPACE_INSPECTOR)
                tui.screen = TUI_SCREEN_WORKSPACES;
            else
                tui_set_status(&tui, "Use :q or Ctrl+C to quit");
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
                tui.inspected_task_id[0] = '\0';
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
        case 'h':
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
        case 'S':
            if (tui.screen == TUI_SCREEN_AGENT_INSPECTOR)
                set_selected_agent_session(&tui);
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
        case 'y':
            if (tui.screen == TUI_SCREEN_AGENTS ||
                tui.screen == TUI_SCREEN_AGENT_INSPECTOR)
                copy_selected_agent_run_command(&tui);
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
                tui.agent_top = 0;
                clamp_agent_selection(&tui);
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
                tui.inspected_task_id[0] = '\0';
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
                tui.task_top = 0;
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
                tui.task_top = 0;
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
                    tui.task_top = 0;
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

static int tui_agent_set_session_smoke(const char *name, const char *session_id) {
    char message[256] = {0};
    if (!set_agent_session_id(name, strcmp(session_id, "-") == 0 ? "" : session_id,
                              message, sizeof(message))) {
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

    char editor_error[256] = {0};
    char *command = editor_command_for_path(agent->prompt_path, editor_error,
                                           sizeof(editor_error));
    if (!command) {
        fprintf(stderr, "%s\n", editor_error);
        dispatch_board_free(&board);
        return 1;
    }
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

static int tui_agent_selection_smoke(const char *mode, int selected_index) {
    DispatchTui tui;
    tui_init(&tui);
    tui.show_archived_agents = strcmp(mode, "all") == 0;
    tui.selected_agent = selected_index;
    if (!tui_load_board(&tui)) {
        fprintf(stderr, "%s\n", tui.status);
        return 1;
    }
    clamp_agent_selection(&tui);
    DispatchAgent *agent = selected_agent(&tui);
    printf("Mode: %s\n", tui.show_archived_agents ? "all" : "enabled");
    printf("Visible agents: %d\n", visible_agent_count(&tui));
    printf("Selected index: %d\n", tui.selected_agent);
    printf("Selected agent: %s\n", agent ? agent->name : "-");
    tui_free_board(&tui);
    return 0;
}

static int tui_agent_run_command_smoke(const char *name) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui agent run command smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchAgent *agent = dispatch_board_find_agent(&board, name);
    if (!agent) {
        fprintf(stderr, "No agent named %s\n", name);
        dispatch_board_free(&board);
        return 1;
    }

    char *command = agent_run_command_text(&board, agent);
    if (!command) {
        fprintf(stderr, "Could not build agent command for %s\n", name);
        dispatch_board_free(&board);
        return 1;
    }
    printf("%s\n", command);
    free(command);
    dispatch_board_free(&board);
    return 0;
}

static int tui_osc52_smoke(const char *text) {
    const char *value = text ? text : "";
    char *payload =
        tui_base64_encode((const unsigned char *)value, strlen(value));
    char *sequence = osc52_sequence_for_text(value);
    printf("OSC52 payload: %s\n", payload);
    printf("OSC52 sequence bytes: %zu\n", strlen(sequence));
    free(payload);
    free(sequence);
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
                     requires_review, depends_text, "user", message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int tui_task_form_submit_smoke(const char *group, const char *title,
                                      const char *description,
                                      const char *review_mode,
                                      const char *depends_text) {
    TuiTaskForm form;
    memset(&form, 0, sizeof(form));
    snprintf(form.group, sizeof(form.group), "%s", group ? group : "");
    snprintf(form.title, sizeof(form.title), "%s", title ? title : "");
    snprintf(form.description, sizeof(form.description), "%s",
             strcmp(description, "-") == 0 ? "" : description);
    snprintf(form.deps, sizeof(form.deps), "%s", depends_text ? depends_text : "");
    if (strcmp(review_mode, "review") == 0) {
        form.requires_review = 1;
    } else if (strcmp(review_mode, "no-review") == 0) {
        form.requires_review = 0;
    } else {
        fprintf(stderr, "Review mode must be review or no-review\n");
        return 1;
    }

    char message[256] = {0};
    if (!task_form_submit(&form, "user", message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

/* Prints the task-form option picker candidates for the current board so tests
 * can verify group and dependency filtering. */
static int tui_task_form_options_smoke(const char *kind, const char *deps_text) {
    char error[256] = {0};
    DispatchBoard board;
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error[0] ? error : "could not load board");
        return 1;
    }

    static TuiTaskFormOption opts[256];
    size_t n = 0;
    if (strcmp(kind, "groups") == 0) {
        n = task_form_group_options(&board, opts, 256);
    } else if (strcmp(kind, "deps") == 0) {
        const char *deps =
            (!deps_text || strcmp(deps_text, "-") == 0) ? "" : deps_text;
        n = task_form_dep_options(&board, deps, opts, 256);
    } else {
        fprintf(stderr, "Kind must be groups or deps\n");
        dispatch_board_free(&board);
        return 1;
    }

    printf("Options: %zu\n", n);
    for (size_t i = 0; i < n; i++)
        printf("%s\t%s\n", opts[i].value, opts[i].label);
    dispatch_board_free(&board);
    return 0;
}

/* Exercises dependency token append/dedupe used by the picker. */
static int tui_task_form_deps_add_smoke(const char *deps_text, const char *id) {
    char deps[256] = {0};
    snprintf(deps, sizeof(deps), "%s", deps_text ? deps_text : "");
    int changed = task_form_deps_append(deps, sizeof(deps), id);
    printf("Changed: %s\n", changed ? "yes" : "no");
    printf("Deps: %s\n", deps);
    return 0;
}

static int tui_agent_form_keys_smoke(const char *keys) {
    TuiAgentForm form;
    memset(&form, 0, sizeof(form));
    snprintf(form.runner, sizeof(form.runner), "codex");
    form.active_field = TUI_AGENT_FORM_NAME;
    snprintf(form.status, sizeof(form.status), "Fill agent fields");

    int submit = 0;
    int cancel = 0;
    for (size_t i = 0; keys && keys[i] != '\0' && !submit && !cancel; i++) {
        int ch = (unsigned char)keys[i];
        if (ch == '\\' && keys[i + 1] != '\0') {
            char escaped = keys[++i];
            if (escaped == 't')
                ch = '\t';
            else if (escaped == 'n')
                ch = '\n';
            else if (escaped == 'e')
                ch = 27;
            else if (escaped == 'b')
                ch = 127;
            else if (escaped == 'u')
                ch = 21;
            else if (escaped == 's')
                ch = ' ';
            else
                ch = (unsigned char)escaped;
        }
        handle_agent_form_key(&form, ch, &submit, &cancel);
    }

    printf("Field: %d\n", form.active_field);
    printf("Name: %s\n", form.name);
    printf("Runner: %s\n", form.runner);
    printf("Model: %s\n", form.model);
    printf("No run script: %s\n", form.no_run_script ? "yes" : "no");
    printf("Submit: %s\n", submit ? "yes" : "no");
    printf("Cancel: %s\n", cancel ? "yes" : "no");
    return 0;
}

static int tui_prompt_cancel_smoke(void) {
    char buffer[32] = "Group";
    int done = 0;
    int cancelled = 0;
    handle_prompt_line_key(buffer, sizeof(buffer), 27, &done, &cancelled);
    printf("Done: %s\n", done ? "yes" : "no");
    printf("Cancelled: %s\n", cancelled ? "yes" : "no");
    printf("Buffer: %s\n", buffer);
    return cancelled && !done ? 0 : 1;
}

static int tui_escdelay_smoke(void) {
    printf("Escape delay ms: %d\n", TUI_ESCAPE_DELAY_MS);
    return TUI_ESCAPE_DELAY_MS <= 50 ? 0 : 1;
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
    TuiLogRecords records;
    if (!load_matching_log_records(field, value, &records)) {
        fprintf(stderr, "No %s\n", DISPATCH_LOG_FILE);
        return 1;
    }

    int count = 0;
    for (size_t i = records.count; i > 0; i--) {
        json_t *record = records.items[i - 1];
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
    }
    printf("Logs: %d\n", count);
    log_records_free(&records);
    return 0;
}

static int tui_logs_window_smoke(int visible_rows, int selected_index,
                                 const char *field, const char *value) {
    DispatchTui tui;
    tui_init(&tui);
    tui.selected_log = selected_index;
    snprintf(tui.log_filter_field, sizeof(tui.log_filter_field), "%s",
             field ? field : "");
    snprintf(tui.log_filter_value, sizeof(tui.log_filter_value), "%s",
             value ? value : "");
    sync_log_scroll(&tui, visible_rows);

    TuiLogRecords records;
    if (!load_matching_log_records(tui.log_filter_field, tui.log_filter_value,
                                   &records)) {
        fprintf(stderr, "No %s\n", DISPATCH_LOG_FILE);
        return 1;
    }

    printf("Selected: %d\n", tui.selected_log);
    printf("Top: %d\n", tui.log_top);
    int visible_index = 0;
    int shown = 0;
    for (size_t i = records.count; i > 0 && shown < visible_rows; i--) {
        json_t *record = records.items[i - 1];
        if (visible_index++ < tui.log_top)
            continue;
        const char *actor = json_string_field(record, "actor");
        const char *command = json_string_field(record, "command");
        const char *action = json_string_field(record, "action");
        const char *task = json_nested_string_field(record, "targets", "task");
        printf("Row: %s %s %s task:%s\n", actor, command, action,
               task[0] ? task : "-");
        shown++;
    }
    printf("Shown: %d\n", shown);
    log_records_free(&records);
    return 0;
}

static int tui_scroll_smoke(const char *screen, int visible_rows,
                            int selected_index) {
    DispatchTui tui;
    tui_init(&tui);
    if (!tui_load_board(&tui)) {
        fprintf(stderr, "%s\n", tui.status);
        return 1;
    }

    if (strcmp(screen, "board") == 0) {
        tui.selected_task = selected_index;
        sync_task_scroll(&tui, visible_rows);
        printf("Screen: board\n");
        printf("Visible: %d\n", visible_task_count_for_tui(&tui));
        printf("Selected: %d\n", tui.selected_task);
        printf("Top: %d\n", tui.task_top);
    } else if (strcmp(screen, "agents") == 0) {
        tui.show_archived_agents = 1;
        tui.selected_agent = selected_index;
        sync_agent_scroll(&tui, visible_rows);
        printf("Screen: agents\n");
        printf("Visible: %d\n", visible_agent_count(&tui));
        printf("Selected: %d\n", tui.selected_agent);
        printf("Top: %d\n", tui.agent_top);
    } else if (strcmp(screen, "workspaces") == 0) {
        tui.selected_workspace = selected_index;
        sync_workspace_scroll(&tui, visible_rows);
        printf("Screen: workspaces\n");
        printf("Visible: %d\n", visible_workspace_count(&tui));
        printf("Selected: %d\n", tui.selected_workspace);
        printf("Top: %d\n", tui.workspace_top);
    } else if (strcmp(screen, "logs") == 0) {
        tui.selected_log = selected_index;
        sync_log_scroll(&tui, visible_rows);
        printf("Screen: logs\n");
        printf("Visible: %d\n", visible_log_count(&tui));
        printf("Selected: %d\n", tui.selected_log);
        printf("Top: %d\n", tui.log_top);
    } else {
        fprintf(stderr, "Scroll screen must be board, agents, workspaces, or logs\n");
        tui_free_board(&tui);
        return 1;
    }

    tui_free_board(&tui);
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
    case TUI_SCREEN_AGENT_FORM:
        return "agent-form";
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
    printf("Running: %s\n", tui.running ? "yes" : "no");
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

static int tui_search_smoke(const char *keys) {
    DispatchTui tui;
    tui_init(&tui);
    tui.search_active = 1;
    for (size_t i = 0; keys && keys[i] != '\0'; i++)
        handle_search_key(&tui, (unsigned char)keys[i]);
    printf("Search active: %s\n", tui.search_active ? "yes" : "no");
    printf("Search: %s\n", tui.search);
    printf("Screen: %s\n", screen_name(tui.screen));
    printf("Status: %s\n", tui.status);
    return 0;
}

static int tui_help_controls_smoke(void) {
    static const DispatchTuiScreen screens[] = {
        TUI_SCREEN_BOARD,      TUI_SCREEN_TASK_INSPECTOR,
        TUI_SCREEN_AGENTS,     TUI_SCREEN_AGENT_INSPECTOR,
        TUI_SCREEN_AGENT_FORM,
        TUI_SCREEN_WORKSPACES, TUI_SCREEN_WORKSPACE_INSPECTOR,
        TUI_SCREEN_LOGS,       TUI_SCREEN_TASK_FORM,
        TUI_SCREEN_GROUP_FORM,
    };
    int n = (int)(sizeof(screens) / sizeof(screens[0]));
    for (int i = 0; i < n; i++) {
        int count = 0;
        const HelpItem *items = help_controls_for_screen(screens[i], &count);
        printf("[%s] %d\n", screen_name(screens[i]), count);
        for (int j = 0; j < count; j++) {
            if (items[j].key)
                printf("  %-10s %s\n", items[j].key, items[j].desc);
            else
                printf("# %s\n", items[j].desc);
        }
    }
    return 0;
}

static int tui_refresh_smoke(void) {
    DispatchTui tui;
    tui_init(&tui);
    if (!tui_load_board(&tui)) {
        fprintf(stderr, "%s\n", tui.status);
        return 1;
    }
    size_t before = tui.board.groups.count;

    char message[256] = {0};
    if (!create_group("Refresh Smoke", "RS", message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        tui_free_board(&tui);
        return 1;
    }

    int reloaded = tui_reload_if_changed(&tui);
    printf("Reloaded: %s\n", reloaded ? "yes" : "no");
    printf("Groups before: %zu\n", before);
    printf("Groups after: %zu\n", tui.board.groups.count);
    printf("Status: %s\n", tui.status);
    int ok = reloaded && tui.board.groups.count == before + 1;
    tui_free_board(&tui);
    return ok ? 0 : 1;
}

static void print_tui_help(void) {
    puts("Usage: dispatch tui [--smoke]");
    puts("");
    puts("Open the ncurses Dispatch terminal UI.");
    puts("");
    puts("Interactive keys:");
    puts("  b board, a/Tab agents, w workspaces, l logs, : command palette");
    puts("  j/k or arrows move, Enter/i inspect, q/Esc backs out");
    puts("  Ctrl+C or :q quits");
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
    puts("  --agent-set-session-smoke <name> <session|->");
    puts("  --prompt-edit-smoke <name>    print prompt editor command and exit");
    puts("  --agent-archive-smoke <name> archive|restore");
    puts("  --agent-selection-smoke enabled|all <selected-index>");
    puts("  --agent-run-command-smoke <name>");
    puts("  --osc52-smoke <text>        print OSC 52 payload metadata and exit");
    puts("  --create-group-smoke <name> <prefix|->");
    puts("  --create-task-smoke <group> <title> <description|-> review|no-review <deps|->");
    puts("  --task-form-submit-smoke <group> <title> <description|-> review|no-review <deps|->");
    puts("  --task-form-options-smoke groups|deps [deps|-]");
    puts("  --task-form-deps-add-smoke <deps|-> <task-id>");
    puts("  --agent-form-keys-smoke <keys>  exercise agent form key handling");
    puts("  --prompt-cancel-smoke");
    puts("  --escdelay-smoke");
    puts("  --dependency-smoke add|remove <dependency-id> <dependent-id>");
    puts("  --workspaces-smoke          print workspace dashboard data and exit");
    puts("  --workspace-inspect-smoke <task-id-or-workspace>");
    puts("  --logs-smoke [actor|command|action|task|agent|workspace <value>]");
    puts("  --logs-window-smoke <visible-rows> <selected-index> [field value]");
    puts("  --scroll-smoke board|agents|workspaces|logs <visible-rows> <selected-index>");
    puts("  --palette-smoke <command>");
    puts("  --palette-complete-smoke <prefix>");
    puts("  --search-smoke <keys>");
    puts("  --refresh-smoke");
    puts("  --help-controls-smoke      print per-view control lists and exit");
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
    if (argc == 5 && strcmp(argv[2], "--agent-set-session-smoke") == 0)
        return tui_agent_set_session_smoke(argv[3], argv[4]);
    if (argc == 4 && strcmp(argv[2], "--prompt-edit-smoke") == 0)
        return tui_prompt_edit_smoke(argv[3]);
    if (argc == 5 && strcmp(argv[2], "--agent-archive-smoke") == 0)
        return tui_agent_archive_smoke(argv[3], argv[4]);
    if (argc == 5 && strcmp(argv[2], "--agent-selection-smoke") == 0)
        return tui_agent_selection_smoke(argv[3], atoi(argv[4]));
    if (argc == 4 && strcmp(argv[2], "--agent-run-command-smoke") == 0)
        return tui_agent_run_command_smoke(argv[3]);
    if (argc == 4 && strcmp(argv[2], "--osc52-smoke") == 0)
        return tui_osc52_smoke(argv[3]);
    if (argc == 5 && strcmp(argv[2], "--create-group-smoke") == 0)
        return tui_create_group_smoke(argv[3], argv[4]);
    if (argc == 8 && strcmp(argv[2], "--create-task-smoke") == 0)
        return tui_create_task_smoke(argv[3], argv[4], argv[5], argv[6],
                                    argv[7]);
    if (argc == 8 && strcmp(argv[2], "--task-form-submit-smoke") == 0)
        return tui_task_form_submit_smoke(argv[3], argv[4], argv[5], argv[6],
                                         argv[7]);
    if ((argc == 4 || argc == 5) &&
        strcmp(argv[2], "--task-form-options-smoke") == 0)
        return tui_task_form_options_smoke(argv[3], argc == 5 ? argv[4] : "-");
    if (argc == 5 && strcmp(argv[2], "--task-form-deps-add-smoke") == 0)
        return tui_task_form_deps_add_smoke(argv[3], argv[4]);
    if (argc == 4 && strcmp(argv[2], "--agent-form-keys-smoke") == 0)
        return tui_agent_form_keys_smoke(argv[3]);
    if (argc == 3 && strcmp(argv[2], "--prompt-cancel-smoke") == 0)
        return tui_prompt_cancel_smoke();
    if (argc == 3 && strcmp(argv[2], "--escdelay-smoke") == 0)
        return tui_escdelay_smoke();
    if (argc == 6 && strcmp(argv[2], "--dependency-smoke") == 0)
        return tui_dependency_smoke(argv[3], argv[4], argv[5]);
    if (argc == 3 && strcmp(argv[2], "--workspaces-smoke") == 0)
        return tui_workspaces_smoke();
    if (argc == 4 && strcmp(argv[2], "--workspace-inspect-smoke") == 0)
        return tui_workspace_inspect_smoke(argv[3]);
    if ((argc == 3 || argc == 5) && strcmp(argv[2], "--logs-smoke") == 0)
        return tui_logs_smoke(argc == 5 ? argv[3] : "", argc == 5 ? argv[4] : "");
    if ((argc == 5 || argc == 7) && strcmp(argv[2], "--logs-window-smoke") == 0)
        return tui_logs_window_smoke(atoi(argv[3]), atoi(argv[4]),
                                    argc == 7 ? argv[5] : "",
                                    argc == 7 ? argv[6] : "");
    if (argc == 6 && strcmp(argv[2], "--scroll-smoke") == 0)
        return tui_scroll_smoke(argv[3], atoi(argv[4]), atoi(argv[5]));
    if (argc == 4 && strcmp(argv[2], "--palette-smoke") == 0)
        return tui_palette_smoke(argv[3]);
    if (argc == 4 && strcmp(argv[2], "--palette-complete-smoke") == 0)
        return tui_palette_complete_smoke(argv[3]);
    if (argc == 4 && strcmp(argv[2], "--search-smoke") == 0)
        return tui_search_smoke(argv[3]);
    if (argc == 3 && strcmp(argv[2], "--refresh-smoke") == 0)
        return tui_refresh_smoke();
    if (argc == 3 && strcmp(argv[2], "--help-controls-smoke") == 0)
        return tui_help_controls_smoke();
    if (argc != 2) {
        print_tui_help();
        return 1;
    }

    return tui_run();
}
