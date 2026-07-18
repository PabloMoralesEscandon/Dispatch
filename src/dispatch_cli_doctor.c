#include "dispatch_cli_internal.h"

static void doctor_ok(const char *message) {
    printf("[ok] %s\n", message);
}

static void doctor_warn(size_t *warnings, const char *message,
                        const char *fix) {
    printf("[warn] %s\n", message);
    if (fix && fix[0])
        printf("       fix: %s\n", fix);
    (*warnings)++;
}

static int path_exists(const char *path) {
    struct stat info;
    return path && stat(path, &info) == 0;
}

static int path_is_executable(const char *path) {
    return path && access(path, X_OK) == 0;
}

static void doctor_check_completion(size_t *warnings, const char *shell_name) {
    char *path = completion_install_path(shell_name);
    if (!path) {
        doctor_warn(warnings, "HOME is not set; cannot check completions",
                    "set HOME or install completions manually");
        return;
    }

    char message[512];
    snprintf(message, sizeof(message), "%s completion installed at %s",
             shell_name, path);
    if (path_exists(path)) {
        doctor_ok(message);
    } else {
        char fix[256];
        snprintf(fix, sizeof(fix), "dispatch completion install %s",
                 shell_name);
        doctor_warn(warnings, message, fix);
    }
    free(path);
}

int cmd_doctor(int argc, char **argv) {
    (void)argv;
    if (argc != 2) {
        fprintf(stderr, "Usage: dispatch doctor\n");
        return 1;
    }

    printf("Dispatch doctor\n");
    size_t warnings = 0;

    DispatchBoard board;
    if (!load_board_or_error(&board)) {
        doctor_warn(&warnings, "could not load Dispatch board",
                    "run dispatch normalize after resolving the load error");
        return 1;
    }
    doctor_ok("board loaded");

    if (board.repo_path && dispatch_path_is_git_repository(board.repo_path)) {
        doctor_ok("configured repository is a git repository");
    } else {
        doctor_warn(&warnings, "configured repository is not a git repository",
                    "run dispatch init <repo-path> from the workflow directory");
    }

    if (path_exists("AGENTS.md"))
        doctor_ok("AGENTS.md found in workflow directory");
    else
        doctor_warn(&warnings, "AGENTS.md missing in workflow directory",
                    "add repository workflow instructions for agents");

    if (command_exists_on_path("dispatch"))
        doctor_ok("dispatch found on PATH");
    else
        doctor_warn(&warnings, "dispatch not found on PATH",
                    "add the workflow dispatch symlink or binary directory to PATH");

    struct stat dispatch_link;
    if (lstat("dispatch", &dispatch_link) == 0) {
        if (S_ISLNK(dispatch_link.st_mode))
            doctor_ok("workflow dispatch entry is a symlink");
        else if (S_ISREG(dispatch_link.st_mode))
            doctor_ok("workflow dispatch entry is a regular executable");
        else
            doctor_warn(&warnings, "workflow dispatch entry has unexpected type",
                        "replace it with a symlink to the built dispatch binary");
    } else {
        doctor_warn(&warnings, "no dispatch executable in workflow directory",
                    "ln -sf Dispatch/dispatch dispatch");
    }

    doctor_check_completion(&warnings, "fish");
    doctor_check_completion(&warnings, "bash");
    doctor_check_completion(&warnings, "zsh");

    for (size_t i = 0; i < board.agents.count; i++) {
        DispatchAgent *agent = &board.agents.items[i];
        char message[512];
        snprintf(message, sizeof(message), "agent %s prompt exists",
                 agent->name);
        if (path_exists(agent->prompt_path))
            doctor_ok(message);
        else
            doctor_warn(&warnings, message, "run dispatch normalize");

        if (agent->run_script_path) {
            snprintf(message, sizeof(message),
                     "agent %s run script is executable", agent->name);
            if (path_is_executable(agent->run_script_path))
                doctor_ok(message);
            else
                doctor_warn(&warnings, message, "run dispatch normalize");
        }

        if (agent->current_task &&
            !dispatch_board_find_task(&board, agent->current_task)) {
            snprintf(message, sizeof(message),
                     "agent %s references missing current task %s",
                     agent->name, agent->current_task);
            doctor_warn(&warnings, message,
                        "clear or update the agent session current task");
        }
    }

    for (size_t i = 0; i < board.workspaces.count; i++) {
        DispatchWorkspace *workspace = &board.workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
            continue;
        char message[512];
        snprintf(message, sizeof(message), "workspace %s path exists",
                 workspace->id);
        if (path_exists(workspace->path))
            doctor_ok(message);
        else
            doctor_warn(&warnings, message,
                        "run dispatch workspace show <id> and prune stale records if needed");
    }

    /* Orphaned workspaces: active records whose task no longer exists. */
    size_t orphaned = 0;
    for (size_t i = 0; i < board.workspaces.count; i++) {
        DispatchWorkspace *workspace = &board.workspaces.items[i];
        if (workspace->state == DISPATCH_WORKSPACE_REMOVED)
            continue;
        if (!dispatch_board_find_task(&board, workspace->task_id)) {
            char message[512];
            snprintf(message, sizeof(message),
                     "workspace %s references missing task %s", workspace->id,
                     workspace->task_id);
            doctor_warn(&warnings, message,
                        "remove it with dispatch workspace remove <id> --force");
            orphaned++;
        }
    }
    if (orphaned == 0)
        doctor_ok("no orphaned workspaces");

    /* Done tasks should carry at least one commit reference. */
    size_t done_without_commits = 0;
    for (size_t i = 0; i < board.tasks.count; i++) {
        DispatchTask *task = &board.tasks.items[i];
        if (task->state != DISPATCH_STATE_DONE || task->commits.count > 0)
            continue;
        char message[512];
        snprintf(message, sizeof(message),
                 "done task %s has no recorded commits", task->id);
        doctor_warn(&warnings, message,
                    "record one with dispatch commit add <id> <sha>");
        done_without_commits++;
    }
    if (done_without_commits == 0)
        doctor_ok("all done tasks have recorded commits");

    /* Dependency anomalies: missing references, self-dependencies, and
     * duplicate entries. */
    size_t dep_anomalies = 0;
    for (size_t i = 0; i < board.tasks.count; i++) {
        DispatchTask *task = &board.tasks.items[i];
        for (size_t d = 0; d < task->depends_on.count; d++) {
            const char *dep = task->depends_on.items[d];
            char message[512];
            if (strcmp(dep, task->id) == 0) {
                snprintf(message, sizeof(message),
                         "task %s depends on itself", task->id);
                doctor_warn(&warnings, message,
                            "remove it with dispatch dep remove");
                dep_anomalies++;
                continue;
            }
            if (!dispatch_board_find_task(&board, dep)) {
                snprintf(message, sizeof(message),
                         "task %s depends on missing task %s", task->id, dep);
                doctor_warn(&warnings, message,
                            "remove it with dispatch dep remove");
                dep_anomalies++;
                continue;
            }
            for (size_t j = 0; j < d; j++) {
                if (strcmp(task->depends_on.items[j], dep) == 0) {
                    snprintf(message, sizeof(message),
                             "task %s lists dependency %s more than once",
                             task->id, dep);
                    doctor_warn(&warnings, message,
                                "remove the duplicate with dispatch dep remove");
                    dep_anomalies++;
                    break;
                }
            }
        }
    }
    if (dep_anomalies == 0)
        doctor_ok("no dependency anomalies");

    printf("Summary: %zu warning%s\n", warnings, warnings == 1 ? "" : "s");
    dispatch_board_free(&board);
    return 0;
}

