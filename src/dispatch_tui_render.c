/* All ncurses rendering: tables, inspectors, help, and the frame. */

#include "dispatch_tui_internal.h"

static void draw_padded(int y, int x, int width, const char *text);
static const char *tui_view_name(DispatchTuiScreen screen);
static int tui_state_color(DispatchState state);
static int draw_count_chip(int y, int x, int cols, int count, const char *label,
                           int color, int dim);
static void draw_board_stats(DispatchTui *tui, int y, int cols);
static int draw_tag(int y, int x, int cols, const char *text, int color,
                    int reverse);
static void draw_filter_line(DispatchTui *tui, int y, int cols);
static void help_draw_column(int top_y, int x, int colw, const HelpItem *items,
                             int n, int max_y);
static void tui_render_help(DispatchTuiScreen screen);
static void draw_list_header(int y, int cols, const char *text);
static void draw_agent_rows(DispatchTui *tui, int start_y, int rows, int cols);
static void draw_workspace_rows(DispatchTui *tui, int start_y, int rows,
                                int cols);
static int log_action_color(const char *action);
static void draw_log_rows(DispatchTui *tui, int start_y, int rows, int cols);
static int draw_inspector_header(int y, int rows, int cols, const char *id,
                                 const char *title, const char *badge,
                                 int badge_color);
static int draw_field(int y, int rows, int cols, const char *label,
                      const char *value);
static int draw_section(int y, int rows, int cols, const char *title);
static void draw_task_inspector(DispatchTui *tui, int rows, int cols);
static void draw_agent_inspector(DispatchTui *tui, int rows, int cols);
static void draw_workspace_inspector(DispatchTui *tui, int rows, int cols);
static int group_visible_task_count(DispatchTui *tui,
                                    const DispatchGroup *group);
static void draw_group_header(DispatchTui *tui, int y, int cols,
                              const DispatchGroup *group);
static void draw_task_row(DispatchTui *tui, int y, int cols,
                          const DispatchTask *task, int visible_index);

