#include "dispatch_exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int child_main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "print") == 0) {
        for (int i = 3; i < argc; i++)
            printf("%s\n", argv[i]);
        return 0;
    }
    if (argc >= 4 && strcmp(argv[2], "exit") == 0)
        return atoi(argv[3]);
    if (argc >= 4 && strcmp(argv[2], "input") == 0) {
        char buffer[256];
        size_t count = fread(buffer, 1, sizeof(buffer), stdin);
        return count == strlen(argv[3]) && memcmp(buffer, argv[3], count) == 0
                   ? 0
                   : 1;
    }
    if (argc >= 4 && strcmp(argv[2], "consume") == 0) {
        char buffer[256];
        size_t count = fread(buffer, 1, sizeof(buffer), stdin);
        return count == strlen(argv[3]) && memcmp(buffer, argv[3], count) == 0
                   ? 0
                   : 1;
    }
    if (argc >= 3 && strcmp(argv[2], "cwd") == 0) {
        char buffer[1024];
        if (!getcwd(buffer, sizeof(buffer)))
            return 1;
        puts(buffer);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "both") == 0) {
        /* Emit more than a typical pipe buffer to both streams to verify
         * concurrent draining does not deadlock. */
        for (int i = 0; i < 4000; i++) {
            fputs("stdout-line-0123456789\n", stdout);
            fputs("stderr-line-9876543210\n", stderr);
        }
        return 3;
    }
    return 2;
}

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "exec helper test failed: %s\n", message);
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--child") == 0)
        return child_main(argc, argv);
    require(argc == 2, "expected temporary directory");

    const char *literal = "space ; $HOME $(touch-not-run) ' quote";
    const char *print_argv[] = {argv[0], "--child", "print", literal, NULL};
    char *output = NULL;
    size_t output_size = 0;
    DispatchExecResult result;
    require(
        dispatch_exec_capture(print_argv, NULL, &output, &output_size, &result),
        "capture failed");
    require(dispatch_exec_result_success(&result), "capture child failed");
    require(output_size == strlen(literal) + 1, "capture size mismatch");
    require(strncmp(output, literal, strlen(literal)) == 0,
            "argv was not passed literally");
    free(output);

    const char *exit_argv[] = {argv[0], "--child", "exit", "7", NULL};
    DispatchExecOptions quiet = {
        .stdout_mode = DISPATCH_EXEC_DEV_NULL,
        .stderr_mode = DISPATCH_EXEC_DEV_NULL,
    };
    require(dispatch_exec_run(exit_argv, &quiet, &result), "run failed");
    require(dispatch_exec_result_status(&result) == 7, "wrong exit status");

    const char *input = "input with spaces; no shell";
    const char *input_argv[] = {argv[0], "--child", "input", input, NULL};
    require(dispatch_exec_feed(input_argv, NULL, input, strlen(input), &result),
            "feed failed");
    require(dispatch_exec_result_success(&result), "feed child failed");

    const char *producer_argv[] = {argv[0], "--child", "print", "pipeline",
                                   NULL};
    const char *consumer_argv[] = {argv[0], "--child", "consume", "pipeline\n",
                                   NULL};
    require(dispatch_exec_pipeline(producer_argv, NULL, consumer_argv, NULL,
                                   &result),
            "pipeline failed");
    require(dispatch_exec_result_success(&result), "pipeline child failed");

    DispatchExecOptions cwd_options = {.working_directory = argv[1]};
    const char *cwd_argv[] = {argv[0], "--child", "cwd", NULL};
    require(dispatch_exec_capture(cwd_argv, &cwd_options, &output, &output_size,
                                  &result),
            "working-directory capture failed");
    require(dispatch_exec_result_success(&result),
            "working-directory child failed");
    require(strncmp(output, argv[1], strlen(argv[1])) == 0,
            "working directory was not applied");
    free(output);

    DispatchExecArgv parsed = {0};
    require(dispatch_exec_argv_parse(
                &parsed, "editor --flag 'path with spaces' \"double quoted\""),
            "command parsing failed");
    require(parsed.count == 4, "wrong parsed argument count");
    require(strcmp(parsed.items[2], "path with spaces") == 0,
            "single-quoted argument mismatch");
    require(strcmp(parsed.items[3], "double quoted") == 0,
            "double-quoted argument mismatch");
    dispatch_exec_argv_free(&parsed);
    require(!dispatch_exec_argv_parse(&parsed, "editor 'unterminated"),
            "unterminated quote was accepted");

    const char *missing_argv[] = {"dispatch-missing-command-xyz", NULL};
    require(dispatch_exec_run(missing_argv, &quiet, &result),
            "missing command did not produce a child result");
    require(dispatch_exec_result_status(&result) == 127,
            "missing command did not exit 127");

    require(dispatch_exec_command_available(argv[0]),
            "test executable should be available");
    require(!dispatch_exec_command_available("dispatch-missing-command-xyz"),
            "missing command reported available");

    const char *both_argv[] = {argv[0], "--child", "both", NULL};
    char *both_out = NULL;
    char *both_err = NULL;
    size_t both_out_size = 0;
    size_t both_err_size = 0;
    require(dispatch_exec_capture_streams(both_argv, NULL, &both_out,
                                          &both_out_size, &both_err,
                                          &both_err_size, &result),
            "capture streams failed");
    require(dispatch_exec_result_status(&result) == 3,
            "capture streams wrong exit status");
    require(both_out_size == 4000 * strlen("stdout-line-0123456789\n"),
            "capture streams stdout size mismatch");
    require(both_err_size == 4000 * strlen("stderr-line-9876543210\n"),
            "capture streams stderr size mismatch");
    require(strstr(both_out, "stderr-line") == NULL,
            "stderr leaked into stdout");
    require(strstr(both_err, "stdout-line") == NULL,
            "stdout leaked into stderr");
    free(both_out);
    free(both_err);
    return 0;
}