static int file_contains_text(const char *path, const char *needle) {
    FILE *file = fopen(path, "r");
    if (!file)
        return 0;

    size_t needle_len = strlen(needle);
    size_t matched = 0;
    int found = 0;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if ((char)ch == needle[matched]) {
            matched++;
            if (matched == needle_len) {
                found = 1;
                break;
            }
        } else {
            matched = (char)ch == needle[0] ? 1 : 0;
        }
    }
    fclose(file);
    return found;
}

static int agent_prompt_needs_refresh(const char *path) {
    return !file_contains_text(path, "## Agent ID") ||
           file_contains_text(path, "## Repository Instructions From AGENTS.md");
}

static int normalize_agent_prompts(DispatchBoard *board) {
    int changed = 0;
    for (size_t i = 0; i < board->agents.count; i++) {
        DispatchAgent *agent = &board->agents.items[i];
        char *prompt_path = agent_prompt_path_for(agent->agent_dir, agent->name);
        int needs_path_update = strcmp(agent->prompt_path, prompt_path) != 0;
        int needs_prompt_refresh =
            needs_path_update || agent_prompt_needs_refresh(agent->prompt_path);
        char *command = agent->run_script_path
                            ? agent_command_for(agent->runner, agent->model,
                                                prompt_path)
                            : NULL;
        int needs_script_refresh =
            command && !file_contains_text(agent->run_script_path, command);
        if (!needs_prompt_refresh && !needs_script_refresh) {
            free(command);
            free(prompt_path);
            continue;
        }

        char *scratch_dir = NULL;
        char *decisions_dir = NULL;
        if (needs_prompt_refresh) {
            if (!create_agent_dirs(agent->agent_dir, &scratch_dir,
                                   &decisions_dir) ||
                !write_agent_prompt(prompt_path, agent->name, agent->runner,
                                    agent->model, agent->agent_dir, scratch_dir,
                                    decisions_dir)) {
                free(command);
                free(prompt_path);
                free(scratch_dir);
                free(decisions_dir);
                return -1;
            }
        }

        if (needs_script_refresh) {
            int ok = write_agent_run_script(agent->run_script_path, command);
            if (!ok) {
                free(command);
                free(prompt_path);
                free(scratch_dir);
                free(decisions_dir);
                return -1;
            }
        }

        if (needs_path_update) {
            free(agent->prompt_path);
            agent->prompt_path = prompt_path;
        } else {
            free(prompt_path);
        }
        agent->updated_at = time(NULL);
        changed++;
        free(command);
        free(scratch_dir);
        free(decisions_dir);
    }
    return changed;
}

int cmd_normalize(void) {
    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    dispatch_board_normalize_states(&locked.board);
    int session_updates = dispatch_board_normalize_agent_sessions(&locked.board);
    int prompt_updates = normalize_agent_prompts(&locked.board);
    if (prompt_updates < 0) {
        locked_board_close(&locked);
        return 1;
    }
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    printf("Normalized %s\n", DISPATCH_STORE_FILE);
    if (session_updates > 0)
        printf("Trimmed %d agent session ID%s\n", session_updates,
               session_updates == 1 ? "" : "s");
    if (prompt_updates > 0)
        printf("Updated %d agent prompt%s\n", prompt_updates,
               prompt_updates == 1 ? "" : "s");
    append_dispatch_log("user", "normalize", "normalize", NULL, 0, NULL, 0,
                        "Normalized dispatch.json");
    locked_board_close(&locked);
    return 0;
}
