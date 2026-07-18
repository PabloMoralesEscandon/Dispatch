#include "dispatch_cli_internal.h"

int dispatch_agent_name_is_valid(const char *name) {
    if (!name || name[0] == '\0')
        return 0;
    if (strlen(name) > DISPATCH_AGENT_NAME_MAX)
        return 0;
    for (size_t i = 0; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

int dispatch_agent_runner_is_valid(const char *runner) {
    return runner &&
           (strcmp(runner, "codex") == 0 || strcmp(runner, "claude") == 0);
}

char *agent_command_for(const char *runner, const char *model,
                               const char *prompt_path) {
    const char *model_flag = model && model[0] ? " --model " : "";
    const char *model_value = model && model[0] ? model : "";
    const char *format = NULL;
    if (strcmp(runner, "codex") == 0) {
        format = "codex%s%s \"$(cat '%s')\"";
    } else {
        format = "claude%s%s \"$(cat '%s')\"";
    }

    size_t size = strlen(format) + strlen(model_flag) + strlen(model_value) +
                  strlen(prompt_path) + 1;
    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(command, size, format, model_flag, model_value, prompt_path);
    return command;
}

char *codex_agent_resume_command_for(const DispatchAgent *agent,
                                     const DispatchWorkspace *workspace) {
    char *model_q = agent->model && agent->model[0] ? shell_quote(agent->model) : NULL;
    char *workspace_q = workspace && workspace->path && workspace->path[0]
                            ? shell_quote(workspace->path)
                            : NULL;
    char *session_q = agent->session_id && agent->session_id[0]
                          ? shell_quote(agent->session_id)
                          : NULL;

    size_t size = strlen("codex resume") + 1;
    if (model_q)
        size += strlen(" --model ") + strlen(model_q);
    if (workspace_q)
        size += strlen(" --cd ") + strlen(workspace_q);
    size += session_q ? strlen(" ") + strlen(session_q) : strlen(" --last");

    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    size_t out = 0;
    out += snprintf(command + out, size - out, "codex resume");
    if (model_q)
        out += snprintf(command + out, size - out, " --model %s", model_q);
    if (workspace_q)
        out += snprintf(command + out, size - out, " --cd %s", workspace_q);
    if (session_q)
        snprintf(command + out, size - out, " %s", session_q);
    else
        snprintf(command + out, size - out, " --last");

    free(model_q);
    free(workspace_q);
    free(session_q);
    return command;
}

static char *generate_uuid_v4(void) {
    unsigned char bytes[16];
    FILE *random = fopen("/dev/urandom", "rb");
    if (!random)
        return NULL;

    size_t count = fread(bytes, 1, sizeof(bytes), random);
    fclose(random);
    if (count != sizeof(bytes))
        return NULL;

    bytes[6] = (unsigned char)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (unsigned char)((bytes[8] & 0x3f) | 0x80);

    char *uuid = malloc(37);
    if (!uuid) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(uuid, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             (unsigned int)bytes[0], (unsigned int)bytes[1],
             (unsigned int)bytes[2], (unsigned int)bytes[3],
             (unsigned int)bytes[4], (unsigned int)bytes[5],
             (unsigned int)bytes[6], (unsigned int)bytes[7],
             (unsigned int)bytes[8], (unsigned int)bytes[9],
             (unsigned int)bytes[10], (unsigned int)bytes[11],
             (unsigned int)bytes[12], (unsigned int)bytes[13],
             (unsigned int)bytes[14], (unsigned int)bytes[15]);
    return uuid;
}

char *claude_agent_resume_command_for(const DispatchAgent *agent,
                                      const DispatchWorkspace *workspace,
                                      int start_with_session_id) {
    char *model_q = agent->model && agent->model[0] ? shell_quote(agent->model) : NULL;
    char *workspace_q = workspace && workspace->path && workspace->path[0]
                            ? shell_quote(workspace->path)
                            : NULL;
    char *session_q = shell_quote(agent->session_id);
    char *prompt_q = start_with_session_id ? shell_quote(agent->prompt_path) : NULL;

    size_t size = strlen("claude") + 1;
    if (workspace_q)
        size += strlen("cd ") + strlen(workspace_q) + strlen(" && ");
    if (model_q)
        size += strlen(" --model ") + strlen(model_q);
    size += strlen(start_with_session_id ? " --session-id " : " --resume ") +
            strlen(session_q);
    if (prompt_q)
        size += strlen(" \"$(cat )\"") + strlen(prompt_q);

    char *command = malloc(size);
    if (!command) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    size_t out = 0;
    if (workspace_q)
        out += snprintf(command + out, size - out, "cd %s && ", workspace_q);
    out += snprintf(command + out, size - out, "claude");
    if (model_q)
        out += snprintf(command + out, size - out, " --model %s", model_q);
    out += snprintf(command + out, size - out, "%s %s",
                    start_with_session_id ? " --session-id" : " --resume",
                    session_q);
    if (prompt_q)
        snprintf(command + out, size - out, " \"$(cat %s)\"", prompt_q);

    free(model_q);
    free(workspace_q);
    free(session_q);
    free(prompt_q);
    return command;
}

int write_agent_prompt(const char *path, const char *name,
                              const char *runner, const char *model,
                              const char *agent_dir, const char *scratch_dir,
                              const char *decisions_dir) {
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Could not write %s: %s\n", path, strerror(errno));
        return 0;
    }

    fprintf(file, "# Dispatch Agent: %s\n\n", name);
    fprintf(file, "This file identifies the Dispatch agent instance. General repository and workflow instructions come from `AGENTS.md` in the workflow directory.\n\n");
    fprintf(file, "## Agent ID\n\n");
    fprintf(file, "- Agent name: `%s`\n", name);
    fprintf(file, "- Runner: `%s`\n", runner);
    fprintf(file, "- Model: `%s`\n", model && model[0] ? model : "runner default");
    fprintf(file, "- Agent directory: `%s`\n", agent_dir);
    fprintf(file, "- Prompt file: `%s`\n", path);
    fprintf(file, "- Scratch directory: `%s`\n", scratch_dir);
    fprintf(file, "- Decisions directory: `%s`\n\n", decisions_dir);

    fprintf(file, "## Actor Usage\n\n");
    fprintf(file, "Always identify as `%s` when a Dispatch command needs the agent actor.\n\n", name);
    fprintf(file, "Use this actor value for task lifecycle commands:\n\n");
    fprintf(file, "```bash\n");
    fprintf(file, "dispatch start <TASK-ID> --actor %s\n", name);
    fprintf(file, "dispatch finish <TASK-ID> --actor %s\n", name);
    fprintf(file, "dispatch review <TASK-ID> --actor %s\n", name);
    fprintf(file, "```\n\n");
    fprintf(file, "Use this actor value for owned workspaces:\n\n");
    fprintf(file, "```bash\n");
    fprintf(file, "dispatch workspace create <TASK-ID> --actor %s\n", name);
    fprintf(file, "```\n\n");
    fprintf(file, "Record this agent's current runner metadata with this agent name:\n\n");
    fprintf(file, "```bash\n");
    fprintf(file, "dispatch agent session %s --session-id <SESSION-ID>\n", name);
    fprintf(file, "dispatch agent session %s --current-task <TASK-ID> --last-workspace <TASK-ID>\n", name);
    fprintf(file, "dispatch agent show %s\n", name);
    fprintf(file, "dispatch agent resume %s\n", name);
    fprintf(file, "```\n\n");
    fprintf(file, "Do not use `user` or another agent name as the actor for this agent's own work unless the user explicitly instructs you to do so.\n");
    fclose(file);
    return 1;
}

int write_agent_run_script(const char *path, const char *command) {
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Could not write %s: %s\n", path, strerror(errno));
        return 0;
    }

    fprintf(file, "#!/usr/bin/env bash\n");
    fprintf(file, "set -euo pipefail\n");
    fprintf(file, "cd \"$(dirname \"$0\")/../../..\"\n");
    fprintf(file, "exec %s\n", command);
    fclose(file);
    if (chmod(path, 0700) != 0) {
        fprintf(stderr, "Could not mark %s executable: %s\n", path,
                strerror(errno));
        return 0;
    }
    return 1;
}

