#include "utils.h"

#include "task.h"

#include <float.h>

void sort_list(Task *disordered, int low, int high) {
    if (low < high) {
        // Hoare devuelve el ultimo indice de la particion izquierda.
        int separator = partition(disordered, low, high);
        sort_list(disordered, low, separator);
        sort_list(disordered, separator + 1, high);
    }
}

int partition(Task *disordered, int low, int high) {
    int i, j;
    i = low - 1;
    j = high + 1;
    int pivot = disordered[low].id;
    while (1) {
        // Avanza desde ambos extremos hasta encontrar elementos fuera de sitio.
        do {
            i++;
        } while (disordered[i].id < pivot);
        do {
            j--;
        } while (disordered[j].id > pivot);
        if (i >= j)
            return j;
        Task temp = disordered[i];
        disordered[i] = disordered[j];
        disordered[j] = temp;
    }
}

void sort_list_value(Task *disordered, int low, int high) {
    if (low < high) {
        int separator = partition_value(disordered, low, high);
        sort_list_value(disordered, low, separator);
        sort_list_value(disordered, separator + 1, high);
    }
}

int partition_value(Task *disordered, int low, int high) {
    int i, j;
    i = low - 1;
    j = high + 1;
    int pivot_priority = disordered[low].priority;
    double pivot;
    if (disordered[low].due == 0)
        pivot = DBL_MAX;
    else
        // Las tareas sin fecha se empujan al final del mismo nivel de prioridad.
        pivot = second_until(disordered[low].due);
    double time_until = DBL_MAX;
    while (1) {
        do {
            i++;
            if (disordered[i].due == 0)
                time_until = DBL_MAX;
            else
                time_until = second_until(disordered[i].due);
        } while ((disordered[i].priority > pivot_priority) ||
                 ((disordered[i].priority == pivot_priority) &&
                  (time_until < pivot)));
        do {
            j--;
            if (disordered[j].due == 0)
                time_until = DBL_MAX;
            else
                time_until = second_until(disordered[j].due);
        } while ((disordered[j].priority < pivot_priority) ||
                 ((disordered[j].priority == pivot_priority) &&
                  (time_until > pivot)));
        if (i >= j)
            return j;
        // Intercambia elementos que quedaron en la particion incorrecta.
        Task temp = disordered[i];
        disordered[i] = disordered[j];
        disordered[j] = temp;
    }
}
