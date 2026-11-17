#include "dispatch_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const dispatch_log_filter_fields[DISPATCH_LOG_FILTER_COUNT] = {
    "task", "actor", "agent", "workspace", "command", "action",
};

int dispatch_log_filter_index(const char *field) {
    for (int i = 0; field && i < DISPATCH_LOG_FILTER_COUNT; i++) {
        if (strcmp(dispatch_log_filter_fields[i], field) == 0)
            return i;
    }
    return -1;
}

static const char *string_field(json_t *object, const char *name) {
    json_t *value = json_object_get(object, name);
    return json_is_string(value) ? json_string_value(value) : "";
}

static const char *target_field(json_t *record, const char *name) {
    json_t *targets = json_object_get(record, "targets");
    return json_is_object(targets) ? string_field(targets, name) : "";
}

int dispatch_log_filter_values(json_t *record, const char *field,
                               const char *values[2]) {
    switch (dispatch_log_filter_index(field)) {
    case 0:
        values[0] = target_field(record, "task");
        return 1;
    case 1:
        values[0] = string_field(record, "actor");
        return 1;
    case 2:
        /* An agent is involved when it is the target or the actor. */
        values[0] = target_field(record, "agent");
        values[1] = string_field(record, "actor");
        return 2;
    case 3:
        values[0] = target_field(record, "workspace");
        return 1;
    case 4:
        values[0] = string_field(record, "command");
        return 1;
    case 5:
        values[0] = string_field(record, "action");
        return 1;
    }
    return -1;
}

static int query_matches(json_t *record, const DispatchLogQuery *query) {
    for (int i = 0; i < DISPATCH_LOG_FILTER_COUNT; i++) {
        const char *wanted = query ? query->filters[i] : NULL;
        if (!wanted)
            continue;
        const char *values[2] = {"", ""};
        int count = dispatch_log_filter_values(
            record, dispatch_log_filter_fields[i], values);
        int matched = 0;
        for (int v = 0; v < count && !matched; v++)
            matched = strcmp(values[v], wanted) == 0;
        if (!matched)
            return 0;
    }
    return 1;
}

typedef enum {
    LOG_LINE_OK,
    LOG_LINE_MALFORMED,
    LOG_LINE_UNSUPPORTED,
} LogLineKind;

static int optional_type_ok(json_t *value, int (*is_type)(const json_t *)) {
    return !value || json_is_null(value) || is_type(value);
}

static int is_string(const json_t *value) { return json_is_string(value); }
static int is_object(const json_t *value) { return json_is_object(value); }

/* Builds the normalized record: known fields in a fixed order with absent
 * optional fields filled in, then any fields this reader does not know. */
static LogLineKind normalize_record(json_t *parsed, size_t line,
                                    json_t **normalized) {
    *normalized = NULL;
    if (!json_is_object(parsed))
        return LOG_LINE_MALFORMED;
    json_t *version = json_object_get(parsed, "version");
    if (!json_is_integer(version))
        return LOG_LINE_MALFORMED;
    if (json_integer_value(version) != DISPATCH_LOG_VERSION)
        return LOG_LINE_UNSUPPORTED;

    static const char *const required[] = {"timestamp", "actor", "command",
                                           "action"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (!json_is_string(json_object_get(parsed, required[i])))
            return LOG_LINE_MALFORMED;
    }
    json_t *targets = json_object_get(parsed, "targets");
    json_t *outcome = json_object_get(parsed, "outcome");
    json_t *message = json_object_get(parsed, "message");
    json_t *context = json_object_get(parsed, "context");
    if (!optional_type_ok(targets, is_object) ||
        !optional_type_ok(outcome, is_string) ||
        !optional_type_ok(message, is_string) ||
        !optional_type_ok(context, is_object))
        return LOG_LINE_MALFORMED;

    json_t *record = json_object();
    json_object_set_new(record, "line", json_integer((json_int_t)line));
    json_object_set(record, "version", version);
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++)
        json_object_set(record, required[i],
                        json_object_get(parsed, required[i]));
    if (json_is_object(targets))
        json_object_set(record, "targets", targets);
    else
        json_object_set_new(record, "targets", json_object());
    json_object_set_new(record, "outcome",
                        outcome ? json_incref(outcome) : json_null());
    json_object_set_new(record, "message",
                        message ? json_incref(message) : json_null());
    json_object_set_new(record, "context",
                        context ? json_incref(context) : json_null());

    const char *key;
    json_t *value;
    json_object_foreach(parsed, key, value) {
        if (!json_object_get(record, key))
            json_object_set(record, key, value);
    }
    *normalized = record;
    return LOG_LINE_OK;
}

