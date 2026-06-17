#include "dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fail(const char *message, const char *detail) {
    if (detail && detail[0])
        fprintf(stderr, "FAIL: %s: %s\n", message, detail);
    else
        fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int assert_string(const char *actual, const char *expected,
                         const char *label) {
    if (!actual || strcmp(actual, expected) != 0)
        return fail(label, actual ? actual : "(null)");
    return 0;
}

static char *join_path(const char *left, const char *right) {
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    int needs_slash = left_len > 0 && left[left_len - 1] != '/';
    char *path = malloc(left_len + (size_t)needs_slash + right_len + 1);
    if (!path) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(path, left, left_len);
    if (needs_slash)
        path[left_len++] = '/';
    memcpy(path + left_len, right, right_len + 1);
    return path;
}

static void make_dir(const char *path) {
    if (mkdir(path, 0700) != 0) {
        perror(path);
        exit(1);
    }
}

static int test_actor_names(void) {
    if (!dispatch_actor_label_is_valid("Codex_A.1-z"))
        return fail("valid actor rejected", "");
    if (dispatch_actor_label_is_valid("-bad"))
        return fail("actor starting with punctuation accepted", "");
    if (dispatch_actor_label_is_valid("bad/name"))
        return fail("actor with path separator accepted", "");
    if (dispatch_actor_label_is_valid("bad name"))
        return fail("actor with whitespace accepted", "");
    if (dispatch_actor_label_is_valid(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")) {
        return fail("actor over 48 chars accepted", "");
    }

    char *slug = dispatch_actor_slug("Codex_A.1-z");
    int result = assert_string(slug, "codex_a.1-z", "actor slug");
    free(slug);
    return result;
}

static int test_generated_names(const char *root) {
    char *branch =
        dispatch_default_workspace_branch("Codex_A.1-z", "WD-06");
    if (assert_string(branch, "agent/codex_a.1-z/WD-06",
                      "default branch")) {
        free(branch);
        return 1;
    }
    free(branch);

    char *repo = join_path(root, "Dispatch");
    char *expected = join_path(root, "Dispatch-agent-codex_a.1-z-WD-06");
    char *workspace =
        dispatch_default_workspace_path(repo, "Codex_A.1-z", "WD-06");
    int result = assert_string(workspace, expected, "default workspace path");
    free(repo);
    free(expected);
    free(workspace);
    return result;
}

static int test_paths(const char *root) {
    char *workflow = join_path(root, "workflow");
    char *repo = join_path(workflow, "repo");
    char *git = join_path(repo, ".git");
    char *other = join_path(workflow, "other");
    make_dir(root);
    make_dir(workflow);
    make_dir(repo);
    make_dir(git);
    make_dir(other);

    char *resolved = dispatch_resolve_path(workflow, "repo");
    if (assert_string(resolved, repo, "resolved repo path")) {
        free(workflow);
        free(repo);
        free(git);
        free(other);
        free(resolved);
        return 1;
    }
    free(resolved);

    if (!dispatch_path_is_git_repository(repo)) {
        free(workflow);
        free(repo);
        free(git);
        free(other);
        return fail("git repository was not detected", "");
    }
    if (dispatch_path_is_git_repository(other)) {
        free(workflow);
        free(repo);
        free(git);
        free(other);
        return fail("non-git directory detected as repository", "");
    }
    if (!dispatch_workspace_path_conflicts(repo, repo)) {
        free(workflow);
        free(repo);
        free(git);
        free(other);
        return fail("repo/workspace same path conflict missed", "");
    }
    if (dispatch_workspace_path_conflicts(repo, other)) {
        free(workflow);
        free(repo);
        free(git);
        free(other);
        return fail("different workspace path flagged as conflict", "");
    }

    free(workflow);
    free(repo);
    free(git);
    free(other);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2)
        return fail("usage: workspace_naming_test <tmp-root>", "");

    if (test_actor_names())
        return 1;
    if (test_generated_names(argv[1]))
        return 1;
    if (test_paths(argv[1]))
        return 1;

    printf("PASS: workspace naming\n");
    return 0;
}