static DispatchAgent *append_agent_record(DispatchBoard *board,
                                          const char *name,
                                          const char *runner,
                                          const char *model,
                                          const char *agent_dir,
                                          const char *prompt_path,
                                          const char *run_script_path) {
    if (dispatch_board_find_agent(board, name))
        return NULL;
    if (board->agents.count >= board->agents.capacity) {
        board->agents.capacity =
            board->agents.capacity == 0 ? 4 : board->agents.capacity * 2;
        board->agents.items = cli_realloc_array(
            board->agents.items, board->agents.capacity,
            sizeof(*board->agents.items));
    }

    DispatchAgent *agent = &board->agents.items[board->agents.count++];
    memset(agent, 0, sizeof(*agent));
    agent->name = cli_strdup(name);
    agent->runner = cli_strdup(runner);
    agent->model = model && model[0] ? cli_strdup(model) : NULL;
    agent->agent_dir = cli_strdup(agent_dir);
    agent->prompt_path = cli_strdup(prompt_path);
    agent->run_script_path =
        run_script_path && run_script_path[0] ? cli_strdup(run_script_path)
                                              : NULL;
    agent->created_at = time(NULL);
    agent->updated_at = agent->created_at;
    return agent;
}

void dispatch_agent_create_result_free(DispatchAgentCreateResult *result) {
    if (!result)
        return;
    free(result->agent_dir);
    free(result->prompt_path);
    free(result->scratch_dir);
    free(result->decisions_dir);
    free(result->run_script_path);
    free(result->command);
    memset(result, 0, sizeof(*result));
}

