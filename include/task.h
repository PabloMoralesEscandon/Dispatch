#ifndef TASK_H
#define TASK_H

#include <stdlib.h>
#include <time.h>

/**
 * @file task.h
 * @brief Legacy task and project domain types.
 */

/** @brief Maximum length, including the null terminator, for category/project names. */
#define NAME_CHARS 21
/** @brief Maximum length, including the null terminator, for a task name. */
#define LONG_NAME_CHARS 41
/** @brief Maximum length, including the null terminator, for a description. */
#define DESC_CHARS 201

/** @brief ANSI sequence that resets terminal style. */
#define ANSI_RESET "\x1b[0m"
/** @brief ANSI sequence that enables bold text. */
#define ANSI_BOLD "\x1b[1m"
/** @brief ANSI sequence that prints blue text. */
#define ANSI_BLUE "\x1b[34m"
/** @brief ANSI sequence that prints bold blue text. */
#define ANSI_BOLD_BLUE "\x1b[1;34m"

/**
 * @enum Status
 * @brief Possible task states.
 */
enum Status {
    TODO,        /**< Pending task. */
    IN_PROGRESS, /**< Task in progress. */
    DONE         /**< Completed task. */
};

/**
 * @enum Priority
 * @brief Supported priority levels.
 */
enum Priority {
    LOW,    /**< Low priority. */
    MEDIUM, /**< Medium priority. */
    HIGH,   /**< High priority. */
    URGENT  /**< Urgent priority. */
};

/**
 * @enum Recurrent
 * @brief Task recurrence interval.
 */
enum Recurrent {
    NO,      /**< Non-recurring task. */
    DAILY,   /**< Repeats every day. */
    WEEKLY,  /**< Repeats every week. */
    MONTHLY, /**< Repeats every month. */
    YEARLY   /**< Repeats every year. */
};

/**
 * @enum Due
 * @brief Relative due-date classification.
 */
enum Due {
    LATER, /**< Due later, or has no due date. */
    YEAR,  /**< Due within the next year. */
    MONTH, /**< Due within the next month. */
    WEEK,  /**< Due within the next week. */
    DAY    /**< Due within the next day. */
};

/** @brief Notify at the due time. */
#define NOTIFY_AT_TIME     4
/** @brief Notify one hour before the due time. */
#define NOTIFY_HOUR_BEFORE 2
/** @brief Notify during the due day. */
#define NOTIFY_DAY_OF      1

/**
 * @brief Append an item to a dynamic array with `items`, `n_items`, and `size` fields.
 *
 * If the buffer has no remaining capacity, it is reallocated with double size.
 *
 * @param da Dynamic array structure.
 * @param element Element to append.
 */
#define append(da, element)                                                    \
    do {                                                                       \
        if (da.n_items >= da.size) {                                           \
            if (da.size == 0)                                                  \
                da.size = 256;                                                 \
            else                                                               \
                da.size *= 2;                                                  \
            da.items = realloc(da.items, da.size * sizeof(*da.items));         \
        }                                                                      \
        da.items[da.n_items++] = element;                                      \
    } while (0)

/**
 * @struct Task
 * @brief Legacy task persisted and displayed by the old TDL code.
 */
typedef struct {
    int id;             /**< Unique task identifier. */
    char *name;         /**< Short task name. */
    char *description;  /**< Free-form description. */
    int priority;       /**< Priority, using @ref Priority. */
    time_t due;         /**< Due date; 0 means no due date. */
    int due_has_time;   /**< Whether the due date includes a time. */
    int notify;         /**< THD notification bitfield. */
    int recurrent;      /**< Recurrence rule, using @ref Recurrent. */
    int status;         /**< Current status, using @ref Status. */
    char *category;     /**< Functional category or "none". */
    char *project;      /**< Associated project or "none". */
} Task;

/**
 * @struct Project
 * @brief Legacy project with its own metadata.
 */
