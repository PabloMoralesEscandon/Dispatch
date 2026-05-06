#ifndef DISPATCH_STORE_H
#define DISPATCH_STORE_H

#include <stddef.h>

#include "dispatch.h"

#define DISPATCH_STORE_FILE "dispatch.json"

int dispatch_store_load(DispatchBoard *board, const char *path, char *error,
                        size_t error_size);
int dispatch_store_save(const DispatchBoard *board, const char *path,
                        char *error, size_t error_size);
int dispatch_store_init_file(const char *path, char *error, size_t error_size);

#endif
