#ifndef DISPATCH_SHA256_H
#define DISPATCH_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Minimal SHA-256 (FIPS 180-4), used to detect edits to installed agent
 * skills (ADR 0011). Not used for anything security-sensitive. */
typedef struct {
    uint32_t state[8];
    uint64_t length;
    unsigned char buffer[64];
    size_t buffered;
} DispatchSha256;

void dispatch_sha256_init(DispatchSha256 *ctx);
void dispatch_sha256_update(DispatchSha256 *ctx, const void *data,
                            size_t size);
/* Writes 64 lowercase hex digits and a terminating NUL to hex. */
void dispatch_sha256_final_hex(DispatchSha256 *ctx, char hex[65]);
void dispatch_sha256_hex(const void *data, size_t size, char hex[65]);

#endif
