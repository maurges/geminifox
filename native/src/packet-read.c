#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    uint32_t size = 0;
    uint32_t capacity = 0;
    char * buf = NULL;

    while (true) {
        size_t r = fread(&size, sizeof(size), 1, stdin);
        if (r == 0) {
            return 0;
        }
        if (size > capacity) {
            free(buf);
            buf = malloc(size);
            capacity = size;
        }
        fread(buf, 1, size, stdin);

        fwrite(buf, 1, size, stdout);
        putchar('\n');
    }
}
