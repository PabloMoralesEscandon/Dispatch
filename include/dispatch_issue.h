#ifndef DISPATCH_ISSUE_H
#define DISPATCH_ISSUE_H

#include <stddef.h>

/* GitHub issue references. A task stores at most one issue, canonically as
 * "owner/repo#number". Parsing is purely lexical: nothing here touches the
 * network or runs git. */

#define DISPATCH_ISSUE_OWNER_MAX 39
#define DISPATCH_ISSUE_REPO_MAX 100
/* Issue numbers are positive and at most nine digits. */
#define DISPATCH_ISSUE_NUMBER_MAX 999999999L

typedef struct {
    char owner[DISPATCH_ISSUE_OWNER_MAX + 1];
    char repo[DISPATCH_ISSUE_REPO_MAX + 1];
    long number;
} DispatchIssueRef;

typedef enum {
    DISPATCH_ISSUE_PARSE_INVALID,
    /* A bare "123" or "#123": number is set, owner/repo must come from the
     * board repository's origin remote. */
    DISPATCH_ISSUE_PARSE_NEEDS_REPO,
    DISPATCH_ISSUE_PARSE_OK
} DispatchIssueParse;

/* Accepts "123", "#123", "owner/repo#123", and
 * "https://github.com/owner/repo/issues/123" (http, www., a trailing slash,
 * query or fragment are tolerated). Surrounding whitespace is ignored. */
DispatchIssueParse dispatch_issue_parse(const char *input,
                                        DispatchIssueRef *ref);

/* Parses exactly the canonical "owner/repo#number" form. */
int dispatch_issue_parse_canonical(const char *value, DispatchIssueRef *ref);

/* Sets ref's owner and repo from "owner/repo". Returns 0 when malformed. */
int dispatch_issue_set_repo(DispatchIssueRef *ref, const char *owner_repo);

/* Extracts "owner/repo" from a GitHub remote URL such as
 * git@github.com:owner/repo.git or https://github.com/owner/repo. Returns 0
 * for non-GitHub or malformed remotes. */
int dispatch_issue_repo_from_remote(const char *remote, char *out,
                                    size_t out_size);

/* Writes "owner/repo#number". */
void dispatch_issue_format(const DispatchIssueRef *ref, char *out,
                           size_t out_size);
/* Writes "owner/repo". */
void dispatch_issue_format_repo(const DispatchIssueRef *ref, char *out,
                                size_t out_size);
/* Writes "https://github.com/owner/repo/issues/number". */
void dispatch_issue_format_url(const DispatchIssueRef *ref, char *out,
                               size_t out_size);

/* Large enough for any canonical ref or URL produced above. */
#define DISPATCH_ISSUE_TEXT_MAX 192

#endif