int dispatch_agent_create(const DispatchAgentCreateOptions *options,
                          DispatchAgentCreateResult *result, char *error,
                          size_t error_size) {
    if (result)
        memset(result, 0, sizeof(*result));
    if (error && error_size > 0)
        error[0] = '\0';

    const char *name = options ? options->name : NULL;
    const char *runner = options ? options->runner : NULL;
    const char *model = options ? options->model : NULL;
    const char *actor =
        options && options->actor && options->actor[0] ? options->actor : "user";
    int no_run_script = options ? options->no_run_script : 0;

    if (!dispatch_agent_name_is_valid(name)) {
        if (error && error_size > 0) {
            snprintf(error, error_size,
                     "Agent name must be 1-%d characters of letters, digits, "
                     "'-' or '_'",
                     DISPATCH_AGENT_NAME_MAX);
        }
        return 0;
    }
    if (!dispatch_agent_runner_is_valid(runner)) {
        if (error && error_size > 0)
            snprintf(error, error_size, "Agent runner must be codex or claude");
        return 0;
    }

    char *agent_dir_base = join_path2(".dispatch/agents", name);
    char *prompt_path = agent_prompt_path_for(agent_dir_base, name);
    char *run_script_path =
        no_run_script ? NULL : join_path2(agent_dir_base, "run.sh");
    char *scratch_dir = NULL;
    char *decisions_dir = NULL;
    char *command = agent_command_for(runner, model, prompt_path);

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked)) {
        free(agent_dir_base);
        free(prompt_path);
        free(run_script_path);
        free(command);
        return 0;
    }
    if (dispatch_board_find_agent(&locked.board, name)) {
        locked_board_close(&locked);
        if (error && error_size > 0)
            snprintf(error, error_size, "Agent %s already exists", name);
        free(agent_dir_base);
        free(prompt_path);
        free(run_script_path);
        free(command);
        return 0;
    }

    if (!create_agent_dirs(agent_dir_base, &scratch_dir, &decisions_dir) ||
        !write_agent_prompt(prompt_path, name, runner, model, agent_dir_base,
                            scratch_dir, decisions_dir) ||
        (!no_run_script && !write_agent_run_script(run_script_path, command)) ||
        !append_agent_record(&locked.board, name, runner, model, agent_dir_base,
                             prompt_path, run_script_path) ||
        !locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        if (error && error_size > 0 && error[0] == '\0')
            snprintf(error, error_size, "Could not create agent %s", name);
        free(agent_dir_base);
        free(prompt_path);
        free(run_script_path);
        free(scratch_dir);
        free(decisions_dir);
        free(command);
        return 0;
    }

    DispatchLogField targets[] = {
        {"agent", name},
    };
    DispatchLogField context[] = {
        {"runner", runner},
        {"model", model && model[0] ? model : ""},
        {"agent_dir", agent_dir_base},
        {"run_script", bool_string(!no_run_script)},
    };
    char message[256];
    snprintf(message, sizeof(message), "Created agent %s", name);
    append_dispatch_log(actor, "agent", "create", targets, 1, context, 4,
                        message);

    locked_board_close(&locked);
    if (result) {
        result->agent_dir = agent_dir_base;
        result->prompt_path = prompt_path;
        result->scratch_dir = scratch_dir;
        result->decisions_dir = decisions_dir;
        result->run_script_path = run_script_path;
        result->command = command;
    } else {
        free(agent_dir_base);
        free(prompt_path);
        free(run_script_path);
        free(scratch_dir);
        free(decisions_dir);
        free(command);
    }
    return 1;
}

