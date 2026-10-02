#define _POSIX_C_SOURCE 200112L

#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"


///// Error and progress values /////
#define INTERNAL_INIT "INTERNAL_INIT"
#define INTERNAL_SSL_CONFIG "INTERNAL_SSL_CONFIG"
#define INTERNAL_SSL_SETUP "INTERNAL_SSL_SETUP"
#define INTERNAL_SSL_HOSTNAME "INTERNAL_SSL_HOSTNAME"

#define NETWORK_FAILED "NETWORK_FAILED"
#define LOOKUP_FAILED "LOOKUP_FAILED"
#define CONNECT_FAILED "CONNECT_FAILED"
#define HANDSHAKE_FAILED "HANDSHAKE_FAILED"
#define REQUEST_FAILED "REQUEST_FAILED"
#define RESPONSE_FAILED "RESPONSE_FAILED"

#define LOOKUP_DOMAIN "LOOKUP_DOMAIN"
#define ESTABLISH_CONNECTION "ESTABLISH_CONNECTION"
#define ESTABLISH_HANDSHAKE "ESTABLISH_HANDSHAKE"
#define SEND_REQUEST "SEND_REQUEST"
#define RESPONSE_PARTIAL "RESPONSE_PARTIAL"


/// Request to a gemini server
struct connect_info {
    char * host;
    char * port;
    unsigned char * request; // Not null terminated
    size_t request_length;
};

/// Response from a gemini server
struct response {
    const char * header;
    const char * body;
};


/// Clone a string slice to a newly allocated string, and null-terminates it
static char * strclone(const char * source, size_t length) {
    char * out = malloc(length + 1);
    memcpy(out, source, length);
    out[length] = '\0';
    return out;
}

static int parse_url(const char * url, struct connect_info * out) {
    out->host = NULL;
    out->port = NULL;
    out->request = NULL;

    // Find start of the domain name
    const char proto[] = "gemini://";
    size_t i = 0;
    while (url[i] == proto[i]) {
        ++i;
    }
    if (i != sizeof(proto) - 1) {
        fprintf(stderr, "incorrect protocol at %lu: %c != %c\n", i, url[i], proto[i]);
        return 1;
    }
    size_t domain_start = i;

    // Find end of domain name
    while (url[i] != ':' && url[i] != '/' && url[i] != '\0') {
        ++i;
    }
    size_t domain_end = i;
    size_t domain_length = domain_end - domain_start;
    out->host = strclone(url + domain_start, domain_length);

    // Non-standard port
    if (url[i] == ':') {
        ++i;
        size_t port_start = i;
        while (url[i] != '/') {
            ++i;
        }
        size_t port_end = i;
        size_t port_length = port_end - port_start;
        out->port = strclone(url + port_start, port_length);
    }

    // Now make the request string
    size_t url_length = strlen(url);
    out->request = malloc(url_length + 2);
    memcpy(out->request, url, url_length);
    out->request[url_length] = '\r';
    out->request[url_length+1] = '\n';
    out->request_length = url_length + 2;

    return 0;
}

static void free_connect_info(struct connect_info * x) {
    if (x->host) {
        free(x->host);
    }
    if (x->port) {
        free(x->port);
    }
    if (x->request) {
        free(x->request);
    }
}

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

static void free_bytevec(struct bytevec * vec) {
    if (vec->data) {
        free(vec->data);
    }
    vec->data = NULL;
    vec->length = 0;
    vec->capacity = 0;
}


/// Write the data as expected by firefox stdio framing
void write_stdio_packet(unsigned char * data, size_t length_ll) {
    uint32_t length = length_ll; // Be what it may
    fwrite(&length, sizeof(length), 1, stdout);
    fwrite(data, 1, length_ll, stdout);
}

/// length_ll includes the null terminator for convenience
void write_progress(const char * message, size_t length_ll) {
    const char prefix[] = "{\"progress\":\"";
    const char suffix[] = "\"}";
    uint32_t length = sizeof(prefix) - 1 + length_ll - 1+ sizeof(suffix) - 1;
    fwrite(&length, sizeof(length), 1, stdout);
    fwrite(prefix, 1, sizeof(prefix) - 1, stdout);
    fwrite(message, 1, length_ll - 1, stdout);
    fwrite(suffix, 1, sizeof(suffix) - 1, stdout);
    fflush(stdout);
}

/// length_ll includes the null terminator for convenience
void write_error(const char * message, size_t length_ll) {
    const char prefix[] = "{\"error\":\"";
    const char suffix[] = "\"}";
    uint32_t length = sizeof(prefix) - 1 + length_ll - 1 + sizeof(suffix) - 1;
    fwrite(&length, sizeof(length), 1, stdout);
    fwrite(prefix, 1, sizeof(prefix) - 1, stdout);
    fwrite(message, 1, length_ll - 1, stdout);
    fwrite(suffix, 1, sizeof(suffix) - 1, stdout);
    fflush(stdout);
}

