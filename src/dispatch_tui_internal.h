#ifndef DISPATCH_TUI_INTERNAL_H
#define DISPATCH_TUI_INTERNAL_H

/* Internal declarations shared by the TUI modules (state/core, rendering,
 * input handling, forms, actions, external command integration, and the
 * headless smoke harness). Not part of the public dispatch_tui.h API. */

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
#include "dispatch_exec.h"
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
    int desc_top;    /* task inspector description scroll offset (rows) */
    int desc_region; /* description rows shown by the last render */
    int desc_total;  /* total wrapped description rows at the last render */
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

typedef struct {
    char title[256];
    char description[512];
    int active_field;
    char status[256];
} TuiTaskEditForm;

enum {
    TUI_TASK_FORM_GROUP = 0,
    TUI_TASK_FORM_TITLE = 1,
    TUI_TASK_FORM_DESCRIPTION = 2,
    TUI_TASK_FORM_DEPS = 3,
    TUI_TASK_FORM_REVIEW = 4,
    TUI_TASK_FORM_FIELD_COUNT = 5,
};

enum {
    TUI_TASK_EDIT_TITLE = 0,
    TUI_TASK_EDIT_DESCRIPTION = 1,
    TUI_TASK_EDIT_FIELD_COUNT = 2,
};

typedef struct {
    char name[128];
    char runner[16];
    char model[128];
    int active_field;
    char status[256];
} TuiAgentForm;

enum {
    TUI_AGENT_FORM_NAME = 0,
    TUI_AGENT_FORM_RUNNER = 1,
    TUI_AGENT_FORM_MODEL = 2,
    TUI_AGENT_FORM_FIELD_COUNT = 3,
};


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


enum {
    TUI_ESCAPE_DELAY_MS = 25,
};

typedef struct {
    const DispatchTui *tui;
    size_t group; /* group index of the task most recently returned */
    size_t task;  /* next storage index to scan within the current group */
} VisibleTaskIter;

typedef enum {
    TUI_ACTION_READY,
    TUI_ACTION_READY_NO_REVIEW,
    TUI_ACTION_START,
    TUI_ACTION_FINISH,
    TUI_ACTION_REVIEW
} DispatchTuiAction;

typedef struct {
    char name[128];
    char prefix[16];
    int active_field;
    char status[256];
} TuiGroupForm;

typedef struct {
    char value[64];
    char label[160];
} TuiTaskFormOption;

typedef struct {
    const char *key;
    const char *desc;
} HelpItem;

enum {
    TUI_GROUP_FORM_NAME = 0,
    TUI_GROUP_FORM_PREFIX = 1,
    TUI_GROUP_FORM_FIELD_COUNT = 2,
};

/* Fixed column widths for the list tables. These are the single source of
 * truth: every value drawn into one of these slots is clamped by cell_text,
 * and dependent positions (color overlays) are derived from the same
 * constants, so an over-length value can never shift later columns or
 * corrupt an overlay. Names are validated to DISPATCH_AGENT_NAME_MAX (which
 * matches TUI_COL_NAME) at input time; clamping covers legacy data. */
enum {
    TUI_COL_MARKER = 2,  /* " >" selection marker */
    TUI_COL_TASK = 8,    /* task ids like DE-01 */
    TUI_COL_STATE = 8,   /* task lifecycle states ("proposed") */
    TUI_COL_NAME = 24,   /* agent names and actor labels */
    TUI_COL_RUNNER = 8,  /* codex/claude */
    TUI_COL_AGENT_STATUS = 9, /* enabled/archived */
    TUI_COL_SESSION = 7, /* yes/no */
    TUI_COL_WS_STATE = 9, /* workspace record state */
    TUI_COL_FLAG = 5,    /* dirty/clean */
    TUI_COL_GIT = 7,     /* git/no-git */
    TUI_COL_TIME = 20,   /* log timestamps */
    TUI_COL_LOG_ACTOR = 14,
    TUI_COL_COMMAND = 10,
    TUI_COL_ACTION = 12,
};

/* Column start positions derived from the widths above: the marker column
 * has no trailing space, every later column is separated by one space. */
enum {
    TUI_X_BOARD_STATE = TUI_COL_MARKER + TUI_COL_TASK + 1,
    TUI_X_AGENT_STATUS =
        TUI_COL_MARKER + TUI_COL_NAME + 1 + TUI_COL_RUNNER + 1,
    TUI_X_WS_DIRTY = TUI_COL_MARKER + TUI_COL_TASK + 1 + TUI_COL_STATE + 1 +
                     TUI_COL_WS_STATE + 1 + TUI_COL_NAME + 1,
    TUI_X_LOG_ACTION = TUI_COL_MARKER + TUI_COL_TIME + 1 +
                       TUI_COL_LOG_ACTOR + 1 + TUI_COL_COMMAND + 1,
};

