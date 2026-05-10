#ifndef PARSER_H
#define PARSER_H

/**
 * @file parser.h
 * @brief Legacy argument and CLI command parser declarations.
 */

/** @brief Total number of parsed option slots. */
#define NUMBER_OPT 9

/** @brief Base prefix for an ANSI escape sequence. */
#define ESC "\x1b["
/** @brief ANSI sequence that resets style and colors. */
#define RESET ESC "0m"

/** @brief Priority option index in the parsed options array. */
#define PRIORITY    0
/** @brief Recurrence option index in the parsed options array. */
#define RECURRENT   1
/** @brief Due-date option index in the parsed options array. */
#define DUE         2
/** @brief Status option index in the parsed options array. */
#define STATUS      3
/** @brief Category option index in the parsed options array. */
#define CATEGORY    4
/** @brief Project option index in the parsed options array. */
#define PROJECT     5
/** @brief Name option index in the parsed options array. */
#define NAME        6
/** @brief Description option index in the parsed options array. */
#define DESC        7
/** @brief Notification option index in the parsed options array. */
#define NOTIFY      8

/** @brief JSON storage file used by Dispatch. */
#define FILE_NAME "dispatch.json"

/**
 * @brief Function type used by each CLI command.
 * @param options Parsed options array.
 * @param id Already resolved numeric identifier, or `-1` when not applicable.
 * @return Command exit code.
 */
typedef int (*CommandHandler)(char *options[], int id);

/**
 * @struct Command
 * @brief Describes a CLI command and its associated handler.
 */
typedef struct {
    const char *name;       /**< Command text name. */
    CommandHandler handler; /**< Function that implements the command. */
    const char *desc;       /**< Description used in help output. */
} Command;

/**
 * @brief Find and execute the handler associated with a command.
 * @param cmd Command name.
 * @param options Parsed options.
 * @param id Already interpreted identifier, when present.
 * @return Command exit code.
 */
int dispatch_command(char *cmd, char *options[], int id);

/**
 * @brief Rebuild the free-form argument between the command and options.
 *
 * This allows names made from multiple words.
 *
 * @param argc Number of arguments received by `main`.
 * @param argv Argument vector received by `main`.
 * @return Dynamically allocated string, or `NULL` when there is no free text.
 */
char *parse_words(int argc, char **argv);

/**
 * @brief Interpret text as a numeric task identifier.
 * @param words Candidate text.
 * @return Parsed ID, or `-1` when the string is not numeric.
 */
int parse_id_name(char *words);

/** @brief Print the general program help. */
void print_help(void);

/**
 * @brief Parse short and long CLI options.
 * @param argc Number of arguments received by `main`.
 * @param argv Argument vector received by `main`.
 * @param options Output array indexed with option macros.
 * @return `0` after parsing completes.
 */
int parse_options(int argc, char **argv, char *options[]);

/** @brief Implement the `add` command. */
int cmd_add(char *options[], int id);

/** @brief Implement the `add_project` command. */
int cmd_add_project(char *options[], int id);

/** @brief Implement the `del` command. */
int cmd_del(char *options[], int id);

/** @brief Implement the `del_project` command. */
int cmd_del_project(char *options[], int id);

/** @brief Implement the `mod` command. */
int cmd_mod(char *options[], int id);

/** @brief Implement the `mod_project` command. */
int cmd_mod_project(char *options[], int id);

/** @brief Implement the `start` command. */
int cmd_start(char *options[], int id);

/** @brief Implement the `done` command. */
int cmd_done(char *options[], int id);

/** @brief Implement the `show` command. */
int cmd_show(char *options[], int id);

/** @brief Implement the `list` command. */
int cmd_list(char *options[], int id);

/** @brief Implement the `list_projects` command. */
int cmd_list_projects(char *options[], int id);

/** @brief Implement the `show_project` command. */
int cmd_proj_show(char *options[], int id);

/** @brief Implement the `clear` command. */
int cmd_clear(char *options[], int id);


/** @brief Set the terminal background color with the ANSI 256-color palette. */
static inline void set_bg256(int n);
/** @brief Set the terminal text color with the ANSI 256-color palette. */
static inline void set_fg256(int n);
/** @brief Enable terminal bold style. */
static inline void term_bold_on(void);
/** @brief Disable terminal bold style. */
static inline void term_bold_off(void);

#endif