void draw_truncated(int y, int x, int width, const char *text) {
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
void draw_title_bar(DispatchTui *tui, const char *view) {
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
void draw_footer(const char *message, const char *hints) {
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

const char *tui_footer_hints(DispatchTuiScreen screen) {
    switch (screen) {
    case TUI_SCREEN_TASK_INSPECTOR:
        return "q/Esc back   e edit   m move   >/< deps   d diff   x/X delete   L logs   PgUp/PgDn scroll";
    case TUI_SCREEN_AGENT_INSPECTOR:
        return "q/Esc back   y run   e prompt   S session   x clear   z archive";
    case TUI_SCREEN_WORKSPACE_INSPECTOR:
        return "q/Esc back   x remove   X force remove";
    case TUI_SCREEN_WORKSPACES:
        return "b board   a agents   n create   x/X remove   P prune";
    case TUI_SCREEN_LOGS:
        return "b board   F filter   C clear   j/k move";
    case TUI_SCREEN_AGENTS:
        return "Enter/i inspect   n create   y run   A all/enabled   z archive";
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
    {"e  m", "edit / move selected"},
    {"R", "ready, skip review"},
    {"n  +", "new task / group"},
    {"d", "diff selected"},
    {"x  X", "delete / force delete"},
    {NULL, "Filter & search"},
    {"1-8", "filter presets"},
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
    {"e", "edit title and description"},
    {"m", "move to another group"},
    {"R", "ready, skip review"},
    {">", "add dependency"},
    {"<", "remove dependency"},
    {"d", "diff"},
    {"x  X", "delete / force delete"},
    {"L", "task logs"},
    {"PgUp/PgDn", "scroll description"},
    {"h / ?", "toggle help"},
};

static const HelpItem help_agents_items[] = {
    {NULL, "Agents"},
    {"j / k", "move selection"},
    {"Enter / i", "inspect agent"},
    {"n", "create agent"},
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
    {"Space", "toggle runner"},
    {"Esc", "cancel"},
};

/* Return the control list for a screen, writing its length to *count. */
const HelpItem *help_controls_for_screen(DispatchTuiScreen screen,
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



/* Write value into a fixed-width column slot: space-padded to exactly width,
 * truncated with a trailing ".." when it does not fit. */
void cell_text(char *out, size_t out_size, const char *value,
                      int width) {
    if (out_size == 0)
        return;
    if (width <= 0 || (size_t)width >= out_size) {
        out[0] = '\0';
        return;
    }
    const char *text = value ? value : "";
    size_t len = strlen(text);
    if (len <= (size_t)width) {
        snprintf(out, out_size, "%-*s", width, text);
    } else if (width > 2) {
        snprintf(out, out_size, "%.*s..", width - 2, text);
    } else {
        snprintf(out, out_size, "%.*s", width, text);
    }
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

/* Format one agents-table row into out. Shared with --agent-row-smoke so
 * tests can assert that over-length values stay inside their columns. */
void format_agent_row(char *out, size_t out_size,
                             const DispatchAgent *agent, int selected) {
    char name[TUI_COL_NAME + 1];
    char runner[TUI_COL_RUNNER + 1];
    char status[TUI_COL_AGENT_STATUS + 1];
    char session[TUI_COL_SESSION + 1];
    char task[TUI_COL_TASK + 1];
    cell_text(name, sizeof(name), agent->name, TUI_COL_NAME);
    cell_text(runner, sizeof(runner), agent->runner, TUI_COL_RUNNER);
    cell_text(status, sizeof(status), agent->archived ? "archived" : "enabled",
              TUI_COL_AGENT_STATUS);
    cell_text(session, sizeof(session), agent->session_id ? "yes" : "no",
              TUI_COL_SESSION);
    cell_text(task, sizeof(task),
              agent->current_task ? agent->current_task : "-", TUI_COL_TASK);
    snprintf(out, out_size, "%s%s %s %s %s %s %s", selected ? " >" : "  ",
             name, runner, status, session, task,
             agent->last_workspace ? agent->last_workspace : "-");
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
    snprintf(header, sizeof(header), "  %-*s %-*s %-*s %-*s %-*s %s",
             TUI_COL_NAME, "Name", TUI_COL_RUNNER, "Runner",
             TUI_COL_AGENT_STATUS, "Status", TUI_COL_SESSION, "Session",
             TUI_COL_TASK, "Task", "Workspace");
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
        char line[1024];
        format_agent_row(line, sizeof(line), agent, selected);
        tui_style_row_on(visible_index, selected);
        draw_padded(y, 0, cols, line);
        tui_style_row_off(visible_index, selected);

        if (!selected && tui_colors_enabled) {
            int color = agent->archived ? TUI_COLOR_MUTED : TUI_COLOR_STATE_READY;
            attron(COLOR_PAIR(color) | A_BOLD);
            mvaddnstr(y, TUI_X_AGENT_STATUS,
                      agent->archived ? "archived" : "enabled",
                      TUI_COL_AGENT_STATUS);
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
    snprintf(header, sizeof(header), "  %-*s %-*s %-*s %-*s %-*s %-*s %s",
             TUI_COL_TASK, "Task", TUI_COL_STATE, "State", TUI_COL_WS_STATE,
             "WS", TUI_COL_NAME, "Actor", TUI_COL_FLAG, "Dirty", TUI_COL_GIT,
             "Git", "Branch / Path");
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
        char task_cell[TUI_COL_TASK + 1];
        char state_cell[TUI_COL_STATE + 1];
        char ws_cell[TUI_COL_WS_STATE + 1];
        char actor_cell[TUI_COL_NAME + 1];
        char dirty_cell[TUI_COL_FLAG + 1];
        char git_cell[TUI_COL_GIT + 1];
        cell_text(task_cell, sizeof(task_cell), workspace->task_id,
                  TUI_COL_TASK);
        cell_text(state_cell, sizeof(state_cell), task_state, TUI_COL_STATE);
        cell_text(ws_cell, sizeof(ws_cell),
                  dispatch_workspace_state_name(workspace->state),
                  TUI_COL_WS_STATE);
        cell_text(actor_cell, sizeof(actor_cell), workspace->actor,
                  TUI_COL_NAME);
        cell_text(dirty_cell, sizeof(dirty_cell), dirty ? "dirty" : "clean",
                  TUI_COL_FLAG);
        cell_text(git_cell, sizeof(git_cell), git_present ? "git" : "no-git",
                  TUI_COL_GIT);
        char line[1200];
        snprintf(line, sizeof(line), "%s%s %s %s %s %s %s %s",
                 selected ? " >" : "  ", task_cell, state_cell, ws_cell,
                 actor_cell, dirty_cell, git_cell, branchpath);
        tui_style_row_on(visible_index, selected);
        draw_padded(y, 0, cols, line);
        tui_style_row_off(visible_index, selected);

        if (!selected && tui_colors_enabled) {
            int sc = has_state ? tui_state_color(wstate) : TUI_COLOR_MUTED;
            attron(COLOR_PAIR(sc) | A_BOLD);
            mvaddnstr(y, TUI_X_BOARD_STATE, task_state, TUI_COL_STATE);
            attroff(COLOR_PAIR(sc) | A_BOLD);
            int dc = dirty ? TUI_COLOR_STATE_DOING : TUI_COLOR_MUTED;
            attron(COLOR_PAIR(dc) | (dirty ? A_BOLD : A_DIM));
            mvaddnstr(y, TUI_X_WS_DIRTY, dirty ? "dirty" : "clean",
                      TUI_COL_FLAG);
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
    snprintf(header, sizeof(header), "  %-*s %-*s %-*s %-*s %s", TUI_COL_TIME,
             "Time", TUI_COL_LOG_ACTOR, "Actor", TUI_COL_COMMAND, "Command",
             TUI_COL_ACTION, "Action", "Target / Message");
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
        char time_cell[TUI_COL_TIME + 1];
        char actor_cell[TUI_COL_LOG_ACTOR + 1];
        char command_cell[TUI_COL_COMMAND + 1];
        char action_cell[TUI_COL_ACTION + 1];
        cell_text(time_cell, sizeof(time_cell), time, TUI_COL_TIME);
        cell_text(actor_cell, sizeof(actor_cell), actor, TUI_COL_LOG_ACTOR);
        cell_text(command_cell, sizeof(command_cell), command,
                  TUI_COL_COMMAND);
        cell_text(action_cell, sizeof(action_cell), action, TUI_COL_ACTION);
        char row[1600];
        snprintf(row, sizeof(row), "%s%s %s %s %s %s", selected ? " >" : "  ",
                 time_cell, actor_cell, command_cell, action_cell, rest);
        tui_style_row_on(visible_index, selected);
        draw_padded(y, 0, cols, row);
        tui_style_row_off(visible_index, selected);

        if (!selected && tui_colors_enabled) {
            int color = log_action_color(action);
            attron(COLOR_PAIR(color) | A_BOLD);
            mvaddnstr(y, TUI_X_LOG_ACTION, action_cell, TUI_COL_ACTION);
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

/* Wrap text into fixed-width rows: the same width-slice wrap the task form
 * box uses, plus hard breaks at newlines so CLI-created multi-paragraph
 * descriptions keep their paragraphs. Returns the total row count; when out
 * is non-NULL it receives row number `row` (empty when out of range). */
int description_rows(const char *text, int width, int row, char *out,
                            size_t out_size) {
    if (out && out_size > 0)
        out[0] = '\0';
    if (!text || width <= 0)
        return 0;

    int count = 0;
    const char *p = text;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : strlen(p);
        size_t off = 0;
        do {
            size_t len =
                seg - off > (size_t)width ? (size_t)width : seg - off;
            if (out && count == row) {
                size_t copy = len < out_size - 1 ? len : out_size - 1;
                memcpy(out, p + off, copy);
                out[copy] = '\0';
            }
            count++;
            off += len;
        } while (off < seg);
        if (!nl)
            break;
        p = nl + 1;
    }
    return count;
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

void join_string_list(const DispatchStringList *list, char *buf,
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

void blocks_text(DispatchBoard *board, const char *task_id, char *buf,
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

    /* Description gets its own multi-line region: the full text is wrapped
     * across as many rows as fit, the fields below reflow after it, and
     * PgUp/PgDn scroll the region when the text is longer than the space
     * available. */
    const char *desc = task->description[0] ? task->description : "-";
    int desc_x = 2 + 15;
    int desc_width = cols - desc_x;
    int total = description_rows(desc, desc_width, -1, NULL, 0);
    /* Leave room for the fixed sections below the description. */
    int reserved = 16;
    int region = rows - 1 - y - reserved;
    if (region < 3)
        region = 3;
    if (region > total)
        region = total;
    int max_top = total - region;
    if (tui->desc_top > max_top)
        tui->desc_top = max_top;
    if (tui->desc_top < 0)
        tui->desc_top = 0;
    tui->desc_region = region;
    tui->desc_total = total;

    if (y < rows - 1) {
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        mvaddnstr(y, 2, "Description", cols - 2);
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    }
    char desc_line[512];
    for (int r = 0; r < region && y < rows - 1; r++, y++) {
        description_rows(desc, desc_width, tui->desc_top + r, desc_line,
                         sizeof(desc_line));
        draw_truncated(y, desc_x, desc_width, desc_line);
    }
    if (total > region && y < rows - 1) {
        snprintf(buf, sizeof(buf), "(%d-%d of %d lines, PgUp/PgDn scroll)",
                 tui->desc_top + 1, tui->desc_top + region, total);
        if (tui_colors_enabled)
            attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        draw_truncated(y, desc_x, desc_width, buf);
        if (tui_colors_enabled)
            attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
        y++;
    }
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

    char id_cell[TUI_COL_TASK + 1];
    char state_cell[TUI_COL_STATE + 1];
    cell_text(id_cell, sizeof(id_cell), task->id, TUI_COL_TASK);
    cell_text(state_cell, sizeof(state_cell), dispatch_state_name(state),
              TUI_COL_STATE);
    char left[1024];
    snprintf(left, sizeof(left), "%s%s %s %s", selected ? " >" : "  ",
             id_cell, state_cell, task->title);

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

    /* Colored state badge overlays the state column. */
    if (!selected && tui_colors_enabled) {
        attron(COLOR_PAIR(tui_state_color(state)) | A_BOLD);
        mvaddnstr(y, TUI_X_BOARD_STATE, dispatch_state_name(state),
                  TUI_COL_STATE);
        attroff(COLOR_PAIR(tui_state_color(state)) | A_BOLD);
    }
}

void draw_board_rows(DispatchTui *tui, int start_y, int rows, int cols) {
    int y = start_y;
    int visible_index = 0;
    int any_visible = 0;
    int visible_rows = rows - start_y - 1;
    sync_task_scroll(tui, visible_rows);

    VisibleTaskIter it;
    visible_task_iter_init(tui, &it);
    size_t header_group = (size_t)-1;
    for (DispatchTask *task = visible_task_iter_next(&it);
         task && y < rows - 1; task = visible_task_iter_next(&it)) {
        if (visible_index < tui->task_top) {
            visible_index++;
            continue;
        }
        if (it.group != header_group) {
            draw_group_header(tui, y++, cols,
                              &tui->board.groups.items[it.group]);
            header_group = it.group;
            if (y >= rows - 1)
                break;
        }

        draw_task_row(tui, y++, cols, task, visible_index);
        visible_index++;
        any_visible = 1;
    }

    if (!any_visible && y < rows - 1) {
        draw_padded(y, 0, cols,
                    tui->search[0] ? "(no matching not-done tasks)"
                                   : "(no not-done tasks)");
    }
}

void tui_render(DispatchTui *tui) {
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

/* Handle one top-level key press. View-specific controls only take
 * effect on their own screen; global keys (quit, help, palette, view
 * navigation) work everywhere. Extracted from the input loop so key
 * scoping can be exercised headlessly by --key-smoke. */