extern int tui_colors_enabled;
extern volatile sig_atomic_t tui_quit_requested;

const char *actor_filter_value(const DispatchBoard *board, int index);

int agent_is_visible(const DispatchTui *tui, const DispatchAgent *agent);

void clamp_agent_selection(DispatchTui *tui);

void clamp_log_selection(DispatchTui *tui);

void clamp_selection(DispatchTui *tui);

void clamp_workspace_selection(DispatchTui *tui);

void clear_secondary_filters(DispatchTui *tui);

void cycle_actor_filter(DispatchTui *tui);

void cycle_group_filter(DispatchTui *tui);

const char *filter_name(DispatchTuiFilter filter);

int handle_search_key(DispatchTui *tui, int ch);

const char *json_nested_string_field(json_t *object, const char *parent,
                                            const char *name);

const char *json_string_field(json_t *object, const char *name);

int load_matching_log_records(const char *field, const char *value,
                                     TuiLogRecords *records);

void log_records_free(TuiLogRecords *records);

int select_agent_by_name(DispatchTui *tui, const char *name);

int select_task_by_id(DispatchTui *tui, const char *task_id);

int select_workspace_by_id(DispatchTui *tui, const char *target);

DispatchAgent *selected_agent(DispatchTui *tui);

DispatchWorkspace *selected_visible_workspace(DispatchTui *tui);

void set_filter(DispatchTui *tui, DispatchTuiFilter filter);

void sync_agent_scroll(DispatchTui *tui, int visible_rows);

void sync_log_scroll(DispatchTui *tui, int visible_rows);

void sync_task_scroll(DispatchTui *tui, int visible_rows);

void sync_workspace_scroll(DispatchTui *tui, int visible_rows);

int task_count_for_state(const DispatchBoard *board,
                                DispatchState state);

int title_starts_with_dispatch_id_like(const char *title);

void tui_free_board(DispatchTui *tui);

void tui_handle_sigint(int signal_number);

void tui_init(DispatchTui *tui);

int tui_load_board(DispatchTui *tui);

int tui_reload_if_changed(DispatchTui *tui);

void tui_set_status(DispatchTui *tui, const char *message);

void tui_style_row_off(int row_index, int selected);

void tui_style_row_on(int row_index, int selected);

void tui_style_title_off(void);

void tui_style_title_on(void);

int tui_task_is_visible(const DispatchTui *tui,
                               const DispatchTask *task);

int visible_agent_count(const DispatchTui *tui);

int visible_log_count(const DispatchTui *tui);

int visible_task_count_for_tui(const DispatchTui *tui);

void visible_task_iter_init(const DispatchTui *tui,
                                   VisibleTaskIter *it);

DispatchTask *visible_task_iter_next(VisibleTaskIter *it);

int visible_workspace_count(const DispatchTui *tui);

void clear_selected_agent_session(DispatchTui *tui);

int create_group(const char *name, const char *prefix, char *message,
                        size_t message_size);

int create_task(const char *group, const char *title,
                       const char *description, int requires_review,
                       const char *depends_text, const char *actor, char *message,
                       size_t message_size);

void execute_palette_command(DispatchTui *tui, const char *command);

int mutate_dependency(const char *dependency_id, const char *dependent_id,
                             int add, char *message, size_t message_size);

int mutate_task(const char *task_id, const char *actor,
                       DispatchTuiAction action, char *message,
                       size_t message_size);

int mutate_task_content(const char *task_id, const char *title,
                               const char *description, const char *actor,
                               char *message, size_t message_size);

int mutate_task_delete(const char *task_id, int force, char *message,
                              size_t message_size);

int mutate_task_group(const char *task_id, const char *group_id,
                             const char *actor, char *message,
                             size_t message_size);

void print_palette_completion(const DispatchBoard *board,
                                     const char *input);

void run_command_palette(DispatchTui *tui);

void run_selected_task_action(DispatchTui *tui,
                                     DispatchTuiAction action);

DispatchTask *selected_visible_task(DispatchTui *tui);

int set_agent_archived_state(const char *name, int archived,
                                    char *message, size_t message_size);

int set_agent_session_id(const char *name, const char *session_id,
                                char *message, size_t message_size);

void set_selected_agent_session(DispatchTui *tui);

void toggle_selected_agent_archived(DispatchTui *tui);

