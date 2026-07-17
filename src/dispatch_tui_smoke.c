/* Headless smoke entry points and the dispatch tui argument parser. */

#include "dispatch_tui_internal.h"

static int tui_smoke(void);
static int tui_inspect_smoke(const char *task_id);
static int tui_filter_smoke(const char *filter_name_arg);
static int tui_action_smoke(const char *action_name, const char *task_id,
                            const char *actor);
static int tui_task_edit_smoke(const char *task_id, const char *title,
                               const char *description, const char *actor);
static int tui_task_move_options_smoke(const char *current_group);
static int tui_task_move_smoke(const char *task_id, const char *group_id,
                               const char *actor);
static int tui_diff_smoke(const char *task_id);
static int tui_diff_exec_smoke(const char *task_id);
static int tui_agents_smoke(void);
static int tui_agent_inspect_smoke(const char *name);
static int tui_agent_session_smoke(const char *name, const char *session_id,
                                   const char *current_task,
                                   const char *last_workspace);
static int tui_agent_set_session_smoke(const char *name, const char *session_id);
static int tui_prompt_edit_smoke(const char *name);
static int tui_prompt_edit_exec_smoke(const char *name);
static int tui_tmux_copy_smoke(const char *text);
static int tui_agent_archive_smoke(const char *name, const char *action);
static int tui_agent_selection_smoke(const char *mode, int selected_index);
static int tui_agent_run_command_smoke(const char *name);
static int tui_osc52_smoke(const char *text);
static int tui_create_group_smoke(const char *name, const char *prefix);
static int tui_create_task_smoke(const char *group, const char *title,
                                 const char *description,
                                 const char *review_mode,
                                 const char *depends_text);
static int tui_task_form_submit_smoke(const char *group, const char *title,
                                      const char *description,
                                      const char *review_mode,
                                      const char *depends_text);
static int tui_task_form_options_smoke(const char *kind, const char *deps_text);
static int tui_task_form_deps_add_smoke(const char *deps_text, const char *id);
static int tui_agent_form_keys_smoke(const char *keys);
static int tui_agent_form_submit_smoke(const char *name, const char *runner,
                                       const char *model);
static int tui_prompt_cancel_smoke(void);
static int tui_escdelay_smoke(void);
static int tui_dependency_smoke(const char *action, const char *dependency_id,
                                const char *dependent_id);
static int tui_workspaces_smoke(void);
static int tui_workspace_inspect_smoke(const char *target);
static int tui_logs_smoke(const char *field, const char *value);
static int tui_logs_window_smoke(int visible_rows, int selected_index,
                                 const char *field, const char *value);
static int tui_scroll_smoke(const char *screen, int visible_rows,
                            int selected_index);
static int tui_selection_smoke(const char *filter_name_arg,
                               int selected_index);
static const char *screen_name(DispatchTuiScreen screen);
static int parse_screen_name(const char *name, DispatchTuiScreen *screen);
static void tui_render_headless_screen(DispatchTui *tui);
static char tui_headless_cell_char(chtype cell);
static int tui_capture_headless_frame(int rows, int cols);
static int tui_render_smoke(const char *screen_arg, int cols, int rows,
                            const char *keys);
static int tui_key_smoke(const char *screen_arg, const char *keys);
static int tui_desc_wrap_smoke(const char *task_id, int width, int region,
                               int top);
