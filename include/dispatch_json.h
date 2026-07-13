#ifndef DISPATCH_JSON_H
#define DISPATCH_JSON_H

#include "dispatch.h"

#include <stdio.h>

#define DISPATCH_JSON_STATE(state) (1U << (unsigned int)(state))

typedef struct {
    const char *command;
    const char *task_id;
    const char *group;
    int include_done;
    unsigned int state_mask;
    int include_warnings;
} DispatchJsonRequest;

/* Removes one --json argument at or after first_arg and updates argc. */
int dispatch_cli_extract_json_flag(int *argc, char **argv, int first_arg,
                                   int *json_output);

/* Writes one versioned JSON response and a trailing newline. */
int dispatch_json_emit(FILE *stream, const DispatchBoard *board,
                       const DispatchJsonRequest *request);

#endif
