#include "dispatch_tui.h"

#include <ncurses.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "dispatch.h"
#include "dispatch_store.h"

typedef struct {
    DispatchBoard board;
    int board_loaded;
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

static int task_is_done(const DispatchBoard *board, const DispatchTask *task) {
    return dispatch_task_effective_state(board, task) == DISPATCH_STATE_DONE;
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

static int task_is_visible(const DispatchBoard *board, const DispatchTask *task,
                           const char *search) {
    if (task_is_done(board, task))
        return 0;
    return task_matches_search(task, search);
}

static int visible_task_count(const DispatchBoard *board, const char *search) {
    int count = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        if (task_is_visible(board, &board->tasks.items[i], search))
            count++;
    }
    return count;
}

static void clamp_selection(DispatchTui *tui) {
    int count = tui->board_loaded ? visible_task_count(&tui->board, tui->search)
                                  : 0;
    if (count <= 0) {
        tui->selected_task = 0;
    } else if (tui->selected_task >= count) {
        tui->selected_task = count - 1;
    } else if (tui->selected_task < 0) {
        tui->selected_task = 0;
    }
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
        "R        reload board",
        "/        search tasks",
        "Esc      clear search",
        "arrows   move selection",
        "j/k      move selection",
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
                task_is_visible(&tui->board, task, tui->search)) {
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
                !task_is_visible(&tui->board, task, tui->search)) {
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
    draw_truncated(0, 0, cols, "Dispatch TUI - Board");
    attroff(A_BOLD);

    if (!tui->board_loaded) {
        draw_truncated(2, 0, cols, "Board not loaded.");
    } else {
        char line[512];
        snprintf(line, sizeof(line), "Board: %s    Repo: %s", tui->board.name,
                 tui->board.repo_path ? tui->board.repo_path : ".");
        draw_truncated(2, 0, cols, line);

        int visible = visible_task_count(&tui->board, tui->search);
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
                 "Filter: not-done%s%s%s    Groups: %zu    Agents: %zu    Workspaces: %zu",
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
                 " %s | q quit | ? help | R reload | / search | j/k move",
                 tui->status[0] ? tui->status : "Ready");
        draw_truncated(rows - 1, 0, cols, status);
        attroff(A_REVERSE);
    }

    if (tui->show_help)
        tui_render_help();

    refresh();
}

static int tui_run(void) {
    DispatchTui tui = {0};
    tui.running = 1;
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
            tui.running = 0;
            break;
        case '?':
            tui.show_help = !tui.show_help;
            break;
        case '/':
            tui.search_active = 1;
            tui_set_status(&tui, "Search");
            break;
        case 27:
            tui.search_active = 0;
            tui.search[0] = '\0';
            tui.selected_task = 0;
            tui_set_status(&tui, "Search cleared");
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
        case 'R':
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
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    printf("dispatch tui smoke ok: %zu tasks, %d visible, %zu groups, %zu agents, %zu workspaces\n",
           board.tasks.count, visible_task_count(&board, ""), board.groups.count,
           board.agents.count, board.workspaces.count);
    dispatch_board_free(&board);
    return 0;
}

static void print_tui_help(void) {
    puts("Usage: dispatch tui [--smoke]");
    puts("");
    puts("Open the ncurses Dispatch terminal UI.");
    puts("  --smoke   load the board and exit without initializing ncurses");
}

int dispatch_tui_main(int argc, char **argv) {
    if (argc == 3 &&
        (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        print_tui_help();
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "--smoke") == 0)
        return tui_smoke();
    if (argc != 2) {
        print_tui_help();
        return 1;
    }

    return tui_run();
}
