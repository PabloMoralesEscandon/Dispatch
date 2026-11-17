#ifndef DISPATCH_LOG_H
#define DISPATCH_LOG_H

/* Reading dispatch.log, the JSON Lines audit trail written by
 * dispatch_store_log_append. Shared by `dispatch log` and the TUI logs
 * view; see docs/adr/0004-json-reads-agents-workspaces-log.md. */

#include <jansson.h>
#include <stddef.h>

/* The log record version this reader understands. */
#define DISPATCH_LOG_VERSION 1

/* Filter fields, in the order `dispatch log` reports them in its query. */
#define DISPATCH_LOG_FILTER_COUNT 6
extern const char *const dispatch_log_filter_fields[DISPATCH_LOG_FILTER_COUNT];

/* Index of a filter field name in dispatch_log_filter_fields, or -1. */
int dispatch_log_filter_index(const char *field);

/* Points values at the record strings a filter on field compares against
 * and returns how many there are: one, or two for "agent" (targets.agent,
 * then actor). A missing or non-string value is "". Returns -1 for an
 * unknown field. */
int dispatch_log_filter_values(json_t *record, const char *field,
                               const char *values[2]);

typedef struct {
    size_t line;    /* 1-based line number in the log file */
    json_t *record; /* normalized record, owned by the entry list */
} DispatchLogEntry;

typedef struct {
    /* Exact-match filter values indexed like dispatch_log_filter_fields;
     * NULL means no filter on that field. All set filters must match. */
    const char *filters[DISPATCH_LOG_FILTER_COUNT];
    size_t limit; /* keep only the newest limit matches; 0 keeps all */
} DispatchLogQuery;

typedef struct {
    int missing;               /* the log file does not exist */
    DispatchLogEntry *items;   /* matching records, oldest first */
    size_t count;
    size_t capacity;
    size_t total;              /* readable records, before filtering */
    size_t matched;            /* readable records matching the filters */
    size_t malformed;          /* lines that are not a valid record */
    size_t first_malformed_line;
    size_t unsupported;        /* valid JSON objects with another version */
    size_t first_unsupported_line;
} DispatchLogRead;

/* Reads every line of path. Blank lines are ignored; malformed lines and
 * records of another version are skipped and counted, never fatal. A
 * missing file reads as empty with missing set. Returns 0 and fills error
 * only when the file exists but cannot be read. */
int dispatch_log_read(const char *path, const DispatchLogQuery *query,
                      DispatchLogRead *out, char *error, size_t error_size);
void dispatch_log_read_free(DispatchLogRead *read);

/* One warning about skipped lines: code is "malformed_log_lines" or
 * "unsupported_log_version". */
typedef struct {
    const char *code;
    size_t count;
    size_t first_line;
    char message[192];
} DispatchLogWarning;

/* Fills up to two warnings describing the lines read skipped, malformed
 * first; returns how many. path names the log in the messages. */
size_t dispatch_log_read_warnings(const DispatchLogRead *read,
                                  const char *path,
                                  DispatchLogWarning warnings[2]);

#endif
