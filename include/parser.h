#ifndef PARSER_H
#define PARSER_H

/**
 * @file parser.h
 * @brief Declaraciones del analizador de argumentos y comandos CLI.
 */

/** @brief Numero total de slots usados para almacenar opciones parseadas. */
#define NUMBER_OPT 9

/** @brief Prefijo base de una secuencia ANSI. */
#define ESC "\x1b["
/** @brief Secuencia ANSI para restaurar estilo y colores. */
#define RESET ESC "0m"

/** @brief Indice de la opcion de prioridad en el array de opciones. */
#define PRIORITY    0
/** @brief Indice de la opcion de recurrencia en el array de opciones. */
#define RECURRENT   1
/** @brief Indice de la opcion de vencimiento en el array de opciones. */
#define DUE         2
/** @brief Indice de la opcion de estado en el array de opciones. */
#define STATUS      3
/** @brief Indice de la opcion de categoria en el array de opciones. */
#define CATEGORY    4
/** @brief Indice de la opcion de proyecto en el array de opciones. */
#define PROJECT     5
/** @brief Indice de la opcion de nombre en el array de opciones. */
#define NAME        6
/** @brief Indice de la opcion de descripcion en el array de opciones. */
#define DESC        7
/** @brief Indice de la opcion de notificacion en el array de opciones. */
#define NOTIFY      8

/** @brief JSON storage file used by Dispatch. */
#define FILE_NAME "dispatch.json"

/**
 * @brief Tipo de funcion usado por cada comando del CLI.
 * @param options Array de opciones parseadas.
 * @param id Identificador numerico ya resuelto, o `-1` si no aplica.
 * @return Codigo de salida del comando.
 */
typedef int (*CommandHandler)(char *options[], int id);

/**
 * @struct Command
 * @brief Describe un comando del CLI y su manejador asociado.
 */
typedef struct {
    const char *name;      /**< Nombre textual del comando. */
    CommandHandler handler; /**< Funcion que implementa el comando. */
    const char *desc;      /**< Descripcion usada en la ayuda. */
} Command;

/**
 * @brief Busca y ejecuta el manejador asociado a un comando.
 * @param cmd Nombre del comando.
 * @param options Opciones parseadas.
 * @param id Identificador ya interpretado, si existe.
 * @return Codigo de salida del comando.
 */
int dispatch_command(char *cmd, char *options[], int id);

/**
 * @brief Reconstruye el argumento libre situado entre el comando y las opciones.
 *
 * Se usa para aceptar nombres compuestos por varias palabras.
 *
 * @param argc Numero de argumentos recibidos por `main`.
 * @param argv Vector de argumentos recibido por `main`.
 * @return Cadena reservada dinamicamente o `NULL` si no hay texto libre.
 */
char *parse_words(int argc, char **argv);

/**
 * @brief Interpreta una cadena como identificador numerico de tarea.
 * @param words Texto candidato.
 * @return ID parseado o `-1` si la cadena no es puramente numerica.
 */
int parse_id_name(char *words);

/** @brief Imprime la ayuda general del programa. */
void print_help(void);

/**
 * @brief Parsea las opciones cortas y largas del CLI.
 * @param argc Numero de argumentos recibidos por `main`.
 * @param argv Vector de argumentos recibido por `main`.
 * @param options Array de salida indexado con las macros de opcion.
 * @return `0` tras completar el parseo.
 */
int parse_options(int argc, char **argv, char *options[]);

/** @brief Implementa el comando `add`. */
int cmd_add(char *options[], int id);

/** @brief Implementa el comando `add_project`. */
int cmd_add_project(char *options[], int id);

/** @brief Implementa el comando `del`. */
int cmd_del(char *options[], int id);

/** @brief Implementa el comando `del_project`. */
int cmd_del_project(char *options[], int id);

/** @brief Implementa el comando `mod`. */
int cmd_mod(char *options[], int id);

/** @brief Implementa el comando `mod_project`. */
int cmd_mod_project(char *options[], int id);

/** @brief Implementa el comando `start`. */
int cmd_start(char *options[], int id);

/** @brief Implementa el comando `done`. */
int cmd_done(char *options[], int id);

/** @brief Implementa el comando `show`. */
int cmd_show(char *options[], int id);

/** @brief Implementa el comando `list`. */
int cmd_list(char *options[], int id);

/** @brief Implementa el comando `list_projects`. */
int cmd_list_projects(char *options[], int id);

/** @brief Implementa el comando `show_project`. */
int cmd_proj_show(char *options[], int id);

/** @brief Implementa el comando `clear`. */
int cmd_clear(char *options[], int id);


/** @brief Establece el color de fondo del terminal usando la paleta ANSI de 256 colores. */
static inline void set_bg256(int n);
/** @brief Establece el color de texto del terminal usando la paleta ANSI de 256 colores. */
static inline void set_fg256(int n);
/** @brief Activa la negrita en el terminal. */
static inline void term_bold_on(void);
/** @brief Desactiva la negrita en el terminal. */
static inline void term_bold_off(void);

#endif
