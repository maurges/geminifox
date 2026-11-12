#include <stddef.h>
#include <stdlib.h>

struct bytevec {
    unsigned char * data;
    size_t length;
    size_t capacity;
};

inline struct bytevec bytevec_empty() {
    struct bytevec r = {
        .data = NULL,
        .length = 0,
        .capacity = 0,
    };
    return r;
}

inline void free_bytevec(struct bytevec * restrict vec) {
    if (vec->data) {
        free(vec->data);
    }
    vec->data = NULL;
    vec->length = 0;
    vec->capacity = 0;
}
struct bytevec base64_encode(const unsigned char * restrict data, size_t length);


struct str {
    union {
        char * val;
        const char * cval;
    };
    size_t len;
};
#define S(s) {.val = s, .len = sizeof(s) - 1}
#define CS(s) {.cval = s, .len = sizeof(s) - 1}
inline struct str nullstr() {
    struct str r = {
        .val = NULL,
        .len = 0,
    };
    return r;
}
inline struct str str_of_parts(char * val, size_t len) {
    struct str r = {
        .val = val,
        .len = len,
    };
    return r;
}

/// Clone a string slice to a newly allocated string, and null-terminates it
char * strclone(const char * restrict source, size_t length);
/// Copy a null-terminated string and return its length
size_t strcpy_len(char * restrict dest, size_t dest_len, const char * restrict source);
size_t concat(char * restrict dst, size_t argc, ...);

#define matches_literal(LIT, str, len) \
    (sizeof(LIT) - 1 == len && strncmp(LIT, str, len) == 0)


int read_all_file(const char * restrict fname, char * restrict buf, size_t size);
int write_all(int fd, const char * restrict buf, size_t size);
// Returns:
// 0 if exists and is writable by user
// -1 if is not writable
// -2 if doesn't exist
// -3 if worse
int is_writable(const char * restrict dir);

/// Writes "$XDG_DATA_HOME/geminifox/" to buf, falling back to ~/.local/share
/// Returns size of data written, or a zero value for error
size_t data_home(char * restrict buf, size_t max_len);