int update_agent_session_metadata(const char *name,
                                         const char *session_id,
                                         const char *current_task,
                                         const char *last_workspace,
                                         int clear_session,
                                         int clear_current_task,
                                         int clear_last_workspace,
                                         char *message,
                                         size_t message_size);

DispatchWorkspace *workspace_for_task(DispatchBoard *board,
                                             const char *task_id);

char *agent_run_command_text(DispatchBoard *board,
                                    const DispatchAgent *agent);

int copy_command_to_tmux_buffer(const char *command);

void copy_selected_agent_run_command(DispatchTui *tui);

int diff_argv_for_task(const DispatchBoard *board,
                              const DispatchTask *task, int force_color,
                              DispatchExecArgv *argv);

void edit_selected_agent_prompt(DispatchTui *tui);

int editor_argv_for_path(const char *path, DispatchExecArgv *argv,
                                char *error, size_t error_size);

char *osc52_sequence_for_text(const char *text);

int path_has_git_metadata(const char *path);

void run_selected_task_diff(DispatchTui *tui);

char *tui_base64_encode(const unsigned char *data, size_t len);

char *tui_trimmed_copy(const char *value);

int workspace_is_dirty(const DispatchWorkspace *workspace);

int agent_form_submit(const TuiAgentForm *form, const char *actor,
                             char *message, size_t message_size);

int handle_agent_form_key(TuiAgentForm *form, int ch, int *submit,
                                 int *cancel);

int handle_prompt_line_key(char *buffer, size_t buffer_size, int ch,
                                  int *done, int *cancelled);

int prompt_line(const char *label, char *buffer, size_t buffer_size,
                       const char *initial);

void render_agent_form_screen(DispatchTui *tui, const TuiAgentForm *form);

void render_group_form_screen(DispatchTui *tui,
                                     const TuiGroupForm *form);

void render_task_form_screen(DispatchTui *tui, const TuiTaskForm *form);

void run_agent_form(DispatchTui *tui);

void run_dependency_prompt(DispatchTui *tui, int add);

void run_group_form(DispatchTui *tui);

void run_log_filter_form(DispatchTui *tui);

void run_task_delete_form(DispatchTui *tui, int force);

void run_task_edit_form(DispatchTui *tui);

void run_task_form(DispatchTui *tui);

void run_task_move_picker(DispatchTui *tui);

void run_workspace_create_form(DispatchTui *tui);

void run_workspace_prune_form(DispatchTui *tui);

void run_workspace_remove_form(DispatchTui *tui, int force);

const char *selected_task_group(DispatchTui *tui);

void set_log_filter(DispatchTui *tui, const char *field,
                           const char *value);

void show_selected_agent_logs(DispatchTui *tui);

void show_selected_task_logs(DispatchTui *tui);

int task_edit_form_submit(const char *task_id,
                                 const TuiTaskEditForm *form,
                                 const char *actor, char *message,
                                 size_t message_size);

size_t task_form_dep_options(const DispatchBoard *board,
                                    const char *deps_text, TuiTaskFormOption *out,
                                    size_t max);

int task_form_deps_append(char *deps_text, size_t size, const char *id);

size_t task_form_group_options(const DispatchBoard *board,
                                      TuiTaskFormOption *out, size_t max);

int task_form_submit(const TuiTaskForm *form, const char *actor,
                            char *message, size_t message_size);

size_t task_move_group_options(const DispatchBoard *board,
                                      const char *current_group,
                                      TuiTaskFormOption *out, size_t max);

void tui_handle_key(DispatchTui *tui, int ch);

int tui_run(void);

void blocks_text(DispatchBoard *board, const char *task_id, char *buf,
                        size_t size);

void cell_text(char *out, size_t out_size, const char *value,
                      int width);

int description_rows(const char *text, int width, int row, char *out,
                            size_t out_size);

void draw_board_rows(DispatchTui *tui, int start_y, int rows, int cols);

void draw_footer(const char *message, const char *hints);

void draw_title_bar(DispatchTui *tui, const char *view);

void draw_truncated(int y, int x, int width, const char *text);

void format_agent_row(char *out, size_t out_size,
                             const DispatchAgent *agent, int selected);

const HelpItem *help_controls_for_screen(DispatchTuiScreen screen,
                                                int *count);

void join_string_list(const DispatchStringList *list, char *buf,
                             size_t size);

const char *tui_footer_hints(DispatchTuiScreen screen);

void tui_render(DispatchTui *tui);

int parse_action_name(const char *name, DispatchTuiAction *action);

int parse_filter_name(const char *name, DispatchTuiFilter *filter);

#endif /* DISPATCH_TUI_INTERNAL_H */
