#include "dispatch_cli_internal.h"

#include <stdarg.h>

/* Per-process --json state for read and mutating commands. The dispatcher
 * begins it before running a command and ends it afterwards, so every
 * failing path produces one JSON error document on stderr. */
typedef struct {
    int active;
    const char *command;
    int has_error;
    int suppress_error;
    char code[48];
    char task_id[128];
    char *message;
    DispatchJsonWarning *warnings;
    size_t warning_count;
    size_t warning_capacity;
    int snapshot_taken;
    DispatchJsonSnapshot before;
} CliJsonContext;

static CliJsonContext context;

void cli_json_begin(const char *command) {
    context.active = 1;
    context.command = command;
}

int cli_json(void) {
    return context.active;
}

static void append_message(const char *text) {
    size_t text_len = strlen(text);
    while (text_len > 0 && text[text_len - 1] == '\n')
        text_len--;
    if (text_len == 0)
        return;

    size_t old_len = context.message ? strlen(context.message) : 0;
    size_t separator = old_len > 0 ? 1 : 0;
    char *message = realloc(context.message, old_len + separator + text_len + 1);
    if (!message) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    if (separator)
        message[old_len] = '\n';
    memcpy(message + old_len + separator, text, text_len);
    message[old_len + separator + text_len] = '\0';
    context.message = message;
}

void cli_error(const char *code, const char *task_id, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (!context.active) {
        vfprintf(stderr, fmt, args);
        va_end(args);
        return;
    }

    char text[2048];
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    /* The first error names the failure; later lines (usage continuations,
     * rollback notes) only extend the message. */
    if (!context.has_error) {
        context.has_error = 1;
        snprintf(context.code, sizeof(context.code), "%s", code);
        snprintf(context.task_id, sizeof(context.task_id), "%s",
                 task_id ? task_id : "");
    }
    append_message(text);
}

void cli_warning(const char *code, const char *task_id, const char *agent,
                 const char *fmt, ...) {
    if (!context.active)
        return;

    char text[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    size_t len = strlen(text);
    while (len > 0 && text[len - 1] == '\n')
        text[--len] = '\0';

    if (context.warning_count >= context.warning_capacity) {
        context.warning_capacity =
            context.warning_capacity == 0 ? 4 : context.warning_capacity * 2;
        context.warnings = cli_realloc_array(context.warnings,
                                             context.warning_capacity,
                                             sizeof(*context.warnings));
    }

    DispatchJsonWarning *warning = &context.warnings[context.warning_count++];
    warning->code = cli_strdup(code);
    warning->message = cli_strdup(text);
    warning->task_id = task_id && task_id[0] ? cli_strdup(task_id) : NULL;
    warning->agent = agent && agent[0] ? cli_strdup(agent) : NULL;
}

void cli_json_snapshot(const DispatchBoard *board) {
    if (!context.active || context.snapshot_taken)
        return;
    dispatch_json_snapshot_take(&context.before, board);
    context.snapshot_taken = 1;
}

int cli_json_finish_emit(int ok) {
    if (ok)
        return 0;
    cli_error("output_failed", NULL, "Could not write JSON result: %s\n",
              strerror(errno));
    return 1;
}

void cli_json_suppress_error(void) {
    context.suppress_error = 1;
}

void cli_json_reset_error(void) {
    context.has_error = 0;
    context.suppress_error = 0;
    context.code[0] = '\0';
    context.task_id[0] = '\0';
    free(context.message);
    context.message = NULL;
}

int cli_json_emit_tasks(const DispatchBoard *board, const char *group,
                        DispatchJsonResult *result) {
    int ok = dispatch_json_emit_mutation(stdout, board, context.command, group,
                                         &context.before, result,
                                         context.warnings,
                                         context.warning_count);
    dispatch_json_result_free(result);
    return cli_json_finish_emit(ok);
}

int cli_json_emit_workspaces(const DispatchBoard *board,
                             const DispatchWorkspace *records, size_t count,
                             DispatchJsonResult *result) {
    int ok = dispatch_json_emit_workspace_mutation(
        stdout, board, context.command, records, count, &context.before, result,
        context.warnings, context.warning_count);
    dispatch_json_result_free(result);
    return cli_json_finish_emit(ok);
}

int cli_json_emit_agent(const DispatchBoard *board, const DispatchAgent *agent,
                        DispatchJsonResult *result) {
    int ok = dispatch_json_emit_agent_mutation(
        stdout, board, context.command, agent, &context.before, result,
        context.warnings, context.warning_count);
    dispatch_json_result_free(result);
    return cli_json_finish_emit(ok);
}

int cli_json_end(int status) {
    if (context.active && status != 0 && !context.suppress_error) {
        dispatch_json_emit_error(
            stderr, context.command,
            context.has_error ? context.code : "internal_error",
            context.message ? context.message : "Command failed",
            context.task_id[0] ? context.task_id : NULL);
    }
    free(context.message);
    for (size_t i = 0; i < context.warning_count; i++) {
        free(context.warnings[i].code);
        free(context.warnings[i].message);
        free(context.warnings[i].task_id);
        free(context.warnings[i].agent);
    }
    free(context.warnings);
    dispatch_json_snapshot_free(&context.before);
    memset(&context, 0, sizeof(context));
    return status;
}