static int cmd_agent_create(int argc, char **argv) {
    const char *name = NULL;
    const char *runner = NULL;
    const char *model = NULL;
    int no_run_script = 0;
    int print_command = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && (i + 1) < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--runner") == 0 && (i + 1) < argc) {
            runner = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && (i + 1) < argc) {
            model = argv[++i];
        } else if (strcmp(argv[i], "--no-run-script") == 0) {
            no_run_script = 1;
        } else if (strcmp(argv[i], "--print-command") == 0) {
            print_command = 1;
        } else {
            fprintf(stderr,
                    "Usage: dispatch agent create --name <name> --runner codex|claude [--model <name>] [--no-run-script] [--print-command]\n");
            return 1;
        }
    }

    DispatchAgentCreateOptions options = {
        .name = name,
        .runner = runner,
        .model = model,
        .actor = "user",
        .no_run_script = no_run_script,
    };
    DispatchAgentCreateResult result = {0};
    char error[256] = {0};
    if (!dispatch_agent_create(&options, &result, error, sizeof(error))) {
        if (error[0] != '\0')
            fprintf(stderr, "%s\n", error);
        return 1;
    }

    printf("Created agent %s (%s)\n", name, runner);
    printf("  prompt: %s\n", result.prompt_path);
    printf("  scratch: %s\n", result.scratch_dir);
    printf("  decisions: %s\n", result.decisions_dir);
    if (result.run_script_path)
        printf("  run script: %s\n", result.run_script_path);
    if (print_command)
        printf("  command: %s\n", result.command);

    dispatch_agent_create_result_free(&result);
    return 0;
}

