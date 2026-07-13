#include "dispatch_exec.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static char *exec_strdup(const char *value) {
    size_t size = strlen(value) + 1;
    char *copy = malloc(size);
    if (copy)
        memcpy(copy, value, size);
    return copy;
}

int dispatch_exec_argv_append(DispatchExecArgv *argv, const char *value) {
    if (!argv || !value)
        return 0;
    if (argv->count + 1 >= argv->capacity) {
        size_t capacity = argv->capacity ? argv->capacity * 2 : 8;
        char **items = realloc(argv->items, capacity * sizeof(*items));
        if (!items)
            return 0;
        argv->items = items;
        argv->capacity = capacity;
    }
    char *copy = exec_strdup(value);
    if (!copy)
        return 0;
    argv->items[argv->count++] = copy;
    argv->items[argv->count] = NULL;
    return 1;
}

void dispatch_exec_argv_free(DispatchExecArgv *argv) {
    if (!argv)
        return;
    for (size_t i = 0; i < argv->count; i++)
        free(argv->items[i]);
    free(argv->items);
    memset(argv, 0, sizeof(*argv));
}

static int token_append(char **token, size_t *length, size_t *capacity,
                        char ch) {
    if (*length + 1 >= *capacity) {
        size_t next = *capacity ? *capacity * 2 : 32;
        char *grown = realloc(*token, next);
        if (!grown)
            return 0;
        *token = grown;
        *capacity = next;
    }
    (*token)[(*length)++] = ch;
    (*token)[*length] = '\0';
    return 1;
}

