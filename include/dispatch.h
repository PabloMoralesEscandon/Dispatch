#ifndef DISPATCH_H
#define DISPATCH_H

#include <stddef.h>
#include <time.h>

typedef enum {
    DISPATCH_STATE_PROPOSED,
    DISPATCH_STATE_READY,
    DISPATCH_STATE_BLOCKED,
    DISPATCH_STATE_DOING,
    DISPATCH_STATE_REVIEW,
    DISPATCH_STATE_DONE,
    DISPATCH_STATE_PAUSED
} DispatchState;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} DispatchStringList;

typedef struct {
    char *actor;
    char *action;
    char *note;
    time_t timestamp;
} DispatchHistoryEntry;

typedef struct {
    DispatchHistoryEntry *items;
    size_t count;
    size_t capacity;
} DispatchHistory;

typedef struct {
    char *id;
    char *name;
    char *prefix;
} DispatchGroup;

typedef struct {
    char *id;
    char *title;
    char *description;
    char *group;
    DispatchState state;
    DispatchStringList depends_on;
    int requires_review;
    char *assigned_to;
    char *started_by;
    char *completed_by;
    time_t created_at;
    time_t started_at;
    time_t completed_at;
    time_t updated_at;
    DispatchHistory history;
} DispatchTask;

typedef struct {
    DispatchGroup *items;
    size_t count;
    size_t capacity;
} DispatchGroups;

typedef struct {
    DispatchTask *items;
    size_t count;
    size_t capacity;
} DispatchTasks;

typedef struct {
    int version;
    char *name;
    DispatchGroups groups;
    DispatchTasks tasks;
} DispatchBoard;

void dispatch_board_init(DispatchBoard *board, const char *name);
void dispatch_board_free(DispatchBoard *board);

DispatchGroup *dispatch_board_find_group(DispatchBoard *board,
                                         const char *group_id);
DispatchTask *dispatch_board_find_task(DispatchBoard *board,
                                       const char *task_id);

int dispatch_board_add_group(DispatchBoard *board, const char *name,
                             const char *prefix);
int dispatch_group_prefix_is_valid(const char *prefix);
DispatchTask *dispatch_board_add_task(DispatchBoard *board,
                                      const char *group_id,
                                      const char *title,
                                      const char *description);
int dispatch_task_set_title(DispatchTask *task, const char *title);
int dispatch_task_set_description(DispatchTask *task, const char *description);

const char *dispatch_state_name(DispatchState state);
int dispatch_state_from_name(const char *name, DispatchState *state);
int dispatch_task_set_state(DispatchTask *task, DispatchState state,
                            const char *actor, const char *note);
int dispatch_task_assign(DispatchTask *task, const char *actor);
void dispatch_task_clear_assignment(DispatchTask *task);
int dispatch_task_append_history(DispatchTask *task, const char *actor,
                                 const char *action, const char *note);

DispatchState dispatch_task_effective_state(const DispatchBoard *board,
                                            const DispatchTask *task);
int dispatch_task_has_unmet_dependencies(const DispatchBoard *board,
                                         const DispatchTask *task);
int dispatch_task_add_dependency(DispatchBoard *board, const char *from_id,
                                 const char *to_id);
int dispatch_board_has_dependency_path(const DispatchBoard *board,
                                       const char *from_id,
                                       const char *to_id);

char *dispatch_next_task_id(const DispatchBoard *board, const char *group_id);

#endif