static int fetch_gemini(struct connect_info * cinfo, struct bytevec * response, bool stdio_mode) {
    int ret = 1;
    const char * pers = "geminifox";

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;

    // Initialize all session data
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    int sock = -1;
    struct addrinfo * addresses = NULL;

    // Seed the rng
    ret = mbedtls_ctr_drbg_seed(
        &ctr_drbg,
        mbedtls_entropy_func,
        &entropy,
        (const unsigned char *)pers,
        strlen(pers)
    );
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ctr_drbg_seed failed with %d\n", ret);
        if (stdio_mode) {
            write_error(INTERNAL_INIT, sizeof(INTERNAL_INIT));
        }
        goto exit;
    }

    // Initialize certificates - skipped

    // Lookup the domain
    if (stdio_mode) {
        write_progress(LOOKUP_DOMAIN, sizeof(LOOKUP_DOMAIN));
    }
    struct addrinfo lookup_params = { 0 };
    lookup_params.ai_family = AF_UNSPEC; // both ipv6 and ipv4
    lookup_params.ai_socktype = SOCK_STREAM; // tcp
    ret = getaddrinfo(cinfo->host, cinfo->port ? cinfo->port : "1965", &lookup_params, &addresses);
    if (ret == EAI_SYSTEM) {
        fprintf(stderr, "system error in domain lookup\n");
        if (stdio_mode) {
            write_error(NETWORK_FAILED, sizeof(NETWORK_FAILED));
        }
        goto exit;
    } else if (ret != 0) {
        fprintf(stderr, "getaddrinfo failed with %d\n", ret);
        if (stdio_mode) {
            write_error(LOOKUP_FAILED, sizeof(LOOKUP_FAILED));
        }
        goto exit;
    }

    // Try to connect to all returned addrs in sequence
    if (stdio_mode) {
        write_progress(ESTABLISH_CONNECTION, sizeof(ESTABLISH_CONNECTION));
    }
    // Starting with ipv6 only
    size_t address_candidates = 0;
    for (struct addrinfo * addr = addresses; addr != NULL; addr = addr->ai_next) {
        address_candidates += 1;
        if (addr->ai_family == AF_INET) {
            continue;
        }
        sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (sock == -1) {
            continue;
        }
        ret = connect(sock, addr->ai_addr, addr->ai_addrlen);
        if (ret != -1) {
            break;
        }
        close(sock);
        sock = -1;
    }
    // If that failed, try ipv4
    if (sock == -1) {
        for (struct addrinfo * addr = addresses; addr != NULL; addr = addr->ai_next) {
            if (addr->ai_family != AF_INET) {
                continue;
            }
            sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
            if (sock == -1) {
                continue;
            }
            ret = connect(sock, addr->ai_addr, addr->ai_addrlen);
            if (ret != -1) {
                break;
            }
            close(sock);
            sock = -1;
        }
    }
    // If that failed too, abort
    if (sock == -1) {
        fprintf(stderr, "failed to connect to any of %lu addresses\n", address_candidates);
        if (stdio_mode) {
            write_error(CONNECT_FAILED, sizeof(CONNECT_FAILED));
        }
        goto exit;
    }


    mbedtls_net_context server_fd = { .fd = sock };

    if (stdio_mode) {
        write_progress(ESTABLISH_HANDSHAKE, sizeof(ESTABLISH_HANDSHAKE));
    }

    // Setup tls config
    ret = mbedtls_ssl_config_defaults(
        &conf,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT
    );
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ssl_config_defaults failed with %d\n", ret);
        if (stdio_mode) {
            write_error(INTERNAL_SSL_CONFIG, sizeof(INTERNAL_SSL_CONFIG));
        }
        goto exit;
    }

    // Setup rng for this config
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    // Disable ssl verification, as gemini certs are self-signed
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);

    // Apply the config to to the ssl struct
    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ssl_setup failed with %d\n", ret);
        if (stdio_mode) {
            write_error(INTERNAL_SSL_SETUP, sizeof(INTERNAL_SSL_SETUP));
        }
        goto exit;
    }
    // Set the hostname on ssl
    ret = mbedtls_ssl_set_hostname(&ssl, cinfo->host);
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ssl_set_hostname failed with code %d\n", ret);
        if (stdio_mode) {
            write_error(INTERNAL_SSL_HOSTNAME, sizeof(INTERNAL_SSL_HOSTNAME));
        }
        goto exit;
    }
    // Apply the ssl to the connection
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    // Perform the handshake
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            fprintf(
                stderr,
                "mbedtls_ssl_handshake failed with %d\n",
                ret
            );
            if (stdio_mode) {
                write_error(HANDSHAKE_FAILED, sizeof(HANDSHAKE_FAILED));
            }
        }
    }

    // Verify the certificate - skipped

    // Write the request
    if (stdio_mode) {
        write_progress(SEND_REQUEST, sizeof(SEND_REQUEST));
    }
    while (true) {
        ret = mbedtls_ssl_write(&ssl, cinfo->request, cinfo->request_length);
        if (ret > 0 && (size_t)ret == cinfo->request_length) {
            // Write success
            break;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            // These two statuses mean we just retry the write
            continue;
        } else {
            if (ret > 0 && (size_t)ret < cinfo->request_length) {
                fprintf(
                    stderr,
                    "mbedtls_ssl_write failed to write the full buffer: %d < %lu",
                    ret,
                    cinfo->request_length
                );
            } else {
                fprintf(stderr, "mbedtls_ssl_write failed with code %d\n", ret);
            }
            if (stdio_mode) {
                write_error(REQUEST_FAILED, sizeof(REQUEST_FAILED));
            }
            goto exit;
        }
    }

    // Read the response from server
    unsigned char buf[4096];
    char status_buf[] = RESPONSE_PARTIAL " 18446744073709551616";
    while (true) {
        ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf));

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            // These two statuses mean we just retry the read
            continue;
        } else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            // Server closed the connection at the end of request, as expected
            ret = 0;
            break;
        } else if (ret == 0) {
            // Server closed the connection without a notification, which is
            // discouraged but permitted
            break;
        } else if (ret > 0) {
            // ret holds the length read
            bytevec_append(response, buf, (size_t)ret);

            if (stdio_mode) {
                size_t s = snprintf(status_buf, sizeof(status_buf), RESPONSE_PARTIAL " %lu", response->length);
                write_progress(status_buf, s + 1);
            }
        } else {
            fprintf(stderr, "mbedtls_ssl_read failed with code %d\n", ret);
            if (stdio_mode) {
                write_error(RESPONSE_FAILED, sizeof(RESPONSE_FAILED));
            }
            goto exit;
        }
    }

