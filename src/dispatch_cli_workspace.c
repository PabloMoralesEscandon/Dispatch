#include "dispatch_cli_internal.h"

static void print_workspace_create_usage(void) {
    fprintf(stderr,
            "Usage: dispatch workspace create <task-id> --actor <name> [--repo <path>] [--dir <path>] [--branch <name>] [--sequence]\n");
}

static void print_workspace_remove_usage(void) {
    fprintf(stderr,
            "Usage: dispatch workspace remove <task-id-or-workspace> [--force]\n");
}

static void print_workspace_prune_usage(void) {
    fprintf(stderr,
            "Usage: dispatch workspace prune [--done] [--stale] [--dry-run]\n");
}

static char *current_workflow_path(void) {
    char *path = getcwd(NULL, 0);
    if (!path) {
        fprintf(stderr, "Could not resolve current directory: %s\n",
                strerror(errno));
        return NULL;
    }
    return path;
}

static char *path_dirname_copy(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return cli_strdup(".");
    if (slash == path)
        return cli_strdup("/");

    size_t length = (size_t)(slash - path);
    char *dirname = malloc(length + 1);
    if (!dirname) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(dirname, path, length);
    dirname[length] = '\0';
    return dirname;
}

static const char *path_basename_view(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static char *resolve_future_path(const char *workflow_dir, const char *path) {
    if (!path || path[0] == '\0')
        return NULL;
    if (path[0] == '/')
        return cli_strdup(path);

    char *dirname = path_dirname_copy(path);
    const char *basename = path_basename_view(path);
    if (basename[0] == '\0' || strcmp(basename, ".") == 0 ||
        strcmp(basename, "..") == 0) {
        free(dirname);
        return NULL;
    }

    char *parent = dispatch_resolve_path(workflow_dir, dirname);
    free(dirname);
    if (!parent)
        return NULL;

    char *resolved = join_path2(parent, basename);
    free(parent);
    return resolved;
}

static int workspace_record_is_live(const DispatchWorkspace *workspace) {
    return workspace->state != DISPATCH_WORKSPACE_REMOVED;
}

static int string_list_contains_cli(const DispatchStringList *list,
                                    const char *value) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], value) == 0)
            return 1;
    }
    return 0;
}

static void string_list_append_cli(DispatchStringList *list,
                                   const char *value) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = cli_realloc_array(list->items, list->capacity,
                                        sizeof(*list->items));
    }
    list->items[list->count++] = cli_strdup(value);
}

static void string_list_free_cli(DispatchStringList *list) {
    for (size_t i = 0; i < list->count; i++)
        free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

int workspace_covers_task(const DispatchWorkspace *workspace,
                                 const char *task_id) {
    return strcmp(workspace->task_id, task_id) == 0 ||
           string_list_contains_cli(&workspace->sequence_tasks, task_id);
}

static int workspace_branch_exists(const DispatchBoard *board,
                                   const char *branch) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace_record_is_live(workspace) &&
            strcmp(workspace->branch, branch) == 0) {
            return 1;
        }
    }
    return 0;
}

static int workspace_path_exists(const DispatchBoard *board, const char *path) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace_record_is_live(workspace) &&
            strcmp(workspace->path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static DispatchWorkspace *live_workspace_for_task(const DispatchBoard *board,
                                                  const char *task_id) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace_record_is_live(workspace) &&
            workspace_covers_task(workspace, task_id)) {
            return workspace;
        }
    }
    return NULL;
}

static void workspace_free_fields(DispatchWorkspace *workspace) {
    free(workspace->id);
    free(workspace->task_id);
    free(workspace->actor);
    free(workspace->path);
    free(workspace->branch);
    free(workspace->repo_path);
    for (size_t i = 0; i < workspace->sequence_tasks.count; i++)
        free(workspace->sequence_tasks.items[i]);
    free(workspace->sequence_tasks.items);
    free(workspace->review_gate);
}