static int cmd_agent_list(int argc, char **argv) {
    (void)argv;
    int include_archived = 0;
    if (argc == 4 && strcmp(argv[3], "--all") == 0)
        include_archived = 1;
    else if (argc != 3) {
        fprintf(stderr, "Usage: dispatch agent list [--all]\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (board.agents.count == 0) {
        printf("(no agents)\n");
        dispatch_board_free(&board);
        return 0;
    }

    int printed = 0;
    for (size_t i = 0; i < board.agents.count; i++) {
        DispatchAgent *agent = &board.agents.items[i];
        if (agent->archived && !include_archived)
            continue;
        printf("%-16s %-8s %-8s %s\n", agent->name, agent->runner,
               agent->archived ? "archived" : "enabled", agent->agent_dir);
        printed = 1;
    }
    if (!printed)
        printf("(no enabled agents)\n");

    dispatch_board_free(&board);
    return 0;
}

static int cmd_agent_show(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: dispatch agent show <name>\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchAgent *agent = dispatch_board_find_agent(&board, argv[3]);
    if (!agent) {
        dispatch_board_free(&board);
        fprintf(stderr, "No agent named %s\n", argv[3]);
        return 1;
    }

    printf("Name: %s\n", agent->name);
    printf("Runner: %s\n", agent->runner);
    printf("Status: %s\n", agent->archived ? "archived" : "enabled");
    printf("Model: %s\n", agent->model ? agent->model : "-");
    printf("Agent dir: %s\n", agent->agent_dir);
    printf("Prompt: %s\n", agent->prompt_path);
    printf("Run script: %s\n",
           agent->run_script_path ? agent->run_script_path : "-");
    printf("Session ID: %s\n", agent->session_id ? agent->session_id : "-");
    printf("Current task: %s\n",
           agent->current_task ? agent->current_task : "-");
    printf("Last workspace: %s\n",
           agent->last_workspace ? agent->last_workspace : "-");
    printf("Updated at: %ld\n", (long)agent->updated_at);

    dispatch_board_free(&board);
    return 0;
}

static int agent_has_active_task(const DispatchBoard *board,
                                 const DispatchAgent *agent,
                                 const char **task_id) {
    if (agent->current_task && agent->current_task[0]) {
        if (task_id)
            *task_id = agent->current_task;
        return 1;
    }

    for (size_t i = 0; i < board->tasks.count; i++) {
        const DispatchTask *task = &board->tasks.items[i];
        if (!task->assigned_to || strcmp(task->assigned_to, agent->name) != 0)
            continue;
        DispatchState state = dispatch_task_effective_state(board, task);
        if (state == DISPATCH_STATE_DOING || state == DISPATCH_STATE_REVIEW) {
            if (task_id)
                *task_id = task->id;
            return 1;
        }
    }
    return 0;
}

static int agent_has_active_workspace(const DispatchBoard *board,
                                      const DispatchAgent *agent,
                                      const char **workspace_id) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        const DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_REMOVED ||
            strcmp(workspace->actor, agent->name) != 0)
            continue;
        if (workspace_id)
            *workspace_id = workspace->id;
        return 1;
    }
    return 0;
}

static int cmd_agent_archive_restore(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: dispatch agent %s <name>\n", argv[2]);
        return 1;
    }

    int archive = strcmp(argv[2], "archive") == 0;
    const char *name = argv[3];

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    DispatchAgent *agent = dispatch_board_find_agent(&locked.board, name);
    if (!agent) {
        locked_board_close(&locked);
        fprintf(stderr, "No agent named %s\n", name);
        return 1;
    }

    if (archive) {
        const char *task_id = NULL;
        const char *workspace_id = NULL;
        if (agent_has_active_task(&locked.board, agent, &task_id)) {
            fprintf(stderr, "Agent %s has active task %s\n", name, task_id);
            locked_board_close(&locked);
            return 1;
        }
        if (agent_has_active_workspace(&locked.board, agent, &workspace_id)) {
            fprintf(stderr, "Agent %s has active workspace %s\n", name,
                    workspace_id);
            locked_board_close(&locked);
            return 1;
        }
    }

    agent->archived = archive;
    agent->updated_at = time(NULL);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    printf("%s agent %s\n", archive ? "Archived" : "Restored", name);
    DispatchLogField targets[] = {
        {"agent", name},
    };
    DispatchLogField context[] = {
        {"archived", bool_string(agent->archived)},
    };
    char message[256];
    snprintf(message, sizeof(message), "%s agent %s",
             archive ? "Archived" : "Restored", name);
    append_dispatch_log("user", "agent", archive ? "archive" : "restore",
                        targets, 1, context, 1, message);
    locked_board_close(&locked);
    return 0;
}

static void print_agent_session_usage(void) {
    fprintf(stderr,
            "Usage: dispatch agent session <name> [--session-id <id>|--clear-session] [--current-task <id>|--clear-current-task] [--last-workspace <id>|--clear-last-workspace]\n");
}

