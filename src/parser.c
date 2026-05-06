#include "parser.h"

#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "memory.h"
#include "task.h"
#include "utils.h"

Command commands[] = {
    {"add", cmd_add, "Add a new task"},
    {"show", cmd_show, "Show information of a task"},
    {"mod", cmd_mod, "Modify an existing task"},
    {"start", cmd_start, "Mark a task as started"},
    {"done", cmd_done, "Mark a task as done"},
    {"del", cmd_del, "Delete a task"},
    {"list", cmd_list, "List all tasks"},
    {NULL, NULL, NULL} // Sentinel to mark end
};

int dispatch_command(char *cmd, char *options[], int id) {
    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(commands[i].name, cmd) == 0) {
            return commands[i].handler(options, id);
        }
    }
    printf("Unknown command: %s\n", cmd);
    return 1;
}

char *parse_words(int argc, char **argv) {
    int i = 2;
    // Agrupa los argumentos posicionales hasta la primera opcion "-x"/"--long".
    while ((i < argc) && (argv[i][0] != '-'))
        i++;
    int size = 0;
    for (int n = 2; n < i; n++) {
        size += strlen(argv[n]) + 1;
    }
    if (!size)
        return NULL;
    char *words = malloc(size);
    if (i > 2)
        strcpy(words, argv[2]);
    for (int n = 3; n < i; n++) {
        strcat(words, " ");
        strcat(words, argv[n]);
    }
    return words;
}

int parse_id_name(char *words) {
    int i = 0;
    while (isdigit(words[i++]))
        ;
    if ((i - 1) == (int)strlen(words)) {
        return atoi(words);
    } else {
        return -1;
    }
}

void print_help(void) {
    puts("Dispatch: a command line workflow board.");
    puts("Usage: dispatch [command] [id or title] <options>");
    puts("");
    puts("Commands:");
    puts("  add            Add a new task");
    puts("  show           Show information of a task");
    puts("  mod            Modify an existing task");
    puts("  start          Mark a task as started");
    puts("  done           Mark a task as done");
    puts("  del            Delete a task");
    puts("  list           List tasks");
    puts("");
    puts("Options:");
    puts("  -p, --priority <val>       Priority: low, medium, high, urgent");
    puts("  -s, --status <val>         Status: todo, ongoing, done");
    puts("  -P, --project <val>        Temporary compatibility field");
    puts("  -n, --name <val>           Task title");
    puts("  -D, --description <val>    Task description");
    puts("  -h, --help                 Show this help and exit");
}