static int remove_workspace_record(DispatchBoard *board, const char *task_id) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (strcmp(workspace->task_id, task_id) != 0)
            continue;

        workspace_free_fields(workspace);
        for (size_t j = i + 1; j < board->workspaces.count; j++)
            board->workspaces.items[j - 1] = board->workspaces.items[j];
        board->workspaces.count--;
        return 1;
    }
    return 0;
}

static DispatchWorkspace *append_workspace_reservation(
    DispatchBoard *board, const char *task_id, const char *actor,
    const char *repo_path, const char *workspace_path, const char *branch,
    const DispatchStringList *sequence_tasks, const char *review_gate) {
    if (board->workspaces.count >= board->workspaces.capacity) {
        board->workspaces.capacity = board->workspaces.capacity == 0
                                         ? 4
                                         : board->workspaces.capacity * 2;
        board->workspaces.items = cli_realloc_array(
            board->workspaces.items, board->workspaces.capacity,
            sizeof(*board->workspaces.items));
    }

    DispatchWorkspace *workspace =
        &board->workspaces.items[board->workspaces.count++];
    memset(workspace, 0, sizeof(*workspace));
    workspace->id = cli_strdup(task_id);
    workspace->task_id = cli_strdup(task_id);
    workspace->actor = cli_strdup(actor);
    workspace->repo_path = cli_strdup(repo_path);
    workspace->path = cli_strdup(workspace_path);
    workspace->branch = cli_strdup(branch);
    for (size_t i = 0; sequence_tasks && i < sequence_tasks->count; i++)
        string_list_append_cli(&workspace->sequence_tasks,
                               sequence_tasks->items[i]);
    workspace->review_gate = review_gate ? cli_strdup(review_gate) : NULL;
    workspace->state = DISPATCH_WORKSPACE_CREATING;
    workspace->created_at = time(NULL);
    workspace->updated_at = workspace->created_at;
    return workspace;
}

static int workspace_all_tasks_done(const DispatchBoard *board,
                                    const DispatchWorkspace *workspace) {
    const DispatchStringList *tasks = &workspace->sequence_tasks;
    if (tasks->count == 0) {
        DispatchTask *task =
            dispatch_board_find_task((DispatchBoard *)board,
                                     workspace->task_id);
        return task &&
               dispatch_task_effective_state(board, task) ==
                   DISPATCH_STATE_DONE;
    }

    for (size_t i = 0; i < tasks->count; i++) {
        DispatchTask *task =
            dispatch_board_find_task((DispatchBoard *)board,
                                     tasks->items[i]);
        if (!task ||
            dispatch_task_effective_state(board, task) !=
                DISPATCH_STATE_DONE) {
            return 0;
        }
    }
    return 1;
}

static DispatchTask *single_dependent_task(const DispatchBoard *board,
                                           const char *task_id,
                                           size_t *dependent_count) {
    DispatchTask *dependent = NULL;
    *dependent_count = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *candidate = &board->tasks.items[i];
        if (!string_list_contains_cli(&candidate->depends_on, task_id))
            continue;
        dependent = candidate;
        (*dependent_count)++;
    }
    return *dependent_count == 1 ? dependent : NULL;
}

static int task_depends_only_on(const DispatchTask *task,
                                const char *dependency_id) {
    return task->depends_on.count == 1 &&
           strcmp(task->depends_on.items[0], dependency_id) == 0;
}

