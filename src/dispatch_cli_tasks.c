#include "dispatch_cli_internal.h"

#include "dispatch_gh.h"
#include "dispatch_issue.h"

static void print_invalid_issue_ref(const char *input) {
    cli_error("invalid_issue", NULL,
              "Invalid issue reference '%s'; use #123, 123, owner/repo#123 or "
              "https://github.com/owner/repo/issues/123\n",
              input);
}

static int issue_ref_is_well_formed(const char *input) {
    DispatchIssueRef ref;
    if (dispatch_issue_parse(input, &ref) == DISPATCH_ISSUE_PARSE_INVALID) {
        print_invalid_issue_ref(input);
        return 0;
    }
    return 1;
}

/* Turns --issue / --from-issue input into a resolved DispatchIssueRef. A bare
 * number resolves against the board repo's origin remote; reading the remote
 * is a local git call, never the network. */
static int resolve_issue_ref_struct(const DispatchBoard *board,
                                    const char *input,
                                    DispatchIssueRef *out_ref) {
    DispatchIssueRef ref = {0};
    DispatchIssueParse parsed = dispatch_issue_parse(input, &ref);
    if (parsed == DISPATCH_ISSUE_PARSE_INVALID) {
        print_invalid_issue_ref(input);
        return 0;
    }

    if (parsed == DISPATCH_ISSUE_PARSE_NEEDS_REPO) {
        const char *repo_path = board->repo_path ? board->repo_path : ".";
        const char *argv[] = {"git",     "-C",     repo_path, "remote",
                              "get-url", "origin", NULL};
        DispatchExecOptions options = {
            .stdin_mode = DISPATCH_EXEC_DEV_NULL,
            .stderr_mode = DISPATCH_EXEC_DEV_NULL,
        };
        DispatchExecResult result;
        char *remote = NULL;
        int captured =
            dispatch_exec_capture(argv, &options, &remote, NULL, &result) &&
            dispatch_exec_result_success(&result);
        char *trimmed = trimmed_copy(captured && remote ? remote : "");
        free(remote);
        if (!trimmed[0]) {
            cli_error("issue_unresolvable", NULL,
                      "Cannot resolve issue #%ld: the board repo has no origin "
                      "remote; use owner/repo#%ld\n",
                      ref.number, ref.number);
            free(trimmed);
            return 0;
        }
        char owner_repo[DISPATCH_ISSUE_TEXT_MAX];
        if (!dispatch_issue_repo_from_remote(trimmed, owner_repo,
                                             sizeof(owner_repo)) ||
            !dispatch_issue_set_repo(&ref, owner_repo)) {
            cli_error("issue_unresolvable", NULL,
                      "Cannot resolve issue #%ld: origin remote %s is not a "
                      "GitHub repository; use owner/repo#%ld\n",
                      ref.number, trimmed, ref.number);
            free(trimmed);
            return 0;
        }
        free(trimmed);
    }

    *out_ref = ref;
    return 1;
}

static int resolve_issue_ref(const DispatchBoard *board, const char *input,
                             char *canonical, size_t canonical_size) {
    DispatchIssueRef ref;
    if (!resolve_issue_ref_struct(board, input, &ref))
        return 0;
    dispatch_issue_format(&ref, canonical, canonical_size);
    return 1;
}

static void report_gh_issue_error(DispatchGhStatus status,
                                  const char *requested_ref,
                                  const DispatchGhResult *gh_result) {
    const char *code = dispatch_gh_error_code(status);
    switch (status) {
    case DISPATCH_GH_MISSING:
        cli_error(code, NULL,
                  "GitHub CLI (gh) is not installed or not on PATH; install "
                  "gh and run 'gh auth login'\n");
        break;
    case DISPATCH_GH_NOT_AUTHENTICATED:
        cli_error(code, NULL,
                  "GitHub CLI (gh) is not logged in; run 'gh auth login'\n");
        break;
    case DISPATCH_GH_NOT_FOUND:
        cli_error(code, NULL, "GitHub issue %s was not found\n",
                  requested_ref);
        break;
    case DISPATCH_GH_NOT_AN_ISSUE:
        cli_error(code, NULL,
                  "GitHub reference %s is a pull request, not an issue (%s)\n",
                  requested_ref, gh_result->detail);
        break;
    case DISPATCH_GH_INVALID_JSON:
        cli_error(code, NULL, "Could not read GitHub issue %s from gh: %s\n",
                  requested_ref,
                  gh_result->detail[0] ? gh_result->detail
                                       : "unexpected JSON output");
        break;
    case DISPATCH_GH_FAILED:
    case DISPATCH_GH_OK:
        cli_error(code, NULL, "Could not fetch GitHub issue %s with gh: %s\n",
                  requested_ref,
                  gh_result->detail[0] ? gh_result->detail
                                       : "gh command failed");
        break;
    }
}