typedef struct {
    int id;             /**< Unique project identifier. */
    char *name;         /**< Short project name. */
    char *description;  /**< Free-form description. */
    int priority;       /**< Default priority for associated tasks. */
    time_t due;         /**< Default due date. */
    int due_has_time;   /**< Whether the due date includes a time. */
    int notify;         /**< Default THD notification bitfield. */
    int recurrent;      /**< Default recurrence. */
    int status;         /**< Project status. */
    char *category;     /**< Default category. */
} Project;

/**
 * @struct ToDoList
 * @brief Dynamic array containing all loaded legacy tasks.
 */
typedef struct {
    Task *items;     /**< Task buffer. */
    size_t n_items;  /**< Number of valid tasks. */
    size_t size;     /**< Reserved capacity in `items`. */
} ToDoList;

/**
 * @struct ToDoProjects
 * @brief Dynamic array containing all loaded legacy projects.
 */
typedef struct {
    Project *items;  /**< Project buffer. */
    size_t n_items;  /**< Number of stored projects. */
    size_t size;     /**< Reserved capacity in `items`. */
} ToDoProjects;

/** @brief Global task list loaded from storage. */
extern ToDoList to_do_list;

/** @brief Global project list derived from tasks. */
extern ToDoProjects to_do_proj;

/**
 * @brief Print a detailed task view.
 * @param task Task to show.
 */
void print_task(Task *task);

/**
 * @brief Convert an internal priority to a readable label.
 * @param priority Priority value.
 * @return Constant string with the text representation.
 */
char *get_priority(int priority);

/**
 * @brief Convert priority text to its internal value.
 * @param priority User-provided priority string.
 * @return @ref Priority value, or `-1` when invalid.
 */
int get_priority_int(char *priority);

/**
 * @brief Convert an internal recurrence to a readable label.
 * @param recurrence Recurrence value.
 * @return Constant string with the text representation.
 */
char *get_recurrence(int recurrence);

/**
 * @brief Convert recurrence text to its internal value.
 * @param recurrence User-provided recurrence string.
 * @return @ref Recurrent value, or `-1` when invalid.
 */
int get_recurrence_int(char *recurrence);

/**
 * @brief Convert an internal status to a readable label.
 * @param status Status value.
 * @return Constant string with the text representation.
 */
char *get_status(int status);

/**
 * @brief Convert status text to its internal value.
 * @param status User-provided status string.
 * @return @ref Status value, or `-1` when invalid.
 */
int get_status_int(char *status);

/**
 * @brief Check whether a decomposed date is valid.
 * @param date `tm` structure with initialized day, month, and year.
 * @return `1` when the date is valid; `0` otherwise.
 */
int is_valid_date(struct tm date);

/**
 * @brief Calculate the seconds remaining until a target instant.
 * @param target Target timestamp.
 * @return `target - now` difference in seconds.
 */
double second_until(time_t target);

/**
 * @brief Classify a date by relative nearness.
 * @param target Target date.
 * @return Matching @ref Due value.
 */
int when_due(time_t target);

/**
 * @brief Check whether a project is already registered in the global list.
 * @param name Project name.
 * @return `1` when found; `0` otherwise.
 */
int is_in_proj_list(char *name);

/**
 * @brief Find a project by name.
 * @param name Project name.
 * @return Project pointer, or `NULL` when not found.
 */
Project *find_project_by_name(const char *name);

/**
 * @brief Find a project by ID.
 * @param id Project identifier.
 * @return Project pointer, or `NULL` when not found.
 */
Project *find_project_by_id(int id);

/**
 * @brief Print a project summary and progress.
 * @param id Persisted project identifier.
 */
void print_proj(int id);

/**
 * @brief Print a detailed project view.
 * @param project Project to show.
 */
void print_project(Project *project);

/** @brief Print the task table header. */
void print_task_table_header();

/**
 * @brief Print a task table row.
 * @param t Task to print.
 */
void print_task_table_row(Task *t);

/** @brief Print the project table header. */
void print_proj_table_header();

/**
 * @brief Print a project table row.
 * @param proj Project to print.
 */
void print_proj_table_row(Project *proj);

#endif
