#ifndef DISPATCH_STORE_H
#define DISPATCH_STORE_H

#include <stddef.h>

#include "dispatch.h"

#define DISPATCH_STORE_FILE "dispatch.json"

typedef struct {
    char *path;
    int fd;
    int acquired;
} DispatchStoreLock;

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

#endif