static int build_workspace_sequence(const DispatchBoard *board,
                                    DispatchTask *start,
                                    DispatchStringList *sequence_tasks,
                                    char **review_gate) {
    DispatchTask *current = start;
    for (size_t guard = 0; guard < board->tasks.count; guard++) {
        if (current->assigned_to) {
            fprintf(stderr, "Sequence task %s must be unassigned\n",
                    current->id);
            return 0;
        }
        DispatchState state = dispatch_task_effective_state(board, current);
        if (state != DISPATCH_STATE_READY && state != DISPATCH_STATE_BLOCKED) {
            fprintf(stderr, "Sequence task %s must be ready or blocked\n",
                    current->id);
            return 0;
        }
        if (live_workspace_for_task(board, current->id)) {
            fprintf(stderr, "Workspace already exists for %s\n", current->id);
            return 0;
        }

        string_list_append_cli(sequence_tasks, current->id);
        if (current->requires_review) {
            *review_gate = cli_strdup(current->id);
            return 1;
        }

        size_t dependent_count = 0;
        DispatchTask *next =
            single_dependent_task(board, current->id, &dependent_count);
        if (dependent_count == 0) {
            /* Natural end of a no-review chain: the sequence terminates
             * cleanly without a review gate. */
            return 1;
        }
        if (!next) {
            fprintf(stderr,
                    "Sequence task %s must have exactly one dependent\n",
                    current->id);
            return 0;
        }
        if (!task_depends_only_on(next, current->id)) {
            fprintf(stderr, "Sequence task %s must depend only on %s\n",
                    next->id, current->id);
            return 0;
        }
        current = next;
    }

    fprintf(stderr, "Sequence from %s does not reach a review gate\n",
            start->id);
    return 0;
}

