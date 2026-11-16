#include "dispatch_issue.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int parse_number(const char *text, size_t len, long *number) {
    if (len == 0 || len > 9 || text[0] == '0')
        return 0;
    long value = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)text[i]))
            return 0;
        value = value * 10 + (text[i] - '0');
    }
    if (value < 1 || value > DISPATCH_ISSUE_NUMBER_MAX)
        return 0;
    *number = value;
    return 1;
}

/* GitHub owners: letters, digits and single inner hyphens. */
static int owner_is_valid(const char *text, size_t len) {
    if (len == 0 || len > DISPATCH_ISSUE_OWNER_MAX)
        return 0;
    if (text[0] == '-' || text[len - 1] == '-')
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!isalnum(c) && c != '-')
            return 0;
    }
    return 1;
}

/* GitHub repositories: letters, digits, '.', '_' and '-', never "." or
 * "..". */
static int repo_is_valid(const char *text, size_t len) {
    if (len == 0 || len > DISPATCH_ISSUE_REPO_MAX)
        return 0;
    if ((len == 1 && text[0] == '.') ||
        (len == 2 && text[0] == '.' && text[1] == '.'))
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-')
            return 0;
    }
    return 1;
}

static int set_owner_repo(DispatchIssueRef *ref, const char *text,
                          size_t len) {
    const char *slash = memchr(text, '/', len);
    if (!slash)
        return 0;
    size_t owner_len = (size_t)(slash - text);
    const char *repo = slash + 1;
    size_t repo_len = len - owner_len - 1;
    if (!owner_is_valid(text, owner_len) || !repo_is_valid(repo, repo_len))
        return 0;
    memcpy(ref->owner, text, owner_len);
    ref->owner[owner_len] = '\0';
    memcpy(ref->repo, repo, repo_len);
    ref->repo[repo_len] = '\0';
    return 1;
}

static int host_is_github(const char *host, size_t len) {
    return (len == 10 && strncasecmp(host, "github.com", 10) == 0) ||
           (len == 14 && strncasecmp(host, "www.github.com", 14) == 0);
}

static void trim_bounds(const char *input, const char **start, size_t *len) {
    const char *s = input ? input : "";
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    *start = s;
    *len = n;
}

static int starts_with_ci(const char *text, size_t len, const char *prefix) {
    size_t plen = strlen(prefix);
    return len >= plen && strncasecmp(text, prefix, plen) == 0;
}

/* https://github.com/owner/repo/issues/N[/][?query][#fragment] */
static int parse_issue_url(const char *text, size_t len,
                           DispatchIssueRef *ref) {
    size_t skip = starts_with_ci(text, len, "https://") ? 8
                  : starts_with_ci(text, len, "http://") ? 7
                                                          : 0;
    if (skip == 0)
        return 0;
    text += skip;
    len -= skip;

    /* Drop any query or fragment, then one trailing slash. */
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '?' || text[i] == '#') {
            len = i;
            break;
        }
    }
    if (len > 0 && text[len - 1] == '/')
        len--;

    const char *slash = memchr(text, '/', len);
    if (!slash || !host_is_github(text, (size_t)(slash - text)))
        return 0;
    const char *path = slash + 1;
    size_t path_len = len - (size_t)(path - text);

    /* path is owner/repo/issues/N */
    const char *segments[4];
    size_t lengths[4];
    size_t count = 0;
    const char *segment = path;
    const char *end = path + path_len;
    for (;;) {
        const char *next = memchr(segment, '/', (size_t)(end - segment));
        if (count == 4)
            return 0;
        segments[count] = segment;
        lengths[count] = (size_t)((next ? next : end) - segment);
        count++;
        if (!next)
            break;
        segment = next + 1;
    }
    if (count != 4)
        return 0;
    if (lengths[2] != 6 || strncmp(segments[2], "issues", 6) != 0)
        return 0;

    DispatchIssueRef parsed = {0};
    if (!owner_is_valid(segments[0], lengths[0]) ||
        !repo_is_valid(segments[1], lengths[1]) ||
        !parse_number(segments[3], lengths[3], &parsed.number))
        return 0;
    memcpy(parsed.owner, segments[0], lengths[0]);
    parsed.owner[lengths[0]] = '\0';
    memcpy(parsed.repo, segments[1], lengths[1]);
    parsed.repo[lengths[1]] = '\0';
    *ref = parsed;
    return 1;
}

