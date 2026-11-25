#ifndef DISPATCH_JSON_H
#define DISPATCH_JSON_H

#include "dispatch.h"
#include "dispatch_log.h"

#include <stdio.h>

#define DISPATCH_JSON_STATE(state) (1U << (unsigned int)(state))

typedef struct {
    const char *code;
    const char *message;
    const char *task_id;
    const char *agent;
    const char *issue; /* optional canonical "owner/repo#N" */
} DispatchJsonWarning;

typedef struct {
    const char *command;
    const char *task_id;
    const char *group;
    int include_done;
    unsigned int state_mask;
    int include_warnings;
    int check_issues;
    const DispatchJsonWarning *extra_warnings;
    size_t extra_warning_count;
} DispatchJsonRequest;

/* Removes one --json argument at or after first_arg and updates argc. */
int dispatch_cli_extract_json_flag(int *argc, char **argv, int first_arg,
                                   int *json_output);

/* Writes one versioned JSON response and a trailing newline. */
int dispatch_json_emit(FILE *stream, const DispatchBoard *board,
                       const DispatchJsonRequest *request);

/* Writes one versioned JSON response listing workspace records. When only
 * is non-NULL, emits just that record; otherwise every non-removed record. */
int dispatch_json_emit_workspaces(FILE *stream, const DispatchBoard *board,
                                  const char *command,
                                  const DispatchWorkspace *only);

/* Like dispatch_json_emit_workspaces, optionally filtering live records by
 * exact external_id and recording it in query.external_id. */
int dispatch_json_emit_workspaces_filtered(FILE *stream,
                                           const DispatchBoard *board,
                                           const char *command,
                                           const DispatchWorkspace *only,
                                           const char *external_id);

/* Writes the agent envelope. When only is non-NULL, emits just that agent
 * (archived or not); otherwise every agent, skipping archived ones unless
 * include_archived is set. */
int dispatch_json_emit_agents(FILE *stream, const DispatchBoard *board,
                              const char *command, const DispatchAgent *only,
                              int include_archived);

/* Writes the `log` envelope for records read from log_path. */
int dispatch_json_emit_log(FILE *stream, const DispatchBoard *board,
                           const char *log_path, const DispatchLogQuery *query,
                           const DispatchLogRead *read);

/* Presentation state of every task, captured before a mutation so the
 * result can report previous states and newly ready tasks. */
typedef struct {
    char **ids;
    DispatchState *states;
    size_t count;
} DispatchJsonSnapshot;

void dispatch_json_snapshot_take(DispatchJsonSnapshot *snapshot,
                                 const DispatchBoard *board);
void dispatch_json_snapshot_free(DispatchJsonSnapshot *snapshot);

/* The result object of a mutating command, built up by the command and
 * consumed by one of the mutation emitters. */
typedef struct DispatchJsonResult DispatchJsonResult;

DispatchJsonResult *dispatch_json_result_new(const char *action);
void dispatch_json_result_free(DispatchJsonResult *result);
/* Records a task the command acted on; order is preserved. */
void dispatch_json_result_add_target(DispatchJsonResult *result,
                                     const char *task_id);
void dispatch_json_result_set_changed(DispatchJsonResult *result,
                                      int changed);
/* Sets an action-specific field; a NULL value is emitted as null. */
void dispatch_json_result_set_string(DispatchJsonResult *result,
                                     const char *key, const char *value);
void dispatch_json_result_set_bool(DispatchJsonResult *result,
                                   const char *key, int value);
/* Appends {"workspace": id, "reason": reason} to result.skipped. */
void dispatch_json_result_add_skipped(DispatchJsonResult *result,
                                      const char *workspace_id,
                                      const char *reason);
/* Initializes empty closed_issues and issue_close_errors arrays on result. */
void dispatch_json_result_init_close_issues(DispatchJsonResult *result);
/* Appends {"task": id, "issue": ref} to result.closed_issues. */
void dispatch_json_result_add_closed_issue(DispatchJsonResult *result,
                                           const char *task_id,
                                           const char *issue);
/* Appends {"task": id, "issue": ref, "code": code, "message": msg} to
 * result.issue_close_errors. */
void dispatch_json_result_add_issue_close_error(DispatchJsonResult *result,
                                                const char *task_id,
                                                const char *issue,
                                                const char *code,
                                                const char *message);

/* Writes the task envelope for a mutation: tasks holds the targets that
 * still exist, followed by result. group fills query.group when non-NULL. */
int dispatch_json_emit_mutation(FILE *stream, const DispatchBoard *board,
                                const char *command, const char *group,
                                const DispatchJsonSnapshot *before,
                                const DispatchJsonResult *result);

/* Writes the workspace envelope for a mutation with the given records,
 * followed by result. */
int dispatch_json_emit_workspace_mutation(
    FILE *stream, const DispatchBoard *board, const char *command,
    const DispatchWorkspace *records, size_t record_count,
    const DispatchJsonSnapshot *before, const DispatchJsonResult *result);

/* Writes {"schema_version", "command", "error": {code, message, task_id}}. */
int dispatch_json_emit_error(FILE *stream, const char *command,
                             const char *code, const char *message,
                             const char *task_id);

#endif
