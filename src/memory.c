#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "task.h"
#include "utils.h"

char *open_json_or_create_empty(const char *file) {
    size_t size = strlen(file) + 1;
    char *path = malloc(size);
    if (!path) {
        puts("Could not allocate path string");
        return NULL;
    }
    snprintf(path, size, "%s", file);

    json_error_t error;
    json_t *root = json_load_file(path, 0, &error);
    if (!root) {
        root = json_object();
        json_object_set_new(root, "tasks", json_array());
        json_object_set_new(root, "projects", json_array());
        if (json_dump_file(root, path, JSON_INDENT(2)) != 0) {
            json_decref(root);
            free(path);
            return NULL;
        }
    }

    json_decref(root);
    return path;
}

static json_t *empty_storage_root(void) {
    json_t *root = json_object();
    json_object_set_new(root, "tasks", json_array());
    json_object_set_new(root, "projects", json_array());
    return root;
}

static void ensure_array_member(json_t *root, const char *key) {
    json_t *array = json_object_get(root, key);
    if (!json_is_array(array))
        json_object_set_new(root, key, json_array());
}

static json_t *read_storage(const char *filename) {
    char *path = open_json_or_create_empty(filename);
    if (!path)
        return empty_storage_root();

    json_error_t error;
    json_t *loaded = json_load_file(path, 0, &error);
    free(path);

    if (!loaded)
        return empty_storage_root();

    if (json_is_array(loaded)) {
        json_t *root = json_object();
        json_object_set_new(root, "tasks", loaded);
        json_object_set_new(root, "projects", json_array());
        return root;
    }

    if (!json_is_object(loaded)) {
        json_decref(loaded);
        return empty_storage_root();
    }

    ensure_array_member(loaded, "tasks");
    ensure_array_member(loaded, "projects");
    return loaded;
}

static int write_storage(const char *filename, json_t *root) {
    char *path = open_json_or_create_empty(filename);
    if (!path)
        return 1;

    int result = json_dump_file(root, path, JSON_INDENT(4));
    free(path);

    if (result != 0) {
        fprintf(stderr, "Error writing JSON to file\n");
        return 1;
    }
    return 0;
}

static int json_int_or(json_t *object, const char *key, int fallback) {
    json_t *value = json_object_get(object, key);
    return json_is_integer(value) ? (int)json_integer_value(value) : fallback;
}

static char *json_string_dup_or(json_t *object, const char *key,
                                const char *fallback) {
    json_t *value = json_object_get(object, key);
    return strdup(json_is_string(value) ? json_string_value(value) : fallback);
}

static time_t json_due_or_zero(json_t *object) {
    json_t *json_sec = json_object_get(object, "tm_sec");
    json_t *json_min = json_object_get(object, "tm_min");
    json_t *json_hour = json_object_get(object, "tm_hour");
    json_t *json_mday = json_object_get(object, "tm_mday");
    json_t *json_mon = json_object_get(object, "tm_mon");
    json_t *json_year = json_object_get(object, "tm_year");
    json_t *json_wday = json_object_get(object, "tm_wday");
    json_t *json_yday = json_object_get(object, "tm_yday");
    json_t *json_isdst = json_object_get(object, "tm_isdst");

    if (!(json_is_integer(json_sec) && json_is_integer(json_min) &&
          json_is_integer(json_hour) && json_is_integer(json_mday) &&
          json_is_integer(json_mon) && json_is_integer(json_year) &&
          json_is_integer(json_wday) && json_is_integer(json_yday) &&
          json_is_integer(json_isdst))) {
        return 0;
    }

    struct tm date = {0};
    date.tm_sec = (int)json_integer_value(json_sec);
    date.tm_min = (int)json_integer_value(json_min);
    date.tm_hour = (int)json_integer_value(json_hour);
    date.tm_mday = (int)json_integer_value(json_mday);
    date.tm_mon = (int)json_integer_value(json_mon);
    date.tm_year = (int)json_integer_value(json_year);
    date.tm_wday = (int)json_integer_value(json_wday);
    date.tm_yday = (int)json_integer_value(json_yday);
    date.tm_isdst = (int)json_integer_value(json_isdst);
    return mktime(&date);
}

static void json_set_due(json_t *object, time_t due) {
    struct tm *date = localtime(&due);
    json_object_set_new(object, "tm_sec", json_integer(date->tm_sec));
    json_object_set_new(object, "tm_min", json_integer(date->tm_min));
    json_object_set_new(object, "tm_hour", json_integer(date->tm_hour));
    json_object_set_new(object, "tm_mday", json_integer(date->tm_mday));
    json_object_set_new(object, "tm_mon", json_integer(date->tm_mon));
    json_object_set_new(object, "tm_year", json_integer(date->tm_year));
    json_object_set_new(object, "tm_wday", json_integer(date->tm_wday));
    json_object_set_new(object, "tm_yday", json_integer(date->tm_yday));
    json_object_set_new(object, "tm_isdst", json_integer(date->tm_isdst));
}

