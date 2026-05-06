#ifndef UTILS_H
#define UTILS_H

#include "task.h"

/**
 * @file utils.h
 * @brief Utilidades de ordenacion para la lista de tareas.
 */

/**
 * @brief Ordena tareas por identificador usando quicksort.
 * @param disordered Array de tareas a ordenar.
 * @param low Indice inferior del rango.
 * @param high Indice superior del rango.
 */
void sort_list(Task *disordered, int low, int high);

/**
 * @brief Ordena tareas por prioridad y fecha de vencimiento.
 * @param disordered Array de tareas a ordenar.
 * @param low Indice inferior del rango.
 * @param high Indice superior del rango.
 */
void sort_list_value(Task *disordered, int low, int high);

/**
 * @brief Particion de Hoare para ordenar por identificador.
 * @param disordered Array de tareas a particionar.
 * @param low Indice inferior del rango.
 * @param high Indice superior del rango.
 * @return Indice del separador calculado.
 */
int partition(Task *disordered, int low, int high);

/**
 * @brief Particion de Hoare para ordenar por prioridad y vencimiento.
 * @param disordered Array de tareas a particionar.
 * @param low Indice inferior del rango.
 * @param high Indice superior del rango.
 * @return Indice del separador calculado.
 */
int partition_value(Task *disordered, int low, int high);

#endif