static int cmd_agent_session(int argc, char **argv) {
    if (argc < 4) {
        print_agent_session_usage();
        return 1;
    }

    const char *name = argv[3];
    const char *session_id = NULL;
    const char *current_task = NULL;
    const char *last_workspace = NULL;
    int clear_session = 0;
    int clear_current_task = 0;
    int clear_last_workspace = 0;
    int updates = 0;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--session-id") == 0 && (i + 1) < argc) {
            session_id = argv[++i];
            updates++;
        } else if (strcmp(argv[i], "--clear-session") == 0) {
            clear_session = 1;
            updates++;
        } else if (strcmp(argv[i], "--current-task") == 0 && (i + 1) < argc) {
            current_task = argv[++i];
            updates++;
        } else if (strcmp(argv[i], "--clear-current-task") == 0) {
            clear_current_task = 1;
            updates++;
        } else if (strcmp(argv[i], "--last-workspace") == 0 &&
                   (i + 1) < argc) {
            last_workspace = argv[++i];
            updates++;
        } else if (strcmp(argv[i], "--clear-last-workspace") == 0) {
            clear_last_workspace = 1;
            updates++;
        } else {
            print_agent_session_usage();
            return 1;
        }
    }

    if (updates == 0) {
        print_agent_session_usage();
        return 1;
    }
    if ((session_id && clear_session) ||
        (current_task && clear_current_task) ||
        (last_workspace && clear_last_workspace)) {
        fprintf(stderr, "Session metadata options conflict\n");
        return 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    DispatchAgent *agent = dispatch_board_find_agent(&locked.board, name);
    if (!agent) {
        locked_board_close(&locked);
        fprintf(stderr, "No agent named %s\n", name);
        return 1;
    }
    if (agent->archived) {
        locked_board_close(&locked);
        fprintf(stderr, "Agent %s is archived; restore it first\n", name);
        return 1;
    }
    if (current_task && !dispatch_board_find_task(&locked.board, current_task)) {
        locked_board_close(&locked);
        fprintf(stderr, "No task with id %s\n", current_task);
        return 1;
    }
    if (last_workspace &&
        !dispatch_board_find_workspace(&locked.board, last_workspace)) {
        locked_board_close(&locked);
        fprintf(stderr, "No workspace for %s\n", last_workspace);
        return 1;
    }

    char *trimmed_session_id = session_id ? trimmed_copy(session_id) : NULL;
    if (session_id || clear_session)
        replace_optional_string(&agent->session_id, trimmed_session_id);
    if (current_task || clear_current_task)
        replace_optional_string(&agent->current_task, current_task);
    if (last_workspace || clear_last_workspace)
        replace_optional_string(&agent->last_workspace, last_workspace);
    agent->updated_at = time(NULL);

    if (!locked_board_save_or_error(&locked)) {
        free(trimmed_session_id);
        locked_board_close(&locked);
        return 1;
    }

    printf("Updated agent session %s\n", name);
    DispatchLogField targets[] = {
        {"agent", name},
    };
    DispatchLogField context[] = {
        {"session_id", agent->session_id ? agent->session_id : ""},
        {"current_task", agent->current_task ? agent->current_task : ""},
        {"last_workspace", agent->last_workspace ? agent->last_workspace : ""},
    };
    char message[256];
    snprintf(message, sizeof(message), "Updated agent session %s", name);
    append_dispatch_log("user", "agent", "session", targets, 1, context, 3,
                        message);
    free(trimmed_session_id);
    locked_board_close(&locked);
    return 0;
}

static int cmd_agent_command(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "Usage: dispatch agent command <name> [--print-command]\n");
        return 1;
    }
    if (argc == 5 && strcmp(argv[4], "--print-command") != 0) {
        fprintf(stderr, "Unknown agent command option: %s\n", argv[4]);
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchAgent *agent = dispatch_board_find_agent(&board, argv[3]);
    if (!agent) {
        dispatch_board_free(&board);
        fprintf(stderr, "No agent named %s\n", argv[3]);
        return 1;
    }
    if (agent->archived) {
        dispatch_board_free(&board);
        fprintf(stderr, "Agent %s is archived; restore it first\n", argv[3]);
        return 1;
    }

    char *command =
        agent_command_for(agent->runner, agent->model, agent->prompt_path);
    printf("%s\n", command);
    free(command);

    dispatch_board_free(&board);
    return 0;
}

