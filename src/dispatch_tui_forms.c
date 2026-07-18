/* Interactive prompts and the task/agent/group form screens. */

#include "dispatch_tui_internal.h"

static int prompt_yes_no(const char *label, int default_yes);
static void task_form_buffer(TuiTaskForm *form, int field, char **buffer,
                             size_t *buffer_size);
static int task_form_deps_contains(const char *deps_text, const char *id);
static void draw_task_form_box(int y, int x, int width, int height,
                               const char *label, const char *value,
                               int active);
static void draw_task_form_review_box(int y, int x, int width,
                                      const TuiTaskForm *form);
static void task_form_move_cursor(const TuiTaskForm *form, int rows, int cols,
                                  int left, int width, int desc_height);
static int handle_task_form_key(TuiTaskForm *form, int ch, int *submit,
                                int *cancel);
static int run_task_form_picker(const char *title,
                                const TuiTaskFormOption *options, size_t count);
static int task_form_open_picker(DispatchTui *tui, TuiTaskForm *form);
static void task_edit_form_buffer(TuiTaskEditForm *form, char **buffer,
                                  size_t *buffer_size);
static void render_task_edit_form(DispatchTui *tui,
                                  const TuiTaskEditForm *form,
                                  const char *task_id);
static void handle_task_edit_form_key(TuiTaskEditForm *form, int ch,
                                      int *submit, int *cancel);
static void agent_form_ensure_runner(TuiAgentForm *form);
static void agent_form_toggle_runner(TuiAgentForm *form);
static void agent_form_buffer(TuiAgentForm *form, int field, char **buffer,
                              size_t *buffer_size);
static void agent_form_move_cursor(const TuiAgentForm *form, int rows, int cols,
                                   int left, int width);
static void group_form_buffer(TuiGroupForm *form, int field, char **buffer,
                              size_t *buffer_size);
static int handle_group_form_key(TuiGroupForm *form, int ch, int *submit,
                                 int *cancel);
static void run_external_argv_in_terminal(DispatchTui *tui,
                                          const char *const argv[],
                                          const char *label);

