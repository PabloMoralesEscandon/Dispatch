#include "dispatch_store.h"

#include <jansson.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *message, const char *detail) {
    if (detail && detail[0])
        fprintf(stderr, "FAIL: %s: %s\n", message, detail);
    else
        fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int assert_json_string(json_t *object, const char *key,
                              const char *expected) {
    json_t *value = json_object_get(object, key);
    if (!json_is_string(value))
        return fail("expected string field", key);
    if (strcmp(json_string_value(value), expected) != 0)
        return fail("unexpected string field", key);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2)
        return fail("usage: log_writer_test <log-path>", "");

    DispatchLogField targets[] = {
        {"task", "DE-01"},
    };
    DispatchLogField context[] = {
        {"new_state", "ready"},
    };
    DispatchLogRecord record = {
        .actor = "user",
        .command = "ready",
        .action = "ready",
        .outcome = "success",
        .message = "Readied DE-01",
        .targets = targets,
        .target_count = 1,
        .context = context,
        .context_count = 1,
    };

    char error[256] = {0};
    if (!dispatch_store_log_append(argv[1], &record, error, sizeof(error)))
        return fail("append failed", error);

    FILE *file = fopen(argv[1], "r");
    if (!file)
        return fail("could not open log", argv[1]);

    char line[2048] = {0};
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return fail("could not read log line", "");
    }
    int extra = fgetc(file);
    fclose(file);
    if (extra != EOF)
        return fail("expected one log line", "");

    json_error_t json_error;
    json_t *entry = json_loads(line, 0, &json_error);
    if (!entry)
        return fail("invalid json", json_error.text);

    json_t *version = json_object_get(entry, "version");
    if (!json_is_integer(version) || json_integer_value(version) != 1) {
        json_decref(entry);
        return fail("unexpected version", "");
    }

    if (assert_json_string(entry, "actor", "user") ||
        assert_json_string(entry, "command", "ready") ||
        assert_json_string(entry, "action", "ready") ||
        assert_json_string(entry, "outcome", "success") ||
        assert_json_string(entry, "message", "Readied DE-01")) {
        json_decref(entry);
        return 1;
    }

    json_t *timestamp = json_object_get(entry, "timestamp");
    if (!json_is_string(timestamp) ||
        strlen(json_string_value(timestamp)) != strlen("2026-06-19T12:34:56Z")) {
        json_decref(entry);
        return fail("unexpected timestamp", "");
    }

    json_t *loaded_targets = json_object_get(entry, "targets");
    json_t *loaded_context = json_object_get(entry, "context");
    if (!json_is_object(loaded_targets) || !json_is_object(loaded_context)) {
        json_decref(entry);
        return fail("targets/context were not objects", "");
    }
    if (assert_json_string(loaded_targets, "task", "DE-01") ||
        assert_json_string(loaded_context, "new_state", "ready")) {
        json_decref(entry);
        return 1;
    }

    json_decref(entry);
    printf("PASS: log writer\n");
    return 0;
}