static int tui_task_delete_smoke(const char *task_id, const char *mode);
static int tui_cell_smoke(int width, const char *text);
static int tui_agent_row_smoke(const char *name);
static int tui_palette_smoke(const char *command);
static int tui_palette_complete_smoke(const char *input);
static int tui_search_smoke(const char *keys);
static int tui_help_controls_smoke(void);
static int tui_refresh_smoke(void);
static void print_tui_help(void);

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
    printf("Description: %s\n", task->description);
    printf("Group: %s\n", task->group);
    printf("State: %s\n",
           dispatch_state_name(dispatch_task_effective_state(&board, task)));
    printf("Requires review: %s\n", task->requires_review ? "yes" : "no");
    printf("Depends on: %zu\n", task->depends_on.count);
    char relationships[1024];
    join_string_list(&task->depends_on, relationships, sizeof(relationships));
    printf("Dependency IDs: %s\n", relationships);
    blocks_text(&board, task->id, relationships, sizeof(relationships));
    printf("Blocks: %s\n", relationships);
    printf("Commits: %zu\n", task->commits.count);
    for (size_t i = 0; i < task->commits.count; i++)
        printf("Commit: %s\n", task->commits.items[i]);
    printf("History: %zu\n", task->history.count);
    for (size_t i = 0; i < task->history.count; i++) {
        DispatchHistoryEntry *entry = &task->history.items[i];
        printf("History entry: %s by %s%s%s\n", entry->action, entry->actor,
               entry->note && entry->note[0] ? ": " : "",
               entry->note && entry->note[0] ? entry->note : "");
    }
    DispatchWorkspace *workspace = workspace_for_task(&board, task->id);
    printf("Workspace: %s\n", workspace ? workspace->id : "-");

    dispatch_board_free(&board);
    return 0;
}

