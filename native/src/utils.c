#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 500

#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "mbedtls/base64.h"
#include "mbedtls/entropy.h"

#if 0
void bytevec_append(struct bytevec * restrict vec, unsigned char * restrict buf, size_t length) {
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

struct bytevec base64_encode(const unsigned char * restrict data, size_t length) {
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
char * strclone(const char * restrict source, size_t length) {
    char * restrict out = malloc(length + 1);
    memcpy(out, source, length);
    out[length] = '\0';
    return out;
}

size_t strcpy_len(char * restrict dest, size_t dest_len, const char * restrict source) {
    size_t i = 0;
    for (; i < dest_len && source[i] != '\0'; ++i) {
        dest[i] = source[i];
    }
    if (i < dest_len) {
        dest[i] = '\0';
    }
    return i;
}

size_t concat(char * restrict dst, size_t argc, ...) {
    va_list args;
    va_start(args, argc);
    size_t offset = 0;
    for (size_t i = 0; i < argc; ++i) {
        const char * restrict buf = va_arg(args, const char *);
        size_t len = va_arg(args, size_t);
        memcpy(dst + offset, buf, len);
        offset += len;
    }
    va_end(args);
    dst[offset] = 0;
    return offset;
}


static int ull2i(unsigned long long x) {
    if (x > INT_MAX) {
        fprintf(stderr, "Overflowing conversion of unsigned long to int\n");
        abort();
    }
    return (int)x;
}


/// Returns negative for error, positive for bytes read. Null-terminates the buf
int read_all_file(const char * restrict fname, char * restrict buf, size_t size) {
    int ret = -1;

    FILE * restrict f = fopen(fname, "r");
    if (f == NULL) {
        fprintf(stderr, "Failed to open %s for reading\n", fname);
        return -1;
    }

    size_t total = 0;
    while (true) {
        size_t read = fread(buf + total, 1, size - total, f);
        total += read;
        if (read == 0) {
            if (feof(f)) {
                buf[total] = '\0';
                ret = ull2i(total);
                goto exit;
            } else if (ferror(f)) {
                fprintf(stderr, "Failed to read from %s\n", fname);
                ret = -1;
                goto exit;
            }
        }
    }

exit:
    if (f) { fclose(f); }
    return ret;
}

int write_all(int fd, const char * restrict buf, size_t size) {
    size_t retry_from = 0;
    while (retry_from < size) {
        ssize_t bytes_written = write(fd, buf + retry_from, size - retry_from);
        if (bytes_written <= 0) {
            if (errno == EINTR) {
                // Apparently that just happens sometimes
                continue;
            } else {
                return -1;
            }
        }
        retry_from += (size_t)bytes_written;
    }
    return 0;
}

int is_writable(const char * restrict dir) {
    struct stat st;
    if (stat(dir, &st) != 0) {
        // Does not exist
        return -2;
    }
    struct statvfs vst;
    if (statvfs(dir, &vst) != 0) {
        // Failed to get filesystem info, so can't access
        return -3;
    }
    if ((vst.f_flag & ST_RDONLY) != 0) {
        // Read only filesystem
        return -1;
    }
    if (faccessat(AT_FDCWD, dir, W_OK, AT_EACCESS) != 0) {
        // Write permission not granted for dir
        return -1;
    }
    return 0;
}


size_t data_home(char * restrict buf, size_t max_len) {
    const char geminifox[] = "geminifox/";
    size_t offset = 0;

    const char * restrict xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home != NULL) {
        offset = strcpy_len(buf, max_len, xdg_data_home);
        if (offset >= max_len) {
            return 0;
        }
        buf[offset] = '/';
        offset += 1;
    } else {
        const char * restrict home = getenv("HOME");
        if (home == NULL) {
            fprintf(stderr, "Failed to lookup $HOME\n");
            return 0;
        }
        offset = strcpy_len(buf, max_len, home);
        const char suffix[] = "/.local/share/";
        if (offset + sizeof(suffix) >= max_len) {
            return 0;
        }
        memcpy(buf + offset, suffix, sizeof(suffix));
        offset += sizeof(suffix) - 1;
    }

    if (offset + sizeof(geminifox) > max_len) {
        return 0;
    }
    memcpy(buf + offset, geminifox, sizeof(geminifox));
    offset += sizeof(geminifox) - 1;
    return offset;
}

int random_buffer(void * /* context */, unsigned char * buf, size_t length) {
#if defined(__linux__)
    // On linux we use getrandom that may fail when filling the buffer, yay
    while (length > 0) {
        ssize_t ret = getrandom(buf, length, 0);
        if (ret <= 0) {
            if (errno == EINTR) {
                continue;
            } else {
                // According to the man this can't happen, but we're being safe
                return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
            }
        }
        buf += ret;
        length -= (size_t)ret;
    }
    return 0;
#else
    // On non-musl we can use a single non-failing function
    arc4random_buf(buf, length);
    return 0;
#endif
}
