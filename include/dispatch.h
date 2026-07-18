#ifndef DISPATCH_H
#define DISPATCH_H

#include <stddef.h>
#include <time.h>

typedef enum {
    DISPATCH_STATE_PROPOSED,
    DISPATCH_STATE_READY,
    DISPATCH_STATE_BLOCKED,
    DISPATCH_STATE_DOING,
    DISPATCH_STATE_REVIEW,
    DISPATCH_STATE_DONE,
    DISPATCH_STATE_PAUSED
} DispatchState;

typedef enum {
    DISPATCH_WORKSPACE_CREATING,
    DISPATCH_WORKSPACE_ACTIVE,
    DISPATCH_WORKSPACE_REMOVED
} DispatchWorkspaceState;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} DispatchStringList;

typedef struct {
    char *actor;
    char *action;
    char *note;
    time_t timestamp;
} DispatchHistoryEntry;

typedef struct {
    DispatchHistoryEntry *items;
    size_t count;
    size_t capacity;
} DispatchHistory;

typedef struct {
    char *id;
    char *name;
    char *prefix;
    char *description; /* recorded scope: what work belongs in the group */
} DispatchGroup;

typedef struct {
    char *id;
    char *title;
    char *description;
    char *group;
    DispatchState state;
    DispatchStringList depends_on;
    DispatchStringList commits;
    int requires_review;
    int priority; /* higher surfaces first among ready tasks */
    char *assigned_to;
    char *started_by;
    char *completed_by;
    time_t created_at;
    time_t started_at;
    time_t completed_at;
    time_t updated_at;
    DispatchHistory history;
} DispatchTask;

typedef struct {
    DispatchGroup *items;
    size_t count;
    size_t capacity;
} DispatchGroups;

typedef struct {
    DispatchTask *items;
    size_t count;
    size_t capacity;
} DispatchTasks;

typedef struct {
    char *name;
    char *runner;
    char *model;
    char *agent_dir;
    char *prompt_path;
    char *run_script_path;
    char *session_id;
    char *current_task;
    char *last_workspace;
    int archived;
    time_t created_at;
    time_t updated_at;
} DispatchAgent;

typedef struct {
    DispatchAgent *items;
    size_t count;
    size_t capacity;
} DispatchAgents;

typedef struct {
    char *id;
    char *task_id;
    char *actor;
    char *path;
    char *branch;
    char *repo_path;
    DispatchWorkspaceState state;
    DispatchStringList sequence_tasks;
    char *review_gate;
    time_t created_at;
    time_t updated_at;
} DispatchWorkspace;

typedef struct {
    DispatchWorkspace *items;
    size_t count;
    size_t capacity;
} DispatchWorkspaces;

typedef struct {
    int version;
    char *name;
    char *repo_path;
    DispatchGroups groups;
    DispatchTasks tasks;
    DispatchAgents agents;
    DispatchWorkspaces workspaces;
} DispatchBoard;

typedef struct {
    const char *name;
    const char *runner;
    const char *model;
    const char *actor;
    int no_run_script;
} DispatchAgentCreateOptions;

typedef struct {
    char *agent_dir;
    char *prompt_path;
    char *scratch_dir;
    char *decisions_dir;
    char *run_script_path;
    char *command;
} DispatchAgentCreateResult;

void dispatch_board_init(DispatchBoard *board, const char *name);
void dispatch_board_set_repo_path(DispatchBoard *board, const char *repo_path);
void dispatch_board_free(DispatchBoard *board);

DispatchGroup *dispatch_board_find_group(DispatchBoard *board,
                                         const char *group_id);
DispatchTask *dispatch_board_find_task(DispatchBoard *board,
                                       const char *task_id);
DispatchAgent *dispatch_board_find_agent(DispatchBoard *board,
                                         const char *name);
DispatchWorkspace *dispatch_board_find_workspace(DispatchBoard *board,
                                                 const char *id_or_task_id);

int dispatch_board_add_group(DispatchBoard *board, const char *name,
                             const char *prefix);
int dispatch_group_set_description(DispatchGroup *group,
                                   const char *description);
int dispatch_group_prefix_is_valid(const char *prefix);
DispatchTask *dispatch_board_add_task(DispatchBoard *board,
                                      const char *group_id,
                                      const char *title,
                                      const char *description);