static void print_task_usage(FILE *stream) {
    fprintf(stream,
            "Usage: dispatch task add <group> <title> [--description <text>] "
            "[--actor <name>] [--no-review] [--issue <ref>]\n");
    fprintf(stream,
            "       dispatch task add <group> --from-issue <ref> "
            "[--actor <name>] [--no-review]\n");
    fprintf(stream,
            "       dispatch task edit <id> [--title <text>] [--description "
            "<text>] [--issue <ref>|--clear-issue] [--actor <name>]\n");
    fprintf(stream,
            "       dispatch task move <id> <group> [--actor <name>]\n");
    fprintf(stream, "       dispatch task delete <id> [--force]\n");
    fprintf(stream,
            "       dispatch task priority <id> <value> [--actor <name>]\n");
}

static void print_task_usage_error(void) {
    cli_error("usage", NULL,
              "Usage: dispatch task add <group> <title> [--description <text>] "
              "[--actor <name>] [--no-review] [--issue <ref>]\n");
    cli_error("usage", NULL,
              "       dispatch task add <group> --from-issue <ref> "
              "[--actor <name>] [--no-review]\n");
    cli_error("usage", NULL,
              "       dispatch task edit <id> [--title <text>] [--description "
              "<text>] [--issue <ref>|--clear-issue] [--actor <name>]\n");
    cli_error("usage", NULL,
              "       dispatch task move <id> <group> [--actor <name>]\n");
    cli_error("usage", NULL, "       dispatch task delete <id> [--force]\n");
    cli_error("usage", NULL,
              "       dispatch task priority <id> <value> [--actor <name>]\n");
}

static void log_issue_change(const char *actor, const char *task_id,
                             int linked, const char *issue) {
    DispatchLogField targets[] = {
        {"task", task_id},
    };
    DispatchLogField context[] = {
        {"issue", issue},
    };
    char message[256];
    if (linked)
        snprintf(message, sizeof(message), "Linked %s to %s", task_id, issue);
    else
        snprintf(message, sizeof(message), "Unlinked %s from %s", issue,
                 task_id);
    append_dispatch_log(actor, "task", linked ? "issue_link" : "issue_unlink",
                        targets, 1, context, 1, message);
}

int cmd_repo(int argc, char **argv) {
    if (argc == 2) {
        DispatchBoard board;
        if (!load_board_or_error(&board))
            return 1;
        printf("Repo: %s\n", board.repo_path ? board.repo_path : ".");
        if (!board.repo_path ||
            !dispatch_path_is_git_repository(board.repo_path)) {
            printf("Warning: repo path is not a git repository\n");
            printf("Repair it with: dispatch repo set <path>\n");
        }
        dispatch_board_free(&board);
        return 0;
    }

    if (argc != 4 || strcmp(argv[2], "set") != 0) {
        fprintf(stderr, "Usage: dispatch repo [set <path>]\n");
        return 1;
    }

    const char *repo_arg = argv[3];
    char *repo_path = dispatch_resolve_path(".", repo_arg);
    if (!repo_path) {
        fprintf(stderr, "Repo path %s does not exist\n", repo_arg);
        return 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked)) {
        free(repo_path);
        return 1;
    }

    dispatch_board_set_repo_path(&locked.board, repo_path);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        free(repo_path);
        return 1;
    }

    printf("Set repo path to %s\n", repo_path);
    if (!dispatch_path_is_git_repository(repo_path))
        printf("Warning: %s is not a git repository\n", repo_path);
    DispatchLogField targets[] = {
        {"repo_path", repo_path},
    };
    append_dispatch_log("user", "repo", "set", targets, 1, NULL, 0,
                        "Set repo path");
    locked_board_close(&locked);
    free(repo_path);
    return 0;
}

/* Why dispatch_board_add_group refused a group, for the JSON error code. */
static const char *group_add_failure_code(const char *name,
                                          const char *prefix) {
    if (!name[0] || strlen(name) > DISPATCH_GROUP_NAME_MAX)
        return "invalid_group";
    if (prefix && prefix[0]) {
        char *upper = cli_strdup(prefix);
        for (char *p = upper; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        int valid = dispatch_group_prefix_is_valid(upper);
        free(upper);
        if (!valid)
            return "invalid_prefix";
    }
    return "group_exists";
}

static int cmd_group_add(int argc, char **argv) {
    if (argc < 4) {
        cli_error("usage", NULL,
                  "Usage: dispatch group add <name> [--prefix XX] "
                  "[--description <text>]\n");
        return 1;
    }

    const char *name = argv[3];
    const char *prefix = NULL;
    const char *description = NULL;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--prefix") == 0 && (i + 1) < argc) {
            prefix = argv[++i];
        } else if (strcmp(argv[i], "--description") == 0 && (i + 1) < argc) {
            description = argv[++i];
        } else {
            cli_error("usage", NULL, "Unknown group option: %s\n", argv[i]);
            return 1;
        }
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    if (!dispatch_board_add_group(board, name, prefix)) {
        const char *code = group_add_failure_code(name, prefix);
        locked_board_close(&locked);
        cli_error(code, NULL, "Could not add group '%s'\n", name);
        return 1;
    }

    DispatchGroup *group = dispatch_board_find_group(board, name);
    if (description)
        dispatch_group_set_description(group, description);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    if (!cli_json())
        printf("Added group %s (%s)\n", group->name, group->prefix);
    DispatchLogField targets[] = {
        {"group", group->id},
    };
    DispatchLogField context[] = {
        {"name", group->name},
        {"prefix", group->prefix},
    };
    char message[256];
    snprintf(message, sizeof(message), "Added group %s", group->name);
    append_dispatch_log("user", "group", "add", targets, 1, context, 2,
                        message);
    int status = 0;
    if (cli_json()) {
        DispatchJsonResult *json_result = dispatch_json_result_new("group_add");
        dispatch_json_result_set_string(json_result, "group", group->id);
        status = cli_json_emit_tasks(board, group->id, json_result);
    }
    locked_board_close(&locked);
    return status;
}

