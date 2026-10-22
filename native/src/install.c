#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 500


#include "install.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
# include <mach-o/dyld.h>
#endif

#include <sys/stat.h>
#include <sys/statvfs.h>

struct str {
    union {
        char * val;
        const char * cval;
    };
    size_t len;
};
#define S(s) {.val = s, .len = sizeof(s) - 1}
#define CS(s) {.cval = s, .len = sizeof(s) - 1}
#define NULLSTR {.val = NULL, .len = 0}

static size_t concat(char * dst, size_t argc, ...) {
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

// Returns:
// 0 if exists and is writable by user
// -1 if is not writable
// -2 if doesn't exist
// -3 if worse
static int is_writable(const char * dir) {
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

static int write_all(int fd, const char * buf, size_t size) {
    size_t retry_from = 0;
    while (retry_from < size) {
        size_t bytes_written = write(fd, buf + retry_from, size - retry_from);
        if (bytes_written <= 0) {
            if (errno == EINTR) {
                // Apparently that just happens sometimes
                continue;
            } else {
                return -1;
            }
        }
        retry_from += bytes_written;
    }
    return 0;
}


static const char manifest_name[] = "geminifox.json";

// Will allocate enough space to put manifest_name at the end
static struct str find_manifest_location(const char * home, size_t home_len) {
    char * manifest_location = NULL;

#ifdef __APPLE__
    // Parts of location relative from home or global
    const char loc_mozilla[] = "Library/Application Support/Mozilla";
    const char loc_hosts[] = "/NativeMessagingHosts/";
    // Global prefixes for manifest locations
    const struct str loc_globals[] = {
        S("/"),
    };
    // Infix when the location is in home
    const char loc_infix[] = "/";
#else
    // Parts of location relative from home or global
    const char loc_mozilla[] = "mozilla";
    const char loc_hosts[] = "/native-messaging-hosts/";
    // Global prefixes for manifest locations
    const struct str loc_globals[] = {
        // The first should be longest
        S("/usr/local/lib/"), // bsd, sometimes linux
        S("/usr/lib/"),
        S("/usr/lib64/"),
    };
    // Infix when the location is in home
    const char loc_infix[] = "/.";
#endif
    size_t loc_max_length =
        + sizeof(loc_mozilla) - 1
        + sizeof(loc_hosts) - 1
        + sizeof(loc_infix) - 1
        + (loc_globals[0].len > home_len ? loc_globals[0].len : home_len)
        + sizeof(manifest_name) - 1
        + 1;
    manifest_location = malloc(loc_max_length);

    // Try global first
    for (size_t i = 0; i < sizeof(loc_globals) / sizeof(loc_globals[0]); ++i) {
        size_t len = concat(
            manifest_location, 3,
            loc_globals[i].val, loc_globals[i].len,
            loc_mozilla, sizeof(loc_mozilla) - 1,
            loc_hosts, sizeof(loc_hosts) - 1
        );
        if (is_writable(manifest_location) == 0) {
            struct str ret = { .val = manifest_location, .len = len };
            return ret;
        }
    }

    // Try in home
    size_t off = concat(
        manifest_location, 3,
        home, home_len,
        loc_infix, sizeof(loc_infix) - 1,
        loc_mozilla, sizeof(loc_mozilla) - 1
    );
    int mozilla_writable = is_writable(manifest_location);
    if (mozilla_writable <= -2) {
        printf("not writable: %s\n", manifest_location);
        // Not in home
        goto errexit;
    }
    size_t len = off + concat(
        manifest_location + off, 1,
        loc_hosts, sizeof(loc_hosts) - 1
    );
    int dir_writable = is_writable(manifest_location);
    if (dir_writable == 0) {
        // All good, can create the file there
    } else if (dir_writable == -2 && mozilla_writable == 0) {
        // Need to create the directory first
        mkdir(manifest_location, 0755); // TODO don't create here maybe
    } else {
        fprintf(
            stderr,
            "Can't create the native manifest directory: ~/.mozilla not writable\n"
        );
        goto errexit;
    }

    struct str ret = { .val = manifest_location, .len = len };
    return ret;

errexit:
    fprintf(stderr, "Can't find location for manifest\n");
    if (manifest_location) { free(manifest_location); }
    struct str errret = { .val = NULL, .len = 0 };
    return errret;
}

static struct str find_bin_location(const char * home, size_t home_len) {
    char * bin_location = NULL;

    const char bin_name[] = "geminifox-native";

    // Try the global location first
    const char usr_local_bin[] = "/usr/local/bin/";
    if (is_writable(usr_local_bin) == 0) {
        // This function must return an allocated string, so alloc and copy
        bin_location = malloc(sizeof(usr_local_bin) - 1 + sizeof(bin_name));
        size_t len = concat(
            bin_location, 2,
            usr_local_bin, sizeof(usr_local_bin) - 1,
            bin_name, sizeof(bin_name)
        );
        struct str ret = {
            .val = bin_location,
            .len = len,
        };
        return ret;
    }

    // Try in home
    const char local_bin[] = "/.local/bin/";
    const char bin[] = "/bin/";
    bin_location = malloc(home_len + sizeof(local_bin) - 1 + sizeof(bin_name));
    memcpy(bin_location, home, home_len);
    // Try local-bin
    memcpy(bin_location + home_len, local_bin, sizeof(local_bin)); // with \0
    if (is_writable(bin_location) == 0) {
        memcpy(bin_location + home_len + sizeof(local_bin) - 1, bin_name, sizeof(bin_name));
        struct str ret = {
            .val = bin_location,
            .len = home_len + sizeof(local_bin) - 1 + sizeof(bin_name) - 1,
        };
        return ret;
    }
    // Try /bin
    memcpy(bin_location + home_len, bin, sizeof(bin)); // with \0
    if (is_writable(bin_location) == 0) {
        memcpy(bin_location + home_len + sizeof(bin) - 1, bin_name, sizeof(bin_name));
        struct str ret = {
            .val = bin_location,
            .len = home_len + sizeof(bin) - 1 + sizeof(bin_name) - 1,
        };
        return ret;
    }

    fprintf(stderr, "Can't find location for binaries\n");
    free(bin_location);
    struct str errret = NULLSTR;
    return errret;
}

static int install_self_to(const char * to, const char * from) {
    int ret = -1;

    int src_fd = 0;
    int dst_fd = 0;

    src_fd = open(from, O_RDONLY);
    if (src_fd < 0) {
        fprintf(stderr, "Failed to open %s for reading\n", from);
        goto exit;
    }
    struct stat source_stat;
    ret = fstat(src_fd, &source_stat);
    if (ret != 0) {
        fprintf(stderr, "Failed to stat %s\n", from);
        goto exit;
    }
    // Preserve permissions of the original file
    dst_fd = open(to, O_WRONLY | O_CREAT | O_TRUNC, source_stat.st_mode & 07777);
    if (dst_fd < 0) {
        fprintf(stderr, "Failed to open %s for writing\n", to);
    }

    // TODO what about mmap? What about actually using system-local copying?
    // Maybe just shell to cp actually?
    char buf[10240];
    size_t bytes_read = 0;
    while ( (bytes_read = read(src_fd, buf, sizeof(buf))) != 0 ) {
        ret = write_all(dst_fd, buf, bytes_read);
        if (ret != 0) {
            fprintf(stderr, "Failed to copy %s to %s\n", from, to);
            goto exit;
        }
    }

    ret = 0;
exit:
    if (src_fd) { close(src_fd); }
    if (dst_fd) { close(dst_fd); }
    return ret;
}

static int install_manifest_to(const char * to, const char * installed_bin_name, size_t installed_bin_name_len) {
    int ret = -1;

    int dst_fd = 0;

    const char manifest_prefix[] =
        "{ \"name\": \"geminifox\"\n"
        ", \"description\": \"Proxy that sends requests via gemini\"\n"
        ", \"type\": \"stdio\"\n"
        ", \"allowed_extensions\": [\"geminifox@amo.morj.men\"]\n"
        ", \"path\": \"";
    struct str manifest_parts[] = {
        CS(manifest_prefix),
        {.cval = installed_bin_name, .len = installed_bin_name_len},
        CS("\"\n}"),
    };

    dst_fd = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (dst_fd < 0) {
        fprintf(stderr, "Failed to open %s for writing\n", to);
        goto exit;
    }

    for (size_t i = 0; i < sizeof(manifest_parts)/sizeof(manifest_parts[0]); ++i) {
        ret = write_all(dst_fd, manifest_parts[i].cval, manifest_parts[i].len);
        if (ret != 0) {
            fprintf(stderr, "Failed to write to %s\n", to);
        }
    }

    ret = 0;
exit:
    if (dst_fd) { close(dst_fd); }
    return ret;
}

int interactive_install(const char * self_path, bool dry_run) {
    int ret = -1;

    struct str manifest_location = NULLSTR;
    struct str binary_location = NULLSTR;

    fprintf(stderr, "Gathering install information...\n");
    const char * home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Failed to look up $HOME\n");
        goto exit;
    }
    size_t home_len = strlen(home);

    manifest_location = find_manifest_location(home, home_len);
    if (!manifest_location.val) {
        // If I use a literal here, this is going to be ass to not-free later
        manifest_location.val = malloc(1);
        manifest_location.val[0] = '\0';
    }
    binary_location = find_bin_location(home, home_len);
    if (!binary_location.val) {
        binary_location.val = malloc(1);
        binary_location.val[0] = '\0';
    }

    do {
        fprintf(
            stdout,
            "Will install:\n\t1) The binary to %s\n\t2) The manifest to %s\n"
            "Press enter to proceed, or input a number to modify the path\n",
            binary_location.val,
            manifest_location.val
        );

        char buf[1024];
        char * pret = fgets(buf, sizeof(buf), stdin);
        if (pret == NULL) {
            fprintf(stderr, "Failed to get user input\n");
            goto exit;
        }
        if (buf[0] == '\n' || buf[0] == '\0') {
            if (binary_location.len == 0 || manifest_location.len == 0) {
                fprintf(stdout, "Please fill the missing paths or abort installation\n");
                continue;
            } else {
                break;
            }
        } else if (buf[1] != '\n' && buf[1] != '\0') {
            fprintf(stderr, "Unexpected choice, aborting\n");
            goto exit;
        }
        char choice = buf[0];
        if (choice == '1' || choice == '2') {
            fprintf(stdout, "Enter the new path: \n");
            fgets(buf, sizeof(buf), stdin);
            size_t input_len = strlen(buf);
            if (buf[input_len - 1] == '\n') {
                // Don't copy the nl into the values
                input_len -= 1;
            }

            if (choice == '1') {
                // Binary path
                if (buf[input_len - 1] == '/') {
                    fprintf(stderr, "The path should point to a file, not a directory\n");
                    continue;
                }
                free(binary_location.val);
                binary_location.val = malloc(input_len + 1);
                binary_location.len = input_len;
                memcpy(binary_location.val, buf, input_len);
                binary_location.val[input_len] = '\0';

            } else if (choice == '2') {
                // Manifest path
                if (buf[input_len - 1] != '/') {
                    fprintf(stderr, "The path should point to a directory and end in /\n");
                    continue;
                }
                free(manifest_location.val);
                manifest_location.val = malloc(input_len + sizeof(manifest_name));
                manifest_location.len = input_len;
                memcpy(manifest_location.val, buf, input_len);
                manifest_location.val[input_len] = '\0';

            } else {
                fprintf(stderr, "Unexpected input one line before (le dev est con)\n");
                goto exit;
            }
        }
    } while (true); // Until broken on buf[0] == '\n'

    fprintf(stderr, "Installing %s to %s\n", self_path, binary_location.val);
    if (!dry_run) {
        ret = install_self_to(binary_location.val, self_path);
        if (ret != 0) {
            goto exit;
        }
    }
    // Append the manifest name to the path
    memcpy(manifest_location.val + manifest_location.len, manifest_name, sizeof(manifest_name));
    fprintf(stderr, "Installing manifest to %s\n", manifest_location.val);
    if (!dry_run) {
        ret = install_manifest_to(manifest_location.val, binary_location.val, binary_location.len);
        if (ret != 0) {
            goto exit;
        }
    }

    if (dry_run) {
        fprintf(stderr, "Dry run completed\n");
    }

    ret = 0;
exit:
    if (manifest_location.val) { free(manifest_location.val); }
    if (binary_location.val) { free(binary_location.val); }
    return ret;
}