DispatchTask *dispatch_board_add_task_with_actor(DispatchBoard *board,
                                                 const char *group_id,
                                                 const char *title,
                                                 const char *description,
                                                 const char *actor);
int dispatch_board_delete_task(DispatchBoard *board, const char *task_id,
                               int force);
int dispatch_task_set_title(DispatchTask *task, const char *title);
int dispatch_task_set_description(DispatchTask *task, const char *description);
int dispatch_task_set_priority(DispatchTask *task, int priority,
                               const char *actor);
int dispatch_task_move_to_group(DispatchBoard *board, DispatchTask *task,
                                const char *group_id, const char *actor);

const char *dispatch_state_name(DispatchState state);
int dispatch_state_from_name(const char *name, DispatchState *state);
const char *dispatch_workspace_state_name(DispatchWorkspaceState state);
int dispatch_workspace_state_from_name(const char *name,
                                       DispatchWorkspaceState *state);
int dispatch_task_set_state(DispatchTask *task, DispatchState state,
                            const char *actor, const char *note);
int dispatch_task_assign(DispatchTask *task, const char *actor);
void dispatch_task_clear_assignment(DispatchTask *task);
int dispatch_task_append_history(DispatchTask *task, const char *actor,
                                 const char *action, const char *note);
int dispatch_task_mark_ready(DispatchBoard *board, DispatchTask *task,
                             const char *actor);
int dispatch_task_start(DispatchBoard *board, DispatchTask *task,
                        const char *actor);
int dispatch_task_finish(DispatchTask *task, const char *actor);
int dispatch_task_unassign(DispatchBoard *board, DispatchTask *task,
                           const char *actor);
int dispatch_task_review(DispatchTask *task, const char *actor);

DispatchState dispatch_task_effective_state(const DispatchBoard *board,
                                            const DispatchTask *task);
int dispatch_task_has_unmet_dependencies(const DispatchBoard *board,
                                         const DispatchTask *task);
size_t dispatch_task_dependent_count(const DispatchBoard *board,
                                     const char *task_id);
int dispatch_task_add_dependency(DispatchBoard *board, const char *from_id,
                                 const char *to_id);
int dispatch_task_remove_dependency(DispatchBoard *board, const char *from_id,
                                    const char *to_id);
int dispatch_board_has_dependency_path(const DispatchBoard *board,
                                       const char *from_id,
                                       const char *to_id);
void dispatch_board_normalize_states(DispatchBoard *board);
int dispatch_board_repair_assignments(DispatchBoard *board);
int dispatch_board_normalize_agent_sessions(DispatchBoard *board);

/* Embedded workflow instruction templates (generated by nob from AGENTS.md
 * and CLAUDE.md at build time). */
extern const char dispatch_template_agents_md[];
extern const char dispatch_template_claude_md[];

/* Maximum stored agent name length. The TUI reserves exactly this much
 * column space for agent names and actor labels, so names validated here
 * always render without truncation. */
#define DISPATCH_AGENT_NAME_MAX 24
/* Maximum group name length shown in board group headers. */
#define DISPATCH_GROUP_NAME_MAX 48

int dispatch_agent_name_is_valid(const char *name);
int dispatch_agent_runner_is_valid(const char *runner);
int dispatch_agent_create(const DispatchAgentCreateOptions *options,
                          DispatchAgentCreateResult *result, char *error,
                          size_t error_size);
void dispatch_agent_create_result_free(DispatchAgentCreateResult *result);
char *codex_agent_resume_command_for(const DispatchAgent *agent,
                                     const DispatchWorkspace *workspace);
char *claude_agent_resume_command_for(const DispatchAgent *agent,
                                      const DispatchWorkspace *workspace,
                                      int start_with_session_id);

char *dispatch_next_task_id(const DispatchBoard *board, const char *group_id);

int dispatch_actor_label_is_valid(const char *actor);
char *dispatch_actor_slug(const char *actor);
char *dispatch_default_workspace_branch(const char *actor,
                                        const char *task_id);
char *dispatch_default_workspace_path(const char *repo_path,
                                      const char *actor,
                                      const char *task_id);
char *dispatch_resolve_path(const char *workflow_dir, const char *path);
int dispatch_path_is_git_repository(const char *path);
int dispatch_workspace_path_conflicts(const char *repo_path,
                                      const char *workspace_path);

#endif
