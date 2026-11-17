/* Unit tests for GitHub issue reference parsing. */

#include "dispatch_issue.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect_parse(const char *input, DispatchIssueParse expected,
                         const char *canonical) {
    DispatchIssueRef ref = {0};
    DispatchIssueParse actual = dispatch_issue_parse(input, &ref);
    if (actual != expected) {
        fprintf(stderr, "FAIL: parse '%s': got %d, expected %d\n", input,
                (int)actual, (int)expected);
        failures++;
        return;
    }
    if (actual != DISPATCH_ISSUE_PARSE_OK || !canonical)
        return;
    char text[DISPATCH_ISSUE_TEXT_MAX];
    dispatch_issue_format(&ref, text, sizeof(text));
    if (strcmp(text, canonical) != 0) {
        fprintf(stderr, "FAIL: parse '%s': got %s, expected %s\n", input,
                text, canonical);
        failures++;
    }
}

static void expect_number(const char *input, long number) {
    DispatchIssueRef ref = {0};
    if (dispatch_issue_parse(input, &ref) != DISPATCH_ISSUE_PARSE_NEEDS_REPO ||
        ref.number != number) {
        fprintf(stderr, "FAIL: bare '%s': expected number %ld\n", input,
                number);
        failures++;
    }
}

static void expect_remote(const char *remote, const char *expected) {
    char out[DISPATCH_ISSUE_TEXT_MAX] = {0};
    int ok = dispatch_issue_repo_from_remote(remote, out, sizeof(out));
    if (expected ? (!ok || strcmp(out, expected) != 0) : ok) {
        fprintf(stderr, "FAIL: remote '%s': got %s, expected %s\n", remote,
                ok ? out : "(none)", expected ? expected : "(none)");
        failures++;
    }
}

static void test_accepted_forms(void) {
    expect_parse("acme/widgets#12", DISPATCH_ISSUE_PARSE_OK,
                 "acme/widgets#12");
    expect_parse("  acme/widgets#12\n", DISPATCH_ISSUE_PARSE_OK,
                 "acme/widgets#12");
    expect_parse("Acme-Co/my.repo_2#7", DISPATCH_ISSUE_PARSE_OK,
                 "Acme-Co/my.repo_2#7");
    expect_parse("9lives/cat#3", DISPATCH_ISSUE_PARSE_OK, "9lives/cat#3");
    expect_parse("https://github.com/acme/widgets/issues/12",
                 DISPATCH_ISSUE_PARSE_OK, "acme/widgets#12");
    expect_parse("http://www.github.com/acme/widgets/issues/12/",
                 DISPATCH_ISSUE_PARSE_OK, "acme/widgets#12");
    expect_parse("HTTPS://GitHub.com/acme/widgets/issues/12#issuecomment-1",
                 DISPATCH_ISSUE_PARSE_OK, "acme/widgets#12");
    expect_parse("https://github.com/acme/widgets/issues/12?x=1",
                 DISPATCH_ISSUE_PARSE_OK, "acme/widgets#12");
    expect_number("12", 12);
    expect_number("#12", 12);
    expect_number(" #999999999 ", 999999999L);
}

