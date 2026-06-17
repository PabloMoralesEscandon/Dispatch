#include "dispatch_store.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *message, const char *detail) {
    if (detail && detail[0])
        fprintf(stderr, "FAIL: %s: %s\n", message, detail);
    else
        fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2)
        return fail("usage: lock_primitive_test <store-path>", "");

    const char *store_path = argv[1];
    char error[256] = {0};
    DispatchStoreLock first = {0};
    DispatchStoreLock second = {0};

    if (!dispatch_store_lock_acquire(&first, store_path, 0, error,
                                     sizeof(error))) {
        return fail("first lock acquire failed", error);
    }

    error[0] = '\0';
    if (dispatch_store_lock_acquire(&second, store_path, 20, error,
                                    sizeof(error))) {
        dispatch_store_lock_release(&second);
        dispatch_store_lock_release(&first);
        return fail("second lock acquire unexpectedly succeeded", "");
    }

    if (strstr(error, "locked by another process") == NULL) {
        dispatch_store_lock_release(&first);
        return fail("lock timeout message missing", error);
    }

    dispatch_store_lock_release(&first);

    error[0] = '\0';
    if (!dispatch_store_lock_acquire(&second, store_path, 0, error,
                                     sizeof(error))) {
        return fail("lock reacquire after release failed", error);
    }
    dispatch_store_lock_release(&second);

    printf("PASS: lock primitive\n");
    return 0;
}
