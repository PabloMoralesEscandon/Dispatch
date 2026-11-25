#include "dispatch_cli_internal.h"

#include "dispatch_log.h"

#include <stdint.h>

#define LOG_USAGE                                                            \
    "Usage: dispatch log [--task <id>] [--actor <name>] [--agent <name>] "   \
    "[--workspace <id>] [--command <name>] [--action <name>] [--limit <n>] " \
    "[--json]\n"

static int parse_limit(const char *text, size_t *limit) {
    if (!text || !text[0])
        return 0;
    for (const char *cursor = text; *cursor; cursor++) {
        if (!isdigit((unsigned char)*cursor))
            return 0;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !end || *end != '\0' || value == 0 || value > SIZE_MAX)
        return 0;
    *limit = (size_t)value;
    return 1;
}

/* Fills query from argv[2..]; returns 0 on a usage error. */
static int parse_log_args(int argc, char **argv, DispatchLogQuery *query) {
    int have_limit = 0;
    for (int i = 2; i < argc; i++) {
        const char *option = argv[i];
        if (strncmp(option, "--", 2) != 0 || i + 1 >= argc)
            return 0;
        const char *value = argv[++i];
        if (strcmp(option, "--limit") == 0) {
            if (have_limit || !parse_limit(value, &query->limit))
                return 0;
            have_limit = 1;
            continue;
        }
        int index = dispatch_log_filter_index(option + 2);
        if (index < 0 || query->filters[index] || !value[0])
            return 0;
        query->filters[index] = value;
    }
    return 1;
}

static void print_targets(json_t *targets) {
    const char *key;
    json_t *value;
    int first = 1;
    json_object_foreach(targets, key, value) {
        if (!json_is_string(value))
            continue;
        printf("%s%s:%s", first ? "" : " ", key, json_string_value(value));
        first = 0;
    }
    if (!first)
        printf("  ");
}

static void print_log_record(json_t *record) {
    json_t *message = json_object_get(record, "message");
    printf("%-20s %-16s %-10s %-20s ",
           json_string_value(json_object_get(record, "timestamp")),
           json_string_value(json_object_get(record, "actor")),
           json_string_value(json_object_get(record, "command")),
           json_string_value(json_object_get(record, "action")));
    print_targets(json_object_get(record, "targets"));
    printf("%s\n", json_is_string(message) ? json_string_value(message) : "");
}

int cmd_log(int argc, char **argv) {
    int json_output = cli_json();
    DispatchLogQuery query;
    memset(&query, 0, sizeof(query));
    if (!parse_log_args(argc, argv, &query)) {
        cli_error("usage", NULL, LOG_USAGE);
        return 1;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    DispatchLogRead read;
    char error[256] = {0};
    if (!dispatch_log_read(DISPATCH_LOG_FILE, &query, &read, error,
                           sizeof(error))) {
        cli_error("log_unreadable", NULL, "Could not read %s: %s\n",
                  DISPATCH_LOG_FILE, error);
        dispatch_board_free(&board);
        return 1;
    }

    int status = 0;
    if (json_output) {
        status = cli_json_finish_emit(dispatch_json_emit_log(
            stdout, &board, DISPATCH_LOG_FILE, &query, &read));
    } else {
        if (read.missing)
            printf("(no %s)\n", DISPATCH_LOG_FILE);
        else if (read.count == 0)
            printf("(no log records)\n");
        for (size_t i = 0; i < read.count; i++)
            print_log_record(read.items[i].record);

        DispatchLogWarning warnings[2];
        size_t count =
            dispatch_log_read_warnings(&read, DISPATCH_LOG_FILE, warnings);
        for (size_t i = 0; i < count; i++)
            fprintf(stderr, "Warning: %s\n", warnings[i].message);
    }

    dispatch_log_read_free(&read);
    dispatch_board_free(&board);
    return status;
}
