#ifndef MEMORY_H
#define MEMORY_H

#include "task.h"

/**
 * @file memory.h
 * @brief Legacy task persistence in JSON storage.
 */

/**
 * @brief Load tasks from disk and rebuild the global lists.
 * @param filename Input JSON filename.
 */
void load(const char *filename);

/**
 * @brief Save a task by appending it to the JSON file.
 * @param task Task to persist.
 * @param filename Output JSON filename.
 */
void save(Task *task, const char *filename);

/**
 * @brief Save a project by appending it to the JSON file.
 * @param project Project to persist.
 * @param filename Output JSON filename.
 */
void save_project(Project *project, const char *filename);

/**
 * @brief Delete a persisted task by identifier.
 * @param filename JSON filename.
 * @param target_id ID of the task to delete.
 * @return `0` when the operation succeeds; `1` when it fails.
 */
int delete_task(const char *filename, int target_id);

/**
 * @brief Delete a persisted project by identifier.
 * @param filename JSON filename.
 * @param target_id ID of the project to delete.
 * @return `0` when the operation succeeds; `1` when it fails.
 */
int delete_project(const char *filename, int target_id);

/**
 * @brief Recalculate due dates for completed recurring tasks that are overdue.
 * @param filename JSON filename associated with storage.
 */
void update_recurrent(const char *filename);

#endif
