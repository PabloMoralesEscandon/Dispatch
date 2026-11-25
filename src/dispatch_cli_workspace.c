#include "dispatch_cli_internal.h"

static void print_workspace_create_usage(void) {
    cli_error("usage", NULL,
              "Usage: dispatch workspace create <task-id> --actor <name> [--repo <path>] [--dir <path>] [--branch <name>] [--sequence]\n");
}

static void print_workspace_adopt_usage(void) {
    cli_error("usage", NULL,
              "Usage: dispatch workspace adopt <task-id> --actor <name> --path <dir> [--branch <name>] [--sequence] [--external <tool>:<id>]\n");
}

static void print_workspace_remove_usage(void) {
    cli_error("usage", NULL,
              "Usage: dispatch workspace remove <task-id-or-workspace> [--force] [--delete-worktree]\n");
}

static void print_workspace_prune_usage(void) {
    cli_error("usage", NULL,
              "Usage: dispatch workspace prune [--done] [--stale] [--dry-run]\n");
}

static void print_workspace_repair_usage(void) {
    cli_error("usage", NULL,
              "Usage: dispatch workspace repair <task-id-or-workspace>\n");
}

static char *current_workflow_path(void) {
    char *path = getcwd(NULL, 0);
    if (!path) {
        cli_error("internal_error", NULL,
                  "Could not resolve current directory: %s\n",
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

char *workspace_reanchored_path(const char *repo_path,
                                const char *workspace_path) {
    if (!repo_path || repo_path[0] == '\0' || !workspace_path ||
        workspace_path[0] == '\0') {
        return NULL;
    }

    const char *workspace_name = path_basename_view(workspace_path);
    if (workspace_name[0] == '\0' || strcmp(workspace_name, ".") == 0 ||
        strcmp(workspace_name, "..") == 0) {
        return NULL;
    }

    char *repo_parent = path_dirname_copy(repo_path);
    char *path = join_path2(repo_parent, workspace_name);
    free(repo_parent);
    return path;
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
    free(workspace->external_id);
}

/* Deep copy of a workspace record, so JSON results can report records
 * after they are removed from the board. */
static DispatchWorkspace workspace_clone(const DispatchWorkspace *workspace) {
    DispatchWorkspace copy = *workspace;
    copy.id = cli_strdup(workspace->id);
    copy.task_id = cli_strdup(workspace->task_id);
    copy.actor = cli_strdup(workspace->actor);
    copy.path = cli_strdup(workspace->path);
    copy.branch = cli_strdup(workspace->branch);
    copy.repo_path = cli_strdup(workspace->repo_path);
    copy.review_gate =
        workspace->review_gate ? cli_strdup(workspace->review_gate) : NULL;
    copy.external_id =
        workspace->external_id ? cli_strdup(workspace->external_id) : NULL;
    memset(&copy.sequence_tasks, 0, sizeof(copy.sequence_tasks));
    for (size_t i = 0; i < workspace->sequence_tasks.count; i++)
        string_list_append_cli(&copy.sequence_tasks,
                               workspace->sequence_tasks.items[i]);
    return copy;
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
            cli_error("invalid_sequence", current->id,
                      "Sequence task %s must be unassigned\n", current->id);
            return 0;
        }
        DispatchState state = dispatch_task_effective_state(board, current);
        if (state != DISPATCH_STATE_READY && state != DISPATCH_STATE_BLOCKED) {
            cli_error("invalid_sequence", current->id,
                      "Sequence task %s must be ready or blocked\n",
                      current->id);
            return 0;
        }
        if (live_workspace_for_task(board, current->id)) {
            cli_error("workspace_exists", current->id,
                      "Workspace already exists for %s\n", current->id);
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
            cli_error("invalid_sequence", current->id,
                      "Sequence task %s must have exactly one dependent\n",
                      current->id);
            return 0;
        }
        if (!task_depends_only_on(next, current->id)) {
            cli_error("invalid_sequence", next->id,
                      "Sequence task %s must depend only on %s\n", next->id,
                      current->id);
            return 0;
        }
        current = next;
    }

    cli_error("invalid_sequence", start->id,
              "Sequence from %s does not reach a review gate\n", start->id);
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

static int worktree_paths_match(const char *listed, const char *path) {
    if (strcmp(listed, path) == 0)
        return 1;
    char *listed_real = realpath(listed, NULL);
    char *path_real = realpath(path, NULL);
    int match = listed_real && path_real && strcmp(listed_real, path_real) == 0;
    free(listed_real);
    free(path_real);
    return match;
}

/* Looks up workspace_path in `git worktree list --porcelain` for repo_path.
 * Paths match textually or by realpath, so a record stored as a resolved
 * path still matches git's spelling. On a match, *branch receives the
 * checked-out branch name (refs/heads/ stripped), or NULL for a detached
 * HEAD. Returns 0 when git fails, else 1 with *found set. */
static int git_worktree_lookup(const char *repo_path,
                               const char *workspace_path, int *found,
                               char **branch) {
    *found = 0;
    if (branch)
        *branch = NULL;
    const char *argv[] = {"git", "-C", repo_path, "worktree", "list",
                          "--porcelain", NULL};
    DispatchExecOptions options = {.stderr_mode = DISPATCH_EXEC_DEV_NULL};
    DispatchExecResult result;
    char *output = NULL;
    if (!dispatch_exec_capture(argv, &options, &output, NULL, &result) ||
        !dispatch_exec_result_success(&result)) {
        free(output);
        return 0;
    }

    int in_match = 0;
    char *cursor = output ? output : "";
    while (*cursor) {
        char *end = strchr(cursor, '\n');
        if (end)
            *end = '\0';
        if (strncmp(cursor, "worktree ", 9) == 0) {
            if (in_match)
                break;
            in_match = worktree_paths_match(cursor + 9, workspace_path);
            if (in_match)
                *found = 1;
        } else if (in_match && branch &&
                   strncmp(cursor, "branch ", 7) == 0) {
            const char *ref = cursor + 7;
            if (strncmp(ref, "refs/heads/", 11) == 0)
                ref += 11;
            *branch = cli_strdup(ref);
        }
        if (!end)
            break;
        cursor = end + 1;
    }
    free(output);
    return 1;
}

static int git_worktree_is_registered(const char *repo_path,
                                      const char *workspace_path) {
    int found = 0;
    return git_worktree_lookup(repo_path, workspace_path, &found, NULL) &&
           found;
}

static int git_worktree_repair(const char *repo_path,
                               const char *workspace_path) {
    const char *argv[] = {"git", "-C", repo_path, "worktree", "repair",
                          workspace_path, NULL};
    return run_command_quiet(argv);
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
        cli_error("workspace_not_found", task_id,
                  "Workspace reservation disappeared for %s\n", task_id);
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
        cli_error("invalid_actor", NULL,
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
        cli_error("agent_archived", NULL,
                  "Agent %s is archived; restore it first\n", actor);
        free(workflow_dir);
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(board, task_id);
    if (!task) {
        locked_board_close(&locked);
        cli_error("task_not_found", task_id, "No task with id %s\n", task_id);
        free(workflow_dir);
        return 1;
    }
    if (dispatch_task_effective_state(board, task) != DISPATCH_STATE_READY ||
        task->assigned_to) {
        const char *code =
            task->assigned_to ? "already_assigned" : "invalid_state";
        locked_board_close(&locked);
        cli_error(code, task_id, "Task %s must be ready and unassigned\n",
                  task_id);
        free(workflow_dir);
        return 1;
    }
    if (live_workspace_for_task(board, task_id)) {
        locked_board_close(&locked);
        cli_error("workspace_exists", task_id,
                  "Workspace already exists for %s\n", task_id);
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
        cli_error("repo_not_git", task_id,
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
        cli_error("internal_error", task_id,
                  "Could not derive workspace branch\n");
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
        cli_error("invalid_path", task_id,
                  "Could not resolve workspace directory\n");
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
        cli_error("workspace_conflict", task_id,
                  "Workspace path must not equal repository path\n");
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
        cli_error("workspace_conflict", task_id,
                  "Workspace branch already reserved: %s\n", branch);
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
        cli_error("workspace_conflict", task_id,
                  "Workspace path already reserved: %s\n", workspace_path);
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
        cli_error("workspace_conflict", task_id,
                  "Workspace path already exists: %s\n", workspace_path);
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
        cli_error("git_failed", task_id,
                  "Could not create git worktree for %s\n", task_id);
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
            cli_error("git_failed", task_id,
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

    int json = cli_json();
    if (!json) {
        printf("Created workspace %s for %s\n", task_id, actor);
        printf("  path: %s\n", workspace_path);
        printf("  branch: %s\n", branch);
        printf("  state: active\n");
    }
    if (!json && sequence) {
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

    int status = 0;
    if (json) {
        DispatchBoard final_board;
        if (!load_board_or_error(&final_board)) {
            status = 1;
        } else {
            DispatchWorkspace *created =
                dispatch_board_find_workspace(&final_board, task_id);
            DispatchJsonResult *json_result =
                dispatch_json_result_new("workspace_create");
            dispatch_json_result_add_target(json_result, task_id);
            dispatch_json_result_set_string(json_result, "workspace", task_id);
            status = cli_json_emit_workspaces(&final_board, created,
                                              created ? 1 : 0, json_result);
            dispatch_board_free(&final_board);
        }
    }

    string_list_free_cli(&sequence_tasks);
    free(review_gate);
    free(sequence_name);
    free(workflow_dir);
    free(repo_path);
    free(branch);
    free(workspace_path);
    return status;
}

static const DispatchWorkspace *
live_workspace_with_external_id(const DispatchBoard *board,
                                const char *external_id) {
    for (size_t i = 0; i < board->workspaces.count; i++) {
        const DispatchWorkspace *workspace = &board->workspaces.items[i];
        if (workspace_record_is_live(workspace) && workspace->external_id &&
            strcmp(workspace->external_id, external_id) == 0) {
            return workspace;
        }
    }
    return NULL;
}

/* Records a git worktree that something other than Dispatch created (for
 * example `orca worktree create`). Validates it like `workspace create`
 * validates its own reservation, but never runs `git worktree add`; see
 * docs/adr/0003-workspace-adopt.md. */
static int cmd_workspace_adopt(int argc, char **argv) {
    if (argc < 4) {
        print_workspace_adopt_usage();
        return 1;
    }

    const char *task_id = argv[3];
    const char *actor = NULL;
    const char *path_option = NULL;
    const char *branch_option = NULL;
    const char *external_option = NULL;
    int sequence = 0;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
            actor = argv[++i];
        } else if (strcmp(argv[i], "--path") == 0 && (i + 1) < argc) {
            path_option = argv[++i];
        } else if (strcmp(argv[i], "--branch") == 0 && (i + 1) < argc) {
            branch_option = argv[++i];
        } else if (strcmp(argv[i], "--external") == 0 && (i + 1) < argc) {
            external_option = argv[++i];
        } else if (strcmp(argv[i], "--sequence") == 0) {
            sequence = 1;
        } else {
            print_workspace_adopt_usage();
            return 1;
        }
    }

    if (!actor || actor[0] == '\0' || !path_option || path_option[0] == '\0' ||
        (branch_option && branch_option[0] == '\0')) {
        print_workspace_adopt_usage();
        return 1;
    }
    if (!dispatch_actor_label_is_valid(actor)) {
        cli_error("invalid_actor", NULL,
                  "Actor must start with an ASCII letter or digit and contain only letters, digits, '.', '_' or '-'\n");
        return 1;
    }
    if (external_option && !dispatch_external_id_is_valid(external_option)) {
        cli_error("invalid_external_id", task_id,
                  "External ID must be <tool>:<id>, with a lowercase tool name and no spaces: %s\n",
                  external_option);
        return 1;
    }

    char *workflow_dir = current_workflow_path();
    if (!workflow_dir)
        return 1;

    int status = 1;
    DispatchStringList sequence_tasks = {0};
    char *review_gate = NULL;
    char *repo_path = NULL;
    char *workspace_path = NULL;
    char *worktree_branch = NULL;
    const char *branch = NULL;
    int locked_open = 0;
    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        goto cleanup;
    locked_open = 1;
    DispatchBoard *board = &locked.board;

    DispatchAgent *actor_agent = dispatch_board_find_agent(board, actor);
    if (actor_agent && actor_agent->archived) {
        cli_error("agent_archived", NULL,
                  "Agent %s is archived; restore it first\n", actor);
        goto cleanup;
    }

    DispatchTask *task = dispatch_board_find_task(board, task_id);
    if (!task) {
        cli_error("task_not_found", task_id, "No task with id %s\n", task_id);
        goto cleanup;
    }
    if (dispatch_task_effective_state(board, task) != DISPATCH_STATE_READY ||
        task->assigned_to) {
        cli_error(task->assigned_to ? "already_assigned" : "invalid_state",
                  task_id, "Task %s must be ready and unassigned\n", task_id);
        goto cleanup;
    }
    if (live_workspace_for_task(board, task_id)) {
        cli_error("workspace_exists", task_id,
                  "Workspace already exists for %s\n", task_id);
        goto cleanup;
    }
    if (sequence &&
        !build_workspace_sequence(board, task, &sequence_tasks, &review_gate))
        goto cleanup;

    const char *configured_repo = board->repo_path;
    repo_path = dispatch_resolve_path(workflow_dir, configured_repo);
    if (!repo_path || !dispatch_path_is_git_repository(repo_path)) {
        cli_error("repo_not_git", task_id,
                  "Configured repository is not a git repository: %s\n",
                  configured_repo ? configured_repo : ".");
        goto cleanup;
    }

    workspace_path = dispatch_resolve_path(workflow_dir, path_option);
    struct stat info;
    if (!workspace_path || stat(workspace_path, &info) != 0 ||
        !S_ISDIR(info.st_mode)) {
        cli_error("invalid_path", task_id,
                  "Workspace path is not an existing directory: %s\n",
                  path_option);
        goto cleanup;
    }
    if (strcmp(repo_path, workspace_path) == 0) {
        cli_error("workspace_conflict", task_id,
                  "Workspace path must not equal repository path\n");
        goto cleanup;
    }
    if (workspace_path_exists(board, workspace_path)) {
        cli_error("workspace_conflict", task_id,
                  "Workspace path already reserved: %s\n", workspace_path);
        goto cleanup;
    }

    int registered = 0;
    if (!git_worktree_lookup(repo_path, workspace_path, &registered,
                             &worktree_branch)) {
        cli_error("git_failed", task_id,
                  "Could not list git worktrees of %s\n", repo_path);
        goto cleanup;
    }
    if (!registered) {
        cli_error("not_a_worktree", task_id,
                  "Workspace path is not a git worktree of %s: %s\n",
                  repo_path, workspace_path);
        goto cleanup;
    }

    if (branch_option) {
        if (worktree_branch && strcmp(worktree_branch, branch_option) != 0) {
            cli_error("branch_mismatch", task_id,
                      "Worktree %s has branch %s checked out, not %s\n",
                      workspace_path, worktree_branch, branch_option);
            goto cleanup;
        }
        branch = branch_option;
    } else if (worktree_branch) {
        branch = worktree_branch;
    } else {
        cli_error("branch_mismatch", task_id,
                  "Worktree %s has a detached HEAD; pass --branch to name its branch\n",
                  workspace_path);
        goto cleanup;
    }
    if (workspace_branch_exists(board, branch)) {
        cli_error("workspace_conflict", task_id,
                  "Workspace branch already reserved: %s\n", branch);
        goto cleanup;
    }
    if (external_option) {
        const DispatchWorkspace *other =
            live_workspace_with_external_id(board, external_option);
        if (other) {
            cli_error("workspace_conflict", task_id,
                      "External ID %s is already recorded for workspace %s\n",
                      external_option, other->id);
            goto cleanup;
        }
    }

    DispatchWorkspace *workspace = append_workspace_reservation(
        board, task_id, actor, repo_path, workspace_path, branch,
        sequence ? &sequence_tasks : NULL, review_gate);
    workspace->state = DISPATCH_WORKSPACE_ACTIVE;
    workspace->adopted = 1;
    workspace->external_id =
        external_option ? cli_strdup(external_option) : NULL;

    size_t note_size = strlen(workspace_path) + strlen(branch) +
                       (external_option ? strlen(external_option) : 0) + 64;
    char *note = malloc(note_size);
    if (!note) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    snprintf(note, note_size, "path=%s branch=%s%s%s", workspace_path, branch,
             external_option ? " external=" : "",
             external_option ? external_option : "");
    dispatch_task_append_history(task, actor, "workspace_adopt", note);
    free(note);

    if (!locked_board_save_or_error(&locked))
        goto cleanup;
    locked_board_close(&locked);
    locked_open = 0;

    int json = cli_json();
    if (!json) {
        printf("Adopted workspace %s for %s\n", task_id, actor);
        printf("  path: %s\n", workspace_path);
        printf("  branch: %s\n", branch);
        printf("  state: active\n");
        if (external_option)
            printf("  external: %s\n", external_option);
        if (sequence) {
            printf("  tasks:");
            for (size_t i = 0; i < sequence_tasks.count; i++)
                printf("%s%s", i == 0 ? " " : ",", sequence_tasks.items[i]);
            printf("\n");
            printf("  review gate: %s\n", review_gate ? review_gate : "-");
        }
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
        {"external_id", external_option ? external_option : ""},
    };
    char message[256];
    snprintf(message, sizeof(message), "Adopted workspace %s", task_id);
    append_dispatch_log(actor, "workspace", "workspace_adopt", targets, 2,
                        context, external_option ? 5 : 4, message);

    status = 0;
    if (json) {
        DispatchBoard final_board;
        if (!load_board_or_error(&final_board)) {
            status = 1;
        } else {
            DispatchWorkspace *adopted =
                dispatch_board_find_workspace(&final_board, task_id);
            DispatchJsonResult *json_result =
                dispatch_json_result_new("workspace_adopt");
            dispatch_json_result_add_target(json_result, task_id);
            dispatch_json_result_set_string(json_result, "workspace", task_id);
            dispatch_json_result_set_string(json_result, "external_id",
                                            external_option);
            status = cli_json_emit_workspaces(&final_board, adopted,
                                              adopted ? 1 : 0, json_result);
            dispatch_board_free(&final_board);
        }
    }

cleanup:
    if (locked_open)
        locked_board_close(&locked);
    string_list_free_cli(&sequence_tasks);
    free(review_gate);
    free(repo_path);
    free(workspace_path);
    free(worktree_branch);
    free(workflow_dir);
    return status;
}

int workspace_git_worktree_present_at(const char *path) {
    if (!path || path[0] == '\0')
        return 0;

    struct stat info;
    if (stat(path, &info) != 0 || !S_ISDIR(info.st_mode))
        return 0;

    char *git_path = join_path2(path, ".git");
    int present = stat(git_path, &info) == 0 &&
                  (S_ISDIR(info.st_mode) || S_ISREG(info.st_mode));
    free(git_path);
    return present;
}

static int workspace_git_worktree_present(const DispatchWorkspace *workspace) {
    return workspace && workspace_git_worktree_present_at(workspace->path);
}

static int cmd_workspace_list(int argc, char **argv) {
    (void)argv;
    int json_output = cli_json();
    if (argc != 3) {
        cli_error("usage", NULL, "Usage: dispatch workspace list [--json]\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (json_output) {
        int status = cli_json_finish_emit(dispatch_json_emit_workspaces(
            stdout, &board, "workspace list", NULL));
        dispatch_board_free(&board);
        return status;
    }

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
        printf("%-8s %-10s %-10s %-16s %s  %s", workspace->task_id,
               task_state, dispatch_workspace_state_name(workspace->state),
               workspace->actor, workspace->branch, workspace->path);
        if (workspace->adopted)
            printf("  [adopted%s%s]",
                   workspace->external_id ? " " : "",
                   workspace->external_id ? workspace->external_id : "");
        printf("\n");
    }

    dispatch_board_free(&board);
    return 0;
}

static int cmd_workspace_show(int argc, char **argv) {
    int json_output = cli_json();
    if (argc != 4) {
        cli_error("usage", NULL,
                  "Usage: dispatch workspace show <task-id-or-workspace> "
                  "[--json]\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchWorkspace *workspace = live_workspace_for_task(&board, argv[3]);
    if (!workspace || workspace->state == DISPATCH_WORKSPACE_REMOVED) {
        dispatch_board_free(&board);
        cli_error("workspace_not_found", NULL, "No workspace for %s\n",
                  argv[3]);
        return 1;
    }

    if (json_output) {
        int status = cli_json_finish_emit(dispatch_json_emit_workspaces(
            stdout, &board, "workspace show", workspace));
        dispatch_board_free(&board);
        return status;
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
    if (workspace->adopted) {
        printf("Origin: adopted\n");
        printf("External ID: %s\n",
               workspace->external_id ? workspace->external_id : "-");
    }
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
    if (argc < 4 || argc > 6) {
        print_workspace_remove_usage();
        return 1;
    }

    const char *target = argv[3];
    int force = 0;
    int delete_worktree_option = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0 && !force) {
            force = 1;
        } else if (strcmp(argv[i], "--delete-worktree") == 0 &&
                   !delete_worktree_option) {
            delete_worktree_option = 1;
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
        cli_error("workspace_not_found", NULL, "No workspace for %s\n",
                  target);
        return 1;
    }

    DispatchTask *task = dispatch_board_find_task(&locked.board,
                                                  workspace->task_id);
    DispatchState task_state = task ? dispatch_task_effective_state(&locked.board,
                                                                    task)
                                    : DISPATCH_STATE_PROPOSED;
    if (!force && task_state == DISPATCH_STATE_DOING) {
        cli_error("invalid_state", workspace->task_id,
                  "Workspace task %s is doing; use --force to remove\n",
                  workspace->task_id);
        locked_board_close(&locked);
        return 1;
    }

    /* An adopted worktree belongs to the tool that created it: removing the
     * record leaves it on disk unless --delete-worktree asks otherwise. */
    int adopted = workspace->adopted;
    int delete_worktree = !adopted || delete_worktree_option;
    char *record_task_id = cli_strdup(workspace->task_id);
    char *repo_path = cli_strdup(workspace->repo_path);
    char *workspace_path = cli_strdup(workspace->path);
    locked_board_close(&locked);

    if (delete_worktree &&
        !git_worktree_is_registered(repo_path, workspace_path)) {
        cli_error("not_a_worktree", record_task_id,
                  "Workspace path is not a git worktree: %s\n",
                  workspace_path);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    if (delete_worktree && !force && git_worktree_is_dirty(workspace_path)) {
        cli_error("workspace_dirty", record_task_id,
                  "Workspace has uncommitted changes: %s\n", workspace_path);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    int removed =
        !delete_worktree ||
        (force ? git_worktree_remove_force(repo_path, workspace_path)
               : git_worktree_remove(repo_path, workspace_path));
    if (!removed) {
        cli_error("git_failed", record_task_id,
                  "Could not remove git worktree: %s\n", workspace_path);
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
    DispatchWorkspace *current =
        dispatch_board_find_workspace(&locked.board, record_task_id);
    DispatchWorkspace removed_record = {0};
    int have_record = 0;
    if (cli_json() && current) {
        removed_record = workspace_clone(current);
        removed_record.state = DISPATCH_WORKSPACE_REMOVED;
        have_record = 1;
    }
    if (!remove_workspace_record(&locked.board, record_task_id)) {
        if (have_record)
            workspace_free_fields(&removed_record);
        locked_board_close(&locked);
        cli_error("workspace_not_found", record_task_id,
                  "Workspace record disappeared for %s\n", record_task_id);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }
    if (!locked_board_save_or_error(&locked)) {
        if (have_record)
            workspace_free_fields(&removed_record);
        locked_board_close(&locked);
        free(record_task_id);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    DispatchLogField targets[] = {
        {"task", record_task_id},
        {"workspace", record_task_id},
    };
    DispatchLogField context[] = {
        {"workspace_path", workspace_path},
        {"force", bool_string(force)},
        {"adopted", bool_string(adopted)},
        {"worktree_deleted", bool_string(delete_worktree)},
    };
    char message[256];
    snprintf(message, sizeof(message),
             delete_worktree ? "Removed workspace %s"
                             : "Removed workspace record %s",
             record_task_id);
    append_dispatch_log("user", "workspace", "workspace_remove", targets, 2,
                        context, adopted ? 4 : 2, message);

    int status = 0;
    if (cli_json()) {
        DispatchJsonResult *json_result =
            dispatch_json_result_new("workspace_remove");
        dispatch_json_result_add_target(json_result, record_task_id);
        dispatch_json_result_set_string(json_result, "workspace",
                                        record_task_id);
        dispatch_json_result_set_bool(json_result, "force", force);
        dispatch_json_result_set_bool(json_result, "worktree_deleted",
                                      delete_worktree);
        status = cli_json_emit_workspaces(&locked.board, &removed_record,
                                          have_record ? 1 : 0, json_result);
    } else if (delete_worktree) {
        printf("Removed workspace %s\n", record_task_id);
        printf("  path: %s\n", workspace_path);
    } else {
        printf("Removed workspace record %s; adopted worktree kept\n",
               record_task_id);
        printf("  path: %s\n", workspace_path);
    }
    if (have_record)
        workspace_free_fields(&removed_record);
    locked_board_close(&locked);
    free(record_task_id);
    free(repo_path);
    free(workspace_path);
    return status;
}

static int cmd_workspace_repair(int argc, char **argv) {
    if (argc != 4) {
        print_workspace_repair_usage();
        return 1;
    }

    const char *target = argv[3];
    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;

    DispatchWorkspace *workspace = live_workspace_for_task(&locked.board,
                                                           target);
    if (!workspace || workspace->state == DISPATCH_WORKSPACE_REMOVED) {
        locked_board_close(&locked);
        cli_error("workspace_not_found", NULL, "No workspace for %s\n", target);
        return 1;
    }
    if (workspace->adopted) {
        /* Repair re-anchors a worktree at Dispatch's default location next
         * to the repository, which an external tool's worktree need not
         * follow. */
        cli_error("workspace_adopted", workspace->task_id,
                  "Workspace %s was adopted; remove its record and adopt the worktree at its new path instead\n",
                  workspace->id);
        locked_board_close(&locked);
        return 1;
    }

    char *repo_path = dispatch_resolve_path(".", locked.board.repo_path);
    if (!repo_path || !dispatch_path_is_git_repository(repo_path)) {
        cli_error("repo_not_git", workspace->task_id,
                  "Configured repository is not a git repository: %s\n",
                  locked.board.repo_path ? locked.board.repo_path : ".");
        locked_board_close(&locked);
        free(repo_path);
        return 1;
    }

    char *workspace_path = workspace_reanchored_path(repo_path,
                                                     workspace->path);
    if (!workspace_path) {
        cli_error("invalid_path", workspace->task_id,
                  "Could not derive a repaired path for workspace %s\n",
                  workspace->id);
        locked_board_close(&locked);
        free(repo_path);
        return 1;
    }

    char *workspace_id = cli_strdup(workspace->id);
    char *workspace_task_id = cli_strdup(workspace->task_id);
    char *old_path = cli_strdup(workspace->path);
    char *old_repo_path = cli_strdup(workspace->repo_path);
    int already_anchored = strcmp(old_path, workspace_path) == 0 &&
                           strcmp(old_repo_path, repo_path) == 0;

    for (size_t i = 0; i < locked.board.workspaces.count; i++) {
        DispatchWorkspace *other = &locked.board.workspaces.items[i];
        if (other == workspace || !workspace_record_is_live(other))
            continue;
        if (strcmp(other->path, workspace_path) == 0) {
            locked_board_close(&locked);
            cli_error("workspace_conflict", workspace_task_id,
                      "Workspace path already reserved: %s\n", workspace_path);
            free(workspace_id);
            free(workspace_task_id);
            free(old_path);
            free(old_repo_path);
            free(repo_path);
            free(workspace_path);
            return 1;
        }
    }
    locked_board_close(&locked);

    if (!workspace_git_worktree_present_at(workspace_path)) {
        cli_error("not_a_worktree", workspace_task_id,
                  "No git worktree found at %s\n", workspace_path);
        free(workspace_id);
        free(workspace_task_id);
        free(old_path);
        free(old_repo_path);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    if (already_anchored) {
        int status = 0;
        if (cli_json()) {
            DispatchBoard board;
            if (!load_board_or_error(&board)) {
                status = 1;
            } else {
                DispatchWorkspace *current =
                    live_workspace_for_task(&board, workspace_id);
                DispatchJsonResult *json_result =
                    dispatch_json_result_new("workspace_repair");
                dispatch_json_result_add_target(json_result, workspace_task_id);
                dispatch_json_result_set_changed(json_result, 0);
                dispatch_json_result_set_string(json_result, "workspace",
                                                workspace_id);
                dispatch_json_result_set_string(json_result, "old_path",
                                                old_path);
                dispatch_json_result_set_string(json_result, "path",
                                                workspace_path);
                dispatch_json_result_set_string(json_result, "old_repo_path",
                                                old_repo_path);
                dispatch_json_result_set_string(json_result, "repo_path",
                                                repo_path);
                status = cli_json_emit_workspaces(&board, current,
                                                  current ? 1 : 0, json_result);
                dispatch_board_free(&board);
            }
        } else {
            printf("Workspace %s is already anchored at %s\n", workspace_id,
                   workspace_path);
        }
        free(workspace_id);
        free(workspace_task_id);
        free(old_path);
        free(old_repo_path);
        free(repo_path);
        free(workspace_path);
        return status;
    }

    if (!git_worktree_repair(repo_path, workspace_path) ||
        !git_worktree_is_registered(repo_path, workspace_path)) {
        cli_error("git_failed", workspace_task_id,
                  "Could not repair git worktree at %s\n", workspace_path);
        free(workspace_id);
        free(workspace_task_id);
        free(old_path);
        free(old_repo_path);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    if (!locked_board_load_or_error(&locked)) {
        free(workspace_id);
        free(workspace_task_id);
        free(old_path);
        free(old_repo_path);
        free(repo_path);
        free(workspace_path);
        return 1;
    }
    workspace = live_workspace_for_task(&locked.board, workspace_id);
    char *current_repo = dispatch_resolve_path(".", locked.board.repo_path);
    if (!workspace || strcmp(workspace->path, old_path) != 0 ||
        strcmp(workspace->repo_path, old_repo_path) != 0 || !current_repo ||
        strcmp(current_repo, repo_path) != 0) {
        locked_board_close(&locked);
        cli_error("workspace_conflict", workspace_task_id,
                  "Workspace %s changed while it was being repaired; retry\n",
                  workspace_id);
        free(current_repo);
        free(workspace_id);
        free(workspace_task_id);
        free(old_path);
        free(old_repo_path);
        free(repo_path);
        free(workspace_path);
        return 1;
    }
    free(current_repo);

    free(workspace->path);
    workspace->path = cli_strdup(workspace_path);
    free(workspace->repo_path);
    workspace->repo_path = cli_strdup(repo_path);
    workspace->updated_at = time(NULL);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        free(workspace_id);
        free(workspace_task_id);
        free(old_path);
        free(old_repo_path);
        free(repo_path);
        free(workspace_path);
        return 1;
    }

    if (!cli_json()) {
        printf("Repaired workspace %s\n", workspace_id);
        printf("  old path: %s\n", old_path);
        printf("  path: %s\n", workspace_path);
        printf("  repo: %s\n", repo_path);
    }
    DispatchLogField targets[] = {
        {"task", workspace_id},
        {"workspace", workspace_id},
    };
    DispatchLogField context[] = {
        {"old_workspace_path", old_path},
        {"workspace_path", workspace_path},
        {"old_repo_path", old_repo_path},
        {"repo_path", repo_path},
    };
    char message[256];
    snprintf(message, sizeof(message), "Repaired workspace %s", workspace_id);
    append_dispatch_log("user", "workspace", "workspace_repair", targets, 2,
                        context, 4, message);

    int status = 0;
    if (cli_json()) {
        DispatchJsonResult *json_result =
            dispatch_json_result_new("workspace_repair");
        dispatch_json_result_add_target(json_result, workspace_task_id);
        dispatch_json_result_set_string(json_result, "workspace", workspace_id);
        dispatch_json_result_set_string(json_result, "old_path", old_path);
        dispatch_json_result_set_string(json_result, "path", workspace_path);
        dispatch_json_result_set_string(json_result, "old_repo_path",
                                        old_repo_path);
        dispatch_json_result_set_string(json_result, "repo_path", repo_path);
        status = cli_json_emit_workspaces(&locked.board, workspace, 1,
                                          json_result);
    }
    locked_board_close(&locked);

    free(workspace_id);
    free(workspace_task_id);
    free(old_path);
    free(old_repo_path);
    free(repo_path);
    free(workspace_path);
    return status;
}

typedef enum {
    PRUNE_CANDIDATE_DONE,
    PRUNE_CANDIDATE_STALE,
    /* Adopted records: only the record is pruned, never the worktree. */
    PRUNE_CANDIDATE_ADOPTED_DONE,
    PRUNE_CANDIDATE_ADOPTED_STALE
} PruneCandidateKind;

typedef struct {
    char *task_id;
    char *repo_path;
    char *workspace_path;
    PruneCandidateKind kind;
    DispatchWorkspace record; /* copy reported by --json */
    int pruned;
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
    candidate->record = workspace_clone(workspace);
    candidate->pruned = 0;
}

static void prune_candidates_free(PruneCandidates *candidates) {
    for (size_t i = 0; i < candidates->count; i++) {
        free(candidates->items[i].task_id);
        free(candidates->items[i].repo_path);
        free(candidates->items[i].workspace_path);
        workspace_free_fields(&candidates->items[i].record);
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

        if (workspace->adopted) {
            /* The external tool owns the worktree. Its record is stale
             * once the worktree directory is gone, whatever git still
             * lists, and a done record is released without touching git. */
            if (prune_stale &&
                !workspace_git_worktree_present_at(workspace->path)) {
                prune_candidates_append(&candidates, workspace,
                                        PRUNE_CANDIDATE_ADOPTED_STALE);
            } else if (prune_done &&
                       workspace_all_tasks_done(&locked.board, workspace)) {
                prune_candidates_append(&candidates, workspace,
                                        PRUNE_CANDIDATE_ADOPTED_DONE);
            }
            continue;
        }

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

    int json = cli_json();
    DispatchJsonResult *json_result =
        json ? dispatch_json_result_new("workspace_prune") : NULL;
    DispatchStringList records_to_remove = {0};
    size_t pruned = 0;
    size_t skipped = 0;
    for (size_t i = 0; i < candidates.count; i++) {
        PruneCandidate *candidate = &candidates.items[i];
        if (candidate->kind == PRUNE_CANDIDATE_ADOPTED_STALE ||
            candidate->kind == PRUNE_CANDIDATE_ADOPTED_DONE) {
            if (!json) {
                if (candidate->kind == PRUNE_CANDIDATE_ADOPTED_STALE)
                    printf("%s stale adopted workspace %s: worktree is gone\n",
                           dry_run ? "Would prune" : "Pruned",
                           candidate->task_id);
                else
                    printf("%s done workspace record %s; adopted worktree kept\n",
                           dry_run ? "Would remove" : "Removed",
                           candidate->task_id);
            }
            pruned++;
            candidate->pruned = 1;
            if (!dry_run)
                string_list_append_cli(&records_to_remove,
                                       candidate->task_id);
            continue;
        }

        int registered = git_worktree_is_registered(candidate->repo_path,
                                                    candidate->workspace_path);

        if (candidate->kind == PRUNE_CANDIDATE_STALE) {
            if (registered) {
                if (json)
                    dispatch_json_result_add_skipped(
                        json_result, candidate->task_id, "worktree_exists");
                else
                    printf("Skipped stale workspace %s: git worktree exists\n",
                           candidate->task_id);
                skipped++;
                continue;
            }

            if (!json)
                printf("%s stale workspace %s\n",
                       dry_run ? "Would prune" : "Pruned", candidate->task_id);
            pruned++;
            candidate->pruned = 1;
            if (!dry_run)
                string_list_append_cli(&records_to_remove,
                                       candidate->task_id);
            continue;
        }

        if (!registered) {
            if (json)
                dispatch_json_result_add_skipped(
                    json_result, candidate->task_id, "not_a_worktree");
            else
                printf("Skipped done workspace %s: path is not a git worktree\n",
                       candidate->task_id);
            skipped++;
            continue;
        }

        if (git_worktree_is_dirty(candidate->workspace_path)) {
            if (json)
                dispatch_json_result_add_skipped(json_result,
                                                 candidate->task_id, "dirty");
            else
                printf("Skipped done workspace %s: workspace has uncommitted changes\n",
                       candidate->task_id);
            skipped++;
            continue;
        }

        if (dry_run) {
            if (!json)
                printf("Would remove done workspace %s\n",
                       candidate->task_id);
            pruned++;
            candidate->pruned = 1;
            continue;
        }

        if (!git_worktree_remove(candidate->repo_path,
                                 candidate->workspace_path)) {
            if (json)
                dispatch_json_result_add_skipped(
                    json_result, candidate->task_id, "remove_failed");
            else
                printf("Skipped done workspace %s: could not remove git worktree\n",
                       candidate->task_id);
            skipped++;
            continue;
        }

        if (!json)
            printf("Removed done workspace %s\n", candidate->task_id);
        pruned++;
        candidate->pruned = 1;
        string_list_append_cli(&records_to_remove, candidate->task_id);
    }

    int ok = dry_run || remove_workspace_records_by_id(&records_to_remove);
    if (!json && pruned == 0 && skipped == 0)
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

    int status = ok ? 0 : 1;
    if (json && ok) {
        /* Report pruned records (removed unless this is a dry run) against
         * the board as it is after pruning. */
        DispatchWorkspace *records =
            cli_realloc_array(NULL, candidates.count + 1, sizeof(*records));
        size_t record_count = 0;
        for (size_t i = 0; i < candidates.count; i++) {
            if (!candidates.items[i].pruned)
                continue;
            records[record_count] = candidates.items[i].record;
            if (!dry_run)
                records[record_count].state = DISPATCH_WORKSPACE_REMOVED;
            record_count++;
        }
        dispatch_json_result_set_changed(json_result, !dry_run && pruned > 0);
        dispatch_json_result_set_bool(json_result, "dry_run", dry_run);
        DispatchBoard final_board;
        if (load_board_or_error(&final_board)) {
            status = cli_json_emit_workspaces(&final_board, records,
                                              record_count, json_result);
            json_result = NULL;
            dispatch_board_free(&final_board);
        } else {
            status = 1;
        }
        free(records);
    }
    dispatch_json_result_free(json_result);

    string_list_free_cli(&records_to_remove);
    prune_candidates_free(&candidates);
    return status;
}

int cmd_workspace(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "create") == 0)
        return cmd_workspace_create(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "adopt") == 0)
        return cmd_workspace_adopt(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "list") == 0)
        return cmd_workspace_list(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "show") == 0)
        return cmd_workspace_show(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "remove") == 0)
        return cmd_workspace_remove(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "repair") == 0)
        return cmd_workspace_repair(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "prune") == 0)
        return cmd_workspace_prune(argc, argv);

    print_workspace_create_usage();
    fprintf(stderr,
            "       dispatch workspace adopt <task-id> --actor <name> --path <dir> [--branch <name>] [--sequence] [--external <tool>:<id>]\n");
    fprintf(stderr, "       dispatch workspace list\n");
    fprintf(stderr, "       dispatch workspace show <task-id-or-workspace>\n");
    fprintf(stderr,
            "       dispatch workspace remove <task-id-or-workspace> [--force] [--delete-worktree]\n");
    fprintf(stderr,
            "       dispatch workspace repair <task-id-or-workspace>\n");
    fprintf(stderr,
            "       dispatch workspace prune [--done] [--stale] [--dry-run]\n");
    return 1;
}
