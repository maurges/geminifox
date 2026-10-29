#include "utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/base64.h"

#if 0
static void bytevec_append(struct bytevec * vec, unsigned char * buf, size_t length) {
    if (vec->data == NULL) {
        vec->capacity = 256;
        while (vec->capacity < length) {
            vec->capacity *= 2;
        }
        vec->data = malloc(vec->capacity);
    } else if (vec->length + length >= vec->capacity) {
        // realloc
        while (vec->capacity < vec->length + length) {
            vec->capacity *= 2;
        }
        unsigned char * data = malloc(vec->capacity);

        memcpy(data, vec->data, vec->length);
        free(vec->data);
        vec->data = data;
    }
    memcpy(vec->data + vec->length, buf, length);
    vec->length += length;
}
#endif

struct bytevec base64_encode(const unsigned char * data, size_t length) {
    size_t out_length = 0;

    int r = mbedtls_base64_encode(NULL, 0, &out_length, data, length);
    if (r != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        fprintf(stderr, "base64 encode failed with %d\n", r);
        return bytevec_empty();
    }
    struct bytevec encoded = {
        .data = malloc(out_length),
        .length = out_length,
        .capacity = out_length,
    };
    r = mbedtls_base64_encode(encoded.data, out_length, &out_length, data, length);

    if (r != 0) {
        fprintf(stderr, "base64 encode failed with %d\n", r);
        free_bytevec(&encoded);
    }
    // Some fucking times the encoded length is less than queried, so update it
    encoded.length = out_length;

    return encoded;
}


/// Clone a string slice to a newly allocated string, and null-terminates it
char * strclone(const char * source, size_t length) {
    char * out = malloc(length + 1);
    memcpy(out, source, length);
    out[length] = '\0';
    return out;
}

size_t concat(char * dst, size_t argc, ...) {
    va_list args;
    va_start(args, argc);
    size_t offset = 0;
    for (size_t i = 0; i < argc; ++i) {
        const char * buf = va_arg(args, const char *);
        size_t len = va_arg(args, size_t);
        memcpy(dst + offset, buf, len);
        offset += len;
    }
    va_end(args);
    dst[offset] = 0;
    return offset;
}
