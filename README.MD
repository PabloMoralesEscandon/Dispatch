TDL (ToDoList)
==============

A simple command-line to-do list manager made from scratch (using C standard libraries and jansson) as both a learning project and for personal usage.

## Installing

### Prerequisites
- Git
- GNU Make
- A C compiler (e.g., GCC or Clang)

### Build
```bash
git clone https://github.com/PabloMoralesEscandon/tdl.git
cd tdl
gcc nob.c -o nob
./nob
```

### Symlink to bin
```bash
ln -sf "$PWD/tdl" ~/.local/bin/tdl
ln -sf "$PWD/tdl" ~/bin/tdl
```

Usage
-----

    tdl [command] [id or name] <options>

Commands
--------

    add            Add a new task
    show           Show information of a task
    mod            Modify an existing task
    start          Mark a task as started
    done           Mark a task as done
    del            Delete a task
    list           List tasks, or use "tdl list projects" to list projects
    clear          Clear all done tasks, if not part of an ongoing project

Project Commands
----------------

    add_project    Add a new project with its own metadata
    mod_project    Modify an existing project
    del_project    Delete an empty project
    list_projects  List all projects
    show_project   Show information of a project

Options
-------

    -p, --priority <val>       Priority: low, medium, high, urgent
    -r, --recurrent <val>      Recurrence: daily, weekly, monthly, yearly
    -d, --due <val>            Due date: DD-MM-YYYY or DD-MM-YYYY HH:MM
    -s, --status <val>         Status: todo, ongoing, done
    -c, --category <val>       Category
    -P, --project <val>        Assign task to project by name
    -n, --name <val>           Task or project name/title
    -D, --description <val>    Task or project description
    -N, --notify <val>         Notify bits: T=4, H=2, D=1, e.g. THD or 7
    -h, --help                 Show this help and exit

Projects
--------

Projects are stored as their own list in `tasks.json`, with IDs and the same
metadata tasks use: description, priority, due date, recurrence, status,
category, and notify bits.

When adding a task with `-P project_name`, the task inherits the project's
priority, due date, recurrence, status, category, description, and notify bits
unless that same option is specified on the task command. If the named project
does not exist, `tdl` creates a default project for that name.

`tdl show_project <id or name>` prints the project's metadata and then lists
the tasks assigned to it.

Dates And Notify
----------------

Due dates accept date-only values and date+time values:

    tdl add Write copy -d "05-06-2026"
    tdl add Publish -d "05-06-2026 14:30"

Date-only tasks display without a time. Tasks with a due time display the time
when listed.

Notify is stored as a three-bit integer named THD:

    T = 4  notify when the task takes place
    H = 2  notify one hour before
    D = 1  notify on the day it takes place

You can pass either a number from `0` to `7` or letters:

    tdl add Publish -d "05-06-2026 14:30" -N TH
    tdl add Review -d "05-06-2026" -N 5

If notify bits are set on a date-only task, the due timestamp is stored at
23:59 internally, but the task still displays as date-only. `tdl` only stores
this metadata for now; it does not send notifications yet.

Examples
--------

    tdl add_project errands -p high -d "05-06-2026 09:00" -c admin -D paperwork -N THD
    tdl add Buy groceries -p low -d "01-05-2026" -c personal -P errands
    tdl list
    tdl add Do taxes -P errands
    tdl add Follow up -P errands -s todo -N D
    tdl show Buy groceries
    tdl start 3
    tdl done Buy groceries
    tdl mod 3 -p 1 -P errands
    tdl mod_project errands -c paperwork
    tdl del 3
    tdl list projects
    tdl list_projects
    tdl show_project errands
