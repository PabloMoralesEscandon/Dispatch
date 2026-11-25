#ifndef DISPATCH_GH_H
#define DISPATCH_GH_H

#include "dispatch_issue.h"

#include <jansson.h>
#include <stddef.h>

/* Thin wrapper around the GitHub CLI (`gh`). Handles subprocess execution,
 * separate stdout/stderr capture, JSON decoding, and classification of common
 * failure modes (missing binary, missing auth, missing resource) so commands
 * that talk to GitHub share one implementation. */

typedef enum {
    DISPATCH_GH_OK = 0,
    DISPATCH_GH_MISSING,
    DISPATCH_GH_NOT_AUTHENTICATED,
    DISPATCH_GH_NOT_FOUND,
    DISPATCH_GH_NOT_AN_ISSUE,
    DISPATCH_GH_INVALID_JSON,
    DISPATCH_GH_FAILED
} DispatchGhStatus;

typedef struct {
    DispatchGhStatus status;
    int exit_code;
    char *stdout_output;
    char *stderr_output;
    /* Short, single-line diagnostic extracted from stderr (or a fallback),
     * populated when status != DISPATCH_GH_OK. */
    char detail[256];
} DispatchGhResult;

typedef struct {
    char *title;
    char *body;
    char *url;
    /* Canonical "owner/repo#number" derived from the returned issue URL. */
    char canonical[DISPATCH_ISSUE_TEXT_MAX];
} DispatchGhIssue;

/* Runs `gh` with the NULL-terminated argument vector (args[0] is the
 * subcommand, e.g. "issue"; "gh" is prepended automatically). Stdin is
 * redirected to /dev/null and stdout/stderr are captured separately. */
DispatchGhStatus dispatch_gh_run(const char *const args[],
                                 DispatchGhResult *result);

/* Runs `gh` and parses stdout as JSON on exit 0. On success, *json_out owns
 * the parsed json_t* (caller must json_decref it). */
DispatchGhStatus dispatch_gh_run_json(const char *const args[],
                                      json_t **json_out,
                                      DispatchGhResult *result);

void dispatch_gh_result_free(DispatchGhResult *result);

/* Maps a non-OK DispatchGhStatus to its stable JSON error code (ADR 0002). */
const char *dispatch_gh_error_code(DispatchGhStatus status);

/* Fetches title, body, and url for a resolved issue ref via
 * `gh issue view <number> --repo <owner>/<repo> --json title,body,url`.
 * Rejects pull requests with DISPATCH_GH_NOT_AN_ISSUE. */
DispatchGhStatus dispatch_gh_issue_view(const DispatchIssueRef *ref,
                                        DispatchGhIssue *issue,
                                        DispatchGhResult *result);

void dispatch_gh_issue_free(DispatchGhIssue *issue);

#endif
