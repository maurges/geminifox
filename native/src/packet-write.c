#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bytevec {
    unsigned char * data;
    size_t length;
    size_t capacity;
};

struct bytevec bytevec_empty() {
    struct bytevec r = {
        .data = NULL,
        .length = 0,
        .capacity = 0,
    };
    return r;
}

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

int main() {
    struct bytevec line = bytevec_empty();
    char buf[4096] = {0};

    while (true) {
        fgets(buf, sizeof(buf), stdin);
        int was_eof = feof(stdin);
        size_t size = strlen(buf);

        bytevec_append(&line, (unsigned char *)buf, size);
        if (line.data[line.length - 1] == '\n' || (was_eof && line.length != 0)) {
            int32_t length = line.length;
            fwrite(&length, sizeof(length), 1, stdout);
            fwrite(line.data, 1, line.length, stdout);
            line.length = 0;
            buf[0] = '\0';
        }
        if (was_eof) {
            return 0;
        }
    }
}
