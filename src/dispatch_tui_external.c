/* External command integration: clipboard/OSC52, diffs, and editors. */

#include "dispatch_tui_internal.h"

static char *tui_shell_quote(const char *value);
static const char *diff_pager(void);
static int command_available(const char *command);
static const char *fallback_editor(void);
static const char *configured_editor(void);
static int send_command_to_osc52_clipboard(const char *command);

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

char *tui_trimmed_copy(const char *value) {
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

char *tui_base64_encode(const unsigned char *data, size_t len) {
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

char *osc52_sequence_for_text(const char *text) {
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

int diff_argv_for_task(const DispatchBoard *board,
                              const DispatchTask *task, int force_color,
                              DispatchExecArgv *argv) {
    if (!task || task->commits.count == 0)
        return 0;
    if (!dispatch_exec_argv_append(argv, "git") ||
        !dispatch_exec_argv_append(argv, "-C") ||
        !dispatch_exec_argv_append(
            argv, board->repo_path ? board->repo_path : ".") ||
        !dispatch_exec_argv_append(argv, "show") ||
        (force_color &&
         !dispatch_exec_argv_append(argv, "--color=always"))) {
        dispatch_exec_argv_free(argv);
        return 0;
    }
    for (size_t i = 0; i < task->commits.count; i++) {
        if (!dispatch_exec_argv_append(argv, task->commits.items[i])) {
            dispatch_exec_argv_free(argv);
            return 0;
        }
    }
    return 1;
}

int path_has_git_metadata(const char *path) {
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

int workspace_is_dirty(const DispatchWorkspace *workspace) {
    if (!workspace || !workspace->path || !path_has_git_metadata(workspace->path))
        return 0;

    const char *argv[] = {"git", "-C", workspace->path, "status",
                          "--porcelain", NULL};
    DispatchExecOptions options = {.stderr_mode = DISPATCH_EXEC_DEV_NULL};
    DispatchExecResult result;
    char *output = NULL;
    size_t output_size = 0;
    if (!dispatch_exec_capture(argv, &options, &output, &output_size, &result))
        return 1;
    int dirty = output_size > 0 || !dispatch_exec_result_success(&result);
    free(output);
    return dirty;
}

static int command_available(const char *command);

/* Pick a pager that keeps the diff on screen until dismissed. Honor $PAGER when
 * set, otherwise prefer less (raw color) then more. NULL when none is found. */
static const char *diff_pager(void) {
    const char *env = getenv("PAGER");
    if (env && env[0])
        return env;
    if (command_available("less"))
        return "less -R";
    if (command_available("more"))
        return "more";
    return NULL;
}

void run_selected_task_diff(DispatchTui *tui) {
    DispatchTask *task = selected_visible_task(tui);
    if (!task) {
        tui_set_status(tui, "No selected task");
        return;
    }
    if (!command_available("git")) {
        tui_set_status(tui, "git is not available to show the diff");
        return;
    }
    const char *pager = diff_pager();
    DispatchExecArgv diff_argv = {0};
    if (!diff_argv_for_task(&tui->board, task, pager != NULL, &diff_argv)) {
        tui_set_status(tui, "No commit metadata for selected task");
        return;
    }

    DispatchExecArgv pager_argv = {0};
    if (pager && !dispatch_exec_argv_parse(&pager_argv, pager)) {
        dispatch_exec_argv_free(&diff_argv);
        tui_set_status(tui, "Could not parse the pager command");
        return;
    }

    def_prog_mode();
    endwin();
    DispatchExecResult result;
    int launched;
    if (pager) {
        DispatchExecOptions git_options = {.merge_stderr_to_stdout = 1};
        launched = dispatch_exec_pipeline(
            (const char *const *)diff_argv.items, &git_options,
            (const char *const *)pager_argv.items, NULL, &result);
    } else {
        launched = dispatch_exec_run((const char *const *)diff_argv.items, NULL,
                                     &result);
    }
    reset_prog_mode();
    refresh();

    char message[256];
    int status = launched ? dispatch_exec_result_status(&result) : -1;
    if (!launched)
        snprintf(message, sizeof(message), "Could not run the diff command");
    else if (status != 0)
        snprintf(message, sizeof(message),
                 "Diff viewer exited with status %d", status);
    else if (!pager)
        snprintf(message, sizeof(message),
                 "Showed diff without a pager; set $PAGER to keep it on screen");
    else
        snprintf(message, sizeof(message), "Closed diff");
    tui_set_status(tui, message);
    dispatch_exec_argv_free(&pager_argv);
    dispatch_exec_argv_free(&diff_argv);
}

static int command_available(const char *command) {
    return dispatch_exec_command_available(command);
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

int editor_argv_for_path(const char *path, DispatchExecArgv *argv,
                                char *error, size_t error_size) {
    const char *editor = configured_editor();
    if (!editor || !editor[0]) {
        snprintf(error, error_size,
                 "No editor configured; set VISUAL or EDITOR to edit %s",
                 path ? path : "the prompt");
        return 0;
    }
    if (!dispatch_exec_argv_parse(argv, editor)) {
        snprintf(error, error_size, "Could not parse editor command");
        return 0;
    }
    if (!dispatch_exec_argv_append(argv, path ? path : "")) {
        dispatch_exec_argv_free(argv);
        snprintf(error, error_size, "Out of memory");
        return 0;
    }
    return 1;
}

void edit_selected_agent_prompt(DispatchTui *tui) {
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
    DispatchExecArgv editor_argv = {0};
    if (!editor_argv_for_path(agent->prompt_path, &editor_argv, error,
                              sizeof(error))) {
        tui_set_status(tui, error);
        return;
    }
    def_prog_mode();
    endwin();
    DispatchExecResult result;
    int launched = dispatch_exec_run((const char *const *)editor_argv.items,
                                     NULL, &result);
    reset_prog_mode();
    refresh();

    char message[256];
    if (launched) {
        snprintf(message, sizeof(message), "Editor exited with status %d",
                 dispatch_exec_result_status(&result));
    } else {
        snprintf(message, sizeof(message), "Could not run editor");
    }
    tui_set_status(tui, message);
    dispatch_exec_argv_free(&editor_argv);
    tui_load_board(tui);
}

char *agent_run_command_text(DispatchBoard *board,
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

int copy_command_to_tmux_buffer(const char *command) {
    if (!getenv("TMUX") || !command_available("tmux"))
        return 0;

    const char *argv[] = {"tmux", "load-buffer", "-", NULL};
    DispatchExecResult result;
    return dispatch_exec_feed(argv, NULL, command, strlen(command), &result) &&
           dispatch_exec_result_success(&result);
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

void copy_selected_agent_run_command(DispatchTui *tui) {
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