static json_t *task_to_json(Task *task) {
    json_t *json_task = json_object();
    json_object_set_new(json_task, "id", json_integer(task->id));
    json_object_set_new(json_task, "name", json_string(task->name));
    json_object_set_new(json_task, "description",
                        json_string(task->description));
    json_object_set_new(json_task, "priority", json_integer(task->priority));
    json_set_due(json_task, task->due);
    // Guarda si la fecha debe mostrarse con hora y los bits THD de notify.
    json_object_set_new(json_task, "due_has_time",
                        json_integer(task->due_has_time));
    json_object_set_new(json_task, "notify", json_integer(task->notify));
    json_object_set_new(json_task, "category", json_string(task->category));
    json_object_set_new(json_task, "recurrent", json_integer(task->recurrent));
    json_object_set_new(json_task, "status", json_integer(task->status));
    json_object_set_new(json_task, "project", json_string(task->project));
    return json_task;
}

static json_t *project_to_json(Project *project) {
    json_t *json_project = json_object();
    json_object_set_new(json_project, "id", json_integer(project->id));
    json_object_set_new(json_project, "name", json_string(project->name));
    json_object_set_new(json_project, "description",
                        json_string(project->description));
    json_object_set_new(json_project, "priority",
                        json_integer(project->priority));
    json_set_due(json_project, project->due);
    // Los proyectos persisten los mismos defaults temporales que las tareas.
    json_object_set_new(json_project, "due_has_time",
                        json_integer(project->due_has_time));
    json_object_set_new(json_project, "notify", json_integer(project->notify));
    json_object_set_new(json_project, "category",
                        json_string(project->category));
    json_object_set_new(json_project, "recurrent",
                        json_integer(project->recurrent));
    json_object_set_new(json_project, "status", json_integer(project->status));
    return json_project;
}

static int parse_task(json_t *json_item, Task *task) {
    if (!json_is_object(json_item))
        return 0;

    json_t *json_id = json_object_get(json_item, "id");
    json_t *json_name = json_object_get(json_item, "name");
    if (!json_is_integer(json_id) || !json_is_string(json_name))
        return 0;

    task->id = (int)json_integer_value(json_id);
    task->name = strdup(json_string_value(json_name));
    task->description = json_string_dup_or(json_item, "description", "none");
    task->priority = json_int_or(json_item, "priority", MEDIUM);
    task->due = json_due_or_zero(json_item);
    // Los ficheros antiguos no tienen estos campos; se cargan como desactivados.
    task->due_has_time = json_int_or(json_item, "due_has_time", 0);
    task->notify = json_int_or(json_item, "notify", 0) & 7;
    task->category = json_string_dup_or(json_item, "category", "none");
    task->recurrent = json_int_or(json_item, "recurrent", NO);
    task->status = json_int_or(json_item, "status", TODO);
    task->project = json_string_dup_or(json_item, "project", "none");
    return 1;
}

static int parse_project(json_t *json_item, Project *project) {
    if (!json_is_object(json_item))
        return 0;

    json_t *json_id = json_object_get(json_item, "id");
    json_t *json_name = json_object_get(json_item, "name");
    if (!json_is_integer(json_id) || !json_is_string(json_name))
        return 0;

    project->id = (int)json_integer_value(json_id);
    project->name = strdup(json_string_value(json_name));
    project->description = json_string_dup_or(json_item, "description", "none");
    project->priority = json_int_or(json_item, "priority", MEDIUM);
    project->due = json_due_or_zero(json_item);
    // Limita notify a los tres bits utiles al cargar desde JSON.
    project->due_has_time = json_int_or(json_item, "due_has_time", 0);
    project->notify = json_int_or(json_item, "notify", 0) & 7;
    project->category = json_string_dup_or(json_item, "category", "none");
    project->recurrent = json_int_or(json_item, "recurrent", NO);
    project->status = json_int_or(json_item, "status", TODO);
    return 1;
}

static int next_project_id(void) {
    int id;
    for (id = 0; id < (int)to_do_proj.n_items; id++) {
        if (!find_project_by_id(id))
            break;
    }
    return id;
}

static Project default_project_for_name(const char *name) {
    Project project = {0};
    project.id = next_project_id();
    project.name = strdup(name);
    project.description = strdup("none");
    project.priority = MEDIUM;
    project.due = 0;
    project.due_has_time = 0;
    project.notify = 0;
    project.category = strdup("none");
    project.recurrent = NO;
    project.status = TODO;
    return project;
}

