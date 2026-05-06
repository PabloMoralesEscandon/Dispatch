#ifndef MEMORY_H
#define MEMORY_H

#include "task.h"

/**
 * @file memory.h
 * @brief Persistencia de tareas en almacenamiento JSON.
 */

/**
 * @brief Carga las tareas desde disco y reconstruye las listas globales.
 * @param filename Nombre del fichero JSON de entrada.
 */
void load(const char *filename);

/**
 * @brief Guarda una tarea anadiendola al fichero JSON.
 * @param task Tarea a persistir.
 * @param filename Nombre del fichero JSON de salida.
 */
void save(Task *task, const char *filename);

/**
 * @brief Guarda un proyecto anadiendolo al fichero JSON.
 * @param project Proyecto a persistir.
 * @param filename Nombre del fichero JSON de salida.
 */
void save_project(Project *project, const char *filename);

/**
 * @brief Elimina una tarea persistida por su identificador.
 * @param filename Nombre del fichero JSON.
 * @param target_id ID de la tarea a eliminar.
 * @return `0` si la operacion termina correctamente; `1` si falla.
 */
int delete_task(const char *filename, int target_id);

/**
 * @brief Elimina un proyecto persistido por su identificador.
 * @param filename Nombre del fichero JSON.
 * @param target_id ID del proyecto a eliminar.
 * @return `0` si la operacion termina correctamente; `1` si falla.
 */
int delete_project(const char *filename, int target_id);

/**
 * @brief Recalcula las fechas de tareas recurrentes ya vencidas y completadas.
 * @param filename Nombre del fichero JSON asociado al almacenamiento.
 */
void update_recurrent(const char *filename);

#endif