static int cmd_group_ready(int argc, char **argv) {
    if (argc < 4) {
        cli_error("usage", NULL,
                  "Usage: dispatch group ready <group> [--actor <name>] [--no-review]\n");
        return 1;
    }

    const char *group_id = argv[3];
    const char *actor = "user";
    int no_review = 0;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
            actor = argv[++i];
        } else if (strcmp(argv[i], "--no-review") == 0) {
            no_review = 1;
        } else {
            cli_error("usage", NULL, "Unknown group ready option: %s\n",
                      argv[i]);
            return 1;
        }
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchGroup *group = dispatch_board_find_group(board, group_id);
    if (!group) {
        locked_board_close(&locked);
        cli_error("group_not_found", NULL,
                  "No group with id, prefix, or name %s\n", group_id);
        return 1;
    }

    DispatchJsonResult *json_result =
        cli_json() ? dispatch_json_result_new("group_ready") : NULL;
    int readied = 0;
    int modified = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        DispatchTask *task = &board->tasks.items[i];
        if (strcmp(task->group, group->id) != 0)
            continue;

        if (task->state == DISPATCH_STATE_PROPOSED) {
            dispatch_task_mark_ready(board, task, actor);
            if (no_review)
                task->requires_review = 0;
            readied++;
            modified++;
            if (json_result)
                dispatch_json_result_add_target(json_result, task->id);
            continue;
        }

        if (no_review &&
            (task->state == DISPATCH_STATE_READY ||
             task->state == DISPATCH_STATE_BLOCKED) &&
            !task->assigned_to && !task->started_by) {
            if (task->requires_review) {
                modified++;
                if (json_result)
                    dispatch_json_result_add_target(json_result, task->id);
            }
            task->requires_review = 0;
        }
    }

    dispatch_board_normalize_states(board);
    if (!locked_board_save_or_error(&locked)) {
        dispatch_json_result_free(json_result);
        locked_board_close(&locked);
        return 1;
    }

    if (!json_result)
        printf("Readied %d task%s in group %s\n", readied,
               readied == 1 ? "" : "s", group->prefix);
    DispatchLogField targets[] = {
        {"group", group->id},
    };
    char readied_value[32];
    snprintf(readied_value, sizeof(readied_value), "%d", readied);
    DispatchLogField context[] = {
        {"readied", readied_value},
        {"no_review", bool_string(no_review)},
    };
    char message[256];
    snprintf(message, sizeof(message), "Readied %d task%s in group %s",
             readied, readied == 1 ? "" : "s", group->prefix);
    append_dispatch_log(actor, "group", "ready", targets, 1, context, 2,
                        message);
    int status = 0;
    if (json_result) {
        dispatch_json_result_set_changed(json_result, modified > 0);
        dispatch_json_result_set_string(json_result, "group", group->id);
        dispatch_json_result_set_bool(json_result, "no_review", no_review);
        status = cli_json_emit_tasks(board, group->id, json_result);
    }
    locked_board_close(&locked);
    return status;
}

static int cmd_group_edit(int argc, char **argv) {
    if (argc != 6 || strcmp(argv[4], "--description") != 0) {
        fprintf(stderr,
                "Usage: dispatch group edit <prefix> --description <text>\n");
        return 1;
    }

    const char *group_id = argv[3];
    const char *description = argv[5];

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    DispatchGroup *group = dispatch_board_find_group(board, group_id);
    if (!group) {
        locked_board_close(&locked);
        fprintf(stderr, "No group with id, prefix, or name %s\n", group_id);
        return 1;
    }

    dispatch_group_set_description(group, description);
    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    printf("Updated group %s description\n", group->prefix);
    DispatchLogField targets[] = {
        {"group", group->id},
    };
    DispatchLogField context[] = {
        {"description", description},
    };
    char message[256];
    snprintf(message, sizeof(message), "Updated group %s description",
             group->prefix);
    append_dispatch_log("user", "group", "edit", targets, 1, context, 1,
                        message);
    locked_board_close(&locked);
    return 0;
}

static void group_task_counts(const DispatchBoard *board,
                              const DispatchGroup *group, size_t *by_state,
                              size_t *total) {
    *total = 0;
    for (int state = 0; state <= DISPATCH_STATE_PAUSED; state++)
        by_state[state] = 0;
    for (size_t i = 0; i < board->tasks.count; i++) {
        const DispatchTask *task = &board->tasks.items[i];
        if (strcmp(task->group, group->id) != 0)
            continue;
        by_state[dispatch_task_effective_state(board, task)]++;
        (*total)++;
    }
}

