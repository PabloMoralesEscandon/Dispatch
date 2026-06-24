#include "dispatch_tui.h"

#include <ncurses.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "dispatch_store.h"

typedef enum {
    TUI_SCREEN_BOARD,
    TUI_SCREEN_TASK_INSPECTOR
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
    char actor[64];
    char status[256];
    char search[128];
    int search_active;
    int selected_task;
    int show_help;
    int running;
} DispatchTui;

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
        "Enter/i  inspect selected task",
        "Esc/q    close inspector",
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
                       : "Dispatch TUI - Board");
    attroff(A_BOLD);

    if (!tui->board_loaded) {
        draw_truncated(2, 0, cols, "Board not loaded.");
    } else if (tui->screen == TUI_SCREEN_TASK_INSPECTOR) {
        draw_task_inspector(tui, rows, cols);
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
                     ? " %s | q/Esc back | r/s/f/v actions | ? help | u reload"
                     : " %s | q quit | Enter/i inspect | r/s/f/v actions | 1-7/R filters | / search",
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
            else
                tui.running = 0;
            break;
        case '\n':
        case KEY_ENTER:
        case 'i':
            if (tui.screen == TUI_SCREEN_BOARD && selected_visible_task(&tui)) {
                tui.screen = TUI_SCREEN_TASK_INSPECTOR;
                tui_set_status(&tui, "Inspecting task");
            }
            break;
        case '?':
            tui.show_help = !tui.show_help;
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
        case 's':
            run_selected_task_action(&tui, TUI_ACTION_START);
            break;
        case 'f':
            run_selected_task_action(&tui, TUI_ACTION_FINISH);
            break;
        case 'v':
            run_selected_task_action(&tui, TUI_ACTION_REVIEW);
            break;
        case 'G':
            cycle_group_filter(&tui);
            break;
        case 'A':
            cycle_actor_filter(&tui);
            break;
        case 'c':
            clear_secondary_filters(&tui);
            break;
        case '/':
            tui.search_active = 1;
            tui_set_status(&tui, "Search");
            break;
        case 27:
            if (tui.screen == TUI_SCREEN_TASK_INSPECTOR) {
                tui.screen = TUI_SCREEN_BOARD;
                tui_set_status(&tui, "Board");
            } else {
                tui.search_active = 0;
                tui.search[0] = '\0';
                tui.selected_task = 0;
                tui_set_status(&tui, "Search cleared");
            }
            break;
        case KEY_UP:
        case 'k':
            tui.selected_task--;
            clamp_selection(&tui);
            break;
        case KEY_DOWN:
        case 'j':
            tui.selected_task++;
            clamp_selection(&tui);
            break;
        case 'u':
            tui_load_board(&tui);
            clamp_selection(&tui);
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

static void print_tui_help(void) {
    puts("Usage: dispatch tui [--smoke]");
    puts("");
    puts("Open the ncurses Dispatch terminal UI.");
    puts("  --smoke   load the board and exit without initializing ncurses");
    puts("  --inspect-smoke <task-id>  print task inspector data and exit");
    puts("  --filter-smoke <filter>    print visible row count and exit");
    puts("  --action-smoke <action> <task-id> [actor]  run lifecycle action and exit");
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
    if (argc != 2) {
        print_tui_help();
        return 1;
    }

    return tui_run();
}