char *shell_quote(const char *value) {
    size_t size = 3;
    for (size_t i = 0; value[i] != '\0'; i++)
        size += value[i] == '\'' ? 4 : 1;

    char *quoted = malloc(size);
    if (!quoted) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    size_t out = 0;
    quoted[out++] = '\'';
    for (size_t i = 0; value[i] != '\0'; i++) {
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

static int run_command_quiet(const char *const argv[]) {
    DispatchExecOptions options = {
        .stdout_mode = DISPATCH_EXEC_DEV_NULL,
        .stderr_mode = DISPATCH_EXEC_DEV_NULL,
    };
    DispatchExecResult result;
    return dispatch_exec_run(argv, &options, &result) &&
           dispatch_exec_result_success(&result);
}

static int git_branch_exists(const char *repo_path, const char *branch) {
    size_t ref_size = strlen("refs/heads/") + strlen(branch) + 1;
    char *ref = malloc(ref_size);
    if (!ref) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(ref, ref_size, "refs/heads/%s", branch);
    const char *argv[] = {"git",      "-C",      repo_path, "rev-parse",
                          "--verify", "--quiet", ref,       NULL};
    int exists = run_command_quiet(argv);
    free(ref);
    return exists;
}

static int git_worktree_add(const char *repo_path, const char *workspace_path,
                            const char *branch, int branch_exists) {
    const char *existing_argv[] = {"git", "-C", repo_path, "worktree", "add",
                                   "--", workspace_path, branch, NULL};
    const char *new_argv[] = {"git", "-C", repo_path, "worktree", "add",
                              "-b", branch, "--", workspace_path, "HEAD",
                              NULL};
    if (branch_exists) {
        return run_command_quiet(existing_argv);
    }
    return run_command_quiet(new_argv);
}

static int git_worktree_remove_force(const char *repo_path,
                                     const char *workspace_path) {
    const char *argv[] = {"git", "-C", repo_path, "worktree", "remove",
                          "--force", "--", workspace_path, NULL};
    return run_command_quiet(argv);
}

static int git_worktree_remove(const char *repo_path,
                               const char *workspace_path) {
    const char *argv[] = {"git", "-C", repo_path, "worktree", "remove", "--",
                          workspace_path, NULL};
    return run_command_quiet(argv);
}

static int git_worktree_is_registered(const char *repo_path,
                                      const char *workspace_path) {
    size_t line_size = strlen("worktree ") + strlen(workspace_path) + 1;
    char *line = malloc(line_size);
    if (!line) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(line, line_size, "worktree %s", workspace_path);
    const char *argv[] = {"git", "-C", repo_path, "worktree", "list",
                          "--porcelain", NULL};
    DispatchExecOptions options = {.stderr_mode = DISPATCH_EXEC_DEV_NULL};
    DispatchExecResult result;
    char *output = NULL;
    int registered = 0;
    if (dispatch_exec_capture(argv, &options, &output, NULL, &result) &&
        dispatch_exec_result_success(&result)) {
        const char *cursor = output;
        while (*cursor) {
            const char *end = strchr(cursor, '\n');
            size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
            if (strlen(line) == length && strncmp(cursor, line, length) == 0) {
                registered = 1;
                break;
            }
            if (!end)
                break;
            cursor = end + 1;
        }
    }
    free(output);
    free(line);
    return registered;
}

static int git_worktree_is_dirty(const char *workspace_path) {
    const char *argv[] = {"git", "-C", workspace_path, "status",
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

static int mark_workspace_active(const char *task_id) {
    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 0;

    DispatchWorkspace *workspace =
        dispatch_board_find_workspace(&locked.board, task_id);
    if (!workspace) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace reservation disappeared for %s\n", task_id);
        return 0;
    }

    workspace->state = DISPATCH_WORKSPACE_ACTIVE;
    workspace->updated_at = time(NULL);
    int ok = locked_board_save_or_error(&locked);
    locked_board_close(&locked);
    return ok;
}

static void remove_workspace_reservation_after_failure(const char *task_id) {
    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return;
    remove_workspace_record(&locked.board, task_id);
    (void)locked_board_save_or_error(&locked);
    locked_board_close(&locked);
}

static int cmd_workspace_create(int argc, char **argv) {
    if (argc < 5) {
        print_workspace_create_usage();
        return 1;
    }

    const char *task_id = argv[3];
    const char *actor = NULL;
    const char *repo_option = NULL;
    const char *dir_option = NULL;
    const char *branch_option = NULL;
    int sequence = 0;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
            actor = argv[++i];
        } else if (strcmp(argv[i], "--repo") == 0 && (i + 1) < argc) {
            repo_option = argv[++i];
        } else if (strcmp(argv[i], "--dir") == 0 && (i + 1) < argc) {
            dir_option = argv[++i];
        } else if (strcmp(argv[i], "--branch") == 0 && (i + 1) < argc) {
            branch_option = argv[++i];
        } else if (strcmp(argv[i], "--sequence") == 0) {
            sequence = 1;
        } else {
            print_workspace_create_usage();
            return 1;
        }
    }

    if (!actor || actor[0] == '\0') {
        print_workspace_create_usage();
        return 1;
    }
    if (!dispatch_actor_label_is_valid(actor)) {
        fprintf(stderr,
                "Actor must start with an ASCII letter or digit and contain only letters, digits, '.', '_' or '-'\n");
        return 1;
    }

    char *workflow_dir = current_workflow_path();
    if (!workflow_dir)
        return 1;

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked)) {
        free(workflow_dir);
        return 1;
    }
    DispatchBoard *board = &locked.board;

    DispatchAgent *actor_agent = dispatch_board_find_agent(board, actor);
    if (actor_agent && actor_agent->archived) {
        locked_board_close(&locked);
        fprintf(stderr, "Agent %s is archived; restore it first\n", actor);
        free(workflow_dir);
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(board, task_id);
    if (!task) {
        locked_board_close(&locked);
        fprintf(stderr, "No task with id %s\n", task_id);
        free(workflow_dir);
        return 1;
    }
    if (dispatch_task_effective_state(board, task) != DISPATCH_STATE_READY ||
        task->assigned_to) {
        locked_board_close(&locked);
        fprintf(stderr, "Task %s must be ready and unassigned\n", task_id);
        free(workflow_dir);
        return 1;
    }
    if (live_workspace_for_task(board, task_id)) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace already exists for %s\n", task_id);
        free(workflow_dir);
        return 1;
    }

    DispatchStringList sequence_tasks = {0};
    char *review_gate = NULL;
    if (sequence &&
        !build_workspace_sequence(board, task, &sequence_tasks, &review_gate)) {
        locked_board_close(&locked);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(workflow_dir);
        return 1;
    }

    const char *configured_repo =
        repo_option && repo_option[0] ? repo_option : board->repo_path;
    char *repo_path = dispatch_resolve_path(workflow_dir, configured_repo);
    if (!repo_path || !dispatch_path_is_git_repository(repo_path)) {
        fprintf(stderr,
                "Configured repository is not a git repository: %s\n",
                configured_repo);
        locked_board_close(&locked);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(workflow_dir);
        free(repo_path);
        return 1;
    }

    char *sequence_name = NULL;
    if (sequence) {
        size_t size = strlen(task_id) + strlen("-sequence") + 1;
        sequence_name = malloc(size);
        if (!sequence_name) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        snprintf(sequence_name, size, "%s-sequence", task_id);
    }

    const char *name_for_defaults = sequence ? sequence_name : task_id;
    char *branch =
        branch_option && branch_option[0]
            ? cli_strdup(branch_option)
            : dispatch_default_workspace_branch(actor, name_for_defaults);
    if (!branch || branch[0] == '\0') {
        locked_board_close(&locked);
        fprintf(stderr, "Could not derive workspace branch\n");
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        return 1;
    }

    char *workspace_path = dir_option && dir_option[0]
                               ? resolve_future_path(workflow_dir, dir_option)
                               : dispatch_default_workspace_path(repo_path,
                                                                 actor,
                                                                 name_for_defaults);
    if (!workspace_path) {
        locked_board_close(&locked);
        fprintf(stderr, "Could not resolve workspace directory\n");
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        return 1;
    }

    if (strcmp(repo_path, workspace_path) == 0) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace path must not equal repository path\n");
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }
    if (workspace_branch_exists(board, branch)) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace branch already reserved: %s\n", branch);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }
    if (workspace_path_exists(board, workspace_path)) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace path already reserved: %s\n",
                workspace_path);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }
    struct stat workspace_info;
    if (stat(workspace_path, &workspace_info) == 0) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace path already exists: %s\n",
                workspace_path);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }

    append_workspace_reservation(board, task_id, actor, repo_path,
                                 workspace_path, branch,
                                 sequence ? &sequence_tasks : NULL,
                                 review_gate);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }

    locked_board_close(&locked);

    int branch_exists = git_branch_exists(repo_path, branch);
    if (!git_worktree_add(repo_path, workspace_path, branch, branch_exists)) {
        (void)git_worktree_remove_force(repo_path, workspace_path);
        remove_workspace_reservation_after_failure(task_id);
        fprintf(stderr, "Could not create git worktree for %s\n", task_id);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }

    if (!mark_workspace_active(task_id)) {
        if (!git_worktree_remove_force(repo_path, workspace_path)) {
            fprintf(stderr,
                    "Could not roll back git worktree %s; manual cleanup may be required\n",
                    workspace_path);
        }
        remove_workspace_reservation_after_failure(task_id);
        string_list_free_cli(&sequence_tasks);
        free(review_gate);
        free(sequence_name);
        free(workflow_dir);
        free(repo_path);
        free(branch);
        free(workspace_path);
        return 1;
    }

    printf("Created workspace %s for %s\n", task_id, actor);
    printf("  path: %s\n", workspace_path);
    printf("  branch: %s\n", branch);
    printf("  state: active\n");
    if (sequence) {
        printf("  tasks:");
        for (size_t i = 0; i < sequence_tasks.count; i++)
            printf("%s%s", i == 0 ? " " : ",", sequence_tasks.items[i]);
        printf("\n");
        printf("  review gate: %s\n", review_gate ? review_gate : "-");
    }

    DispatchLogField targets[] = {
        {"task", task_id},
        {"workspace", task_id},
    };
    DispatchLogField context[] = {
        {"workspace_path", workspace_path},
        {"workspace_branch", branch},
        {"repo_path", repo_path},
        {"sequence", bool_string(sequence)},
    };
    char message[256];
    snprintf(message, sizeof(message), "Created workspace %s", task_id);
    append_dispatch_log(actor, "workspace", "workspace_create", targets, 2,
                        context, 4, message);

    string_list_free_cli(&sequence_tasks);
    free(review_gate);
    free(sequence_name);
    free(workflow_dir);
    free(repo_path);
    free(branch);
    free(workspace_path);
    return 0;
}