static int cmd_group_list(int argc, char **argv) {
    (void)argv;
    if (argc != 3) {
        fprintf(stderr, "Usage: dispatch group list\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (board.groups.count == 0) {
        printf("(no groups)\n");
        dispatch_board_free(&board);
        return 0;
    }

    for (size_t i = 0; i < board.groups.count; i++) {
        const DispatchGroup *group = &board.groups.items[i];
        size_t by_state[DISPATCH_STATE_PAUSED + 1];
        size_t total = 0;
        group_task_counts(&board, group, by_state, &total);
        size_t open = total - by_state[DISPATCH_STATE_DONE];
        printf("%-4s %-24s %2zu open / %2zu tasks  %s\n", group->prefix,
               group->name, open, total,
               group->description && group->description[0]
                   ? group->description
                   : "-");
    }

    dispatch_board_free(&board);
    return 0;
}

static int cmd_group_show(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: dispatch group show <prefix>\n");
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchGroup *group = dispatch_board_find_group(&board, argv[3]);
    if (!group) {
        dispatch_board_free(&board);
        fprintf(stderr, "No group with id, prefix, or name %s\n", argv[3]);
        return 1;
    }

    size_t by_state[DISPATCH_STATE_PAUSED + 1];
    size_t total = 0;
    group_task_counts(&board, group, by_state, &total);

    printf("Group: %s\n", group->name);
    printf("Prefix: %s\n", group->prefix);
    printf("Description: %s\n",
           group->description && group->description[0] ? group->description
                                                       : "-");
    printf("Tasks: %zu total  proposed:%zu ready:%zu blocked:%zu doing:%zu "
           "review:%zu done:%zu paused:%zu\n",
           total, by_state[DISPATCH_STATE_PROPOSED],
           by_state[DISPATCH_STATE_READY], by_state[DISPATCH_STATE_BLOCKED],
           by_state[DISPATCH_STATE_DOING], by_state[DISPATCH_STATE_REVIEW],
           by_state[DISPATCH_STATE_DONE], by_state[DISPATCH_STATE_PAUSED]);
    if (by_state[DISPATCH_STATE_READY] > 0) {
        printf("Ready:\n");
        for (size_t i = 0; i < board.tasks.count; i++) {
            const DispatchTask *task = &board.tasks.items[i];
            if (strcmp(task->group, group->id) != 0 ||
                dispatch_task_effective_state(&board, task) !=
                    DISPATCH_STATE_READY)
                continue;
            printf("  %-8s %s\n", task->id, task_display_title(task));
        }
    }

    dispatch_board_free(&board);
    return 0;
}

int cmd_group(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "add") == 0)
        return cmd_group_add(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "ready") == 0)
        return cmd_group_ready(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "edit") == 0)
        return cmd_group_edit(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "list") == 0)
        return cmd_group_list(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "show") == 0)
        return cmd_group_show(argc, argv);

    fprintf(stderr, "Usage: dispatch group add <name> [--prefix XX] "
                    "[--description <text>]\n");
    fprintf(stderr,
            "       dispatch group ready <group> [--actor <name>] [--no-review]\n");
    fprintf(stderr,
            "       dispatch group edit <prefix> --description <text>\n");
    fprintf(stderr, "       dispatch group list\n");
    fprintf(stderr, "       dispatch group show <prefix>\n");
    return 1;
}

