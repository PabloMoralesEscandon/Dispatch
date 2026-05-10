#include <stdio.h>
#include <string.h>

#include "task.h"

ToDoList to_do_list;

ToDoProjects to_do_proj;

static const char *dash_if_missing(const char *s) {
    // Treat NULL, empty string, or literal "none" as missing.
    if (s == NULL || s[0] == '\0' || strcmp(s, "none") == 0) return "-";
    return s;
}

static void format_due_date(char *out, size_t size, time_t due,
                            int due_has_time) {
    if (due == (time_t)0) {
        snprintf(out, size, "-");
        return;
    }
    struct tm *tm_info = localtime(&due);
    if (!tm_info) {
        snprintf(out, size, "-");
        return;
    }
    // Only show a time when the user entered an explicit time.
    strftime(out, size, due_has_time ? "%d-%m-%Y %H:%M" : "%d-%m-%Y",
             tm_info);
}

static void print_label_value(const char *label, const char *value) {
    printf(ANSI_BOLD_BLUE "%s" ANSI_RESET " %s\n", label, value);
}

void print_task(Task *task) {
    if (!task) {
        printf("Task is NULL\n");
        return;
    }

    char due_buf[17];
    format_due_date(due_buf, sizeof(due_buf), task->due, task->due_has_time);

    // Labels are bold blue; values use the normal terminal style.
    printf(ANSI_BOLD_BLUE "Task ID:" ANSI_RESET " %d\n", task->id);
    print_label_value("Name:", task->name ? task->name : "-");

    if (task->description && task->description[0] != '\0' && strcmp(task->description, "none") != 0) {
        print_label_value("Description:", task->description);
    }

    print_label_value("Priority:", get_priority(task->priority));
    print_label_value("Due:", due_buf);

    if (task->recurrent != NO) {
        print_label_value("Recurrence:", get_recurrence(task->recurrent));
    }

    print_label_value("Status:", get_status(task->status));
    print_label_value("Category:", dash_if_missing(task->category));
    print_label_value("Project:",  dash_if_missing(task->project));
}

char *get_priority(int priority){
    switch(priority){
        case LOW:
            return "Low";
        case MEDIUM:
            return "Medium";
        case HIGH:
            return "High";
        case URGENT:
            return "URGENT";
	default:
	    return "?";
    }
}

int get_priority_int(char *priority){
    if(!strcmp(priority, "low") || !strcmp(priority, "l")) return LOW;
    else if(!strcmp(priority, "medium") || !strcmp(priority, "m")) return MEDIUM;
    else if(!strcmp(priority, "high") || !strcmp(priority, "h")) return HIGH;
    else if(!strcmp(priority, "urgent") || !strcmp(priority, "u")) return URGENT;
    else{
           return -1;
    }
}

char *get_recurrence(int recurrence){
    switch(recurrence){
        case NO:
            return "No";
        case DAILY:
            return "Daily";
        case WEEKLY:
            return "Weekly";
        case MONTHLY:
            return "Monthly";
        case YEARLY:
            return "Yearly";
	default:
	    return "?";

    }
}

int get_recurrence_int(char *recurrence){
    if(!strcmp(recurrence, "daily")) return DAILY;
    else if(!strcmp(recurrence, "weekly")) return WEEKLY;
    else if(!strcmp(recurrence, "monthly")) return MONTHLY;
    else if(!strcmp(recurrence, "yearly")) return YEARLY;
    else{
        return -1;
    }
}

char *get_status(int status){
    switch(status){
        case TODO:
            return "To do";
        case IN_PROGRESS:
            return "In progress";
        case DONE:
            return "Done";
	default:
	    return "?";
    }
}

int get_status_int(char *status){
    if(!strcmp(status, "To do")) return TODO;
    if(!strcmp(status, "to do")) return TODO;
    else if(!strcmp(status, "In progress")) return IN_PROGRESS;
    else if(!strcmp(status, "in progress")) return IN_PROGRESS;
    else if(!strcmp(status, "todo")) return TODO;
    else if(!strcmp(status, "ongoing")) return IN_PROGRESS;
    else if(!strcmp(status, "Done")) return DONE;
    else if(!strcmp(status, "done")) return DONE;
    else{
	return -1;
    }
}

int is_leap_year(int year){
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int is_valid_date(struct tm date){
    int day = date.tm_mday;
    int month = date.tm_mon + 1;
    int year = date.tm_year + 1900;
    if (year < 1) return 0;
    if (month < 1 || month > 12) return 0;
    int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) daysInMonth[1] = 29;
    return (day >= 1 && day <= daysInMonth[month - 1]);
}


double second_until(time_t target){
    time_t now = time(NULL);
    return difftime(target, now);  // target - now, in seconds
}

