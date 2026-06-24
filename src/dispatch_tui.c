#include "dispatch_tui.h"

#include <ncurses.h>
#include <stdio.h>
#include <string.h>

#include "dispatch.h"
#include "dispatch_store.h"

typedef struct {
    DispatchBoard board;
    int board_loaded;
    char status[256];
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
        "Tab      cycle screen (future)",
        "arrows   move selection (future)",
        "j/k      move selection (future)",
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

static void tui_render(DispatchTui *tui) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    erase();

    attron(A_BOLD);
    draw_truncated(0, 0, cols, "Dispatch TUI");
    attroff(A_BOLD);

    if (!tui->board_loaded) {
        draw_truncated(2, 0, cols, "Board not loaded.");
    } else {
        char line[512];
        snprintf(line, sizeof(line), "Board: %s    Repo: %s", tui->board.name,
                 tui->board.repo_path ? tui->board.repo_path : ".");
        draw_truncated(2, 0, cols, line);

        snprintf(line, sizeof(line),
                 "Tasks: %zu total  ready:%d  doing:%d  review:%d  blocked:%d  done:%d",
                 tui->board.tasks.count,
                 task_count_for_state(&tui->board, DISPATCH_STATE_READY),
                 task_count_for_state(&tui->board, DISPATCH_STATE_DOING),
                 task_count_for_state(&tui->board, DISPATCH_STATE_REVIEW),
                 task_count_for_state(&tui->board, DISPATCH_STATE_BLOCKED),
                 task_count_for_state(&tui->board, DISPATCH_STATE_DONE));
        draw_truncated(4, 0, cols, line);

        snprintf(line, sizeof(line), "Groups: %zu    Agents: %zu    Workspaces: %zu",
                 tui->board.groups.count, tui->board.agents.count,
                 tui->board.workspaces.count);
        draw_truncated(5, 0, cols, line);

        draw_truncated(7, 0, cols,
                       "Board view foundation is active. Press ? for help.");
    }

    if (rows > 1) {
        attron(A_REVERSE);
        mvhline(rows - 1, 0, ' ', cols);
        char status[512];
        snprintf(status, sizeof(status), " %s | q quit | ? help | R reload",
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
        case 'R':
        case 'r':
            tui_load_board(&tui);
            break;
        case KEY_RESIZE:
            tui_set_status(&tui, "Resized");
            break;
        default:
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

    printf("dispatch tui smoke ok: %zu tasks, %zu groups, %zu agents, %zu workspaces\n",
           board.tasks.count, board.groups.count, board.agents.count,
           board.workspaces.count);
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