static int cmd_agent_resume(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "Usage: dispatch agent resume <name> [--print-command]\n");
        return 1;
    }
    if (argc == 5 && strcmp(argv[4], "--print-command") != 0) {
        fprintf(stderr, "Unknown agent resume option: %s\n", argv[4]);
        return 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    DispatchAgent *agent = dispatch_board_find_agent(&locked.board, argv[3]);
    if (!agent) {
        locked_board_close(&locked);
        fprintf(stderr, "No agent named %s\n", argv[3]);
        return 1;
    }
    if (agent->archived) {
        locked_board_close(&locked);
        fprintf(stderr, "Agent %s is archived; restore it first\n", argv[3]);
        return 1;
    }

    DispatchWorkspace *workspace = NULL;
    if (agent->last_workspace) {
        workspace =
            dispatch_board_find_workspace(&locked.board, agent->last_workspace);
        if (!workspace || workspace->state == DISPATCH_WORKSPACE_REMOVED) {
            locked_board_close(&locked);
            fprintf(stderr, "No active workspace for %s\n",
                    agent->last_workspace);
            return 1;
        }
    }

    char *command = NULL;
    char *resume_script_path = NULL;
    int generated_session = 0;
    if (strcmp(agent->runner, "codex") == 0) {
        command = codex_agent_resume_command_for(agent, workspace);
    } else if (strcmp(agent->runner, "claude") == 0) {
        if (!agent->session_id || !agent->session_id[0]) {
            char *uuid = generate_uuid_v4();
            if (!uuid) {
                locked_board_close(&locked);
                fprintf(stderr, "Could not generate Claude session UUID\n");
                return 1;
            }
            replace_optional_string(&agent->session_id, uuid);
            free(uuid);
            agent->updated_at = time(NULL);
            generated_session = 1;
            if (!locked_board_save_or_error(&locked)) {
                locked_board_close(&locked);
                return 1;
            }
        }

        command =
            claude_agent_resume_command_for(agent, workspace, generated_session);
        resume_script_path = join_path2(agent->agent_dir, "resume.sh");
        if (!write_agent_run_script(resume_script_path, command)) {
            locked_board_close(&locked);
            free(command);
            free(resume_script_path);
            return 1;
        }
    } else {
        locked_board_close(&locked);
        fprintf(stderr, "Agent resume is not implemented for runner %s\n",
                agent->runner);
        return 1;
    }

    printf("%s\n", command);
    if (resume_script_path)
        printf("resume script: %s\n", resume_script_path);

    if (generated_session) {
        DispatchLogField targets[] = {
            {"agent", agent->name},
        };
        DispatchLogField context[] = {
            {"session_id", agent->session_id},
            {"runner", agent->runner},
        };
        char message[256];
        snprintf(message, sizeof(message), "Generated Claude session %s",
                 agent->name);
        append_dispatch_log("user", "agent", "resume", targets, 1, context, 2,
                            message);
    }

    free(command);
    free(resume_script_path);

    locked_board_close(&locked);
    return 0;
}

int cmd_agent(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "create") == 0)
        return cmd_agent_create(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "list") == 0)
        return cmd_agent_list(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "show") == 0)
        return cmd_agent_show(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "command") == 0)
        return cmd_agent_command(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "session") == 0)
        return cmd_agent_session(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "resume") == 0)
        return cmd_agent_resume(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "archive") == 0)
        return cmd_agent_archive_restore(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "restore") == 0)
        return cmd_agent_archive_restore(argc, argv);

    fprintf(stderr,
            "Usage: dispatch agent create --name <name> --runner codex|claude [--model <name>] [--no-run-script] [--print-command]\n");
    fprintf(stderr, "       dispatch agent list [--all]\n");
    fprintf(stderr, "       dispatch agent show <name>\n");
    fprintf(stderr, "       dispatch agent archive <name>\n");
    fprintf(stderr, "       dispatch agent restore <name>\n");
    fprintf(stderr, "       dispatch agent command <name> [--print-command]\n");
    fprintf(stderr,
            "       dispatch agent session <name> [--session-id <id>|--clear-session] [--current-task <id>|--clear-current-task] [--last-workspace <id>|--clear-last-workspace]\n");
    fprintf(stderr, "       dispatch agent resume <name> [--print-command]\n");
    return 1;
}