int parse_filter_name(const char *name, DispatchTuiFilter *filter) {
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

int parse_action_name(const char *name, DispatchTuiAction *action) {
    if (strcmp(name, "ready") == 0) {
        *action = TUI_ACTION_READY;
    } else if (strcmp(name, "ready-no-review") == 0) {
        *action = TUI_ACTION_READY_NO_REVIEW;
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

static int tui_task_edit_smoke(const char *task_id, const char *title,
                               const char *description, const char *actor) {
    TuiTaskEditForm form;
    memset(&form, 0, sizeof(form));
    snprintf(form.title, sizeof(form.title), "%s", title);
    snprintf(form.description, sizeof(form.description), "%s",
             strcmp(description, "-") == 0 ? "" : description);
    char message[256] = {0};
    if (!task_edit_form_submit(task_id, &form, actor, message,
                               sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int tui_task_move_options_smoke(const char *current_group) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui move options smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchGroup *group = dispatch_board_find_group(&board, current_group);
    if (!group) {
        fprintf(stderr, "No group with id, prefix, or name %s\n",
                current_group);
        dispatch_board_free(&board);
        return 1;
    }

    TuiTaskFormOption options[64];
    size_t count = task_move_group_options(&board, group->id, options, 64);
    printf("Options: %zu\n", count);
    for (size_t i = 0; i < count; i++)
        printf("%s\n", options[i].label);
    dispatch_board_free(&board);
    return 0;
}

static int tui_task_move_smoke(const char *task_id, const char *group_id,
                               const char *actor) {
    char message[256] = {0};
    if (!mutate_task_group(task_id, group_id, actor, message,
                           sizeof(message))) {
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

    DispatchExecArgv command = {0};
    if (!diff_argv_for_task(&board, task, 0, &command)) {
        fprintf(stderr, "No commit metadata for %s\n", task_id);
        dispatch_board_free(&board);
        return 1;
    }

    for (size_t i = 0; i < command.count; i++)
        printf("argv[%zu]: %s\n", i, command.items[i]);
    dispatch_exec_argv_free(&command);
    dispatch_board_free(&board);
    return 0;
}

static int tui_diff_exec_smoke(const char *task_id) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui diff exec smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    DispatchExecArgv command = {0};
    if (!task || !diff_argv_for_task(&board, task, 0, &command)) {
        fprintf(stderr, "No commit metadata for %s\n", task_id);
        dispatch_board_free(&board);
        return 1;
    }

    DispatchExecOptions options = {.merge_stderr_to_stdout = 1};
    DispatchExecResult result;
    char *output = NULL;
    size_t output_size = 0;
    int launched =
        dispatch_exec_capture((const char *const *)command.items, &options,
                              &output, &output_size, &result);
    int status = launched ? dispatch_exec_result_status(&result) : -1;
    printf("Diff status: %d\n", status);
    printf("Diff bytes: %zu\n", output_size);
    printf("Contains marker: %s\n",
           output && strstr(output, "safe exec marker") ? "yes" : "no");

    free(output);
    dispatch_exec_argv_free(&command);
    dispatch_board_free(&board);
    return launched && status == 0 && output_size > 0 ? 0 : 1;
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
    DispatchExecArgv command = {0};
    if (!editor_argv_for_path(agent->prompt_path, &command, editor_error,
                              sizeof(editor_error))) {
        fprintf(stderr, "%s\n", editor_error);
        dispatch_board_free(&board);
        return 1;
    }
    for (size_t i = 0; i < command.count; i++)
        printf("argv[%zu]: %s\n", i, command.items[i]);
    dispatch_exec_argv_free(&command);
    dispatch_board_free(&board);
    return 0;
}

static int tui_prompt_edit_exec_smoke(const char *name) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui prompt edit exec smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchAgent *agent = dispatch_board_find_agent(&board, name);
    DispatchExecArgv command = {0};
    if (!agent || !agent->prompt_path ||
        !editor_argv_for_path(agent->prompt_path, &command, error,
                              sizeof(error))) {
        fprintf(stderr, "%s\n", error[0] ? error : "No matching agent prompt");
        dispatch_board_free(&board);
        return 1;
    }

    DispatchExecResult result;
    int launched =
        dispatch_exec_run((const char *const *)command.items, NULL, &result);
    int status = launched ? dispatch_exec_result_status(&result) : -1;
    printf("Editor status: %d\n", status);
    dispatch_exec_argv_free(&command);
    dispatch_board_free(&board);
    return launched && status == 0 ? 0 : 1;
}

static int tui_tmux_copy_smoke(const char *text) {
    int copied = copy_command_to_tmux_buffer(text ? text : "");
    printf("Tmux copied: %s\n", copied ? "yes" : "no");
    return copied ? 0 : 1;
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
    printf("Submit: %s\n", submit ? "yes" : "no");
    printf("Cancel: %s\n", cancel ? "yes" : "no");
    return 0;
}

static int tui_agent_form_submit_smoke(const char *name, const char *runner,
                                       const char *model) {
    TuiAgentForm form;
    memset(&form, 0, sizeof(form));
    snprintf(form.name, sizeof(form.name), "%s", name ? name : "");
    snprintf(form.runner, sizeof(form.runner), "%s", runner ? runner : "");
    snprintf(form.model, sizeof(form.model), "%s",
             model && strcmp(model, "-") != 0 ? model : "");

    char message[256] = {0};
    if (!agent_form_submit(&form, "user", message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    printf("%s\n", message);
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

static int tui_selection_smoke(const char *filter_name_arg,
                               int selected_index) {
    DispatchTui tui;
    tui_init(&tui);
    if (!parse_filter_name(filter_name_arg, &tui.filter)) {
        fprintf(stderr, "Unknown TUI filter %s\n", filter_name_arg);
        return 1;
    }
    if (!tui_load_board(&tui)) {
        fprintf(stderr, "%s\n", tui.status);
        return 1;
    }

    tui.selected_task = selected_index;
    clamp_selection(&tui);

    DispatchTask *action_task = selected_visible_task(&tui);

    /* Recompute the highlighted task with an independent walk in grouped
     * render order (the order draw_board_rows paints), so this fails if
     * selection resolution ever falls back to flat storage order. */
    DispatchTask *highlighted = NULL;
    int visible_index = 0;
    for (size_t g = 0; g < tui.board.groups.count && !highlighted; g++) {
        DispatchGroup *group = &tui.board.groups.items[g];
        for (size_t i = 0; i < tui.board.tasks.count; i++) {
            DispatchTask *task = &tui.board.tasks.items[i];
            if (strcmp(task->group, group->id) != 0 ||
                !tui_task_is_visible(&tui, task))
                continue;
            if (visible_index == tui.selected_task) {
                highlighted = task;
                break;
            }
            visible_index++;
        }
    }

    printf("Selected: %d\n", tui.selected_task);
    printf("Action task: %s\n", action_task ? action_task->id : "-");
    printf("Highlighted task: %s\n", highlighted ? highlighted->id : "-");
    int match = action_task && highlighted && action_task == highlighted;
    printf("Match: %s\n", match ? "yes" : "no");
    tui_free_board(&tui);
    return match ? 0 : 1;
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

static int parse_screen_name(const char *name, DispatchTuiScreen *screen) {
    if (strcmp(name, "board") == 0) {
        *screen = TUI_SCREEN_BOARD;
    } else if (strcmp(name, "task") == 0) {
        *screen = TUI_SCREEN_TASK_INSPECTOR;
    } else if (strcmp(name, "agents") == 0) {
        *screen = TUI_SCREEN_AGENTS;
    } else if (strcmp(name, "agent") == 0) {
        *screen = TUI_SCREEN_AGENT_INSPECTOR;
    } else if (strcmp(name, "agent-form") == 0) {
        *screen = TUI_SCREEN_AGENT_FORM;
    } else if (strcmp(name, "task-form") == 0) {
        *screen = TUI_SCREEN_TASK_FORM;
    } else if (strcmp(name, "group-form") == 0) {
        *screen = TUI_SCREEN_GROUP_FORM;
    } else if (strcmp(name, "workspaces") == 0) {
        *screen = TUI_SCREEN_WORKSPACES;
    } else if (strcmp(name, "workspace") == 0) {
        *screen = TUI_SCREEN_WORKSPACE_INSPECTOR;
    } else if (strcmp(name, "logs") == 0) {
        *screen = TUI_SCREEN_LOGS;
    } else {
        return 0;
    }
    return 1;
}

static void tui_render_headless_screen(DispatchTui *tui) {
    if (tui->screen == TUI_SCREEN_TASK_FORM) {
        TuiTaskForm form;
        memset(&form, 0, sizeof(form));
        snprintf(form.group, sizeof(form.group), "%s", selected_task_group(tui));
        form.requires_review = 1;
        form.active_field = TUI_TASK_FORM_GROUP;
        snprintf(form.status, sizeof(form.status), "Fill task fields");
        render_task_form_screen(tui, &form);
    } else if (tui->screen == TUI_SCREEN_AGENT_FORM) {
        TuiAgentForm form;
        memset(&form, 0, sizeof(form));
        snprintf(form.runner, sizeof(form.runner), "codex");
        form.active_field = TUI_AGENT_FORM_NAME;
        snprintf(form.status, sizeof(form.status), "Fill agent fields");
        render_agent_form_screen(tui, &form);
    } else if (tui->screen == TUI_SCREEN_GROUP_FORM) {
        TuiGroupForm form;
        memset(&form, 0, sizeof(form));
        form.active_field = TUI_GROUP_FORM_NAME;
        snprintf(form.status, sizeof(form.status), "Fill group fields");
        render_group_form_screen(tui, &form);
    } else {
        tui_render(tui);
    }
}

static char tui_headless_cell_char(chtype cell) {
    unsigned char ch = (unsigned char)(cell & A_CHARTEXT);
    if (cell & A_ALTCHARSET) {
        switch (ch) {
        case 'q':
            return '-';
        case 'x':
            return '|';
        default:
            return '+';
        }
    }
    return ch >= 32 && ch <= 126 ? (char)ch : ' ';
}

static int tui_capture_headless_frame(int rows, int cols) {
    char *line = malloc((size_t)cols + 1);
    if (!line) {
        fprintf(stderr, "Out of memory capturing TUI frame\n");
        return 0;
    }

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++)
            line[x] = tui_headless_cell_char(mvinch(y, x));
        int length = cols;
        while (length > 0 && line[length - 1] == ' ')
            length--;
        line[length] = '\0';
        puts(line);
    }
    free(line);
    return 1;
}

static int tui_render_smoke(const char *screen_arg, int cols, int rows,
                            const char *keys) {
    if (cols < 40 || cols > 300 || rows < 10 || rows > 120) {
        fprintf(stderr,
                "Headless TUI dimensions must be 40-300 columns and 10-120 rows\n");
        return 1;
    }

    DispatchTui tui;
    tui_init(&tui);
    if (!parse_screen_name(screen_arg, &tui.screen)) {
        fprintf(stderr, "Unknown TUI screen %s\n", screen_arg);
        return 1;
    }
    if (!tui_load_board(&tui)) {
        fprintf(stderr, "%s\n", tui.status);
        return 1;
    }

    FILE *input = fopen("/dev/null", "r");
    FILE *output = tmpfile();
    if (!input || !output) {
        fprintf(stderr, "Could not create headless TUI streams\n");
        if (input)
            fclose(input);
        if (output)
            fclose(output);
        tui_free_board(&tui);
        return 1;
    }

    SCREEN *virtual_screen = newterm("xterm", output, input);
    if (!virtual_screen) {
        fprintf(stderr, "Could not initialize headless ncurses screen\n");
        fclose(input);
        fclose(output);
        tui_free_board(&tui);
        return 1;
    }

    set_term(virtual_screen);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    if (resizeterm(rows, cols) == ERR) {
        fprintf(stderr, "Could not resize headless TUI screen\n");
        endwin();
        delscreen(virtual_screen);
        fclose(input);
        fclose(output);
        tui_free_board(&tui);
        return 1;
    }

    for (size_t i = 0; keys && keys[i] != '\0'; i++)
        tui_handle_key(&tui, (unsigned char)keys[i]);
    tui_render_headless_screen(&tui);
    printf("Frame: %s %dx%d\n", screen_name(tui.screen), cols, rows);
    int captured = tui_capture_headless_frame(rows, cols);

    endwin();
    delscreen(virtual_screen);
    fclose(input);
    fclose(output);
    tui_colors_enabled = 0;
    tui_free_board(&tui);
    return captured ? 0 : 1;
}

/* Feed keys through the real top-level key handler starting on a given
 * screen, then report the state a render would use. Exercises per-view key
 * scoping without ncurses. */
static int tui_key_smoke(const char *screen_arg, const char *keys) {
    DispatchTui tui;
    tui_init(&tui);
    if (!parse_screen_name(screen_arg, &tui.screen)) {
        fprintf(stderr, "Unknown TUI screen %s\n", screen_arg);
        return 1;
    }
    if (!tui_load_board(&tui)) {
        fprintf(stderr, "%s\n", tui.status);
        return 1;
    }

    for (size_t i = 0; keys && keys[i] != '\0'; i++)
        tui_handle_key(&tui, (unsigned char)keys[i]);

    printf("Screen: %s\n", screen_name(tui.screen));
    printf("Filter: %s\n", filter_name(tui.filter));
    printf("Search active: %s\n", tui.search_active ? "yes" : "no");
    printf("Status: %s\n", tui.status);
    DispatchTask *task = selected_visible_task(&tui);
    printf("Selected task: %s\n", task ? task->id : "-");
    tui_free_board(&tui);
    return 0;
}

/* Print a task's wrapped description region the way the inspector renders
 * it: total wrapped rows, the clamped scroll window for a given width /
 * region height / top offset, and each visible line between brackets. Lets
 * tests verify the whole description is reachable end to end. */
static int tui_desc_wrap_smoke(const char *task_id, int width, int region,
                               int top) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui desc wrap smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(&board, task_id);
    if (!task) {
        fprintf(stderr, "No task with id %s\n", task_id);
        dispatch_board_free(&board);
        return 1;
    }

    const char *desc = task->description[0] ? task->description : "-";
    int total = description_rows(desc, width, -1, NULL, 0);
    if (region > total)
        region = total;
    int max_top = total - region;
    if (top > max_top)
        top = max_top;
    if (top < 0)
        top = 0;

    printf("Total: %d\n", total);
    printf("Region: %d\n", region);
    printf("Top: %d\n", top);
    char line[512];
    for (int r = 0; r < region; r++) {
        description_rows(desc, width, top + r, line, sizeof(line));
        printf("Line: [%s]\n", line);
    }
    dispatch_board_free(&board);
    return 0;
}

/* Run the TUI task-delete mutation headlessly, mirroring what the x/X
 * confirmation form executes after the typed id matches. */
static int tui_task_delete_smoke(const char *task_id, const char *mode) {
    int force = 0;
    if (strcmp(mode, "force") == 0) {
        force = 1;
    } else if (strcmp(mode, "no-force") != 0) {
        fprintf(stderr, "Delete mode must be force or no-force\n");
        return 1;
    }

    char message[256] = {0};
    int ok = mutate_task_delete(task_id, force, message, sizeof(message));
    printf("%s\n", message);
    return ok ? 0 : 1;
}

/* Print a clamped column cell between brackets so tests can assert exact
 * padding and truncation. */
static int tui_cell_smoke(int width, const char *text) {
    char cell[256];
    cell_text(cell, sizeof(cell), text, width);
    printf("Cell: [%s]\n", cell);
    return 0;
}

/* Print the agents-table row for one agent between brackets, plus the
 * derived column positions, so tests can assert that over-length values do
 * not shift later columns or the status overlay. */
static int tui_agent_row_smoke(const char *name) {
    DispatchBoard board;
    char error[256] = {0};
    if (!dispatch_store_init_file(DISPATCH_STORE_FILE, NULL, error,
                                  sizeof(error)) ||
        !dispatch_store_load(&board, DISPATCH_STORE_FILE, error,
                             sizeof(error))) {
        fprintf(stderr, "dispatch tui agent row smoke failed: %s\n",
                error[0] ? error : "could not load board");
        return 1;
    }

    DispatchAgent *agent = dispatch_board_find_agent(&board, name);
    if (!agent) {
        fprintf(stderr, "No agent named %s\n", name);
        dispatch_board_free(&board);
        return 1;
    }

    char row[1024];
    format_agent_row(row, sizeof(row), agent, 0);
    printf("Row: [%s]\n", row);
    printf("Status col: %d\n", (int)TUI_X_AGENT_STATUS);
    printf("Status text: %.*s\n", (int)TUI_COL_AGENT_STATUS,
           row + TUI_X_AGENT_STATUS);
    dispatch_board_free(&board);
    return 0;
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
    puts("  R ready selected task without a review gate");
    puts("  x/X delete selected task (X force-deletes past dependents)");
    puts("  tmux: no control-prefix bindings; run alongside tmux panes");
    puts("");
    puts("  --smoke   load the board and exit without initializing ncurses");
    puts("  --inspect-smoke <task-id>  print task inspector data and exit");
    puts("  --filter-smoke <filter>    print visible row count and exit");
    puts("  --action-smoke <action> <task-id> [actor]  run lifecycle action and exit");
    puts("  --task-edit-smoke <task-id> <title> <description|-> [actor]");
    puts("  --task-move-smoke <task-id> <group> [actor]");
    puts("  --task-move-options-smoke <current-group>");
    puts("  --diff-smoke <task-id>     print external diff command and exit");
    puts("  --diff-exec-smoke <task-id>  execute external diff and exit");
    puts("  --agents-smoke             print agent dashboard data and exit");
    puts("  --agent-inspect-smoke <name>  print agent inspector data and exit");
    puts("  --agent-session-smoke <name> <session|- > <task|- > <workspace|- >");
    puts("  --agent-set-session-smoke <name> <session|->");
    puts("  --prompt-edit-smoke <name>    print prompt editor command and exit");
    puts("  --prompt-edit-exec-smoke <name>  execute prompt editor and exit");
    puts("  --agent-archive-smoke <name> archive|restore");
    puts("  --agent-selection-smoke enabled|all <selected-index>");
    puts("  --agent-run-command-smoke <name>");
    puts("  --osc52-smoke <text>        print OSC 52 payload metadata and exit");
    puts("  --tmux-copy-smoke <text>    copy text through tmux stdin and exit");
    puts("  --create-group-smoke <name> <prefix|->");
    puts("  --create-task-smoke <group> <title> <description|-> review|no-review <deps|->");
    puts("  --task-form-submit-smoke <group> <title> <description|-> review|no-review <deps|->");
    puts("  --task-form-options-smoke groups|deps [deps|-]");
    puts("  --task-form-deps-add-smoke <deps|-> <task-id>");
    puts("  --agent-form-keys-smoke <keys>  exercise agent form key handling");
    puts("  --agent-form-submit-smoke <name> codex|claude <model|->");
    puts("  --prompt-cancel-smoke");
    puts("  --escdelay-smoke");
    puts("  --dependency-smoke add|remove <dependency-id> <dependent-id>");
    puts("  --workspaces-smoke          print workspace dashboard data and exit");
    puts("  --workspace-inspect-smoke <task-id-or-workspace>");
    puts("  --logs-smoke [actor|command|action|task|agent|workspace <value>]");
    puts("  --logs-window-smoke <visible-rows> <selected-index> [field value]");
    puts("  --scroll-smoke board|agents|workspaces|logs <visible-rows> <selected-index>");
    puts("  --selection-smoke <filter> <selected-index>  verify highlight and action task match");
    puts("  --key-smoke <screen> <keys>  feed keys to the input handler on a screen");
    puts("  --render-smoke <screen> <cols> <rows> [keys]  capture a headless frame");
    puts("  --cell-smoke <width> <text>  print a clamped table cell");
    puts("  --task-delete-smoke <task-id> force|no-force  run the delete mutation and exit");
    puts("  --agent-row-smoke <name>     print the agents-table row for one agent");
    puts("  --desc-wrap-smoke <task-id> <width> <region> <top>  print the wrapped description window");
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
    if ((argc == 6 || argc == 7) &&
        strcmp(argv[2], "--task-edit-smoke") == 0)
        return tui_task_edit_smoke(argv[3], argv[4], argv[5],
                                   argc == 7 ? argv[6] : "user");
    if ((argc == 5 || argc == 6) &&
        strcmp(argv[2], "--task-move-smoke") == 0)
        return tui_task_move_smoke(argv[3], argv[4],
                                   argc == 6 ? argv[5] : "user");
    if (argc == 4 && strcmp(argv[2], "--task-move-options-smoke") == 0)
        return tui_task_move_options_smoke(argv[3]);
    if (argc == 4 && strcmp(argv[2], "--diff-smoke") == 0)
        return tui_diff_smoke(argv[3]);
    if (argc == 4 && strcmp(argv[2], "--diff-exec-smoke") == 0)
        return tui_diff_exec_smoke(argv[3]);
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
    if (argc == 4 && strcmp(argv[2], "--prompt-edit-exec-smoke") == 0)
        return tui_prompt_edit_exec_smoke(argv[3]);
    if (argc == 5 && strcmp(argv[2], "--agent-archive-smoke") == 0)
        return tui_agent_archive_smoke(argv[3], argv[4]);
    if (argc == 5 && strcmp(argv[2], "--agent-selection-smoke") == 0)
        return tui_agent_selection_smoke(argv[3], atoi(argv[4]));
    if (argc == 4 && strcmp(argv[2], "--agent-run-command-smoke") == 0)
        return tui_agent_run_command_smoke(argv[3]);
    if (argc == 4 && strcmp(argv[2], "--osc52-smoke") == 0)
        return tui_osc52_smoke(argv[3]);
    if (argc == 4 && strcmp(argv[2], "--tmux-copy-smoke") == 0)
        return tui_tmux_copy_smoke(argv[3]);
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
    if (argc == 6 && strcmp(argv[2], "--agent-form-submit-smoke") == 0)
        return tui_agent_form_submit_smoke(argv[3], argv[4], argv[5]);
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
    if (argc == 5 && strcmp(argv[2], "--selection-smoke") == 0)
        return tui_selection_smoke(argv[3], atoi(argv[4]));
    if (argc == 5 && strcmp(argv[2], "--key-smoke") == 0)
        return tui_key_smoke(argv[3], argv[4]);
    if ((argc == 6 || argc == 7) && strcmp(argv[2], "--render-smoke") == 0)
        return tui_render_smoke(argv[3], atoi(argv[4]), atoi(argv[5]),
                                argc == 7 ? argv[6] : "");
    if (argc == 5 && strcmp(argv[2], "--cell-smoke") == 0)
        return tui_cell_smoke(atoi(argv[3]), argv[4]);
    if (argc == 5 && strcmp(argv[2], "--task-delete-smoke") == 0)
        return tui_task_delete_smoke(argv[3], argv[4]);
    if (argc == 4 && strcmp(argv[2], "--agent-row-smoke") == 0)
        return tui_agent_row_smoke(argv[3]);
    if (argc == 7 && strcmp(argv[2], "--desc-wrap-smoke") == 0)
        return tui_desc_wrap_smoke(argv[3], atoi(argv[4]), atoi(argv[5]),
                                   atoi(argv[6]));
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