int cmd_task(int argc, char **argv) {
    if (!cli_json() && argc == 3 &&
        (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        print_task_usage(stdout);
        return 0;
    }
    if (!cli_json() && argc == 4 && strcmp(argv[2], "add") == 0 &&
        (strcmp(argv[3], "--help") == 0 || strcmp(argv[3], "-h") == 0)) {
        fprintf(stdout,
                "Usage: dispatch task add <group> <title> [--description "
                "<text>] [--actor <name>] [--no-review] [--issue <ref>]\n"
                "       dispatch task add <group> --from-issue <ref> "
                "[--actor <name>] [--no-review]\n");
        return 0;
    }

    if (argc >= 4 && strcmp(argv[2], "edit") == 0) {
        const char *task_id = argv[3];
        const char *title = NULL;
        const char *description = NULL;
        const char *issue = NULL;
        int clear_issue = 0;
        const char *actor = "user";

        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--title") == 0 && (i + 1) < argc) {
                title = argv[++i];
            } else if (strcmp(argv[i], "--description") == 0 &&
                       (i + 1) < argc) {
                description = argv[++i];
            } else if (strcmp(argv[i], "--issue") == 0 && (i + 1) < argc) {
                issue = argv[++i];
            } else if (strcmp(argv[i], "--clear-issue") == 0) {
                clear_issue = 1;
            } else if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
                actor = argv[++i];
            } else {
                cli_error("usage", NULL, "Unknown task edit option: %s\n",
                          argv[i]);
                return 1;
            }
        }

        if (!title && !description && !issue && !clear_issue) {
            cli_error("usage", NULL,
                      "Task edit requires --title, --description, "
                      "--issue, or --clear-issue\n");
            return 1;
        }
        if (issue && clear_issue) {
            cli_error("usage", NULL,
                      "Use either --issue or --clear-issue, not both\n");
            return 1;
        }
        if (issue && !issue_ref_is_well_formed(issue))
            return 1;
        if (title && title[0] == '\0') {
            cli_error("invalid_title", NULL, "Task title must not be empty\n");
            return 1;
        }
        if (title && title_starts_with_dispatch_id(title)) {
            cli_error("invalid_title", NULL,
                      "Task titles should not include Dispatch IDs; use a human-readable title without a prefix like DE-01\n");
            return 1;
        }
        if (!dispatch_actor_label_is_valid(actor)) {
            cli_error("invalid_actor", NULL,
                      "Actor must start with an ASCII letter or digit and contain only letters, digits, '.', '_' or '-'\n");
            return 1;
        }

        LockedBoard locked;
        if (!locked_board_load_or_error(&locked))
            return 1;
        DispatchTask *task =
            dispatch_board_find_task(&locked.board, task_id);
        if (!task) {
            locked_board_close(&locked);
            cli_error("task_not_found", task_id, "No task with id %s\n",
                      task_id);
            return 1;
        }

        char canonical[DISPATCH_ISSUE_TEXT_MAX] = "";
        if (issue && !resolve_issue_ref(&locked.board, issue, canonical,
                                        sizeof(canonical))) {
            locked_board_close(&locked);
            return 1;
        }

        int edited = title || description;
        if ((title && !dispatch_task_set_title(task, title)) ||
            (description &&
             !dispatch_task_set_description(task, description))) {
            locked_board_close(&locked);
            cli_error("internal_error", task_id, "Could not edit task %s\n",
                      task_id);
            return 1;
        }

        const char *note = title && description
                               ? "title and description"
                               : title ? "title" : "description";
        if (edited &&
            !dispatch_task_append_history(task, actor, "edited", note)) {
            locked_board_close(&locked);
            return 1;
        }

        /* One issue per task: linking a different issue unlinks the old one
         * first, so history pairs every link with its unlink. */
        char previous_issue[DISPATCH_ISSUE_TEXT_MAX] = "";
        int unlinked = 0;
        int linked = 0;
        if (task->issue &&
            (clear_issue || (issue && strcmp(task->issue, canonical) != 0))) {
            snprintf(previous_issue, sizeof(previous_issue), "%s",
                     task->issue);
            if (!dispatch_task_unlink_issue(task, actor)) {
                locked_board_close(&locked);
                return 1;
            }
            unlinked = 1;
        }
        if (issue && !task->issue) {
            if (!dispatch_task_link_issue(task, canonical, actor)) {
                locked_board_close(&locked);
                return 1;
            }
            linked = 1;
        }

        if ((edited || unlinked || linked) &&
            !locked_board_save_or_error(&locked)) {
            locked_board_close(&locked);
            return 1;
        }

        int json = cli_json();
        if (edited) {
            if (!json)
                printf("Edited task %s\n", task_id);
            DispatchLogField targets[] = {
                {"task", task_id},
            };
            DispatchLogField context[] = {
                {"title_changed", bool_string(title != NULL)},
                {"description_changed", bool_string(description != NULL)},
                {"title", title ? title : ""},
                {"description", description ? description : ""},
            };
            char message[256];
            snprintf(message, sizeof(message), "Edited task %s", task_id);
            append_dispatch_log(actor, "task", "edit", targets, 1, context, 4,
                                message);
        }
        if (unlinked) {
            if (!json)
                printf("Unlinked %s from %s\n", previous_issue, task_id);
            log_issue_change(actor, task_id, 0, previous_issue);
        }
        if (linked) {
            if (!json)
                printf("Linked %s to %s\n", task_id, canonical);
            log_issue_change(actor, task_id, 1, canonical);
        }
        if (!json && issue && !linked)
            printf("Task %s is already linked to %s\n", task_id, canonical);
        if (!json && clear_issue && !unlinked)
            printf("Task %s has no linked issue\n", task_id);
        int status = 0;
        if (json) {
            DispatchJsonResult *json_result =
                dispatch_json_result_new("task_edit");
            dispatch_json_result_add_target(json_result, task_id);
            dispatch_json_result_set_changed(json_result,
                                             edited || unlinked || linked);
            status = cli_json_emit_tasks(&locked.board, NULL, json_result);
        }
        locked_board_close(&locked);
        return status;
    }

    if (argc >= 5 && strcmp(argv[2], "move") == 0) {
        const char *task_id = argv[3];
        const char *group_id = argv[4];
        const char *actor = "user";

        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
                actor = argv[++i];
            } else {
                cli_error("usage", NULL, "Unknown task move option: %s\n",
                          argv[i]);
                return 1;
            }
        }
        if (!dispatch_actor_label_is_valid(actor)) {
            cli_error("invalid_actor", NULL,
                      "Actor must start with an ASCII letter or digit and contain only letters, digits, '.', '_' or '-'\n");
            return 1;
        }

        LockedBoard locked;
        if (!locked_board_load_or_error(&locked))
            return 1;
        DispatchTask *task =
            dispatch_board_find_task(&locked.board, task_id);
        if (!task) {
            locked_board_close(&locked);
            cli_error("task_not_found", task_id, "No task with id %s\n",
                      task_id);
            return 1;
        }
        DispatchGroup *group =
            dispatch_board_find_group(&locked.board, group_id);
        if (!group) {
            locked_board_close(&locked);
            cli_error("group_not_found", task_id,
                      "No group with id, prefix, or name %s\n", group_id);
            return 1;
        }
        if (strcmp(task->group, group->id) == 0) {
            cli_error("already_in_group", task_id,
                      "Task %s already belongs to group %s\n", task_id,
                      group->id);
            locked_board_close(&locked);
            return 1;
        }

        char *from_group = cli_strdup(task->group);
        char *to_group = cli_strdup(group->id);
        if (!dispatch_task_move_to_group(&locked.board, task, to_group, actor) ||
            !locked_board_save_or_error(&locked)) {
            free(from_group);
            free(to_group);
            locked_board_close(&locked);
            return 1;
        }

        if (!cli_json())
            printf("Moved task %s from %s to %s (ID unchanged)\n", task_id,
                   from_group, to_group);
        DispatchLogField targets[] = {
            {"task", task_id},
            {"group", to_group},
        };
        DispatchLogField context[] = {
            {"from_group", from_group},
            {"id_changed", "false"},
        };
        char message[256];
        snprintf(message, sizeof(message), "Moved task %s from %s to %s",
                 task_id, from_group, to_group);
        append_dispatch_log(actor, "task", "move", targets, 2, context, 2,
                            message);
        int status = 0;
        if (cli_json()) {
            DispatchJsonResult *json_result =
                dispatch_json_result_new("task_move");
            dispatch_json_result_add_target(json_result, task_id);
            dispatch_json_result_set_string(json_result, "from_group",
                                            from_group);
            dispatch_json_result_set_string(json_result, "to_group", to_group);
            status = cli_json_emit_tasks(&locked.board, NULL, json_result);
        }
        free(from_group);
        free(to_group);
        locked_board_close(&locked);
        return status;
    }

    if (argc >= 4 && strcmp(argv[2], "delete") == 0) {
        const char *task_id = argv[3];
        int force = 0;
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--force") == 0) {
                force = 1;
            } else {
                cli_error("usage", NULL, "Unknown task delete option: %s\n",
                          argv[i]);
                return 1;
            }
        }

        LockedBoard locked;
        if (!locked_board_load_or_error(&locked))
            return 1;
        int exists = dispatch_board_find_task(&locked.board, task_id) != NULL;
        if (!dispatch_board_delete_task(&locked.board, task_id, force)) {
            locked_board_close(&locked);
            cli_error(exists ? "has_dependents" : "task_not_found", task_id,
                      "Could not delete %s%s\n",
                      task_id,
                      force ? "" : " (task may have dependents; use --force)");
            return 1;
        }
        if (!locked_board_save_or_error(&locked)) {
            locked_board_close(&locked);
            return 1;
        }
        if (!cli_json())
            printf("Deleted task %s\n", task_id);
        DispatchLogField targets[] = {
            {"task", task_id},
        };
        DispatchLogField context[] = {
            {"force", bool_string(force)},
        };
        char message[256];
        snprintf(message, sizeof(message), "Deleted task %s", task_id);
        append_dispatch_log("user", "task", "delete", targets, 1, context, 1,
                            message);
        int status = 0;
        if (cli_json()) {
            DispatchJsonResult *json_result =
                dispatch_json_result_new("task_delete");
            dispatch_json_result_add_target(json_result, task_id);
            dispatch_json_result_set_bool(json_result, "force", force);
            status = cli_json_emit_tasks(&locked.board, NULL, json_result);
        }
        locked_board_close(&locked);
        return status;
    }

    if (argc >= 5 && strcmp(argv[2], "priority") == 0) {
        const char *task_id = argv[3];
        char *end = NULL;
        long value = strtol(argv[4], &end, 10);
        if (!end || *end != '\0' || value < -99 || value > 99) {
            fprintf(stderr, "Priority must be an integer between -99 and 99\n");
            return 1;
        }
        const char *actor = "user";
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
                actor = argv[++i];
            } else {
                fprintf(stderr, "Unknown task priority option: %s\n", argv[i]);
                return 1;
            }
        }

        LockedBoard locked;
        if (!locked_board_load_or_error(&locked))
            return 1;
        DispatchTask *task = dispatch_board_find_task(&locked.board, task_id);
        if (!task || !dispatch_task_set_priority(task, (int)value, actor)) {
            locked_board_close(&locked);
            fprintf(stderr, "No task with id %s\n", task_id);
            return 1;
        }
        if (!locked_board_save_or_error(&locked)) {
            locked_board_close(&locked);
            return 1;
        }
        printf("Set priority of %s to %d\n", task_id, (int)value);
        DispatchLogField targets[] = {
            {"task", task_id},
        };
        DispatchLogField context[] = {
            {"priority", argv[4]},
        };
        char message[256];
        snprintf(message, sizeof(message), "Set priority of %s", task_id);
        append_dispatch_log(actor, "task", "priority", targets, 1, context, 1,
                            message);
        locked_board_close(&locked);
        return 0;
    }

    if (argc < 5 || strcmp(argv[2], "add") != 0 ||
        strncmp(argv[3], "--", 2) == 0) {
        print_task_usage_error();
        return 1;
    }

    const char *group = argv[3];
    const char *title = NULL;
    const char *description = "";
    int has_description = 0;
    const char *actor = "user";
    const char *issue = NULL;
    const char *from_issue = NULL;
    int requires_review = 1;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--description") == 0 && (i + 1) < argc) {
            description = argv[++i];
            has_description = 1;
        } else if (strcmp(argv[i], "--actor") == 0 && (i + 1) < argc) {
            actor = argv[++i];
        } else if (strcmp(argv[i], "--no-review") == 0) {
            requires_review = 0;
        } else if (strcmp(argv[i], "--issue") == 0 && (i + 1) < argc) {
            issue = argv[++i];
        } else if (strcmp(argv[i], "--from-issue") == 0) {
            if ((i + 1) >= argc) {
                cli_error("usage", NULL,
                          "Option --from-issue requires an issue reference\n");
                return 1;
            }
            if (from_issue) {
                cli_error("usage", NULL,
                          "--from-issue may only be given once\n");
                return 1;
            }
            from_issue = argv[++i];
        } else if (!title && strncmp(argv[i], "--", 2) != 0) {
            title = argv[i];
        } else {
            cli_error("usage", NULL, "Unknown task option: %s\n", argv[i]);
            return 1;
        }
    }

    if (from_issue && issue) {
        cli_error("usage", NULL,
                  "Use either --from-issue or --issue, not both\n");
        return 1;
    }
    if (from_issue && title) {
        cli_error("usage", NULL,
                  "Cannot pass both a title and --from-issue; omit the title "
                  "to use the issue's title, or use --issue instead\n");
        return 1;
    }
    if (from_issue && has_description) {
        cli_error("usage", NULL,
                  "Cannot combine --description with --from-issue; use --issue "
                  "with a title and description, or edit the task after adding "
                  "it\n");
        return 1;
    }
    if (!from_issue && !title) {
        print_task_usage_error();
        return 1;
    }

    if (!from_issue && title_starts_with_dispatch_id(title)) {
        cli_error("invalid_title", NULL,
                  "Task titles should not include Dispatch IDs; use a human-readable title without a prefix like DE-01\n");
        return 1;
    }
    if (issue && !issue_ref_is_well_formed(issue))
        return 1;
    if (from_issue && !issue_ref_is_well_formed(from_issue))
        return 1;
    if (!dispatch_actor_label_is_valid(actor)) {
        cli_error("invalid_actor", NULL,
                  "Actor must start with an ASCII letter or digit and contain only letters, digits, '.', '_' or '-'\n");
        return 1;
    }

    DispatchGhIssue fetched = {0};
    char canonical[DISPATCH_ISSUE_TEXT_MAX] = "";
    int link_issue = 0;

    if (from_issue) {
        /* Validate the group and resolve bare refs before calling gh, and
         * release the board lock during the network call. */
        DispatchBoard preview;
        if (!load_board_or_error(&preview))
            return 1;
        if (!dispatch_board_find_group(&preview, group)) {
            dispatch_board_free(&preview);
            cli_error("group_not_found", NULL,
                      "No group with id, prefix, or name %s\n", group);
            return 1;
        }
        DispatchIssueRef issue_ref = {0};
        if (!resolve_issue_ref_struct(&preview, from_issue, &issue_ref)) {
            dispatch_board_free(&preview);
            return 1;
        }
        dispatch_board_free(&preview);
        cli_json_snapshot_reset();

        char requested_ref[DISPATCH_ISSUE_TEXT_MAX];
        dispatch_issue_format(&issue_ref, requested_ref, sizeof(requested_ref));

        DispatchGhResult gh_result = {0};
        DispatchGhStatus gh_status =
            dispatch_gh_issue_view(&issue_ref, &fetched, &gh_result);
        if (gh_status != DISPATCH_GH_OK) {
            report_gh_issue_error(gh_status, requested_ref, &gh_result);
            dispatch_gh_result_free(&gh_result);
            dispatch_gh_issue_free(&fetched);
            return 1;
        }
        dispatch_gh_result_free(&gh_result);

        if (fetched.title[0] == '\0') {
            cli_error("invalid_title", NULL,
                      "GitHub issue %s has an empty title\n",
                      fetched.canonical);
            dispatch_gh_issue_free(&fetched);
            return 1;
        }
        if (title_starts_with_dispatch_id(fetched.title)) {
            cli_error("invalid_title", NULL,
                      "GitHub issue %s title '%s' starts with a Dispatch ID; "
                      "add it with an explicit title and --issue %s instead\n",
                      fetched.canonical, fetched.title, fetched.canonical);
            dispatch_gh_issue_free(&fetched);
            return 1;
        }

        title = fetched.title;
        description = fetched.body;
        snprintf(canonical, sizeof(canonical), "%s", fetched.canonical);
        link_issue = 1;
    }

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked)) {
        dispatch_gh_issue_free(&fetched);
        return 1;
    }
    DispatchBoard *board = &locked.board;

    if (issue) {
        if (!resolve_issue_ref(board, issue, canonical, sizeof(canonical))) {
            locked_board_close(&locked);
            dispatch_gh_issue_free(&fetched);
            return 1;
        }
        link_issue = 1;
    }

    DispatchTask *task =
        dispatch_board_add_task_with_actor(board, group, title, description,
                                           actor);
    if (!task) {
        const char *code = dispatch_board_find_group(board, group)
                               ? "invalid_title"
                               : "group_not_found";
        cli_error(code, NULL, "Could not add task '%s' to group '%s'\n", title,
                  group);
        locked_board_close(&locked);
        dispatch_gh_issue_free(&fetched);
        return 1;
    }
    task->requires_review = requires_review;
    if (link_issue && !dispatch_task_link_issue(task, canonical, actor)) {
        locked_board_close(&locked);
        dispatch_gh_issue_free(&fetched);
        return 1;
    }

    char *task_id = strdup(task->id);
    if (!locked_board_save_or_error(&locked)) {
        free(task_id);
        locked_board_close(&locked);
        dispatch_gh_issue_free(&fetched);
        return 1;
    }

    if (!cli_json())
        printf("Added task %s\n", task_id);
    DispatchLogField targets[] = {
        {"task", task_id},
        {"group", group},
    };
    DispatchLogField context[] = {
        {"title", title},
        {"requires_review", bool_string(requires_review)},
    };
    char message[256];
    snprintf(message, sizeof(message), "Added task %s", task_id);
    append_dispatch_log(actor, "task", "add", targets, 2, context, 2,
                        message);
    if (link_issue) {
        if (!cli_json())
            printf("Linked %s to %s\n", task_id, canonical);
        log_issue_change(actor, task_id, 1, canonical);
    }
    int status = 0;
    if (cli_json()) {
        DispatchJsonResult *json_result = dispatch_json_result_new("task_add");
        dispatch_json_result_add_target(json_result, task_id);
        status = cli_json_emit_tasks(board, NULL, json_result);
    }
    free(task_id);
    locked_board_close(&locked);
    dispatch_gh_issue_free(&fetched);
    return status;
}