static int parse_canonical_bounds(const char *text, size_t len,
                                  DispatchIssueRef *ref) {
    const char *hash = memchr(text, '#', len);
    if (!hash)
        return 0;
    DispatchIssueRef parsed = {0};
    size_t left = (size_t)(hash - text);
    if (!set_owner_repo(&parsed, text, left) ||
        !parse_number(hash + 1, len - left - 1, &parsed.number))
        return 0;
    *ref = parsed;
    return 1;
}

DispatchIssueParse dispatch_issue_parse(const char *input,
                                        DispatchIssueRef *ref) {
    const char *text;
    size_t len;
    trim_bounds(input, &text, &len);
    if (len == 0 || !ref)
        return DISPATCH_ISSUE_PARSE_INVALID;

    DispatchIssueRef parsed = {0};
    if (text[0] == '#' || isdigit((unsigned char)text[0])) {
        const char *digits = text[0] == '#' ? text + 1 : text;
        size_t digits_len = text[0] == '#' ? len - 1 : len;
        if (parse_number(digits, digits_len, &parsed.number)) {
            *ref = parsed;
            return DISPATCH_ISSUE_PARSE_NEEDS_REPO;
        }
        /* An owner may start with a digit: fall through to owner/repo#N. */
    }
    if (parse_issue_url(text, len, &parsed) ||
        parse_canonical_bounds(text, len, &parsed)) {
        *ref = parsed;
        return DISPATCH_ISSUE_PARSE_OK;
    }
    return DISPATCH_ISSUE_PARSE_INVALID;
}

int dispatch_issue_parse_canonical(const char *value, DispatchIssueRef *ref) {
    if (!value || !ref)
        return 0;
    return parse_canonical_bounds(value, strlen(value), ref);
}

int dispatch_issue_set_repo(DispatchIssueRef *ref, const char *owner_repo) {
    if (!ref || !owner_repo)
        return 0;
    DispatchIssueRef parsed = *ref;
    if (!set_owner_repo(&parsed, owner_repo, strlen(owner_repo)))
        return 0;
    *ref = parsed;
    return 1;
}

int dispatch_issue_repo_from_remote(const char *remote, char *out,
                                    size_t out_size) {
    const char *text;
    size_t len;
    trim_bounds(remote, &text, &len);
    if (len == 0 || !out || out_size == 0)
        return 0;

    while (len > 0 && text[len - 1] == '/')
        len--;
    if (len >= 4 && strncmp(text + len - 4, ".git", 4) == 0)
        len -= 4;

    const char *host;
    size_t host_len;
    const char *path;
    const char *scheme_end = NULL;
    for (size_t i = 0; i + 2 < len; i++) {
        if (text[i] == ':' && text[i + 1] == '/' && text[i + 2] == '/') {
            scheme_end = text + i;
            break;
        }
        if (text[i] == '/' || text[i] == '@')
            break;
    }

    if (scheme_end) {
        /* scheme://[user[:password]@]host[:port]/owner/repo */
        const char *authority = scheme_end + 3;
        const char *end = text + len;
        const char *slash = memchr(authority, '/', (size_t)(end - authority));
        if (!slash)
            return 0;
        host = authority;
        for (const char *c = authority; c < slash; c++) {
            if (*c == '@')
                host = c + 1;
        }
        const char *colon = memchr(host, ':', (size_t)(slash - host));
        host_len = (size_t)((colon ? colon : slash) - host);
        path = slash + 1;
    } else {
        /* scp-like [user@]host:owner/repo */
        const char *colon = memchr(text, ':', len);
        if (!colon)
            return 0;
        host = text;
        for (const char *c = text; c < colon; c++) {
            if (*c == '@')
                host = c + 1;
            if (*c == '/')
                return 0;
        }
        host_len = (size_t)(colon - host);
        path = colon + 1;
    }

    if (!host_is_github(host, host_len))
        return 0;

    size_t path_len = len - (size_t)(path - text);
    DispatchIssueRef parsed = {0};
    if (!set_owner_repo(&parsed, path, path_len))
        return 0;
    int written = snprintf(out, out_size, "%s/%s", parsed.owner, parsed.repo);
    return written > 0 && (size_t)written < out_size;
}

void dispatch_issue_format(const DispatchIssueRef *ref, char *out,
                           size_t out_size) {
    snprintf(out, out_size, "%s/%s#%ld", ref->owner, ref->repo, ref->number);
}

void dispatch_issue_format_repo(const DispatchIssueRef *ref, char *out,
                                size_t out_size) {
    snprintf(out, out_size, "%s/%s", ref->owner, ref->repo);
}

void dispatch_issue_format_url(const DispatchIssueRef *ref, char *out,
                               size_t out_size) {
    snprintf(out, out_size, "https://github.com/%s/%s/issues/%ld", ref->owner,
             ref->repo, ref->number);
}
