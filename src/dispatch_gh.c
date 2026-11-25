#include "dispatch_gh.h"

#include "dispatch_exec.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *gh_strdup(const char *value) {
    size_t size = strlen(value) + 1;
    char *copy = malloc(size);
    if (copy)
        memcpy(copy, value, size);
    return copy;
}

static int ascii_tolower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : (int)c;
}

static int equals_ci(const char *left, const char *right) {
    if (!left || !right)
        return 0;
    while (*left && *right) {
        if (ascii_tolower((unsigned char)*left) !=
            ascii_tolower((unsigned char)*right))
            return 0;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

/* Portable ASCII case-insensitive substring check. */
static int contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0])
        return 0;
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               ascii_tolower((unsigned char)p[i]) ==
                   ascii_tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len)
            return 1;
    }
    return 0;
}

static int text_indicates_auth_error(const char *text) {
    static const char *const patterns[] = {
        "gh auth login",
        "not logged in",
        "not authenticated",
        "requires authentication",
        "authentication required",
        "bad credentials",
        "http 401",
        "no oauth token",
    };
    if (!text || !text[0])
        return 0;
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (contains_ci(text, patterns[i]))
            return 1;
    }
    return 0;
}

static int text_indicates_not_found(const char *text) {
    static const char *const patterns[] = {
        "could not resolve to an issue",
        "could not resolve to a repository",
        "http 404",
        "not found",
    };
    if (!text || !text[0])
        return 0;
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (contains_ci(text, patterns[i]))
            return 1;
    }
    return 0;
}

/* Copies the first non-empty trimmed line of text into out. */
static void extract_first_line(const char *text, char *out, size_t out_size) {
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!text)
        return;

    const char *p = text;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n' && *p != '\r')
            p++;
        const char *line_end = p;
        while (*p == '\n' || *p == '\r')
            p++;

        while (line_start < line_end &&
               (*line_start == ' ' || *line_start == '\t'))
            line_start++;
        while (line_end > line_start &&
               (line_end[-1] == ' ' || line_end[-1] == '\t'))
            line_end--;

        if (line_end > line_start) {
            size_t len = (size_t)(line_end - line_start);
            if (len >= out_size)
                len = out_size - 1;
            memcpy(out, line_start, len);
            out[len] = '\0';
            return;
        }
    }
}