int handle_prompt_line_key(char *buffer, size_t buffer_size, int ch,
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

int prompt_line(const char *label, char *buffer, size_t buffer_size,
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

const char *selected_task_group(DispatchTui *tui) {
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

void run_group_form(DispatchTui *tui);

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
int task_form_deps_append(char *deps_text, size_t size, const char *id) {
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
size_t task_form_group_options(const DispatchBoard *board,
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

size_t task_move_group_options(const DispatchBoard *board,
                                      const char *current_group,
                                      TuiTaskFormOption *out, size_t max) {
    size_t count = 0;
    for (size_t i = 0; board && i < board->groups.count && count < max; i++) {
        const DispatchGroup *group = &board->groups.items[i];
        if (current_group && strcmp(group->id, current_group) == 0)
            continue;
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
size_t task_form_dep_options(const DispatchBoard *board,
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

void render_task_form_screen(DispatchTui *tui, const TuiTaskForm *form) {
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

int task_form_submit(const TuiTaskForm *form, const char *actor,
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

void run_task_form(DispatchTui *tui) {
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

static void task_edit_form_buffer(TuiTaskEditForm *form, char **buffer,
                                  size_t *buffer_size) {
    if (form->active_field == TUI_TASK_EDIT_TITLE) {
        *buffer = form->title;
        *buffer_size = sizeof(form->title);
    } else {
        *buffer = form->description;
        *buffer_size = sizeof(form->description);
    }
}

static void render_task_edit_form(DispatchTui *tui,
                                  const TuiTaskEditForm *form,
                                  const char *task_id) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    erase();
    draw_title_bar(tui, "Edit Task");

    if (rows < 16 || cols < 50) {
        draw_truncated(2, 0, cols, "Terminal too small for task editor.");
        draw_truncated(3, 0, cols, "Resize to at least 50x16.");
        refresh();
        return;
    }

    int width = cols - 4;
    if (width > 76)
        width = 76;
    int left = (cols - width) / 2;
    /* Grow the description box to fill the space between the Title box and the
     * footer so the whole description is visible instead of being clipped to a
     * fixed height. The box top sits at row 8 (label at row 7); leave the last
     * row for the footer. */
    int desc_height = rows - 9;
    if (desc_height < 5)
        desc_height = 5;

    char heading[256];
    snprintf(heading, sizeof(heading), "Editing %s as %s", task_id,
             tui->actor);
    if (tui_colors_enabled)
        attron(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);
    draw_truncated(1, left, width, heading);
    if (tui_colors_enabled)
        attroff(COLOR_PAIR(TUI_COLOR_MUTED) | A_DIM);

    draw_task_form_box(3, left, width, 3, "Title", form->title,
                       form->active_field == TUI_TASK_EDIT_TITLE);
    draw_task_form_box(7, left, width, desc_height, "Description",
                       form->description,
                       form->active_field == TUI_TASK_EDIT_DESCRIPTION);
    draw_footer(form->status[0] ? form->status : "Edit task",
                "Enter next/save   Tab move   Esc cancel");

    const char *buffer = form->active_field == TUI_TASK_EDIT_TITLE
                             ? form->title
                             : form->description;
    int height = form->active_field == TUI_TASK_EDIT_TITLE ? 3 : desc_height;
    int y = form->active_field == TUI_TASK_EDIT_TITLE ? 3 : 7;
    int inner_width = width - 4;
    if (inner_width > 0) {
        size_t len = strlen(buffer);
        int max_pos = inner_width * (height - 2) - 1;
        int pos = (int)(len > (size_t)max_pos ? (size_t)max_pos : len);
        move(y + 2 + pos / inner_width, left + 2 + pos % inner_width);
    }
    refresh();
}

static void handle_task_edit_form_key(TuiTaskEditForm *form, int ch,
                                      int *submit, int *cancel) {
    *submit = 0;
    *cancel = 0;
    if (ch == 27) {
        *cancel = 1;
        return;
    }
    if (ch == '\t' || ch == KEY_DOWN || ch == KEY_UP) {
        form->active_field =
            (form->active_field + 1) % TUI_TASK_EDIT_FIELD_COUNT;
        return;
    }
    if (ch == '\n' || ch == KEY_ENTER) {
        if (form->active_field == TUI_TASK_EDIT_DESCRIPTION)
            *submit = 1;
        else
            form->active_field = TUI_TASK_EDIT_DESCRIPTION;
        return;
    }

    char *buffer = NULL;
    size_t buffer_size = 0;
    task_edit_form_buffer(form, &buffer, &buffer_size);
    size_t len = strlen(buffer);
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (len > 0)
            buffer[len - 1] = '\0';
    } else if (ch == 21) {
        buffer[0] = '\0';
    } else if (ch >= 0 && ch < 256 && isprint((unsigned char)ch) &&
               len + 1 < buffer_size) {
        buffer[len] = (char)ch;
        buffer[len + 1] = '\0';
    }
}

int task_edit_form_submit(const char *task_id,
                                 const TuiTaskEditForm *form,
                                 const char *actor, char *message,
                                 size_t message_size) {
    return mutate_task_content(task_id, form->title, form->description, actor,
                               message, message_size);
}

void run_task_edit_form(DispatchTui *tui) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        tui_set_status(tui, "No selected task");
        return;
    }

    char task_id[64];
    snprintf(task_id, sizeof(task_id), "%s", task->id);
    TuiTaskEditForm form;
    memset(&form, 0, sizeof(form));
    snprintf(form.title, sizeof(form.title), "%s", task->title);
    snprintf(form.description, sizeof(form.description), "%s",
             task->description ? task->description : "");
    snprintf(form.status, sizeof(form.status), "Update task fields");

    timeout(-1);
    curs_set(1);
    for (;;) {
        render_task_edit_form(tui, &form, task_id);
        int ch = getch();
        if (ch == KEY_RESIZE)
            continue;
        int submit = 0;
        int cancel = 0;
        handle_task_edit_form_key(&form, ch, &submit, &cancel);
        if (cancel) {
            tui_set_status(tui, "Task edit cancelled");
            break;
        }
        if (submit) {
            char message[256] = {0};
            if (!task_edit_form_submit(task_id, &form, tui->actor, message,
                                       sizeof(message))) {
                snprintf(form.status, sizeof(form.status), "%s", message);
                continue;
            }
            tui_load_board(tui);
            select_task_by_id(tui, task_id);
            tui_set_status(tui, message);
            break;
        }
    }
    curs_set(0);
    timeout(1000);
}

void run_task_move_picker(DispatchTui *tui) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        tui_set_status(tui, "No selected task");
        return;
    }

    char task_id[64];
    char current_group[32];
    snprintf(task_id, sizeof(task_id), "%s", task->id);
    snprintf(current_group, sizeof(current_group), "%s", task->group);

    TuiTaskFormOption options[64];
    size_t count =
        task_move_group_options(&tui->board, current_group, options, 64);
    if (count == 0) {
        tui_set_status(tui, "No other group to move this task to");
        return;
    }

    int selected = run_task_form_picker("Move task to group", options, count);
    if (selected < 0) {
        tui_set_status(tui, "Task move cancelled");
        return;
    }

    char message[256] = {0};
    mutate_task_group(task_id, options[selected].value, tui->actor, message,
                      sizeof(message));
    tui_load_board(tui);
    select_task_by_id(tui, task_id);
    tui_set_status(tui, message);
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

    draw_footer(form->status[0] ? form->status : "New agent",
                tui_footer_hints(TUI_SCREEN_AGENT_FORM));

    agent_form_move_cursor(form, rows, cols, left, width);
    refresh();
}

int handle_agent_form_key(TuiAgentForm *form, int ch, int *submit,
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
        if (form->active_field == TUI_AGENT_FORM_MODEL)
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

int agent_form_submit(const TuiAgentForm *form, const char *actor,
                             char *message, size_t message_size) {
    DispatchAgentCreateOptions options = {
        .name = form->name,
        .runner = form->runner[0] ? form->runner : "codex",
        .model = form->model,
        .actor = actor && actor[0] ? actor : "user",
        /* TUI-created agents always get a run script; --no-run-script
         * remains available on the CLI as a scripting escape hatch. */
        .no_run_script = 0,
    };
    DispatchAgentCreateResult result = {0};
    char error[256] = {0};
    if (!dispatch_agent_create(&options, &result, error, sizeof(error))) {
        snprintf(message, message_size, "%s",
                 error[0] ? error : "Could not create agent");
        return 0;
    }

    snprintf(message, message_size, "Created agent %s (%s)", options.name,
             options.runner);
    dispatch_agent_create_result_free(&result);
    return 1;
}

void run_agent_form(DispatchTui *tui) {
    TuiAgentForm form;
    memset(&form, 0, sizeof(form));
    snprintf(form.runner, sizeof(form.runner), "codex");
    form.active_field = TUI_AGENT_FORM_NAME;
    snprintf(form.status, sizeof(form.status), "Fill agent fields");

    timeout(-1);
    curs_set(1);
    int running = 1;
    while (running) {
        render_agent_form_screen(tui, &form);
        int ch = getch();
        if (ch == KEY_RESIZE)
            continue;
        int submit = 0;
        int cancel = 0;
        handle_agent_form_key(&form, ch, &submit, &cancel);
        if (cancel) {
            tui_set_status(tui, "Agent creation cancelled");
            break;
        }
        if (submit) {
            char message[256] = {0};
            if (agent_form_submit(&form, tui->actor, message,
                                  sizeof(message))) {
                tui_load_board(tui);
                select_agent_by_name(tui, form.name);
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

/* New group form ----------------------------------------------------------- */



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

void render_group_form_screen(DispatchTui *tui,
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

void run_group_form(DispatchTui *tui) {
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

void run_dependency_prompt(DispatchTui *tui, int add) {
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

static void run_external_argv_in_terminal(DispatchTui *tui,
                                          const char *const argv[],
                                          const char *label) {
    def_prog_mode();
    endwin();
    DispatchExecResult result;
    int launched = dispatch_exec_run(argv, NULL, &result);
    reset_prog_mode();
    refresh();

    char message[256];
    if (launched) {
        snprintf(message, sizeof(message), "%s exited with status %d", label,
                 dispatch_exec_result_status(&result));
    } else {
        snprintf(message, sizeof(message), "Could not run %s", label);
    }
    tui_load_board(tui);
    tui_set_status(tui, message);
}

void run_workspace_create_form(DispatchTui *tui) {
    char task_id[64];
    char actor[64];
    if (!prompt_line("Workspace task ID: ", task_id, sizeof(task_id), ""))
        return;
    if (!prompt_line("Actor: ", actor, sizeof(actor), tui->actor))
        return;
    int sequence = prompt_yes_no("Create sequence workspace? [y/N]: ", 0);

    const char *normal_argv[] = {"./dispatch", "workspace", "create", task_id,
                                 "--actor",    actor,       NULL};
    const char *sequence_argv[] = {"./dispatch", "workspace", "create",
                                   task_id,      "--actor",   actor,
                                   "--sequence", NULL};
    run_external_argv_in_terminal(tui, sequence ? sequence_argv : normal_argv,
                                  "Workspace create");
    clamp_workspace_selection(tui);
}

void run_workspace_remove_form(DispatchTui *tui, int force) {
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

    const char *normal_argv[] = {"./dispatch", "workspace", "remove",
                                 workspace->task_id, NULL};
    const char *force_argv[] = {"./dispatch",       "workspace", "remove",
                                workspace->task_id, "--force",   NULL};
    run_external_argv_in_terminal(tui, force ? force_argv : normal_argv,
                                  "Workspace remove");
    clamp_workspace_selection(tui);
}

/* Delete the selected task after a typed-id confirmation, mirroring
 * run_workspace_remove_form. The force variant surfaces which dependents
 * will lose the dependency before asking for confirmation. */
void run_task_delete_form(DispatchTui *tui, int force) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        tui_set_status(tui, "No selected task");
        return;
    }

    char task_id[64];
    snprintf(task_id, sizeof(task_id), "%s", task->id);

    char label[256];
    char blocks[128];
    blocks_text(&tui->board, task_id, blocks, sizeof(blocks));
    if (force && strcmp(blocks, "-") != 0) {
        snprintf(label, sizeof(label),
                 "Type %s to force delete (unblocks: %s): ", task_id, blocks);
    } else {
        snprintf(label, sizeof(label), "Type %s to delete: ", task_id);
    }

    char confirm[64];
    if (!prompt_line(label, confirm, sizeof(confirm), ""))
        return;
    if (strcmp(confirm, task_id) != 0) {
        tui_set_status(tui, "Task delete cancelled");
        return;
    }

    char message[256] = {0};
    int deleted = mutate_task_delete(task_id, force, message, sizeof(message));
    tui_load_board(tui);
    if (deleted && tui->screen == TUI_SCREEN_TASK_INSPECTOR) {
        tui->inspected_task_id[0] = '\0';
        tui->screen = TUI_SCREEN_BOARD;
    }
    clamp_selection(tui);
    tui_set_status(tui, message);
}

void run_workspace_prune_form(DispatchTui *tui) {
    char confirm[32];
    if (!prompt_line("Type prune to remove done clean workspaces: ", confirm,
                     sizeof(confirm), ""))
        return;
    if (strcmp(confirm, "prune") != 0) {
        tui_set_status(tui, "Workspace prune cancelled");
        return;
    }
    const char *argv[] = {"./dispatch", "workspace", "prune", "--done", NULL};
    run_external_argv_in_terminal(tui, argv, "Workspace prune");
    clamp_workspace_selection(tui);
}

void set_log_filter(DispatchTui *tui, const char *field,
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

void run_log_filter_form(DispatchTui *tui) {
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

void show_selected_task_logs(DispatchTui *tui) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        tui_set_status(tui, "No selected task");
        return;
    }
    set_log_filter(tui, "task", task->id);
    tui->screen = TUI_SCREEN_LOGS;
}

void show_selected_agent_logs(DispatchTui *tui) {
    DispatchAgent *agent = selected_agent(tui);
    if (!agent) {
        tui_set_status(tui, "No selected agent");
        return;
    }
    set_log_filter(tui, "agent", agent->name);
    tui->screen = TUI_SCREEN_LOGS;
}
