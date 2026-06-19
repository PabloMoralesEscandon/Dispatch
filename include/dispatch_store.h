#ifndef DISPATCH_STORE_H
#define DISPATCH_STORE_H

#include <stddef.h>

#include "dispatch.h"

#define DISPATCH_STORE_FILE "dispatch.json"
#define DISPATCH_LOG_FILE "dispatch.log"

typedef struct {
    char *path;
    int fd;
    int acquired;
} DispatchStoreLock;

typedef struct {
    const char *key;
    const char *value;
} DispatchLogField;

typedef struct {
    const char *actor;
    const char *command;
    const char *action;
    const char *outcome;
    const char *message;
    const DispatchLogField *targets;
    size_t target_count;
    const DispatchLogField *context;
    size_t context_count;
} DispatchLogRecord;

int dispatch_store_load(DispatchBoard *board, const char *path, char *error,
                        size_t error_size);
int dispatch_store_save(const DispatchBoard *board, const char *path,
                        char *error, size_t error_size);
int dispatch_store_init_file(const char *path, const char *repo_path,
                             char *error, size_t error_size);
int dispatch_store_lock_acquire(DispatchStoreLock *lock, const char *path,
                                int timeout_ms, char *error,
                                size_t error_size);
void dispatch_store_lock_release(DispatchStoreLock *lock);
int dispatch_store_log_append(const char *path, const DispatchLogRecord *record,
                              char *error, size_t error_size);

#endif