static char *trim_ascii_copy(const char *text) {
    const char *start = text ? text : "";
    while (*start && isspace((unsigned char)*start))
        start++;
    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    size_t len = (size_t)(end - start);
    char *copy = malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

/* Normalizes CRLF and standalone CR line endings from GitHub web forms to LF. */
static char *normalize_newlines_copy(const char *text) {
    if (!text)
        return gh_strdup("");
    size_t size = strlen(text);
    char *copy = malloc(size + 1);
    if (!copy)
        return NULL;
    size_t out = 0;
    for (size_t i = 0; i < size; i++) {
        if (text[i] == '\r') {
            if (i + 1 < size && text[i + 1] == '\n')
                i++;
            copy[out++] = '\n';
        } else {
            copy[out++] = text[i];
        }
    }
    copy[out] = '\0';
    return copy;
}

static int url_looks_like_pull_request(const char *url) {
    return contains_ci(url, "github.com/") && contains_ci(url, "/pull/");
}

void dispatch_gh_result_free(DispatchGhResult *result) {
    if (!result)
        return;
    free(result->stdout_output);
    free(result->stderr_output);
    memset(result, 0, sizeof(*result));
}

void dispatch_gh_issue_free(DispatchGhIssue *issue) {
    if (!issue)
        return;
    free(issue->title);
    free(issue->body);
    free(issue->url);
    memset(issue, 0, sizeof(*issue));
}

const char *dispatch_gh_error_code(DispatchGhStatus status) {
    switch (status) {
    case DISPATCH_GH_MISSING:
        return "gh_missing";
    case DISPATCH_GH_NOT_AUTHENTICATED:
        return "gh_not_authenticated";
    case DISPATCH_GH_NOT_FOUND:
        return "issue_not_found";
    case DISPATCH_GH_NOT_AN_ISSUE:
        return "invalid_issue";
    case DISPATCH_GH_INVALID_JSON:
    case DISPATCH_GH_FAILED:
        return "gh_failed";
    case DISPATCH_GH_OK:
        return "internal_error";
    }
    return "internal_error";
}

DispatchGhStatus dispatch_gh_run(const char *const args[],
                                 DispatchGhResult *result) {
    DispatchGhResult local = {0};
    DispatchGhResult *out = result ? result : &local;
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;

    if (!args || !args[0]) {
        out->status = DISPATCH_GH_FAILED;
        snprintf(out->detail, sizeof(out->detail), "missing gh arguments");
        if (!result)
            dispatch_gh_result_free(&local);
        return out->status;
    }

    if (!dispatch_exec_command_available("gh")) {
        out->status = DISPATCH_GH_MISSING;
        snprintf(out->detail, sizeof(out->detail),
                 "gh is not installed or not on PATH");
        if (!result)
            dispatch_gh_result_free(&local);
        return out->status;
    }

    DispatchExecArgv argv = {0};
    if (!dispatch_exec_argv_append(&argv, "gh")) {
        out->status = DISPATCH_GH_FAILED;
        snprintf(out->detail, sizeof(out->detail), "out of memory");
        if (!result)
            dispatch_gh_result_free(&local);
        return out->status;
    }
    for (size_t i = 0; args[i] != NULL; i++) {
        if (!dispatch_exec_argv_append(&argv, args[i])) {
            dispatch_exec_argv_free(&argv);
            out->status = DISPATCH_GH_FAILED;
            snprintf(out->detail, sizeof(out->detail), "out of memory");
            if (!result)
                dispatch_gh_result_free(&local);
            return out->status;
        }
    }

    DispatchExecOptions options = {
        .stdin_mode = DISPATCH_EXEC_DEV_NULL,
    };
    DispatchExecResult exec_result = {0};
    char *stdout_output = NULL;
    char *stderr_output = NULL;
    int ran = dispatch_exec_capture_streams(
        (const char *const *)argv.items, &options, &stdout_output, NULL,
        &stderr_output, NULL, &exec_result);
    int saved_errno = errno;
    dispatch_exec_argv_free(&argv);

    if (!ran) {
        out->status = DISPATCH_GH_FAILED;
        snprintf(out->detail, sizeof(out->detail), "could not execute gh: %s",
                 strerror(saved_errno));
        if (!result)
            dispatch_gh_result_free(&local);
        return out->status;
    }

    out->stdout_output = stdout_output;
    out->stderr_output = stderr_output;
    out->exit_code = dispatch_exec_result_status(&exec_result);

    if (dispatch_exec_result_success(&exec_result)) {
        out->status = DISPATCH_GH_OK;
        if (!result)
            dispatch_gh_result_free(&local);
        return DISPATCH_GH_OK;
    }

    extract_first_line(stderr_output, out->detail, sizeof(out->detail));
    if (!out->detail[0])
        extract_first_line(stdout_output, out->detail, sizeof(out->detail));

    if (!exec_result.exited) {
        out->status = DISPATCH_GH_FAILED;
        if (!out->detail[0]) {
            snprintf(out->detail, sizeof(out->detail),
                     "gh terminated by signal %d", exec_result.signal_number);
        }
    } else if (exec_result.exit_code == 127) {
        out->status = DISPATCH_GH_MISSING;
        snprintf(out->detail, sizeof(out->detail),
                 "gh is not installed or not on PATH");
    } else if (exec_result.exit_code == 4 ||
               text_indicates_auth_error(stderr_output) ||
               text_indicates_auth_error(stdout_output)) {
        out->status = DISPATCH_GH_NOT_AUTHENTICATED;
    } else if (text_indicates_not_found(stderr_output) ||
               text_indicates_not_found(stdout_output)) {
        out->status = DISPATCH_GH_NOT_FOUND;
    } else {
        out->status = DISPATCH_GH_FAILED;
        if (!out->detail[0]) {
            snprintf(out->detail, sizeof(out->detail),
                     "gh exited with status %d", exec_result.exit_code);
        }
    }

    DispatchGhStatus status = out->status;
    if (!result)
        dispatch_gh_result_free(&local);
    return status;
}

DispatchGhStatus dispatch_gh_run_json(const char *const args[],
                                      json_t **json_out,
                                      DispatchGhResult *result) {
    if (json_out)
        *json_out = NULL;

    DispatchGhResult local = {0};
    DispatchGhResult *out = result ? result : &local;

    DispatchGhStatus status = dispatch_gh_run(args, out);
    if (status != DISPATCH_GH_OK) {
        if (!result)
            dispatch_gh_result_free(&local);
        return status;
    }

    json_error_t json_err;
    json_t *parsed = json_loads(out->stdout_output ? out->stdout_output : "", 0,
                                &json_err);
    if (!parsed) {
        out->status = DISPATCH_GH_INVALID_JSON;
        if (json_err.text[0]) {
            snprintf(out->detail, sizeof(out->detail),
                     "invalid JSON from gh: %s", json_err.text);
        } else {
            snprintf(out->detail, sizeof(out->detail),
                     "unexpected JSON output from gh");
        }
        if (!result)
            dispatch_gh_result_free(&local);
        return DISPATCH_GH_INVALID_JSON;
    }

    if (json_out)
        *json_out = parsed;
    else
        json_decref(parsed);
    if (!result)
        dispatch_gh_result_free(&local);
    return DISPATCH_GH_OK;
}

DispatchGhStatus dispatch_gh_issue_view(const DispatchIssueRef *ref,
                                        DispatchGhIssue *issue,
                                        DispatchGhResult *result) {
    if (issue)
        memset(issue, 0, sizeof(*issue));

    DispatchGhResult local = {0};
    DispatchGhResult *out = result ? result : &local;
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;

    if (!ref || !issue || !ref->owner[0] || !ref->repo[0] || ref->number <= 0) {
        out->status = DISPATCH_GH_FAILED;
        snprintf(out->detail, sizeof(out->detail), "invalid issue reference");
        if (!result)
            dispatch_gh_result_free(&local);
        return DISPATCH_GH_FAILED;
    }

    char number_str[32];
    char repo_str[DISPATCH_ISSUE_TEXT_MAX];
    snprintf(number_str, sizeof(number_str), "%ld", ref->number);
    dispatch_issue_format_repo(ref, repo_str, sizeof(repo_str));

    const char *args[] = {
        "issue", "view", number_str, "--repo", repo_str, "--json",
        "title,body,url", NULL,
    };
    json_t *root = NULL;
    DispatchGhStatus status = dispatch_gh_run_json(args, &root, out);
    if (status != DISPATCH_GH_OK) {
        if (!result)
            dispatch_gh_result_free(&local);
        return status;
    }

    if (!json_is_object(root)) {
        json_decref(root);
        out->status = DISPATCH_GH_INVALID_JSON;
        snprintf(out->detail, sizeof(out->detail),
                 "gh output was not a JSON object");
        if (!result)
            dispatch_gh_result_free(&local);
        return DISPATCH_GH_INVALID_JSON;
    }

    json_t *title_val = json_object_get(root, "title");
    json_t *body_val = json_object_get(root, "body");
    json_t *url_val = json_object_get(root, "url");
    if (!json_is_string(title_val) || !json_is_string(url_val) ||
        (body_val != NULL && !json_is_null(body_val) &&
         !json_is_string(body_val))) {
        json_decref(root);
        out->status = DISPATCH_GH_INVALID_JSON;
        snprintf(out->detail, sizeof(out->detail),
                 "gh JSON object missing expected title, body, or url fields");
        if (!result)
            dispatch_gh_result_free(&local);
        return DISPATCH_GH_INVALID_JSON;
    }

    const char *url_str = json_string_value(url_val);
    DispatchIssueRef url_ref = {0};
    if (dispatch_issue_parse(url_str, &url_ref) != DISPATCH_ISSUE_PARSE_OK) {
        if (url_looks_like_pull_request(url_str)) {
            out->status = DISPATCH_GH_NOT_AN_ISSUE;
            snprintf(out->detail, sizeof(out->detail), "%s", url_str);
            json_decref(root);
            if (!result)
                dispatch_gh_result_free(&local);
            return DISPATCH_GH_NOT_AN_ISSUE;
        }
        out->status = DISPATCH_GH_INVALID_JSON;
        snprintf(out->detail, sizeof(out->detail),
                 "gh returned an unrecognized issue URL: %s", url_str);
        json_decref(root);
        if (!result)
            dispatch_gh_result_free(&local);
        return DISPATCH_GH_INVALID_JSON;
    }

    issue->title = trim_ascii_copy(json_string_value(title_val));
    issue->body =
        normalize_newlines_copy(json_is_string(body_val) ? json_string_value(body_val)
                                                         : "");
    issue->url = gh_strdup(url_str);
    dispatch_issue_format(&url_ref, issue->canonical, sizeof(issue->canonical));
    json_decref(root);

    if (!issue->title || !issue->body || !issue->url) {
        dispatch_gh_issue_free(issue);
        out->status = DISPATCH_GH_FAILED;
        snprintf(out->detail, sizeof(out->detail), "out of memory");
        if (!result)
            dispatch_gh_result_free(&local);
        return DISPATCH_GH_FAILED;
    }

    if (!result)
        dispatch_gh_result_free(&local);
    return DISPATCH_GH_OK;
}

DispatchGhStatus dispatch_gh_issue_state(const DispatchIssueRef *ref,
                                         DispatchGhIssueState *state,
                                         DispatchGhResult *result) {
    if (state)
        *state = DISPATCH_GH_ISSUE_STATE_UNKNOWN;

    DispatchGhResult local = {0};
    DispatchGhResult *out = result ? result : &local;
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;

    if (!ref || !state || !ref->owner[0] || !ref->repo[0] || ref->number <= 0) {
        out->status = DISPATCH_GH_FAILED;
        snprintf(out->detail, sizeof(out->detail), "invalid issue reference");
        if (!result)
            dispatch_gh_result_free(&local);
        return DISPATCH_GH_FAILED;
    }

    char number_str[32];
    char repo_str[DISPATCH_ISSUE_TEXT_MAX];
    snprintf(number_str, sizeof(number_str), "%ld", ref->number);
    dispatch_issue_format_repo(ref, repo_str, sizeof(repo_str));

    const char *args[] = {
        "issue", "view", number_str, "--repo", repo_str, "--json", "state",
        NULL,
    };
    json_t *root = NULL;
    DispatchGhStatus status = dispatch_gh_run_json(args, &root, out);
    if (status != DISPATCH_GH_OK) {
        if (!result)
            dispatch_gh_result_free(&local);
        return status;
    }

    const char *state_text =
        json_is_object(root)
            ? json_string_value(json_object_get(root, "state"))
            : NULL;
    if (equals_ci(state_text, "OPEN")) {
        *state = DISPATCH_GH_ISSUE_STATE_OPEN;
    } else if (equals_ci(state_text, "CLOSED")) {
        *state = DISPATCH_GH_ISSUE_STATE_CLOSED;
    } else {
        json_decref(root);
        out->status = DISPATCH_GH_INVALID_JSON;
        snprintf(out->detail, sizeof(out->detail),
                 "gh JSON object missing valid state field");
        if (!result)
            dispatch_gh_result_free(&local);
        return DISPATCH_GH_INVALID_JSON;
    }

    json_decref(root);
    if (!result)
        dispatch_gh_result_free(&local);
    return DISPATCH_GH_OK;
}

DispatchGhStatus dispatch_gh_issue_close(const DispatchIssueRef *ref,
                                         const char *comment,
                                         DispatchGhResult *result) {
    DispatchGhResult local = {0};
    DispatchGhResult *out = result ? result : &local;
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;

    if (!ref || !ref->owner[0] || !ref->repo[0] || ref->number <= 0) {
        out->status = DISPATCH_GH_FAILED;
        snprintf(out->detail, sizeof(out->detail), "invalid issue reference");
        if (!result)
            dispatch_gh_result_free(&local);
        return DISPATCH_GH_FAILED;
    }

    char number_str[32];
    char repo_str[DISPATCH_ISSUE_TEXT_MAX];
    snprintf(number_str, sizeof(number_str), "%ld", ref->number);
    dispatch_issue_format_repo(ref, repo_str, sizeof(repo_str));

    DispatchGhStatus status;
    if (comment && comment[0]) {
        const char *args[] = {
            "issue", "close", number_str, "--repo", repo_str, "--comment",
            comment, NULL,
        };
        status = dispatch_gh_run(args, out);
    } else {
        const char *args[] = {
            "issue", "close", number_str, "--repo", repo_str, NULL,
        };
        status = dispatch_gh_run(args, out);
    }

    if (!result)
        dispatch_gh_result_free(&local);
    return status;
}