int dispatch_exec_argv_parse(DispatchExecArgv *argv, const char *command) {
    if (!argv || !command || argv->items || argv->count || argv->capacity)
        return 0;

    DispatchExecArgv parsed = {0};
    char *token = NULL;
    size_t length = 0;
    size_t capacity = 0;
    char quote = '\0';
    int escaped = 0;
    int token_started = 0;

    for (size_t i = 0;; i++) {
        char ch = command[i];
        if (escaped) {
            if (ch == '\0' || !token_append(&token, &length, &capacity, ch))
                goto fail;
            escaped = 0;
            token_started = 1;
            continue;
        }
        if (ch == '\\' && quote != '\'') {
            escaped = 1;
            token_started = 1;
            continue;
        }
        if (quote) {
            if (ch == '\0')
                goto fail;
            if (ch == quote) {
                quote = '\0';
                token_started = 1;
            } else if (!token_append(&token, &length, &capacity, ch)) {
                goto fail;
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            token_started = 1;
            continue;
        }
        if (ch == '\0' || isspace((unsigned char)ch)) {
            if (token_started) {
                if (!token && !token_append(&token, &length, &capacity, '\0'))
                    goto fail;
                if (!dispatch_exec_argv_append(&parsed, token ? token : ""))
                    goto fail;
                length = 0;
                if (token)
                    token[0] = '\0';
                token_started = 0;
            }
            if (ch == '\0')
                break;
            continue;
        }
        if (!token_append(&token, &length, &capacity, ch))
            goto fail;
        token_started = 1;
    }

    free(token);
    *argv = parsed;
    return parsed.count > 0;

fail:
    free(token);
    dispatch_exec_argv_free(&parsed);
    return 0;
}

static int executable_file(const char *path) {
    struct stat info;
    return path && path[0] && stat(path, &info) == 0 && S_ISREG(info.st_mode) &&
           access(path, X_OK) == 0;
}

int dispatch_exec_command_available(const char *command) {
    if (!command || !command[0])
        return 0;
    if (strchr(command, '/'))
        return executable_file(command);

    const char *path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin";
    size_t command_length = strlen(command);
    const char *start = path;
    for (;;) {
        const char *end = strchr(start, ':');
        size_t directory_length = end ? (size_t)(end - start) : strlen(start);
        size_t size =
            (directory_length ? directory_length : 1) + 1 + command_length + 1;
        char *candidate = malloc(size);
        if (!candidate)
            return 0;
        if (directory_length) {
            memcpy(candidate, start, directory_length);
            candidate[directory_length] = '/';
            memcpy(candidate + directory_length + 1, command,
                   command_length + 1);
        } else {
            candidate[0] = '.';
            candidate[1] = '/';
            memcpy(candidate + 2, command, command_length + 1);
        }
        int available = executable_file(candidate);
        free(candidate);
        if (available)
            return 1;
        if (!end)
            break;
        start = end + 1;
    }
    return 0;
}

static int redirect_to_dev_null(int target, int write_mode) {
    int fd = open("/dev/null", write_mode ? O_WRONLY : O_RDONLY);
    if (fd < 0)
        return 0;
    int ok = dup2(fd, target) >= 0;
    if (fd != target)
        close(fd);
    return ok;
}

static int configure_child(const DispatchExecOptions *options, int stdin_fd,
                           int stdout_fd, int stderr_fd, int close_fd) {
    DispatchExecOptions defaults = {0};
    if (!options)
        options = &defaults;
    if (options->working_directory && chdir(options->working_directory) != 0)
        return 0;

    if (stdin_fd >= 0) {
        if (dup2(stdin_fd, STDIN_FILENO) < 0)
            return 0;
    } else if (options->stdin_mode == DISPATCH_EXEC_DEV_NULL &&
               !redirect_to_dev_null(STDIN_FILENO, 0)) {
        return 0;
    }
    if (stdout_fd >= 0) {
        if (dup2(stdout_fd, STDOUT_FILENO) < 0)
            return 0;
    } else if (options->stdout_mode == DISPATCH_EXEC_DEV_NULL &&
               !redirect_to_dev_null(STDOUT_FILENO, 1)) {
        return 0;
    }
    if (stderr_fd >= 0) {
        if (dup2(stderr_fd, STDERR_FILENO) < 0)
            return 0;
    } else if (options->stderr_mode == DISPATCH_EXEC_DEV_NULL &&
               !redirect_to_dev_null(STDERR_FILENO, 1)) {
        return 0;
    }
    if (options->merge_stderr_to_stdout &&
        dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
        return 0;
    }

    if (stdin_fd > STDERR_FILENO)
        close(stdin_fd);
    if (stdout_fd > STDERR_FILENO && stdout_fd != stdin_fd)
        close(stdout_fd);
    if (stderr_fd > STDERR_FILENO && stderr_fd != stdin_fd &&
        stderr_fd != stdout_fd) {
        close(stderr_fd);
    }
    if (close_fd > STDERR_FILENO && close_fd != stdin_fd &&
        close_fd != stdout_fd && close_fd != stderr_fd) {
        close(close_fd);
    }
    return 1;
}

static int spawn_process(const char *const argv[],
                         const DispatchExecOptions *options, int stdin_fd,
                         int stdout_fd, int stderr_fd, int close_fd,
                         pid_t *child) {
    if (!argv || !argv[0] || !argv[0][0] || !child) {
        errno = EINVAL;
        return 0;
    }
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        if (!configure_child(options, stdin_fd, stdout_fd, stderr_fd, close_fd))
            _exit(126);
        execvp(argv[0], (char *const *)argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    *child = pid;
    return 1;
}

static int wait_for_process(pid_t child, DispatchExecResult *result) {
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return 0;
    }
    if (result) {
        memset(result, 0, sizeof(*result));
        if (WIFEXITED(status)) {
            result->exited = 1;
            result->exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result->signaled = 1;
            result->signal_number = WTERMSIG(status);
        }
    }
    return 1;
}

int dispatch_exec_run(const char *const argv[],
                      const DispatchExecOptions *options,
                      DispatchExecResult *result) {
    pid_t child;
    if (!spawn_process(argv, options, -1, -1, -1, -1, &child))
        return 0;
    return wait_for_process(child, result);
}

static int append_output(char **output, size_t *length, size_t *capacity,
                         const char *data, size_t size) {
    if (*length + size + 1 > *capacity) {
        size_t next = *capacity ? *capacity : 4096;
        while (next < *length + size + 1)
            next *= 2;
        char *grown = realloc(*output, next);
        if (!grown)
            return 0;
        *output = grown;
        *capacity = next;
    }
    memcpy(*output + *length, data, size);
    *length += size;
    (*output)[*length] = '\0';
    return 1;
}

int dispatch_exec_capture(const char *const argv[],
                          const DispatchExecOptions *options, char **output,
                          size_t *output_size, DispatchExecResult *result) {
    if (!output) {
        errno = EINVAL;
        return 0;
    }
    *output = NULL;
    if (output_size)
        *output_size = 0;

    int fds[2];
    if (pipe(fds) != 0)
        return 0;
    pid_t child;
    if (!spawn_process(argv, options, -1, fds[1], -1, fds[0], &child)) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    close(fds[1]);

    size_t length = 0;
    size_t capacity = 0;
    char buffer[4096];
    int read_ok = 1;
    for (;;) {
        ssize_t count = read(fds[0], buffer, sizeof(buffer));
        if (count > 0) {
            if (!append_output(output, &length, &capacity, buffer,
                               (size_t)count)) {
                read_ok = 0;
                break;
            }
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            read_ok = 0;
            break;
        }
    }
    close(fds[0]);
    int wait_ok = wait_for_process(child, result);
    if (!*output) {
        *output = exec_strdup("");
        if (!*output)
            read_ok = 0;
    }
    if (output_size)
        *output_size = length;
    if (!read_ok || !wait_ok) {
        free(*output);
        *output = NULL;
        if (output_size)
            *output_size = 0;
        return 0;
    }
    return 1;
}

static int write_all(int fd, const unsigned char *data, size_t size) {
    struct sigaction ignore = {0};
    struct sigaction previous;
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    int restore_signal = sigaction(SIGPIPE, &ignore, &previous) == 0;
    int ok = 1;
    while (size > 0) {
        ssize_t count = write(fd, data, size);
        if (count > 0) {
            data += count;
            size -= (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            ok = 0;
            break;
        }
    }
    if (restore_signal)
        sigaction(SIGPIPE, &previous, NULL);
    return ok;
}

int dispatch_exec_feed(const char *const argv[],
                       const DispatchExecOptions *options, const void *input,
                       size_t input_size, DispatchExecResult *result) {
    if (!input && input_size > 0) {
        errno = EINVAL;
        return 0;
    }
    int fds[2];
    if (pipe(fds) != 0)
        return 0;
    pid_t child;
    if (!spawn_process(argv, options, fds[0], -1, -1, fds[1], &child)) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    close(fds[0]);
    int write_ok = write_all(fds[1], input, input_size);
    close(fds[1]);
    int wait_ok = wait_for_process(child, result);
    return write_ok && wait_ok;
}

int dispatch_exec_pipeline(const char *const producer_argv[],
                           const DispatchExecOptions *producer_options,
                           const char *const consumer_argv[],
                           const DispatchExecOptions *consumer_options,
                           DispatchExecResult *result) {
    int fds[2];
    if (pipe(fds) != 0)
        return 0;

    pid_t producer;
    if (!spawn_process(producer_argv, producer_options, -1, fds[1], -1, fds[0],
                       &producer)) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    pid_t consumer;
    if (!spawn_process(consumer_argv, consumer_options, fds[0], -1, -1, fds[1],
                       &consumer)) {
        close(fds[0]);
        close(fds[1]);
        kill(producer, SIGTERM);
        wait_for_process(producer, NULL);
        return 0;
    }
    close(fds[0]);
    close(fds[1]);

    int producer_ok = wait_for_process(producer, NULL);
    int consumer_ok = wait_for_process(consumer, result);
    return producer_ok && consumer_ok;
}

int dispatch_exec_result_success(const DispatchExecResult *result) {
    return result && result->exited && result->exit_code == 0;
}

int dispatch_exec_result_status(const DispatchExecResult *result) {
    if (!result)
        return -1;
    if (result->exited)
        return result->exit_code;
    if (result->signaled)
        return 128 + result->signal_number;
    return -1;
}