int when_due(time_t target){
    if(target == 0) return LATER;
    double seconds = second_until(target);
    if(seconds<(60*60*24)) return DAY;
    if(seconds<(60*60*24*7)) return WEEK;
    time_t now = time(NULL);
    struct tm time_now = *localtime(&now);
    time_now.tm_mon+=1;
    time_t time_1_mon = mktime(&time_now);
    if((target < time_1_mon) && (target > now)) return MONTH;
    time_now.tm_mon-=1;
    time_now.tm_year+=1;
    time_t time_1_year = mktime(&time_now);
    if((target < time_1_year) && (target > now)) return YEAR;
    return LATER;
}

int is_in_proj_list(char *name){
    return find_project_by_name(name) != NULL;
}

Project *find_project_by_name(const char *name) {
    if (name == NULL)
        return NULL;
    for (size_t i = 0; i < to_do_proj.n_items; i++) {
        if (!strcmp(name, to_do_proj.items[i].name))
            return &to_do_proj.items[i];
    }
    return NULL;
}

Project *find_project_by_id(int id) {
    for (size_t i = 0; i < to_do_proj.n_items; i++) {
        if (to_do_proj.items[i].id == id)
            return &to_do_proj.items[i];
    }
    return NULL;
}

void print_project(Project *project) {
    if (!project) {
        printf("Project is NULL\n");
        return;
    }

    char due_buf[17];
    format_due_date(due_buf, sizeof(due_buf), project->due,
                    project->due_has_time);

    printf(ANSI_BOLD_BLUE "Project ID:" ANSI_RESET " %d\n", project->id);
    print_label_value("Name:", project->name ? project->name : "-");

    if (project->description && project->description[0] != '\0' &&
        strcmp(project->description, "none") != 0) {
        print_label_value("Description:", project->description);
    }

    print_label_value("Priority:", get_priority(project->priority));
    print_label_value("Due:", due_buf);

    if (project->recurrent != NO) {
        print_label_value("Recurrence:", get_recurrence(project->recurrent));
    }

    print_label_value("Status:", get_status(project->status));
    print_label_value("Category:", dash_if_missing(project->category));
}

void print_proj(int id){
    Project *project = find_project_by_id(id);
    if (!project) {
        printf("No project with id %d\n", id);
        return;
    }
    float tasks = 0;
    float done = 0;
    for(size_t i=0; i<to_do_list.n_items; i++){
	if(!strcmp(project->name, to_do_list.items[i].project)){
	    tasks++;
	    if(to_do_list.items[i].status == DONE) done++;
	}
    }
    print_project(project);
    printf("Project %s has %d tasks.\n", project->name, (int)tasks);
    int width = 20;
    float percent = tasks == 0 ? 0.0f : done / tasks;
    int progress = (int)(percent * width);
    printf("Project is %.2f%% done.\n", percent*100.0);
    for(int i=0; i<width; i++){
	if(i<progress){
	   printf("#");
	} else printf("-");
    }
    printf("\n\n");

    
}

void print_task_table_header() {
    printf("%-5s %-45s %-10s %-17s %-10s %-15s %-25s %-25s\n",
           "ID", "Name", "Priority", "Due", "Recurrent", "Status", "Category", "Project");
}

void print_task_table_row(Task *t) {
    char buffer[17];
    format_due_date(buffer, sizeof(buffer), t->due, t->due_has_time);
    printf("%-5d %-45s %-10s %-17s %-10s %-15s %-25s %-25s\n",
           t->id,
           t->name ? t->name : "(none)",
           get_priority(t->priority),
           buffer,
           get_recurrence(t->recurrent),
           get_status(t->status),
           dash_if_missing(t->category),
           dash_if_missing(t->project));
}

void print_proj_table_header() {
    printf("%-5s %-25s %-10s %-17s %-10s %-15s %-25s %10s\n",
           "ID", "Project", "Priority", "Due", "Recurrent", "Status",
           "Category", "Progress");
}

void print_proj_table_row(Project *proj) {
    float tasks = 0;
    float done = 0;
    for(size_t i=0; i<to_do_list.n_items; i++){
	if(!strcmp(to_do_list.items[i].project, proj->name)){
	    tasks++;
	    if(to_do_list.items[i].status == DONE) done++;
	}
    }
    float percent = tasks == 0 ? 0.0f : 100.0f * done / tasks;
    char buffer[17];
    format_due_date(buffer, sizeof(buffer), proj->due, proj->due_has_time);
    printf("%-5d %-25s %-10s %-17s %-10s %-15s %-25s %9.2f%%\n",
	proj->id, proj->name, get_priority(proj->priority), buffer,
	get_recurrence(proj->recurrent), get_status(proj->status),
	dash_if_missing(proj->category), percent);
}