static int line_is_blank(const char *line, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r' &&
            line[i] != '\n')
            return 0;
    }
    return 1;
}

static void append_entry(DispatchLogRead *out, size_t line, json_t *record) {
    if (out->count >= out->capacity) {
        size_t capacity = out->capacity == 0 ? 16 : out->capacity * 2;
        DispatchLogEntry *items =
            realloc(out->items, capacity * sizeof(*items));
        if (!items) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        out->items = items;
        out->capacity = capacity;
    }
    out->items[out->count].line = line;
    out->items[out->count].record = record;
    out->count++;
}

/* Keeps only the newest limit entries. */
static void apply_limit(DispatchLogRead *out, size_t limit) {
    if (limit == 0 || out->count <= limit)
        return;
    size_t drop = out->count - limit;
    for (size_t i = 0; i < drop; i++)
        json_decref(out->items[i].record);
    memmove(out->items, out->items + drop, limit * sizeof(*out->items));
    out->count = limit;
}

int dispatch_log_read(const char *path, const DispatchLogQuery *query,
                      DispatchLogRead *out, char *error, size_t error_size) {
    memset(out, 0, sizeof(*out));
    FILE *file = fopen(path, "r");
    if (!file) {
        if (errno == ENOENT) {
            out->missing = 1;
            return 1;
        }
        snprintf(error, error_size, "%s", strerror(errno));
        return 0;
    }

    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t length;
    size_t line_number = 0;
    while ((length = getline(&line, &line_capacity, file)) >= 0) {
        line_number++;
        if (line_is_blank(line, (size_t)length))
            continue;

        json_error_t parse_error;
        json_t *parsed = json_loadb(line, (size_t)length, 0, &parse_error);
        json_t *record = NULL;
        LogLineKind kind =
            parsed ? normalize_record(parsed, line_number, &record)
                   : LOG_LINE_MALFORMED;
        json_decref(parsed);

        if (kind == LOG_LINE_MALFORMED) {
            if (out->malformed++ == 0)
                out->first_malformed_line = line_number;
            continue;
        }
        if (kind == LOG_LINE_UNSUPPORTED) {
            if (out->unsupported++ == 0)
                out->first_unsupported_line = line_number;
            continue;
        }

        out->total++;
        if (!query_matches(record, query)) {
            json_decref(record);
            continue;
        }
        out->matched++;
        append_entry(out, line_number, record);
        /* Trim as we go so a limited read of a long log stays small. */
        size_t limit = query ? query->limit : 0;
        if (limit && out->count > limit && out->count - limit >= limit)
            apply_limit(out, limit);
    }

    int read_failed = ferror(file);
    int saved_errno = errno;
    free(line);
    fclose(file);
    if (read_failed) {
        snprintf(error, error_size, "%s", strerror(saved_errno));
        dispatch_log_read_free(out);
        return 0;
    }
    apply_limit(out, query ? query->limit : 0);
    return 1;
}

void dispatch_log_read_free(DispatchLogRead *read) {
    if (!read)
        return;
    for (size_t i = 0; i < read->count; i++)
        json_decref(read->items[i].record);
    free(read->items);
    memset(read, 0, sizeof(*read));
}

size_t dispatch_log_read_warnings(const DispatchLogRead *read,
                                  const char *path,
                                  DispatchLogWarning warnings[2]) {
    size_t count = 0;
    if (read->malformed > 0) {
        DispatchLogWarning *warning = &warnings[count++];
        warning->code = "malformed_log_lines";
        warning->count = read->malformed;
        warning->first_line = read->first_malformed_line;
        snprintf(warning->message, sizeof(warning->message),
                 "skipped %zu malformed line%s in %s, first at line %zu",
                 read->malformed, read->malformed == 1 ? "" : "s", path,
                 read->first_malformed_line);
    }
    if (read->unsupported > 0) {
        DispatchLogWarning *warning = &warnings[count++];
        warning->code = "unsupported_log_version";
        warning->count = read->unsupported;
        warning->first_line = read->first_unsupported_line;
        snprintf(warning->message, sizeof(warning->message),
                 "skipped %zu record%s with an unsupported version in %s, "
                 "first at line %zu",
                 read->unsupported, read->unsupported == 1 ? "" : "s", path,
                 read->first_unsupported_line);
    }
    return count;
}
