/* Unit tests for the reusable gh CLI helper (dispatch_gh.c). */

#include "dispatch_gh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void write_stub(const char *dir, const char *script) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/gh", dir);
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "FAIL: could not write stub %s\n", path);
        exit(1);
    }
    fputs("#!/bin/sh\n", file);
    fputs(script, file);
    fclose(file);
    if (chmod(path, 0755) != 0) {
        fprintf(stderr, "FAIL: could not chmod stub %s\n", path);
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <temp-dir>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    char stub_path[1200];
    snprintf(stub_path, sizeof(stub_path), "%s:/usr/bin:/bin", dir);

    DispatchIssueRef ref = {0};
    check(dispatch_issue_parse_canonical("acme/widgets#5", &ref),
          "canonical ref parsed");

    /* Missing gh on PATH. */
    setenv("PATH", "/no-such-dispatch-gh-dir", 1);
    DispatchGhIssue issue = {0};
    DispatchGhResult res = {0};
    DispatchGhStatus status = dispatch_gh_issue_view(&ref, &issue, &res);
    check(status == DISPATCH_GH_MISSING, "missing gh returns MISSING");
    check(strcmp(dispatch_gh_error_code(status), "gh_missing") == 0,
          "missing gh maps to gh_missing");
    dispatch_gh_result_free(&res);
    dispatch_gh_issue_free(&issue);

    setenv("PATH", stub_path, 1);

    /* Happy path: stderr noise must not corrupt JSON, CRLF normalized,
     * canonical ref taken from returned URL. */
    write_stub(
        dir,
        "printf '%s\\n' 'A new release of gh is available' >&2\n"
        "printf '%s\\n' '{\"title\":\"  Ship feature  \",\"body\":\"Line 1\\r\\nLine 2\\rLine 3\","
        "\"url\":\"https://github.com/Acme/Widgets/issues/5\"}'\n");
    status = dispatch_gh_issue_view(&ref, &issue, &res);
    check(status == DISPATCH_GH_OK, "valid issue view succeeds");
    check(issue.title && strcmp(issue.title, "Ship feature") == 0,
          "title trimmed");
    check(issue.body && strcmp(issue.body, "Line 1\nLine 2\nLine 3") == 0,
          "body CRLF/CR normalized");
    check(strcmp(issue.canonical, "Acme/Widgets#5") == 0,
          "canonical ref derived from URL");
    dispatch_gh_result_free(&res);
    dispatch_gh_issue_free(&issue);

    /* Null body maps to empty string. */
    write_stub(
        dir,
        "printf '{\"title\":\"Empty body\",\"body\":null,"
        "\"url\":\"https://github.com/acme/widgets/issues/5\"}\\n'\n");
    status = dispatch_gh_issue_view(&ref, &issue, &res);
    check(status == DISPATCH_GH_OK, "null body succeeds");
    check(issue.body && strcmp(issue.body, "") == 0, "null body -> empty");
    dispatch_gh_result_free(&res);
    dispatch_gh_issue_free(&issue);

    /* Auth failure via exit code 4. */
    write_stub(dir,
               "printf 'To get started with GitHub CLI, please run:  gh auth login\\n' >&2\n"
               "exit 4\n");
    status = dispatch_gh_issue_view(&ref, &issue, &res);
    check(status == DISPATCH_GH_NOT_AUTHENTICATED, "exit 4 -> NOT_AUTHENTICATED");
    check(strcmp(dispatch_gh_error_code(status), "gh_not_authenticated") == 0,
          "auth error code");
    dispatch_gh_result_free(&res);
    dispatch_gh_issue_free(&issue);

    /* Auth failure via HTTP 401 with exit code 1. */
    write_stub(dir,
               "printf 'HTTP 401: Bad credentials\\n' >&2\n"
               "exit 1\n");
    status = dispatch_gh_issue_view(&ref, &issue, &res);
    check(status == DISPATCH_GH_NOT_AUTHENTICATED, "HTTP 401 -> NOT_AUTHENTICATED");
    dispatch_gh_result_free(&res);
    dispatch_gh_issue_free(&issue);

    /* Not found via GraphQL message. */
    write_stub(
        dir,
        "printf 'GraphQL: Could not resolve to an issue or pull request with the number of 5.\\n' >&2\n"
        "exit 1\n");
    status = dispatch_gh_issue_view(&ref, &issue, &res);
    check(status == DISPATCH_GH_NOT_FOUND, "GraphQL missing -> NOT_FOUND");
    check(strcmp(dispatch_gh_error_code(status), "issue_not_found") == 0,
          "not found error code");
    dispatch_gh_result_free(&res);
    dispatch_gh_issue_free(&issue);

    /* Pull request URL rejected as NOT_AN_ISSUE. */
    write_stub(
        dir,
        "printf '{\"title\":\"A PR\",\"body\":\"desc\","
        "\"url\":\"https://github.com/acme/widgets/pull/5\"}\\n'\n");
    status = dispatch_gh_issue_view(&ref, &issue, &res);
    check(status == DISPATCH_GH_NOT_AN_ISSUE, "PR URL -> NOT_AN_ISSUE");
    check(strcmp(dispatch_gh_error_code(status), "invalid_issue") == 0,
          "PR maps to invalid_issue");
    dispatch_gh_result_free(&res);
    dispatch_gh_issue_free(&issue);

    /* Malformed JSON rejected. */
    write_stub(dir, "printf 'not json\\n'\n");
    status = dispatch_gh_issue_view(&ref, &issue, &res);
    check(status == DISPATCH_GH_INVALID_JSON, "malformed JSON -> INVALID_JSON");
    check(strcmp(dispatch_gh_error_code(status), "gh_failed") == 0,
          "invalid JSON maps to gh_failed");
    dispatch_gh_result_free(&res);
    dispatch_gh_issue_free(&issue);

    /* Generic non-zero failure preserves stderr diagnostic. */
    write_stub(dir, "printf 'HTTP 502: Bad Gateway\\n' >&2\nexit 1\n");
    status = dispatch_gh_issue_view(&ref, &issue, &res);
    check(status == DISPATCH_GH_FAILED, "502 -> FAILED");
    check(strstr(res.detail, "HTTP 502: Bad Gateway") != NULL,
          "stderr detail preserved");
    dispatch_gh_result_free(&res);
    dispatch_gh_issue_free(&issue);

    /* Generic dispatch_gh_run_json helper works for other gh commands (e.g. #6). */
    write_stub(dir, "printf '{\"state\":\"OPEN\"}\\n'\n");
    const char *state_args[] = {"issue", "view", "5", "--repo", "acme/widgets",
                                "--json", "state", NULL};
    json_t *json = NULL;
    status = dispatch_gh_run_json(state_args, &json, &res);
    check(status == DISPATCH_GH_OK && json != NULL, "run_json succeeds");
    if (json) {
        json_t *state = json_object_get(json, "state");
        check(json_is_string(state) &&
                  strcmp(json_string_value(state), "OPEN") == 0,
              "run_json parsed state");
        json_decref(json);
    }
    dispatch_gh_result_free(&res);

    if (failures) {
        fprintf(stderr, "%d gh helper test failure(s)\n", failures);
        return 1;
    }
    printf("gh helper tests passed\n");
    return 0;
}
