#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <unistd.h>
#include <stdint.h>

struct connect_info {
    char * host;
    char * port;
    unsigned char * request; // Not null terminated
    size_t request_length;
};

struct response {
    const char * header;
    const char * body;
};


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

static int connect(struct connect_info * cinfo, struct bytevec * response) {
    int ret = 1;
    int exit_code = MBEDTLS_EXIT_FAILURE;
    const char * pers = "ssl_client1";

    mbedtls_net_context server_fd;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;

    // Initialize all session data
    mbedtls_net_init(&server_fd);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);
    // psa_crypto_init?

    // Seed the rng
    ret = mbedtls_ctr_drbg_seed(
        &ctr_drbg,
        mbedtls_entropy_func,
        &entropy,
        (const unsigned char *)pers,
        strlen(pers)
    );
    if (ret != 0) {
        fprintf(stderr, " failed\n  ! mbedtls_ctr_drbg_seed returned %d\n", ret);
        goto exit;
    }

    // Initialize certificates - skipped

    // Start the connection
    ret = mbedtls_net_connect(
        &server_fd,
        cinfo->host,
        cinfo->port ? cinfo->port : "1965",
        MBEDTLS_NET_PROTO_TCP
    );
    if (ret != 0) {
        fprintf(stderr, " failed\n  ! mbedtls_net_connect returned %d\n\n", ret);
        goto exit;
    }

    // Setup tls config
    ret = mbedtls_ssl_config_defaults(
        &conf,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT
    );
    if (ret != 0) {
        fprintf(stderr, " failed\n  ! mbedtls_ssl_config_defaults returned %d\n\n", ret);
        goto exit;
    }

    // Disable ssl verification, as gemini certs are self-signed
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg); // wtf is this?

    // Apply the config to to the ssl struct
    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) {
        fprintf(stderr, " failed\n  ! mbedtls_ssl_setup returned %d\n\n", ret);
        goto exit;
    }
    // Set the hostname on ssl
    ret = mbedtls_ssl_set_hostname(&ssl, cinfo->host);
    if (ret != 0) {
        fprintf(stderr, " failed\n  ! mbedtls_ssl_set_hostname returned %d\n\n", ret);
        goto exit;
    }
    // Apply the ssl to the connection
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    // Perform the handshake
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            fprintf(stderr, 
                " failed\n  ! mbedtls_ssl_handshake returned -0x%x\n\n",
                (unsigned int) -ret
            );
        }
    }

    // Verify the certificate - skipped

    // Write the request
    while ((ret = mbedtls_ssl_write(&ssl, cinfo->request, cinfo->request_length)) <= 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            fprintf(stderr, " failed\n  ! mbedtls_ssl_write returned %d\n\n", ret);
            goto exit;
        }
    }

    // Read the response from server
    unsigned char buf[4096];
    do {
        ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf));

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            // So how does this work? We don't update the buffer, do we just overwrite it?
            continue;
        }

        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            // Server closed the connection at the end of request, as expected
            break;
        }

        // ret holds the length read
        bytevec_append(response, buf, ret);

        if (ret == 0) {
            // EOF
            break;
        }
    } while (1);

    mbedtls_ssl_close_notify(&ssl);
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        exit_code = MBEDTLS_EXIT_SUCCESS;
    }

exit:
    // Clean up allocated things and return the exit code
    mbedtls_net_free(&server_fd);
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return exit_code;
}

int main(int argc, const char ** argv) {
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [REQ]", argv[0]);
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
        fprintf(stderr, "will read %u bytes", length);
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
    int r = connect(&c, &response);
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

        // Again the framing
        uint32_t out_length_ = out_length;
        fwrite(&out_length_, sizeof(out_length_), 1, stdout);
        fwrite(buf, 1, out_length, stdout);
    } else {
        fwrite(response.data, 1, response.length, stdout);
    }
    fflush(stdout);

    free_bytevec(&response);

    return r;
}