void load(const char *filename) {
    json_t *root = read_storage(filename);
    json_t *projects = json_object_get(root, "projects");
    json_t *tasks = json_object_get(root, "tasks");

    free(to_do_list.items);
    free(to_do_proj.items);
    to_do_list.items = NULL;
    to_do_list.n_items = 0;
    to_do_list.size = 0;
    to_do_proj.items = NULL;
    to_do_proj.n_items = 0;
    to_do_proj.size = 0;

    size_t index;
    json_t *value;
    json_array_foreach(projects, index, value) {
        Project project;
        if (parse_project(value, &project))
            append(to_do_proj, project);
        else
            fprintf(stderr, "Invalid project at index %zu\n", index);
    }

    json_array_foreach(tasks, index, value) {
        Task task;
        if (!parse_task(value, &task)) {
            fprintf(stderr, "Invalid task at index %zu\n", index);
            continue;
        }
        append(to_do_list, task);
        if (strcmp(task.project, "none") && !is_in_proj_list(task.project)) {
            Project project = default_project_for_name(task.project);
            append(to_do_proj, project);
        }
    }

    json_decref(root);

    sort_list(to_do_list.items, 0, to_do_list.n_items - 1);
}

void save(Task *task, const char *filename) {
    json_t *root = read_storage(filename);
    json_t *tasks = json_object_get(root, "tasks");
    json_array_append_new(tasks, task_to_json(task));
    write_storage(filename, root);
    json_decref(root);
}

void save_project(Project *project, const char *filename) {
    json_t *root = read_storage(filename);
    json_t *projects = json_object_get(root, "projects");
    json_array_append_new(projects, project_to_json(project));
    write_storage(filename, root);
    json_decref(root);
}

static int delete_from_array_by_id(json_t *array, int target_id) {
    size_t index;
    json_t *value;
    json_array_foreach(array, index, value) {
        json_t *id_obj = json_object_get(value, "id");
        if (json_is_integer(id_obj) && json_integer_value(id_obj) == target_id) {
            json_array_remove(array, index);
            return 1;
        }
    }
    return 0;
}

int delete_task(const char *filename, int target_id) {
    json_t *root = read_storage(filename);
    json_t *tasks = json_object_get(root, "tasks");
    int found = delete_from_array_by_id(tasks, target_id);

    if (!found)
        printf("No task with ID %d found\n", target_id);

    int result = write_storage(filename, root);
    json_decref(root);
    return result;
}

int delete_project(const char *filename, int target_id) {
    json_t *root = read_storage(filename);
    json_t *projects = json_object_get(root, "projects");
    delete_from_array_by_id(projects, target_id);

    int result = write_storage(filename, root);
    json_decref(root);
    return result;
}

void update_recurrent(const char *filename) {
    (void)filename;
    for (size_t i = 0; i < to_do_list.n_items; i++) {
        if (to_do_list.items[i].recurrent &&
            to_do_list.items[i].status == DONE &&
            (second_until(to_do_list.items[i].due) < 0)) {
            struct tm time = *localtime(&to_do_list.items[i].due);
            switch (to_do_list.items[i].recurrent) {
            case DAILY:
                // Avanza hasta encontrar la siguiente ocurrencia futura.
                time.tm_mday += 1;
                to_do_list.items[i].due = mktime(&time);
                while (second_until(to_do_list.items[i].due) < 0) {
                    time.tm_mday += 1;
                    to_do_list.items[i].due = mktime(&time);
                }
                break;
            case WEEKLY:
                time.tm_mday += 7;
                to_do_list.items[i].due = mktime(&time);
                while (second_until(to_do_list.items[i].due) < 0) {
                    time.tm_mday += 7;
                    to_do_list.items[i].due = mktime(&time);
                }
                break;
            case MONTHLY:
                time.tm_mon += 1;
                to_do_list.items[i].due = mktime(&time);
                while (second_until(to_do_list.items[i].due) < 0) {
                    time.tm_mon += 1;
                    to_do_list.items[i].due = mktime(&time);
                }
                break;
            case YEARLY:
                time.tm_year += 1;
                to_do_list.items[i].due = mktime(&time);
                while (second_until(to_do_list.items[i].due) < 0) {
                    time.tm_year += 1;
                    to_do_list.items[i].due = mktime(&time);
                }
                break;
            }
            to_do_list.items[i].status = TODO;
            delete_task(filename, to_do_list.items[i].id);
            save(&to_do_list.items[i], filename);
        }
    }
}