exit:
    // Clean up allocated things and return the exit code
    if (addresses) {
        freeaddrinfo(addresses);
    }
    if (sock != -1) {
        close(sock);
    }
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return ret;
}

int main(int argc, const char ** argv) {
    if (argc > 3) {
        fprintf(stderr, "Usage: %s REQ\nOr run with 0 or 2 arguments for stdio mode\n", argv[0]);
        return 1;
    }

    // firefox gives the path to the manifest and the path to the script. I don't care
    bool stdio_mode = argc == 1 || argc == 3;

    struct connect_info c;

    if (stdio_mode) {
        fprintf(stderr, "stdio mode, awaiting command\n");

        // The message is preceded by its length in four bytes
        uint32_t length = 0;
        fread(&length, sizeof(length), 1, stdin);
        fflush(stderr);

        // Maximum gemini url length is 1024
        unsigned char buf[1025];
        unsigned char buf_b64_[1500];
        unsigned char * buf_b64 = buf_b64_; // Because we want to mutate the pointer later

        size_t b64_size = fread(buf_b64, 1, length, stdin);
        int err = ferror(stdin);
        if (err != 0) {
            fprintf(stderr, "error reading data\n");
            return 1;
        }
        // strip trailing whitespace
        while (buf_b64[b64_size - 1] == '\n') {
            b64_size -= 1;
        }
        // skip json framing if present
        if (buf_b64[0] == '"') {
            buf_b64 += 1;
            b64_size -= 2;
        }

        size_t size = 0;
        err = mbedtls_base64_decode(buf, sizeof(buf), &size, buf_b64, b64_size);
        buf[size] = '\0';
        if (err != 0) {
            fprintf(stderr, "failed to parse base64\n");
            return err;
        }

        err = parse_url((char *)buf, &c);
        if (err != 0) {
            fprintf(stderr, "failed to parse url\n");
            return err;
        }
    } else {
        int r = parse_url(argv[1], &c);
        if (r != 0) {
            fprintf(stderr, "failed to parse\n");
            return r;
        }
    }

    struct bytevec response = bytevec_empty();
    int r = fetch_gemini(&c, &response, stdio_mode);
    free_connect_info(&c);

    if (stdio_mode) {
        // print the result as base64 json, for stdio mode
        size_t out_length = 0;
        int r = mbedtls_base64_encode(NULL, 0, &out_length, response.data, response.length);
        if (r != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && r != 0) {
            fprintf(stderr, "base64 encode failed\n");
        }
        unsigned char * buf = malloc(out_length + 2);
        r = mbedtls_base64_encode(buf + 1, out_length, &out_length, response.data, response.length);
        if (r != 0) {
            fprintf(stderr, "base64 encode failed\n");
        }
        // add json string quotes
        buf[0] = '"';
        buf[out_length + 1] = '"';
        out_length += 2;

        write_stdio_packet(buf, out_length);
    } else {
        fwrite(response.data, 1, response.length, stdout);
    }
    fflush(stdout);

    free_bytevec(&response);

    return r;
}