static int workspace_git_worktree_present(const DispatchWorkspace *workspace) {
    if (!workspace || !workspace->path || workspace->path[0] == '\0')
        return 0;

    struct stat info;
    if (stat(workspace->path, &info) != 0 || !S_ISDIR(info.st_mode))
        return 0;

    char *git_path = join_path2(workspace->path, ".git");
    int present = stat(git_path, &info) == 0 &&
                  (S_ISDIR(info.st_mode) || S_ISREG(info.st_mode));
    free(git_path);
    return present;
}

static int cmd_workspace_list(int argc, char **argv) {
    (void)argv;
    if (argc != 3) {
        fprintf(stderr, "Usage: dispatch workspace list\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (board.workspaces.count == 0) {
        printf("(no workspaces)\n");
        dispatch_board_free(&board);
        return 0;
    }

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
        printf("%-8s %-10s %-10s %-16s %s  %s\n", workspace->task_id,
               task_state, dispatch_workspace_state_name(workspace->state),
               workspace->actor, workspace->branch, workspace->path);
    }

    dispatch_board_free(&board);
    return 0;
}

static int cmd_workspace_show(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: dispatch workspace show <task-id-or-workspace>\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchWorkspace *workspace = live_workspace_for_task(&board, argv[3]);
    if (!workspace || workspace->state == DISPATCH_WORKSPACE_REMOVED) {
        dispatch_board_free(&board);
        fprintf(stderr, "No workspace for %s\n", argv[3]);
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(&board, workspace->task_id);
    const char *task_state =
        task ? dispatch_state_name(dispatch_task_effective_state(&board, task))
             : "missing";

    printf("Task: %s\n", workspace->task_id);
    printf("Task state: %s\n", task_state);
    printf("Workspace state: %s\n",
           dispatch_workspace_state_name(workspace->state));
    printf("Actor: %s\n", workspace->actor);
    printf("Branch: %s\n", workspace->branch);
    printf("Path: %s\n", workspace->path);
    printf("Repo: %s\n", workspace->repo_path);
    if (workspace->sequence_tasks.count > 0) {
        printf("Sequence tasks:");
        for (size_t i = 0; i < workspace->sequence_tasks.count; i++)
            printf("%s%s", i == 0 ? " " : ",",
                   workspace->sequence_tasks.items[i]);
        printf("\n");
        printf("Review gate: %s\n",
               workspace->review_gate ? workspace->review_gate : "-");
    }
    printf("Git worktree: %s\n",
           workspace_git_worktree_present(workspace) ? "present" : "missing");

    dispatch_board_free(&board);
    return 0;
}

static int cmd_workspace_remove(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        print_workspace_remove_usage();
        return 1;
    }

    const char *target = argv[3];
    int force = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else {
            print_workspace_remove_usage();
            return 1;
        }
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    DispatchWorkspace *workspace = live_workspace_for_task(&locked.board,
                                                           target);
    if (!workspace || workspace->state == DISPATCH_WORKSPACE_REMOVED) {
        locked_board_close(&locked);
        fprintf(stderr, "No workspace for %s\n", target);
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(&locked.board,
                                                  workspace->task_id);
    DispatchState task_state = task ? dispatch_task_effective_state(&locked.board,
                                                                    task)
                                    : DISPATCH_STATE_PROPOSED;
    if (!force && task_state == DISPATCH_STATE_DOING) {
        fprintf(stderr,
                "Workspace task %s is doing; use --force to remove\n",
                workspace->task_id);
        locked_board_close(&locked);
        return 1;
    }

    char *record_task_id = cli_strdup(workspace->task_id);
    char *repo_path = cli_strdup(workspace->repo_path);
    char *workspace_path = cli_strdup(workspace->path);
    locked_board_close(&locked);

    if (!git_worktree_is_registered(repo_path, workspace_path)) {
        fprintf(stderr, "Workspace path is not a git worktree: %s\n",
                workspace_path);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    if (!force && git_worktree_is_dirty(workspace_path)) {
        fprintf(stderr, "Workspace has uncommitted changes: %s\n",
                workspace_path);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    int removed = force ? git_worktree_remove_force(repo_path, workspace_path)
                        : git_worktree_remove(repo_path, workspace_path);
    if (!removed) {
        fprintf(stderr, "Could not remove git worktree: %s\n",
                workspace_path);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    if (!locked_board_load_or_error(&locked)) {
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }
    if (!remove_workspace_record(&locked.board, record_task_id)) {
        locked_board_close(&locked);
        fprintf(stderr, "Workspace record disappeared for %s\n",
                record_task_id);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }
    locked_board_close(&locked);

    printf("Removed workspace %s\n", record_task_id);
    printf("  path: %s\n", workspace_path);
    DispatchLogField targets[] = {
        {"task", record_task_id},
        {"workspace", record_task_id},
    };
    DispatchLogField context[] = {
        {"workspace_path", workspace_path},
        {"force", bool_string(force)},
    };
    char message[256];
    snprintf(message, sizeof(message), "Removed workspace %s", record_task_id);
    append_dispatch_log("user", "workspace", "workspace_remove", targets, 2,
                        context, 2, message);
    free(record_task_id);
    free(repo_path);
    free(workspace_path);
    return 0;
}

typedef enum {
    PRUNE_CANDIDATE_DONE,
    PRUNE_CANDIDATE_STALE
} PruneCandidateKind;

typedef struct {
    char *task_id;
    char *repo_path;
    char *workspace_path;
    PruneCandidateKind kind;
} PruneCandidate;

typedef struct {
    PruneCandidate *items;
    size_t count;
    size_t capacity;
} PruneCandidates;

static void prune_candidates_append(PruneCandidates *candidates,
                                    const DispatchWorkspace *workspace,
                                    PruneCandidateKind kind) {
    if (candidates->count >= candidates->capacity) {
        candidates->capacity =
            candidates->capacity == 0 ? 4 : candidates->capacity * 2;
        candidates->items = cli_realloc_array(candidates->items,
                                              candidates->capacity,
                                              sizeof(*candidates->items));
    }

    PruneCandidate *candidate = &candidates->items[candidates->count++];
    candidate->task_id = cli_strdup(workspace->task_id);
    candidate->repo_path = cli_strdup(workspace->repo_path);
    candidate->workspace_path = cli_strdup(workspace->path);
    candidate->kind = kind;
}

static void prune_candidates_free(PruneCandidates *candidates) {
    for (size_t i = 0; i < candidates->count; i++) {
        free(candidates->items[i].task_id);
        free(candidates->items[i].repo_path);
        free(candidates->items[i].workspace_path);
    }
    free(candidates->items);
    memset(candidates, 0, sizeof(*candidates));
}

static int remove_workspace_records_by_id(DispatchStringList *task_ids) {
    if (task_ids->count == 0)
        return 1;

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 0;

    for (size_t i = 0; i < task_ids->count; i++)
        (void)remove_workspace_record(&locked.board, task_ids->items[i]);

    int ok = locked_board_save_or_error(&locked);
    locked_board_close(&locked);
    return ok;
}

static int cmd_workspace_prune(int argc, char **argv) {
    int prune_done = 0;
    int prune_stale = 0;
    int dry_run = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--done") == 0) {
            prune_done = 1;
        } else if (strcmp(argv[i], "--stale") == 0) {
            prune_stale = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else {
            print_workspace_prune_usage();
            return 1;
        }
    }

    if (!prune_done && !prune_stale) {
        print_workspace_prune_usage();
        return 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    PruneCandidates candidates = {0};
    for (size_t i = 0; i < locked.board.workspaces.count; i++) {
        DispatchWorkspace *workspace = &locked.board.workspaces.items[i];
        if (!workspace_record_is_live(workspace))
            continue;

        if (prune_stale &&
            workspace->state == DISPATCH_WORKSPACE_CREATING) {
            prune_candidates_append(&candidates, workspace,
                                    PRUNE_CANDIDATE_STALE);
            continue;
        }

        if (prune_done && workspace_all_tasks_done(&locked.board, workspace)) {
            prune_candidates_append(&candidates, workspace,
                                    PRUNE_CANDIDATE_DONE);
        }
    }
    locked_board_close(&locked);

    DispatchStringList records_to_remove = {0};
    size_t pruned = 0;
    size_t skipped = 0;
    for (size_t i = 0; i < candidates.count; i++) {
        PruneCandidate *candidate = &candidates.items[i];
        int registered = git_worktree_is_registered(candidate->repo_path,
                                                    candidate->workspace_path);

        if (candidate->kind == PRUNE_CANDIDATE_STALE) {
            if (registered) {
                printf("Skipped stale workspace %s: git worktree exists\n",
                       candidate->task_id);
                skipped++;
                continue;
            }

            printf("%s stale workspace %s\n",
                   dry_run ? "Would prune" : "Pruned", candidate->task_id);
            pruned++;
            if (!dry_run)
                string_list_append_cli(&records_to_remove,
                                       candidate->task_id);
            continue;
        }

        if (!registered) {
            printf("Skipped done workspace %s: path is not a git worktree\n",
                   candidate->task_id);
            skipped++;
            continue;
        }

        if (git_worktree_is_dirty(candidate->workspace_path)) {
            printf("Skipped done workspace %s: workspace has uncommitted changes\n",
                   candidate->task_id);
            skipped++;
            continue;
        }

        if (dry_run) {
            printf("Would remove done workspace %s\n", candidate->task_id);
            pruned++;
            continue;
        }

        if (!git_worktree_remove(candidate->repo_path,
                                 candidate->workspace_path)) {
            printf("Skipped done workspace %s: could not remove git worktree\n",
                   candidate->task_id);
            skipped++;
            continue;
        }

        printf("Removed done workspace %s\n", candidate->task_id);
        pruned++;
        string_list_append_cli(&records_to_remove, candidate->task_id);
    }

    int ok = dry_run || remove_workspace_records_by_id(&records_to_remove);
    if (pruned == 0 && skipped == 0)
        printf("No workspaces pruned\n");

    if (ok && !dry_run) {
        char pruned_value[32];
        char skipped_value[32];
        snprintf(pruned_value, sizeof(pruned_value), "%zu", pruned);
        snprintf(skipped_value, sizeof(skipped_value), "%zu", skipped);
        DispatchLogField context[] = {
            {"pruned", pruned_value},
            {"skipped", skipped_value},
            {"done", bool_string(prune_done)},
            {"stale", bool_string(prune_stale)},
        };
        append_dispatch_log("user", "workspace", "workspace_prune", NULL, 0,
                            context, 4, "Pruned workspaces");
    }

    string_list_free_cli(&records_to_remove);
    prune_candidates_free(&candidates);
    return ok ? 0 : 1;
}

int cmd_workspace(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "create") == 0)
        return cmd_workspace_create(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "list") == 0)
        return cmd_workspace_list(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "show") == 0)
        return cmd_workspace_show(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "remove") == 0)
        return cmd_workspace_remove(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "prune") == 0)
        return cmd_workspace_prune(argc, argv);

    print_workspace_create_usage();
    fprintf(stderr, "       dispatch workspace list\n");
    fprintf(stderr, "       dispatch workspace show <task-id-or-workspace>\n");
    fprintf(stderr,
            "       dispatch workspace remove <task-id-or-workspace> [--force]\n");
    fprintf(stderr,
            "       dispatch workspace prune [--done] [--stale] [--dry-run]\n");
    return 1;
}