int parse_options(int argc, char **argv, char **options) {
    static struct option long_options[] = {
        {"priority", required_argument, 0, 'p'},
        {"status", required_argument, 0, 's'},
        {"project", required_argument, 0, 'P'},
        {"name", required_argument, 0, 'n'},
        {"description", required_argument, 0, 'D'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    int opt;

    while ((opt = getopt_long(argc, argv, "p:s:P:n:D:h", long_options,
                              NULL)) != -1) {
        switch (opt) {
        case 'p':
            options[PRIORITY] = strdup(optarg);
            break;
        case 's':
            options[STATUS] = strdup(optarg);
            break;
        case 'P':
            options[PROJECT] = strdup(optarg);
            break;
        case 'n':
            options[NAME] = strdup(optarg);
            break;
        case 'D':
            options[DESC] = strdup(optarg);
            break;
        case 'h':
            print_help();
            exit(0);
        default:
            printf("Unknown option:\n");
        }
    }

    return 0;
}

static int parse_notify_value(char *value, int *notify) {
    // Almacena notify como bitfield THD: T=4, H=2, D=1.
    int all_digits = 1;
    for (size_t i = 0; value[i] != '\0'; i++) {
        if (!isdigit(value[i])) {
            all_digits = 0;
            break;
        }
    }

    if (all_digits) {
        int parsed = atoi(value);
        if (parsed < 0 || parsed > 7) {
            printf("Invalid notify option: %s\nValid options: 0-7 or letters "
                   "T, H, D, e.g. THD\n",
                   value);
            return 0;
        }
        *notify = parsed;
        return 1;
    }

    int parsed = 0;
    for (size_t i = 0; value[i] != '\0'; i++) {
        char c = (char)toupper(value[i]);
        if (c == 'T')
            parsed |= NOTIFY_AT_TIME;
        else if (c == 'H')
            parsed |= NOTIFY_HOUR_BEFORE;
        else if (c == 'D')
            parsed |= NOTIFY_DAY_OF;
        else if (c == ',' || c == '+' || c == '-' || c == '_' || c == ' ')
            continue;
        else {
            printf("Invalid notify option: %s\nValid options: 0-7 or letters "
                   "T, H, D, e.g. THD\n",
                   value);
            return 0;
        }
    }

    *notify = parsed;
    return 1;
}

static void normalize_due_for_notify(time_t *due, int due_has_time,
                                     int notify) {
    // Las fechas sin hora usan 23:59 internamente si tienen notify.
    // due_has_time queda a 0 para no mostrar hora en la salida.
    if (*due == 0 || due_has_time || notify == 0)
        return;

    struct tm date = *localtime(due);
    date.tm_hour = 23;
    date.tm_min = 59;
    date.tm_sec = 0;
    *due = mktime(&date);
}

static int parse_due_value(const char *value, int notify, time_t *due,
                           int *due_has_time) {
    // Acepta fecha sola o fecha con hora; due_has_time controla la salida.
    struct tm date = {0};
    size_t len = strlen(value);
    if ((len == 10 || len == 16) &&
        (isdigit(value[0]) && isdigit(value[1])) &&
        (isdigit(value[3]) && isdigit(value[4])) &&
        (isdigit(value[6]) && isdigit(value[7])) &&
        (isdigit(value[8]) && isdigit(value[9])) &&
        ((value[2] == value[5]) && (value[2] == '-'))) {
        date.tm_mday = (value[0] - '0') * 10 + (value[1] - '0');
        date.tm_mon = (value[3] - '0') * 10 + (value[4] - '0') - 1;
        date.tm_year = (value[8] - '0') * 10 + (value[9] - '0');
        date.tm_year += (value[6] - '0') * 1000 + (value[7] - '0') * 100;
        date.tm_year -= 1900;
        if (len == 16) {
            if ((value[10] != ' ' && value[10] != 'T') ||
                !isdigit(value[11]) || !isdigit(value[12]) ||
                value[13] != ':' || !isdigit(value[14]) ||
                !isdigit(value[15])) {
                printf("Failed to parse date or invalid format. Use "
                       "DD-MM-YYYY or DD-MM-YYYY HH:MM.\n");
                return 0;
            }
            date.tm_hour = (value[11] - '0') * 10 + (value[12] - '0');
            date.tm_min = (value[14] - '0') * 10 + (value[15] - '0');
            if (date.tm_hour > 23 || date.tm_min > 59) {
                printf("Failed to parse date or invalid time. Use HH:MM in "
                       "24-hour format.\n");
                return 0;
            }
            *due_has_time = 1;
        } else {
            *due_has_time = 0;
            if (notify) {
                // Guarda 23:59 sin marcar la fecha como fecha con hora.
                date.tm_hour = 23;
                date.tm_min = 59;
            }
        }
        date.tm_isdst = -1;
    } else {
        printf("Failed to parse date or invalid format. Make sure date is "
               "in format DD-MM-YYYY or DD-MM-YYYY HH:MM.\n");
        return 0;
    }

    if (!is_valid_date(date)) {
        printf("Failed to parse date or invalid format. Make sure date is "
               "in format DD-MM-YYYY.\n");
        return 0;
    }

    *due = mktime(&date);
    return 1;
}

static int next_task_id(void) {
    int task_id;
    for (task_id = 0; task_id < (int)to_do_list.n_items; task_id++) {
        if (task_id != to_do_list.items[task_id].id)
            break;
    }
    return task_id;
}

static int next_project_id(void) {
    int project_id = 0;
    while (find_project_by_id(project_id))
        project_id++;
    return project_id;
}

static Project make_default_project(const char *name) {
    Project project = {0};
    project.id = next_project_id();
    project.name = strdup(name);
    project.description = strdup("none");
    project.priority = MEDIUM;
    project.due = 0;
    project.due_has_time = 0;
    project.notify = 0;
    project.recurrent = NO;
    project.status = TODO;
    project.category = strdup("none");
    return project;
}

static Project *find_or_create_project(const char *name) {
    Project *project = find_project_by_name(name);
    if (project)
        return project;

    Project new_project = make_default_project(name);
    append(to_do_proj, new_project);
    save_project(&new_project, FILE_NAME);
    return find_project_by_name(name);
}

static int apply_priority(char *value, int *priority) {
    int parsed = get_priority_int(value);
    if (parsed == -1) {
        printf("Invalid priority option: %s\nValid options: low, medium, "
               "high, urgent\n",
               value);
        return 0;
    }
    *priority = parsed;
    return 1;
}

static int apply_recurrent(char *value, int *recurrent) {
    int parsed = get_recurrence_int(value);
    if (parsed == -1) {
        printf("Invalid recurrence option: %s\nValid options: daily, "
               "weekly, monthly, yearly\n",
               value);
        return 0;
    }
    *recurrent = parsed;
    return 1;
}

int cmd_add(char *options[], int id) {
    (void)id;
    Task new_task = {0};
    Project *project = NULL;

    new_task.id = next_task_id();

    if (options[PROJECT]) {
        if ((strlen(options[PROJECT]) + 1) > NAME_CHARS) {
            printf("Project is too long. Max %d characters.\n", NAME_CHARS);
            return 1;
        }
        project = find_or_create_project(options[PROJECT]);
        new_task.project = strdup(options[PROJECT]);
    } else {
        new_task.project = strdup("none");
    }

    if (options[NAME]) {
        if ((strlen(options[NAME]) + 1) > LONG_NAME_CHARS) {
            printf("Name is too long. Max %d characters.\n", LONG_NAME_CHARS);
            return 1;
        }
        new_task.name = strdup(options[NAME]);
    } else {
        char name[LONG_NAME_CHARS];
        snprintf(name, sizeof(name), "Task %d", new_task.id);
        new_task.name = strdup(name);
    }

    if (options[STATUS]) {
        int status = get_status_int(options[STATUS]);
        if (status == -1) {
            printf("Invalid status option: %s\n", options[STATUS]);
            return 1;
        }
        new_task.status = status;
    } else if (project) {
        new_task.status = project->status;
    } else {
        new_task.status = TODO;
    }

    if (options[PRIORITY]) {
        if (!apply_priority(options[PRIORITY], &new_task.priority))
            return 1;
    } else if (project) {
        new_task.priority = project->priority;
    } else {
        new_task.priority = MEDIUM;
    }

    if (options[RECURRENT]) {
        if (!apply_recurrent(options[RECURRENT], &new_task.recurrent))
            return 1;
    } else if (project) {
        new_task.recurrent = project->recurrent;
    } else {
        new_task.recurrent = NO;
    }

    if (options[NOTIFY]) {
        // Hereda notify del proyecto salvo que se indique en la tarea.
        if (!parse_notify_value(options[NOTIFY], &new_task.notify))
            return 1;
    } else if (project) {
        new_task.notify = project->notify;
    } else {
        new_task.notify = 0;
    }

    if (options[DUE]) {
        if (!parse_due_value(options[DUE], new_task.notify, &new_task.due,
                             &new_task.due_has_time))
            return 0;
    } else if (project) {
        new_task.due = project->due;
        new_task.due_has_time = project->due_has_time;
    } else {
        new_task.due = 0;
        new_task.due_has_time = 0;
    }
    normalize_due_for_notify(&new_task.due, new_task.due_has_time,
                             new_task.notify);

    if (options[CATEGORY]) {
        if ((strlen(options[CATEGORY]) + 1) > NAME_CHARS) {
            printf("Category is too long. Max %d characters.\n", NAME_CHARS);
            return 1;
        }
        new_task.category = strdup(options[CATEGORY]);
    } else if (project) {
        new_task.category = strdup(project->category);
    } else {
        new_task.category = strdup("none");
    }

    if (options[DESC]) {
        if ((strlen(options[DESC]) + 1) > DESC_CHARS) {
            printf("Description is too long. Max %d characters.\n", DESC_CHARS);
            return 1;
        }
        new_task.description = strdup(options[DESC]);
    } else if (project) {
        new_task.description = strdup(project->description);
    } else {
        new_task.description = strdup("none");
    }

    append(to_do_list, new_task);
    save(&new_task, FILE_NAME);
    return 0;
}

int cmd_add_project(char *options[], int id) {
    (void)id;
    Project new_project = {0};

    if (options[NAME]) {
        if ((strlen(options[NAME]) + 1) > LONG_NAME_CHARS) {
            printf("Name is too long. Max %d characters.\n", LONG_NAME_CHARS);
            return 1;
        }
        if (find_project_by_name(options[NAME])) {
            printf("Project already exists: %s\n", options[NAME]);
            return 1;
        }
        new_project.name = strdup(options[NAME]);
    } else {
        char name[LONG_NAME_CHARS];
        snprintf(name, sizeof(name), "Project %d", next_project_id());
        new_project.name = strdup(name);
    }

    new_project.id = next_project_id();
    new_project.status = TODO;

    if (options[PRIORITY]) {
        if (!apply_priority(options[PRIORITY], &new_project.priority))
            return 1;
    } else {
        new_project.priority = MEDIUM;
    }

    if (options[RECURRENT]) {
        if (!apply_recurrent(options[RECURRENT], &new_project.recurrent))
            return 1;
    } else {
        new_project.recurrent = NO;
    }

    if (options[NOTIFY]) {
        // El notify del proyecto sera el valor por defecto de sus tareas.
        if (!parse_notify_value(options[NOTIFY], &new_project.notify))
            return 1;
    } else {
        new_project.notify = 0;
    }

    if (options[DUE]) {
        if (!parse_due_value(options[DUE], new_project.notify,
                             &new_project.due, &new_project.due_has_time))
            return 0;
    } else {
        new_project.due = 0;
        new_project.due_has_time = 0;
    }
    normalize_due_for_notify(&new_project.due, new_project.due_has_time,
                             new_project.notify);

    if (options[STATUS]) {
        int status = get_status_int(options[STATUS]);
        if (status == -1) {
            printf("Invalid status option: %s\n", options[STATUS]);
            return 1;
        }
        new_project.status = status;
    }

    if (options[CATEGORY]) {
        if ((strlen(options[CATEGORY]) + 1) > NAME_CHARS) {
            printf("Category is too long. Max %d characters.\n", NAME_CHARS);
            return 1;
        }
        new_project.category = strdup(options[CATEGORY]);
    } else {
        new_project.category = strdup("none");
    }

    if (options[DESC]) {
        if ((strlen(options[DESC]) + 1) > DESC_CHARS) {
            printf("Description is too long. Max %d characters.\n", DESC_CHARS);
            return 1;
        }
        new_project.description = strdup(options[DESC]);
    } else {
        new_project.description = strdup("none");
    }

    append(to_do_proj, new_project);
    save_project(&new_project, FILE_NAME);
    return 0;
}

int cmd_del(char *options[], int id) {
    if (id != -1) {
        delete_task(FILE_NAME, id);
        printf("Deleted task: %d\n", id);
        return 0;
    } else if (options[NAME] != NULL) {
        for (size_t i = 0; i < to_do_list.n_items; i++) {
            if (!strcmp(options[NAME], to_do_list.items[i].name)) {
                id = to_do_list.items[i].id;
                delete_task(FILE_NAME, id);
                printf("Deleted task: %s\n", options[NAME]);
                return 0;
            }
        }
        printf("No task with name: %s\n", options[NAME]);
        return 1;
    } else if (options[CATEGORY] != NULL) {
        for (size_t i = 0; i < to_do_list.n_items; i++) {
            if (!strcmp(options[CATEGORY], to_do_list.items[i].category)) {
                id = to_do_list.items[i].id;
                delete_task(FILE_NAME, id);
                printf("Deleted task: %s\n", to_do_list.items[i].name);
            }
        }
    } else if (options[PROJECT] != NULL) {
        for (size_t i = 0; i < to_do_list.n_items; i++) {
            if (!strcmp(options[PROJECT], to_do_list.items[i].project)) {
                id = to_do_list.items[i].id;
                delete_task(FILE_NAME, id);
                printf("Deleted task: %s\n", to_do_list.items[i].name);
            }
        }
    } else {
        printf(
            "Please provide id, name, category or project of task to delete\n");
        return 1;
    }
    return 0;
}

int cmd_del_project(char *options[], int id) {
    Project *project = NULL;

    if (id != -1) {
        project = find_project_by_id(id);
    } else if (options[NAME] != NULL) {
        project = find_project_by_name(options[NAME]);
    } else {
        printf("Please provide id or name of project to delete\n");
        return 1;
    }

    if (!project) {
        if (id != -1)
            printf("No project with id %d\n", id);
        else
            printf("No project with name %s\n", options[NAME]);
        return 1;
    }

    for (size_t i = 0; i < to_do_list.n_items; i++) {
        if (!strcmp(to_do_list.items[i].project, project->name)) {
            printf("Project %s still has tasks; delete or move them first.\n",
                   project->name);
            return 1;
        }
    }

    int project_id = project->id;
    char *project_name = project->name;
    delete_project(FILE_NAME, project_id);

    for (size_t i = 0; i < to_do_proj.n_items; i++) {
        if (to_do_proj.items[i].id == project_id) {
            to_do_proj.items[i] = to_do_proj.items[to_do_proj.n_items - 1];
            to_do_proj.n_items--;
            break;
        }
    }

    printf("Deleted project: %s\n", project_name);
    return 0;
}

// Fix implementation (if no id -> get id -> logic)
// Same for start and done
int cmd_mod(char *options[], int id) {
    int index = -1;
    if (id == -1) {
        if (options[NAME] != NULL) {
            for (size_t i = 0; i < to_do_list.n_items; i++) {
                if (!strcmp(options[NAME], to_do_list.items[i].name)) {
                    index = i;
                    break;
                }
            }
            if (index == -1) {
                printf("Could not find task %s\n", options[NAME]);
                return 1;
            }
        } else {
            printf("Provide either an id or a name for the task.\n");
            return 1;
        }

    } else
        for (size_t i = 0; i < to_do_list.n_items; i++) {
            if (id == to_do_list.items[i].id) {
                index = i;
                break;
            }
        }
    if (index == -1) {
        printf("Could not find task with id %d\n", id);
        return 1;
    }
    if (options[NAME]) {
        if ((strlen(options[NAME]) + 1) > LONG_NAME_CHARS) {
            printf("Name is too long. Max %d characters.\n", LONG_NAME_CHARS);
            return 1;
        }
        free(to_do_list.items[index].name);
        to_do_list.items[index].name = strdup(options[NAME]);
    }
    if (options[PRIORITY]) {
        if (!apply_priority(options[PRIORITY], &to_do_list.items[index].priority))
            return 1;
    }
    if (options[RECURRENT]) {
        if (!apply_recurrent(options[RECURRENT],
                             &to_do_list.items[index].recurrent))
            return 1;
    }
    if (options[NOTIFY]) {
        if (!parse_notify_value(options[NOTIFY],
                                &to_do_list.items[index].notify))
            return 1;
    }
    if (options[DUE]) {
        if (!parse_due_value(options[DUE], to_do_list.items[index].notify,
                             &to_do_list.items[index].due,
                             &to_do_list.items[index].due_has_time))
            return 0;
    }
    if (options[NOTIFY]) {
        normalize_due_for_notify(&to_do_list.items[index].due,
                                 to_do_list.items[index].due_has_time,
                                 to_do_list.items[index].notify);
    }
    if (options[STATUS]) {
        int status = get_status_int(options[STATUS]);
        if (status == -1) {
            printf("Invalid status option: %s\n", options[STATUS]);
            return 1;
        }
        to_do_list.items[index].status = status;
    }
    if (options[PROJECT]) {
        if ((strlen(options[PROJECT]) + 1) > NAME_CHARS) {
            printf("Project is too long. Max %d characters.\n", NAME_CHARS);
            return 1;
        }
        find_or_create_project(options[PROJECT]);
        free(to_do_list.items[index].project);
        to_do_list.items[index].project = strdup(options[PROJECT]);
    }
    if (options[CATEGORY]) {
        if ((strlen(options[CATEGORY]) + 1) > NAME_CHARS) {
            printf("Category is too long. Max %d characters.\n", NAME_CHARS);
            return 1;
        }
        free(to_do_list.items[index].category);
        to_do_list.items[index].category = strdup(options[CATEGORY]);
    }
    if (options[DESC]) {
        if ((strlen(options[DESC]) + 1) > DESC_CHARS) {
            printf("Description is too long. Max %d characters.\n", DESC_CHARS);
            return 1;
        }
        free(to_do_list.items[index].description);
        to_do_list.items[index].description = strdup(options[DESC]);
    }
    id = to_do_list.items[index].id;
    delete_task(FILE_NAME, id);
    save(&to_do_list.items[index], FILE_NAME);
    return 0;
}

int cmd_mod_project(char *options[], int id) {
    Project *project = NULL;

    if (id != -1) {
        project = find_project_by_id(id);
    } else if (options[NAME] != NULL) {
        project = find_project_by_name(options[NAME]);
    } else {
        printf("Provide either an id or a name for the project.\n");
        return 1;
    }

    if (!project) {
        if (id != -1)
            printf("Could not find project with id %d\n", id);
        else
            printf("Could not find project %s\n", options[NAME]);
        return 1;
    }

    int project_id = project->id;

    if (options[NAME] && id != -1) {
        if ((strlen(options[NAME]) + 1) > LONG_NAME_CHARS) {
            printf("Name is too long. Max %d characters.\n", LONG_NAME_CHARS);
            return 1;
        }
        if (find_project_by_name(options[NAME])) {
            printf("Project already exists: %s\n", options[NAME]);
            return 1;
        }
        char *old_name = project->name;
        project->name = strdup(options[NAME]);
        for (size_t i = 0; i < to_do_list.n_items; i++) {
            if (!strcmp(to_do_list.items[i].project, old_name)) {
                delete_task(FILE_NAME, to_do_list.items[i].id);
                free(to_do_list.items[i].project);
                to_do_list.items[i].project = strdup(project->name);
                save(&to_do_list.items[i], FILE_NAME);
            }
        }
        free(old_name);
    }

    if (options[PRIORITY]) {
        if (!apply_priority(options[PRIORITY], &project->priority))
            return 1;
    }

    if (options[RECURRENT]) {
        if (!apply_recurrent(options[RECURRENT], &project->recurrent))
            return 1;
    }

    if (options[NOTIFY]) {
        if (!parse_notify_value(options[NOTIFY], &project->notify))
            return 1;
    }
    if (options[DUE]) {
        if (!parse_due_value(options[DUE], project->notify, &project->due,
                             &project->due_has_time))
            return 0;
    }
    if (options[NOTIFY]) {
        normalize_due_for_notify(&project->due, project->due_has_time,
                                 project->notify);
    }

    if (options[STATUS]) {
        int status = get_status_int(options[STATUS]);
        if (status == -1) {
            printf("Invalid status option: %s\n", options[STATUS]);
            return 1;
        }
        project->status = status;
    }

    if (options[CATEGORY]) {
        if ((strlen(options[CATEGORY]) + 1) > NAME_CHARS) {
            printf("Category is too long. Max %d characters.\n", NAME_CHARS);
            return 1;
        }
        free(project->category);
        project->category = strdup(options[CATEGORY]);
    }

    if (options[DESC]) {
        if ((strlen(options[DESC]) + 1) > DESC_CHARS) {
            printf("Description is too long. Max %d characters.\n", DESC_CHARS);
            return 1;
        }
        free(project->description);
        project->description = strdup(options[DESC]);
    }

    delete_project(FILE_NAME, project_id);
    save_project(project, FILE_NAME);
    return 0;
}

int cmd_start(char *options[], int id) {
    int index = -1;
    if (id != -1) {
        for (size_t i = 0; i < to_do_list.n_items; i++) {
            if (id == to_do_list.items[i].id) {
                to_do_list.items[i].status = IN_PROGRESS;
                index = i;
                break;
            }
        }
    } else if (options[NAME] != NULL) {
        for (size_t i = 0; i < to_do_list.n_items; i++) {
            if (!strcmp(options[NAME], to_do_list.items[i].name)) {
                to_do_list.items[i].status = IN_PROGRESS;
                id = to_do_list.items[i].id;
                index = i;
                break;
            }
        }
    } else {
        printf("Provide name or id of the task to start.\n");
        return 1;
    }
    if (index == -1) {
        printf("Could not find the task to start\n");
        return 1;
    }
    delete_task(FILE_NAME, id);
    save(&to_do_list.items[index], FILE_NAME);
    return 0;
}

int cmd_done(char *options[], int id) {
    int index = -1;
    if (id != -1) {
        for (size_t i = 0; i < to_do_list.n_items; i++) {
            if (id == to_do_list.items[i].id) {
                to_do_list.items[i].status = DONE;
                index = i;
                break;
            }
        }
    } else if (options[NAME] != NULL) {
        for (size_t i = 0; i < to_do_list.n_items; i++) {
            if (!strcmp(options[NAME], to_do_list.items[i].name)) {
                to_do_list.items[i].status = DONE;
                id = to_do_list.items[i].id;
                index = i;
                break;
            }
        }
    } else {
        printf("Provide anme or id ot he completed task.\n");
        return 1;
    }
    if (index == -1) {
        printf("Could not find the completed task");
        return 1;
    }
    delete_task(FILE_NAME, id);
    save(&to_do_list.items[index], FILE_NAME);
    return 0;
}

int cmd_show(char *options[], int id) {
    if (id != -1) {
        for (size_t i = 0; i < to_do_list.n_items; i++) {
            if (id == to_do_list.items[i].id) {
                print_task(&to_do_list.items[i]);
                return (0);
            }
        }
        printf("No task with id %d\n", id);
        return 1;
    }
    for (size_t i = 0; i < to_do_list.n_items; i++) {
        if (!strcmp(options[NAME], to_do_list.items[i].name)) {
            print_task(&to_do_list.items[i]);
            return (0);
        }
    }
    printf("Could not find task: %s", options[NAME]);
    return 1;
}

int cmd_list(char *options[], int id) {
    sort_list_value(to_do_list.items, 0, to_do_list.n_items - 1);
    set_bg256(230);
    set_fg256(232);
    term_bold_on();
    print_task_table_header();
    term_bold_off();
    printf(RESET);
    int alternate = 0;
    for (size_t i = 0; i < to_do_list.n_items; i++) {
        // Cada filtro descarta filas antes de imprimir la tabla.
        if ((id != -1) && (to_do_list.items[i].id != id))
            continue;
        if (options[PRIORITY] && (to_do_list.items[i].priority !=
                                  get_priority_int(options[PRIORITY])))
            continue;
        if (options[RECURRENT] && (to_do_list.items[i].recurrent !=
                                   get_recurrence_int(options[RECURRENT])))
            continue;
        if (options[DUE]) {
            if (!strcmp(options[DUE], "today")) {
                if (when_due(to_do_list.items[i].due) != DAY)
                    continue;
            }
            if (!strcmp(options[DUE], "week")) {
                if (when_due(to_do_list.items[i].due) < WEEK)
                    continue;
            }
            if (!strcmp(options[DUE], "month")) {
                if (when_due(to_do_list.items[i].due) < MONTH)
                    continue;
            }
            if (!strcmp(options[DUE], "year")) {
                if (when_due(to_do_list.items[i].due) < YEAR)
                    continue;
            }
        }
        if (((options[STATUS] && strcmp(options[STATUS], "all")) &&
             (options[STATUS] && (to_do_list.items[i].status !=
                                  get_status_int(options[STATUS])))) ||
            (!options[STATUS] && (to_do_list.items[i].status == DONE)))
            continue;
        if (options[CATEGORY] &&
            (strcmp(to_do_list.items[i].category, options[CATEGORY])))
            continue;
        if (options[PROJECT] &&
            (strcmp(to_do_list.items[i].project, options[PROJECT])))
            continue;
        if (options[NAME] && (strcmp(to_do_list.items[i].name, options[NAME])))
            continue;
        if (options[DESC] &&
            (strcmp(to_do_list.items[i].description, options[DESC])))
            continue;
        int bg;
        if (alternate) {
            bg = 235;
        } else
            bg = 237;
        alternate = !alternate;
        // Alterna el fondo para mejorar la legibilidad de la salida tabular.
        set_bg256(bg);
        print_task_table_row(&to_do_list.items[i]);
        printf(RESET);
    }
    return 0;
}

int cmd_list_projects(char *options[], int id) {
    (void)options;
    (void)id;
    set_bg256(230);
    set_fg256(232);
    term_bold_on();
    print_proj_table_header();
    term_bold_off();
    printf(RESET);
    int alternate = 0;
    for (size_t i = 0; i < to_do_proj.n_items; i++) {
        int bg;
        if (alternate) {
            bg = 235;
        } else
            bg = 237;
        alternate = !alternate;
        set_bg256(bg);
        print_proj_table_row(&to_do_proj.items[i]);
        printf(RESET);
    }
    return 0;
}

int cmd_proj_show(char *options[], int id) {
    Project *project = NULL;
    if (id != -1) {
        project = find_project_by_id(id);
        if (!project) {
            printf("No project with id %d.", id);
            return 1;
        }
    } else {
        project = find_project_by_name(options[NAME]);
        if (!project) {
            printf("No project with name %s", options[NAME]);
            return 1;
        }
    }

    options[PROJECT] = strdup(project->name);
    if (!options[STATUS])
        options[STATUS] = strdup("all");
    options[NAME] = NULL;
    print_proj(project->id);
    cmd_list(options, -1);
    return 0;
}

int cmd_clear(char *options[], int id) {
    (void)id;
    (void)options;
    for (size_t i = 0; i < to_do_list.n_items; i++) {
        if (to_do_list.items[i].status == DONE) {
            int in_project = 0;
            if (strcmp(to_do_list.items[i].project, "none")) {
                for (size_t n = 0; n < to_do_proj.n_items; n++) {
                    in_project |= !strcmp(to_do_list.items[i].project,
                                          to_do_proj.items[n].name);
                }
            }
            if (!in_project)
                delete_task(FILE_NAME, to_do_list.items[i].id);
        }
    }
    return 0;
}
static inline void set_bg256(int n) { printf(ESC "48;5;%dm", n); }
static inline void set_fg256(int n) { printf(ESC "38;5;%dm", n); }
static inline void term_bold_on(void) {
    fputs("\x1b[1m", stdout);
} // bold on [web:14]
static inline void term_bold_off(void) { fputs("\x1b[22m", stdout); }
