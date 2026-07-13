#ifndef DISPATCH_EXEC_H
#define DISPATCH_EXEC_H

#include <stddef.h>

typedef enum {
    DISPATCH_EXEC_INHERIT,
    DISPATCH_EXEC_DEV_NULL,
} DispatchExecRedirect;

typedef struct {
    const char *working_directory;
    DispatchExecRedirect stdin_mode;
    DispatchExecRedirect stdout_mode;
    DispatchExecRedirect stderr_mode;
    int merge_stderr_to_stdout;
} DispatchExecOptions;

typedef struct {
    int exited;
    int exit_code;
    int signaled;
    int signal_number;
} DispatchExecResult;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} DispatchExecArgv;

int dispatch_exec_argv_append(DispatchExecArgv *argv, const char *value);
int dispatch_exec_argv_parse(DispatchExecArgv *argv, const char *command);
void dispatch_exec_argv_free(DispatchExecArgv *argv);

int dispatch_exec_command_available(const char *command);
int dispatch_exec_run(const char *const argv[],
                      const DispatchExecOptions *options,
                      DispatchExecResult *result);
int dispatch_exec_capture(const char *const argv[],
                          const DispatchExecOptions *options, char **output,
                          size_t *output_size, DispatchExecResult *result);
int dispatch_exec_feed(const char *const argv[],
                       const DispatchExecOptions *options, const void *input,
                       size_t input_size, DispatchExecResult *result);
int dispatch_exec_pipeline(const char *const producer_argv[],
                           const DispatchExecOptions *producer_options,
                           const char *const consumer_argv[],
                           const DispatchExecOptions *consumer_options,
                           DispatchExecResult *result);

int dispatch_exec_result_success(const DispatchExecResult *result);
int dispatch_exec_result_status(const DispatchExecResult *result);

#endif
