#ifndef DISPATCH_CLI_INTERNAL_H
#define DISPATCH_CLI_INTERNAL_H

/* Shared internals for the dispatch_cli_* modules. Not part of
 * the public CLI interface. */

#include "dispatch_cli.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dispatch.h"
#include "dispatch_exec.h"
#include "dispatch_json.h"
#include "dispatch_store.h"
#include "dispatch_tui.h"

typedef struct {
    const char *name;
    const char *summary;
} DispatchCliCommand;

typedef struct {
    DispatchBoard board;
    DispatchStoreLock lock;
    int loaded;
} LockedBoard;

extern const DispatchCliCommand commands[];

char *agent_command_for(const char *runner, const char *model, const char *prompt_path);
char *agent_prompt_path_for(const char *agent_dir, const char *name);
void append_dispatch_log(const char *actor, const char *command, const char *action, const DispatchLogField *targets, size_t target_count, const DispatchLogField *context, size_t context_count, const char *message);
const char *bool_string(int value);
void *cli_realloc_array(void *items, size_t count, size_t item_size);
char *cli_strdup(const char *value);
int cmd_agent(int argc, char **argv);
int cmd_blocked(int argc, char **argv);
int cmd_commit(int argc, char **argv);
int cmd_completion(int argc, char **argv);
int cmd_dep(int argc, char **argv);
int cmd_doctor(int argc, char **argv);
int cmd_finish(int argc, char **argv);
int cmd_group(int argc, char **argv);
int cmd_list(int argc, char **argv);
int cmd_normalize(void);
int cmd_queue_list(int argc, char **argv, DispatchState state, const char *label);
int cmd_ready(int argc, char **argv);
int cmd_ready_list(int json_output);
int cmd_repo(int argc, char **argv);
int cmd_review(int argc, char **argv);
int cmd_show(int argc, char **argv);
int cmd_start(int argc, char **argv);
int cmd_status(int argc, char **argv);
int cmd_task(int argc, char **argv);
int cmd_workspace(int argc, char **argv);
int command_exists_on_path(const char *command);
char *completion_install_path(const char *shell_name);
int create_agent_dirs(const char *agent_dir, char **scratch_dir, char **decisions_dir);
char *join_path2(const char *left, const char *right);
int load_board_or_error(DispatchBoard *board);
void locked_board_close(LockedBoard *locked);
int locked_board_load_or_error(LockedBoard *locked);
int locked_board_save_or_error(LockedBoard *locked);
int make_dir_if_needed(const char *path);
int print_ready_tasks_from_board(const DispatchBoard *board, const char *indent);
int ready_task_count(const DispatchBoard *board);
void replace_optional_string(char **target, const char *value);
int save_task_transition(LockedBoard *locked, const char *verb, const char *task_id);
char *shell_quote(const char *value);
const char *task_display_title(const DispatchTask *task);
DispatchWorkspace *task_workspace(const DispatchBoard *board, const char *task_id, int active_only);
int title_starts_with_dispatch_id(const char *title);
char *trimmed_copy(const char *value);
int workspace_covers_task(const DispatchWorkspace *workspace, const char *task_id);
int write_agent_prompt(const char *path, const char *name, const char *runner, const char *model, const char *agent_dir, const char *scratch_dir, const char *decisions_dir);
int write_agent_run_script(const char *path, const char *command);

#endif
