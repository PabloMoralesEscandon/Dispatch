#include <jansson.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static json_t *resolve_path(json_t *root, const char *path) {
    char *copy = strdup(path);
    if (!copy)
        return NULL;

    json_t *value = root;
    char *save = NULL;
    for (char *part = strtok_r(copy, ".", &save); part;
         part = strtok_r(NULL, ".", &save)) {
        if (json_is_object(value)) {
            value = json_object_get(value, part);
        } else if (json_is_array(value)) {
            char *end = NULL;
            errno = 0;
            unsigned long index = strtoul(part, &end, 10);
            if (errno || !end || *end != '\0') {
                value = NULL;
            } else {
                value = json_array_get(value, (size_t)index);
            }
        } else {
            value = NULL;
        }
        if (!value)
            break;
    }

    free(copy);
    return value;
}

static int assert_value(json_t *value, const char *path, const char *type,
                        const char *expected) {
    if (!value) {
        fprintf(stderr, "missing JSON path: %s\n", path);
        return 0;
    }
    if (strcmp(type, "string") == 0) {
        if (!json_is_string(value) ||
            (strcmp(expected, "*") != 0 &&
             strcmp(json_string_value(value), expected) != 0)) {
            fprintf(stderr, "unexpected string at %s\n", path);
            return 0;
        }
        return 1;
    }
    if (strcmp(type, "integer") == 0) {
        char *end = NULL;
        json_int_t wanted = (json_int_t)strtoll(expected, &end, 10);
        if (!json_is_integer(value) ||
            (strcmp(expected, "*") != 0 &&
             (!end || *end != '\0' || json_integer_value(value) != wanted))) {
            fprintf(stderr, "unexpected integer at %s\n", path);
            return 0;
        }
        return 1;
    }
    if (strcmp(type, "boolean") == 0) {
        int wanted = strcmp(expected, "true") == 0;
        if (!json_is_boolean(value) || json_boolean_value(value) != wanted) {
            fprintf(stderr, "unexpected boolean at %s\n", path);
            return 0;
        }
        return 1;
    }
    if (strcmp(type, "null") == 0) {
        if (!json_is_null(value)) {
            fprintf(stderr, "expected null at %s\n", path);
            return 0;
        }
        return 1;
    }
    if (strcmp(type, "object") == 0) {
        if (!json_is_object(value)) {
            fprintf(stderr, "expected object at %s\n", path);
            return 0;
        }
        return 1;
    }
    if (strcmp(type, "array_length") == 0) {
        char *end = NULL;
        unsigned long wanted = strtoul(expected, &end, 10);
        if (!json_is_array(value) || !end || *end != '\0' ||
            json_array_size(value) != (size_t)wanted) {
            fprintf(stderr, "unexpected array length at %s\n", path);
            return 0;
        }
        return 1;
    }

    fprintf(stderr, "unknown assertion type: %s\n", type);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4 || (argc - 1) % 3 != 0) {
        fprintf(stderr, "usage: json_assert <path> <type> <expected> ...\n");
        return 2;
    }

    json_error_t error;
    json_t *root = json_loadf(stdin, JSON_REJECT_DUPLICATES, &error);
    if (!root) {
        fprintf(stderr, "invalid JSON at line %d: %s\n", error.line,
                error.text);
        return 1;
    }
    if (!json_is_object(root)) {
        fprintf(stderr, "JSON root is not an object\n");
        json_decref(root);
        return 1;
    }

    for (int i = 1; i < argc; i += 3) {
        json_t *value = resolve_path(root, argv[i]);
        if (!assert_value(value, argv[i], argv[i + 1], argv[i + 2])) {
            json_decref(root);
            return 1;
        }
    }

    json_decref(root);
    return 0;
}
