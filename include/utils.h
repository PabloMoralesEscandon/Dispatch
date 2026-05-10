#ifndef UTILS_H
#define UTILS_H

#include "task.h"

/**
 * @file utils.h
 * @brief Legacy sorting helpers for the task list.
 */

/**
 * @brief Sort tasks by identifier using quicksort.
 * @param disordered Task array to sort.
 * @param low Lower range index.
 * @param high Upper range index.
 */
void sort_list(Task *disordered, int low, int high);

/**
 * @brief Sort tasks by priority and due date.
 * @param disordered Task array to sort.
 * @param low Lower range index.
 * @param high Upper range index.
 */
void sort_list_value(Task *disordered, int low, int high);

/**
 * @brief Hoare partition for sorting by identifier.
 * @param disordered Task array to partition.
 * @param low Lower range index.
 * @param high Upper range index.
 * @return Calculated separator index.
 */
int partition(Task *disordered, int low, int high);

/**
 * @brief Hoare partition for sorting by priority and due date.
 * @param disordered Task array to partition.
 * @param low Lower range index.
 * @param high Upper range index.
 * @return Calculated separator index.
 */
int partition_value(Task *disordered, int low, int high);

#endif
