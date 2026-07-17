/* Top-level key handling and the interactive run loop. */

#include "dispatch_tui_internal.h"

void tui_handle_key(DispatchTui *tui, int ch) {
    switch (ch) {
    case 'q':
        if (tui->screen == TUI_SCREEN_TASK_INSPECTOR) {
            tui->inspected_task_id[0] = '\0';
            tui->screen = TUI_SCREEN_BOARD;
        } else if (tui->screen == TUI_SCREEN_AGENT_INSPECTOR)
            tui->screen = TUI_SCREEN_AGENTS;
        else if (tui->screen == TUI_SCREEN_WORKSPACE_INSPECTOR)
            tui->screen = TUI_SCREEN_WORKSPACES;
        else
            tui_set_status(tui, "Use :q or Ctrl+C to quit");
        break;
    case '\t':
        tui->screen = tui->screen == TUI_SCREEN_AGENTS ? TUI_SCREEN_BOARD
                                                     : TUI_SCREEN_AGENTS;
        break;
    case 'a':
        tui->screen = TUI_SCREEN_AGENTS;
        break;
    case 'b':
        tui->screen = TUI_SCREEN_BOARD;
        break;
    case 'w':
        tui->screen = TUI_SCREEN_WORKSPACES;
        break;
    case 'l':
        tui->screen = TUI_SCREEN_LOGS;
        break;
    case 'L':
        if (tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            show_selected_task_logs(tui);
        else if (tui->screen == TUI_SCREEN_AGENT_INSPECTOR)
            show_selected_agent_logs(tui);
        else
            tui->screen = TUI_SCREEN_LOGS;
        break;
    case '\n':
    case KEY_ENTER:
    case 'i':
        if (tui->screen == TUI_SCREEN_BOARD && selected_visible_task(tui)) {
            tui->inspected_task_id[0] = '\0';
            tui->desc_top = 0;
            tui->screen = TUI_SCREEN_TASK_INSPECTOR;
            tui_set_status(tui, "Inspecting task");
        } else if (tui->screen == TUI_SCREEN_AGENTS && selected_agent(tui)) {
            tui->screen = TUI_SCREEN_AGENT_INSPECTOR;
            tui_set_status(tui, "Inspecting agent");
        } else if (tui->screen == TUI_SCREEN_WORKSPACES &&
                   selected_visible_workspace(tui)) {
            tui->screen = TUI_SCREEN_WORKSPACE_INSPECTOR;
            tui_set_status(tui, "Inspecting workspace");
        }
        break;
    case '?':
    case 'h':
        tui->show_help = !tui->show_help;
        break;
    case ':':
        run_command_palette(tui);
        break;
    case '1':
        if (tui->screen == TUI_SCREEN_BOARD)
            set_filter(tui, TUI_FILTER_NOT_DONE);
        break;
    case '2':
        if (tui->screen == TUI_SCREEN_BOARD)
            set_filter(tui, TUI_FILTER_ALL);
        break;
    case '3':
        if (tui->screen == TUI_SCREEN_BOARD)
            set_filter(tui, TUI_FILTER_READY);
        break;
    case '4':
        if (tui->screen == TUI_SCREEN_BOARD)
            set_filter(tui, TUI_FILTER_BLOCKED);
        break;
    case '5':
        if (tui->screen == TUI_SCREEN_BOARD)
            set_filter(tui, TUI_FILTER_REVIEW);
        break;
    case '6':
        if (tui->screen == TUI_SCREEN_BOARD)
            set_filter(tui, TUI_FILTER_DOING);
        break;
    case '7':
        if (tui->screen == TUI_SCREEN_BOARD)
            set_filter(tui, TUI_FILTER_DONE);
        break;
    case '8':
        if (tui->screen == TUI_SCREEN_BOARD)
            set_filter(tui, TUI_FILTER_ATTENTION);
        break;
    case 'r':
        if (tui->screen == TUI_SCREEN_BOARD ||
            tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_selected_task_action(tui, TUI_ACTION_READY);
        break;
    case 'R':
        if (tui->screen == TUI_SCREEN_BOARD ||
            tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_selected_task_action(tui, TUI_ACTION_READY_NO_REVIEW);
        break;
    case 'n':
        if (tui->screen == TUI_SCREEN_BOARD) {
            tui->screen = TUI_SCREEN_TASK_FORM;
            run_task_form(tui);
            tui->screen = TUI_SCREEN_BOARD;
        } else if (tui->screen == TUI_SCREEN_AGENTS) {
            tui->screen = TUI_SCREEN_AGENT_FORM;
            run_agent_form(tui);
            tui->screen = TUI_SCREEN_AGENTS;
        } else if (tui->screen == TUI_SCREEN_WORKSPACES) {
            run_workspace_create_form(tui);
        }
        break;
    case '+':
        if (tui->screen == TUI_SCREEN_BOARD) {
            tui->screen = TUI_SCREEN_GROUP_FORM;
            run_group_form(tui);
            tui->screen = TUI_SCREEN_BOARD;
        }
        break;
    case '>':
        if (tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_dependency_prompt(tui, 1);
        break;
    case '<':
        if (tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_dependency_prompt(tui, 0);
        break;
    case 's':
        if (tui->screen == TUI_SCREEN_BOARD ||
            tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_selected_task_action(tui, TUI_ACTION_START);
        break;
    case 'S':
        if (tui->screen == TUI_SCREEN_AGENT_INSPECTOR)
            set_selected_agent_session(tui);
        break;
    case 'f':
        if (tui->screen == TUI_SCREEN_BOARD ||
            tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_selected_task_action(tui, TUI_ACTION_FINISH);
        break;
    case 'v':
        if (tui->screen == TUI_SCREEN_BOARD ||
            tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_selected_task_action(tui, TUI_ACTION_REVIEW);
        break;
    case 'd':
        if (tui->screen == TUI_SCREEN_BOARD ||
            tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_selected_task_diff(tui);
        break;
    case 'x':
        if (tui->screen == TUI_SCREEN_AGENT_INSPECTOR)
            clear_selected_agent_session(tui);
        else if (tui->screen == TUI_SCREEN_WORKSPACES ||
                 tui->screen == TUI_SCREEN_WORKSPACE_INSPECTOR)
            run_workspace_remove_form(tui, 0);
        else if (tui->screen == TUI_SCREEN_BOARD ||
                 tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_task_delete_form(tui, 0);
        break;
    case 'X':
        if (tui->screen == TUI_SCREEN_WORKSPACES ||
            tui->screen == TUI_SCREEN_WORKSPACE_INSPECTOR)
            run_workspace_remove_form(tui, 1);
        else if (tui->screen == TUI_SCREEN_BOARD ||
                 tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_task_delete_form(tui, 1);
        break;
    case 'P':
        if (tui->screen == TUI_SCREEN_WORKSPACES)
            run_workspace_prune_form(tui);
        break;
    case 'e':
        if (tui->screen == TUI_SCREEN_BOARD ||
            tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_task_edit_form(tui);
        else if (tui->screen == TUI_SCREEN_AGENT_INSPECTOR)
            edit_selected_agent_prompt(tui);
        break;
    case 'm':
        if (tui->screen == TUI_SCREEN_BOARD ||
            tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            run_task_move_picker(tui);
        break;
    case 'z':
        if (tui->screen == TUI_SCREEN_AGENTS ||
            tui->screen == TUI_SCREEN_AGENT_INSPECTOR)
            toggle_selected_agent_archived(tui);
        break;
    case 'y':
        if (tui->screen == TUI_SCREEN_AGENTS ||
            tui->screen == TUI_SCREEN_AGENT_INSPECTOR)
            copy_selected_agent_run_command(tui);
        break;
    case 'G':
        if (tui->screen == TUI_SCREEN_BOARD)
            cycle_group_filter(tui);
        break;
    case 'F':
        if (tui->screen == TUI_SCREEN_LOGS)
            run_log_filter_form(tui);
        break;
    case 'A':
        if (tui->screen == TUI_SCREEN_AGENTS) {
            tui->show_archived_agents = !tui->show_archived_agents;
            tui->agent_top = 0;
            clamp_agent_selection(tui);
            tui_set_status(tui, tui->show_archived_agents
                                    ? "Showing all agents"
                                    : "Showing enabled agents");
        } else if (tui->screen == TUI_SCREEN_BOARD) {
            cycle_actor_filter(tui);
        }
        break;
    case 'c':
        if (tui->screen == TUI_SCREEN_BOARD)
            clear_secondary_filters(tui);
        break;
    case 'C':
        if (tui->screen == TUI_SCREEN_LOGS)
            set_log_filter(tui, "", "");
        break;
    case '/':
        if (tui->screen == TUI_SCREEN_BOARD) {
            tui->search_active = 1;
            tui_set_status(tui, "Search");
        }
        break;
    case 27:
        if (tui->screen == TUI_SCREEN_TASK_INSPECTOR) {
            tui->inspected_task_id[0] = '\0';
            tui->screen = TUI_SCREEN_BOARD;
            tui_set_status(tui, "Board");
        } else if (tui->screen == TUI_SCREEN_AGENT_INSPECTOR) {
            tui->screen = TUI_SCREEN_AGENTS;
            tui_set_status(tui, "Agents");
        } else if (tui->screen == TUI_SCREEN_WORKSPACE_INSPECTOR) {
            tui->screen = TUI_SCREEN_WORKSPACES;
            tui_set_status(tui, "Workspaces");
        } else {
            tui->search_active = 0;
            tui->search[0] = '\0';
            tui->selected_task = 0;
            tui->task_top = 0;
            tui_set_status(tui, "Search cleared");
        }
        break;
    case KEY_UP:
    case 'k':
        if (tui->screen == TUI_SCREEN_AGENTS) {
            tui->selected_agent--;
            clamp_agent_selection(tui);
        } else if (tui->screen == TUI_SCREEN_WORKSPACES) {
            tui->selected_workspace--;
            clamp_workspace_selection(tui);
        } else if (tui->screen == TUI_SCREEN_LOGS) {
            tui->selected_log--;
            clamp_log_selection(tui);
        } else {
            tui->selected_task--;
            tui->desc_top = 0;
            clamp_selection(tui);
        }
        break;
    case KEY_DOWN:
    case 'j':
        if (tui->screen == TUI_SCREEN_AGENTS) {
            tui->selected_agent++;
            clamp_agent_selection(tui);
        } else if (tui->screen == TUI_SCREEN_WORKSPACES) {
            tui->selected_workspace++;
            clamp_workspace_selection(tui);
        } else if (tui->screen == TUI_SCREEN_LOGS) {
            tui->selected_log++;
            clamp_log_selection(tui);
        } else {
            tui->selected_task++;
            tui->desc_top = 0;
            clamp_selection(tui);
        }
        break;
    case KEY_NPAGE:
        if (tui->screen == TUI_SCREEN_TASK_INSPECTOR)
            tui->desc_top += tui->desc_region > 0 ? tui->desc_region : 1;
        break;
    case KEY_PPAGE:
        if (tui->screen == TUI_SCREEN_TASK_INSPECTOR) {
            tui->desc_top -= tui->desc_region > 0 ? tui->desc_region : 1;
            if (tui->desc_top < 0)
                tui->desc_top = 0;
        }
        break;
    case 'u':
        tui_load_board(tui);
        clamp_selection(tui);
        clamp_agent_selection(tui);
        clamp_workspace_selection(tui);
        clamp_log_selection(tui);
        break;
    case KEY_RESIZE:
        tui_set_status(tui, "Resized");
        break;
    case KEY_BACKSPACE:
    case 127:
    case '\b':
        if (tui->search_active && strlen(tui->search) > 0) {
            tui->search[strlen(tui->search) - 1] = '\0';
            tui->selected_task = 0;
            tui->task_top = 0;
        }
        break;
    default:
        if (tui->search_active && ch >= 0 && ch < 256 &&
            isprint((unsigned char)ch)) {
            size_t len = strlen(tui->search);
            if (len + 1 < sizeof(tui->search)) {
                tui->search[len] = (char)ch;
                tui->search[len + 1] = '\0';
                tui->selected_task = 0;
                tui->task_top = 0;
            }
        }
        break;
    }

}

int tui_run(void) {
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
        tui_handle_key(&tui, ch);
    }

    endwin();
    tui_free_board(&tui);
    return 0;
}