int cmd_dep(int argc, char **argv) {
    if (argc != 5 ||
        (strcmp(argv[2], "add") != 0 && strcmp(argv[2], "remove") != 0)) {
        cli_error("usage", NULL,
                  "Usage: dispatch dep add <dependency-id> <dependent-id>\n");
        cli_error("usage", NULL,
                  "       dispatch dep remove <dependency-id> <dependent-id>\n");
        cli_error("usage", NULL,
                  "Example: dispatch dep add DE-01 DE-02 means DE-02 depends "
                  "on DE-01\n");
        return 1;
    }

    const char *from_id = argv[3];
    const char *to_id = argv[4];

    LockedBoard locked;
    if (!locked_board_load_or_error(&locked))
        return 1;
    DispatchBoard *board = &locked.board;

    int adding = strcmp(argv[2], "add") == 0;
    DispatchTask *from = dispatch_board_find_task(board, from_id);
    DispatchTask *to = dispatch_board_find_task(board, to_id);
    int already_present = 0;
    for (size_t i = 0; to && i < to->depends_on.count; i++) {
        if (strcmp(to->depends_on.items[i], from_id) == 0)
            already_present = 1;
    }

    int ok;
    if (adding) {
        ok = dispatch_task_add_dependency(board, from_id, to_id);
    } else {
        ok = dispatch_task_remove_dependency(board, from_id, to_id);
    }

    if (!ok) {
        const char *code;
        const char *code_task = to_id;
        if (!to || (adding && !from)) {
            code = "task_not_found";
            code_task = to ? from_id : to_id;
        } else {
            code = adding ? "dependency_cycle" : "dependency_not_found";
        }
        locked_board_close(&locked);
        cli_error(code, code_task,
                  "Could not %s dependency %s -> %s (%s depends on %s)\n",
                  argv[2], from_id, to_id, to_id, from_id);
        return 1;
    }

    if (!locked_board_save_or_error(&locked)) {
        locked_board_close(&locked);
        return 1;
    }

    if (!cli_json())
        printf("%s dependency %s -> %s (%s depends on %s)\n",
               strcmp(argv[2], "add") == 0 ? "Added" : "Removed", from_id,
               to_id, to_id, from_id);
    DispatchLogField targets[] = {
        {"dependency", from_id},
        {"dependent", to_id},
    };
    char message[256];
    snprintf(message, sizeof(message), "%s dependency %s -> %s",
             strcmp(argv[2], "add") == 0 ? "Added" : "Removed", from_id,
             to_id);
    append_dispatch_log("user", "dep",
                        strcmp(argv[2], "add") == 0 ? "dependency_add"
                                                     : "dependency_remove",
                        targets, 2, NULL, 0, message);
    int status = 0;
    if (cli_json()) {
        DispatchJsonResult *json_result = dispatch_json_result_new(
            adding ? "dependency_add" : "dependency_remove");
        dispatch_json_result_add_target(json_result, to_id);
        dispatch_json_result_set_changed(json_result,
                                         adding ? !already_present : 1);
        dispatch_json_result_set_string(json_result, "dependency", from_id);
        dispatch_json_result_set_string(json_result, "dependent", to_id);
        status = cli_json_emit_tasks(board, NULL, json_result);
    }
    locked_board_close(&locked);
    return status;
}