static void test_rejected_forms(void) {
    const char *invalid[] = {
        "",
        "   ",
        "#",
        "0",
        "#0",
        "012",
        "1234567890",
        "12abc",
        "#-1",
        "acme#12",
        "acme/widgets",
        "acme/widgets#",
        "acme/widgets#0",
        "acme/widgets#x",
        "acme/widgets/extra#12",
        "-acme/widgets#12",
        "acme-/widgets#12",
        "ac_me/widgets#12",
        "acme/./#12",
        "acme/..#12",
        "acme/wid gets#12",
        "acme/widgets#12#13",
        "acm\xc3\xa9/widgets#12",
        "acme/widg\xc3\xa9ts#12",
        "#\xd9\xa1",
        "https://github.com/acme/widgets/pull/12",
        "https://github.com/acme/widgets/issues",
        "https://github.com/acme/widgets/issues/12/comments",
        "https://gitlab.com/acme/widgets/issues/12",
        "https://github.com.evil/acme/widgets/issues/12",
        "ftp://github.com/acme/widgets/issues/12",
        "github.com/acme/widgets/issues/12",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        expect_parse(invalid[i], DISPATCH_ISSUE_PARSE_INVALID, NULL);

    char long_owner[64];
    memset(long_owner, 'a', 40);
    strcpy(long_owner + 40, "/r#1");
    expect_parse(long_owner, DISPATCH_ISSUE_PARSE_INVALID, NULL);
}

static void test_canonical_and_urls(void) {
    DispatchIssueRef ref = {0};
    if (!dispatch_issue_parse_canonical("acme/widgets#12", &ref) ||
        ref.number != 12 || strcmp(ref.owner, "acme") != 0 ||
        strcmp(ref.repo, "widgets") != 0) {
        fprintf(stderr, "FAIL: canonical parse\n");
        failures++;
    }
    if (dispatch_issue_parse_canonical("#12", &ref) ||
        dispatch_issue_parse_canonical(
            "https://github.com/acme/widgets/issues/12", &ref)) {
        fprintf(stderr, "FAIL: canonical parse accepted a non-canonical ref\n");
        failures++;
    }

    char text[DISPATCH_ISSUE_TEXT_MAX];
    dispatch_issue_parse_canonical("acme/widgets#12", &ref);
    dispatch_issue_format_url(&ref, text, sizeof(text));
    if (strcmp(text, "https://github.com/acme/widgets/issues/12") != 0) {
        fprintf(stderr, "FAIL: url format: %s\n", text);
        failures++;
    }
    dispatch_issue_format_repo(&ref, text, sizeof(text));
    if (strcmp(text, "acme/widgets") != 0) {
        fprintf(stderr, "FAIL: repo format: %s\n", text);
        failures++;
    }

    DispatchIssueRef bare = {.number = 5};
    if (!dispatch_issue_set_repo(&bare, "acme/widgets") ||
        bare.number != 5 || dispatch_issue_set_repo(&bare, "acme") ||
        strcmp(bare.owner, "acme") != 0) {
        fprintf(stderr, "FAIL: set repo\n");
        failures++;
    }
}

static void test_remotes(void) {
    expect_remote("git@github.com:acme/widgets.git", "acme/widgets");
    expect_remote("git@github.com:acme/widgets", "acme/widgets");
    expect_remote("https://github.com/acme/widgets.git\n", "acme/widgets");
    expect_remote("https://github.com/acme/widgets/", "acme/widgets");
    expect_remote("https://token@github.com/acme/widgets", "acme/widgets");
    expect_remote("ssh://git@github.com/acme/widgets.git", "acme/widgets");
    expect_remote("ssh://git@github.com:22/acme/widgets.git", "acme/widgets");
    expect_remote("git://github.com/acme/widgets.git", "acme/widgets");
    expect_remote("github.com:acme/widgets.git", "acme/widgets");
    expect_remote("git@gitlab.com:acme/widgets.git", NULL);
    expect_remote("https://example.com/acme/widgets.git", NULL);
    expect_remote("https://github.com/acme", NULL);
    expect_remote("https://github.com/acme/widgets/extra", NULL);
    expect_remote("/srv/git/widgets.git", NULL);
    expect_remote("../widgets", NULL);
    expect_remote("", NULL);
}

int main(void) {
    /* Parse under the same UTF-8 locale the CLI runs with. */
    setlocale(LC_ALL, "");
    test_accepted_forms();
    test_rejected_forms();
    test_canonical_and_urls();
    test_remotes();
    if (failures) {
        fprintf(stderr, "%d issue ref test failure(s)\n", failures);
        return 1;
    }
    printf("issue ref tests passed\n");
    return 0;
}
