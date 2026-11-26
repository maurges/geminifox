#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mbedtls/base64.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include "install.h"
#include "utils.h"


#ifndef VERSION
# define VERSION "0.0.1"
#endif


///// Error and progress values /////
const struct str INTERNAL_INIT = S("INTERNAL_INIT");
const struct str INTERNAL_SSL_CONFIG = S("INTERNAL_SSL_CONFIG");
const struct str INTERNAL_SSL_SETUP = S("INTERNAL_SSL_SETUP");
const struct str INTERNAL_SSL_HOSTNAME = S("INTERNAL_SSL_HOSTNAME");
const struct str INTERNAL_MKDIR = S("INTERNAL_MKDIR_FAILED");
const struct str INTERNAL_PATH_SIZE = S("INTERNAL_PATH_SIZE");
const struct str INTERNAL_CERT_NAME = S("INTERNAL_CERT_NAME");
const struct str INTERNAL_CERT_BYTES = S("INTERNAL_CERT_BYTES");
const struct str INTERNAL_WRITE = S("INTERNAL_WRITE");
const struct str INTERNAL_COMMAND_READ = S("INTERNAL_COMMAND_READ");
const struct str INTERNAL_COMMAND_PARSE = S("INTERNAL_COMMAND_PARSE");

const struct str NETWORK_FAILED = S("NETWORK_FAILED");
const struct str LOOKUP_FAILED = S("LOOKUP_FAILED");
const struct str CONNECT_FAILED = S("CONNECT_FAILED");
const struct str HANDSHAKE_FAILED = S("HANDSHAKE_FAILED");
const struct str REQUEST_FAILED = S("REQUEST_FAILED");
const struct str RESPONSE_FAILED = S("RESPONSE_FAILED");
const struct str KEY_MALFORMED = S("KEY_MALFORMED");

const struct str BAD_URL = S("BAD_URL");
const struct str LOOKUP_DOMAIN = S("LOOKUP_DOMAIN");
const struct str ESTABLISH_CONNECTION = S("ESTABLISH_CONNECTION");
const struct str ESTABLISH_HANDSHAKE = S("ESTABLISH_HANDSHAKE");
const struct str SEND_REQUEST = S("SEND_REQUEST");
const struct str KEY_MISSING = S("KEY_MISSING");
const struct str KEY_LOADED = S("KEY_LOADED");
const struct str KEY_CREATED = S("KEY_CREATED");


struct output {
    void (* write_progress) (struct str message);
    void (* write_error) (struct str message);
    void (* write_result) (const unsigned char * restrict message, size_t size_ll);
};


static void write_json_body(
    const char * restrict field,
    size_t field_len,
    const char * restrict message,
    size_t message_len
) {
    const char prefix[] = "{\"version\":\"" VERSION "\",\"";
    const char infix[] = "\":\"";
    const char suffix[] = "\"}";
    size_t length_s = sizeof(prefix) - 1 + field_len + sizeof(infix) - 1 + message_len + sizeof(suffix) - 1;
    if (length_s >= UINT32_MAX) {
        fprintf(stderr, "Overflow converting size_t to uint32_t\n");
        abort();
    }
    uint32_t length = (uint32_t)length_s;
    fwrite(&length, sizeof(length), 1, stdout);
    fwrite(prefix, 1, sizeof(prefix) - 1, stdout);
    fwrite(field, 1, field_len, stdout);
    fwrite(infix, 1, sizeof(infix) - 1, stdout);
    fwrite(message, 1, message_len, stdout);
    fwrite(suffix, 1, sizeof(suffix) - 1, stdout);
    fflush(stdout);
}
static void write_progress_json(struct str message) {
    const char field[] = "progress";
    write_json_body(field, sizeof(field) - 1, message.cval, message.len);
}
static void write_error_json(struct str message) {
    const char field[] = "error";
    write_json_body(field, sizeof(field) - 1, message.cval, message.len);
}
static void write_result_json(const unsigned char * restrict message_raw, size_t length_raw) {
    struct bytevec r = base64_encode(message_raw, length_raw);
    const char field[] = "part";
    write_json_body(field, sizeof(field) - 1, (const char * restrict)r.data, r.length);
}

static void write_null(struct str) {}
static void write_verbatim(const unsigned char * restrict message, size_t length) {
    fwrite(message, 1, length, stdout);
    fflush(stdout);
}


/// Request to a gemini server
struct connect_info {
    char * host;
    char * port;
    unsigned char * request; // Not null terminated
    size_t request_length;
};

static int parse_url(const char * restrict url, struct connect_info * restrict out) {
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
        while (url[i] != '/' && url[i] != '\0') {
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

static void free_connect_info(struct connect_info * restrict x) {
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


static int connect_tcp(const struct connect_info * restrict cinfo, struct output p) {
    int ret = -1;
    int sock = -1;
    struct addrinfo * addresses = NULL;

    // Lookup the domain
    p.write_progress(LOOKUP_DOMAIN);
    struct addrinfo lookup_params = { 0 };
    lookup_params.ai_family = AF_UNSPEC; // both ipv6 and ipv4
    lookup_params.ai_socktype = SOCK_STREAM; // tcp
    ret = getaddrinfo(cinfo->host, cinfo->port ? cinfo->port : "1965", &lookup_params, &addresses);
    if (ret == EAI_SYSTEM) {
        fprintf(stderr, "system error in domain lookup\n");
        p.write_error(NETWORK_FAILED);
        goto exit;
    } else if (ret != 0) {
        fprintf(stderr, "getaddrinfo failed with %d\n", ret);
        p.write_error(LOOKUP_FAILED);
        goto exit;
    }

    // Try to connect to all returned addrs in sequence
    p.write_progress(ESTABLISH_CONNECTION);
    // Starting with ipv6 only
    size_t address_candidates = 0;
    for (struct addrinfo * restrict addr = addresses; addr != NULL; addr = addr->ai_next) {
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
        for (struct addrinfo * restrict addr = addresses; addr != NULL; addr = addr->ai_next) {
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
        p.write_error(CONNECT_FAILED);
        ret = -1;
        goto exit;
    }

exit:
    if (addresses) {
        freeaddrinfo(addresses);
    }
    if (sock != -1) {
        return sock;
    } else {
        // Non-negative ret is a valid socket, so
        if (ret > 0) {
            ret = -ret;
        } else if (ret == 0) {
            ret = -1;
        }
        return ret ;
    }
}

struct tls_context {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_pk_context privkey;
    mbedtls_x509_crt pubkey;
};
static struct tls_context new_tls_context() {
    struct tls_context c;
    mbedtls_ssl_init(&c.ssl);
    mbedtls_ssl_config_init(&c.conf);
    mbedtls_x509_crt_init(&c.cacert);
    mbedtls_pk_init(&c.privkey);
    mbedtls_x509_crt_init(&c.pubkey);

    return c;
}
static void free_tls_context(struct tls_context * restrict c) {
    mbedtls_x509_crt_free(&c->pubkey);
    mbedtls_pk_free(&c->privkey);
    mbedtls_x509_crt_free(&c->cacert);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
}

// Adapted from from mbedtls_net_send
static int tls_send_callback(void * context, const unsigned char * buf, size_t length) {
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    int fd = (int)(intptr_t)context;

    ret = (int)write(fd, buf, length);
    if (ret < 0) {
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        } else if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        } else {
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
    }
    return ret;
}
// Adapted from from mbedtls_net_recv
static int tls_recv_callback(void * context, unsigned char * buf, size_t length) {
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    int fd = (int)(intptr_t)context;

    ret = (int)read(fd, buf, length);
    if (ret < 0) {
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        } else if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        } else {
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
    }
    return ret;
}
/// RNG for mbedtls
static int random_buffer(void * /* context */, unsigned char * buf, size_t length) {
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
        length -= ret;
    }
    return 0;
#else
    // On non-musl we can use a single non-failing function
    arc4random_buf(buf, length);
    return 0;
#endif
}

static int tls_handshake(
    int sock,
    const char * restrict hostname,
    struct tls_context * restrict c,
    bool use_key,
    struct output p
) {
    int ret = -1;

    p.write_progress(ESTABLISH_HANDSHAKE);

    // Setup tls config
    ret = mbedtls_ssl_config_defaults(
        &c->conf,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT
    );
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ssl_config_defaults failed with %d\n", ret);
        p.write_error(INTERNAL_SSL_CONFIG);
        goto exit;
    }

    // Setup rng for this config
    mbedtls_ssl_conf_rng(&c->conf, random_buffer, NULL);
    // Disable ssl verification, as gemini certs are self-signed
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);

    if (use_key) {
        ret = mbedtls_ssl_conf_own_cert(&c->conf, &c->pubkey, &c->privkey);
        if (ret != 0) {
            fprintf(stderr, "mbedtls_ssl_conf_own_cert failed with %d\n", ret);
            p.write_error(INTERNAL_SSL_CONFIG);
            goto exit;
        }
    }

    // Apply the config to to the ssl struct
    ret = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ssl_setup failed with %d\n", ret);
        p.write_error(INTERNAL_SSL_SETUP);
        goto exit;
    }
    // Set the hostname on ssl
    ret = mbedtls_ssl_set_hostname(&c->ssl, hostname);
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ssl_set_hostname failed with code %d\n", ret);
        p.write_error(INTERNAL_SSL_HOSTNAME);
        goto exit;
    }
    // Apply the ssl to the connection
    mbedtls_ssl_set_bio(&c->ssl, (void *)(intptr_t)sock, tls_send_callback, tls_recv_callback, NULL);

    // Perform the handshake
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            fprintf(
                stderr,
                "mbedtls_ssl_handshake failed with %d\n",
                ret
            );
            p.write_error(HANDSHAKE_FAILED);
            goto exit;
        }
    }

exit:
    if (ret != 0) {
        fprintf(stderr, "free tls context\n");
        free_tls_context(c);
    }
    return ret;
}

/// Writes directory to buf and returns its length. Returns 0 on error
static size_t path_for_key(const char * restrict domain, char * restrict buf, size_t buf_len) {
    int ret = -1;

    size_t path_off = data_home(buf, buf_len);
    if (path_off == 0) {
        fprintf(stderr, "Can't create keys as the data directory is undefined\n");
        return 0;
    }

    const char keys_subdir[] = "client_certificates/";
    if (path_off + sizeof(keys_subdir) >= buf_len) {
        fprintf(stderr, "Internal: requested path is too large\n");
        return 0;
    }
    memcpy(buf + path_off, keys_subdir, sizeof(keys_subdir));
    path_off += sizeof(keys_subdir) - 1;
    // Create the keys directory just in case
    ret = mkdir(buf, 0755);
    if (ret != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create directory for keys %s\n", buf);
        return 0;
    }

    size_t domain_len = strcpy_len(buf + path_off, buf_len - path_off, domain);
    path_off += domain_len;
    if (path_off + 1 >= buf_len) {
        fprintf(stderr, "Internal: requested path is too large\n");
        return 0;
    }
    buf[path_off] = '/';
    path_off += 1;
    return path_off;
}

/// 0 if the key was loaded, 1 if it wasn't, other errors for mbedtls failure
static int load_key(
    const struct connect_info * restrict cinfo,
    mbedtls_pk_context * restrict privkey,
    mbedtls_x509_crt * restrict pubkey,
    struct output p
) {
    int ret = -1;

    char path[1024] = {};
    const char root[] = "root.der";
    const char root_key[] = "root.key.der";
    size_t path_off = path_for_key(cinfo->host, path, sizeof(path));
    if (path_off == 0) {
        fprintf(stderr, "Failed to access the directory with certificates\n");
        p.write_progress(KEY_MISSING);
        ret = 1;
        goto exit;
    }
    if (path_off + sizeof(root_key) >= sizeof(path)) {
        fprintf(stderr, "Internal: requested path is too large\n");
        p.write_error(INTERNAL_PATH_SIZE);
        return -1;
    }
    if (is_writable(path) <= -2) {
        fprintf(stderr, "Key for %s doesn't exist\n", cinfo->host);
        p.write_progress(KEY_MISSING);
        ret = 1;
        goto exit;
    }

    char buf[4096] = {};

    // Read the key
    memcpy(path + path_off, root_key, sizeof(root_key));
    ret = read_all_file(path, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        goto exit;
    }
    size_t buf_len = (size_t)ret;
    ret = mbedtls_pk_parse_key(
        privkey,
        (unsigned char *)buf,
        buf_len,
        NULL,
        0,
        random_buffer,
        NULL
    );
    if (ret != 0) {
        fprintf(stderr, "Failed to parse private key at %s, code %d\n", path, ret);
        p.write_error(KEY_MALFORMED);
        goto exit;
    }

    // Read the cert
    memcpy(path + path_off, root, sizeof(root));
    ret = read_all_file(path, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        goto exit;
    }
    buf_len = (size_t)ret;
    ret = mbedtls_x509_crt_parse(
        pubkey,
        (unsigned char *)buf,
        buf_len
    );
    if (ret != 0) {
        fprintf(
            stderr,
            "Failed to parse public certificate at %s, code %d\n",
            path,
            ret
        );
        p.write_error(KEY_MALFORMED);
        goto exit;
    }

    fprintf(stderr, "Loaded key for %s\n", cinfo->host);
    p.write_progress(KEY_LOADED);
exit:
    return ret;
}

static int fetch_gemini(const struct connect_info * restrict cinfo, const struct output p) {
    int ret = 1;

    struct tls_context c = new_tls_context();
    int sock = -1;
    unsigned char * restrict buf = NULL;

    sock = connect_tcp(cinfo, p);
    if (sock < 0) {
        goto exit;
    }

    ret = load_key(cinfo, &c.privkey, &c.pubkey, p);
    if (ret != 0 && ret != 1) {
        goto exit;
    }
    bool use_key = ret != 1;

    ret = tls_handshake(sock, cinfo->host, &c, use_key, p);
    if (ret != 0) {
        goto exit;
    }

    // Verify the certificate - skipped
    // Will maybe add the check against a saved cert, but for now w/e

    // Write the request
    p.write_progress(SEND_REQUEST);
    while (true) {
        ret = mbedtls_ssl_write(&c.ssl, cinfo->request, cinfo->request_length);
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
            p.write_error(REQUEST_FAILED);
            goto exit;
        }
    }

    // Read the response from server
    const size_t buf_size = 524288; // Half a megabyte, since stdio limit is a megabyte
    buf = malloc(buf_size);
    while (true) {
        ret = mbedtls_ssl_read(&c.ssl, buf, buf_size);

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
            p.write_result(buf, (size_t)ret);
        } else {
            fprintf(stderr, "mbedtls_ssl_read failed with code %d\n", ret);
            p.write_error(RESPONSE_FAILED);
            goto exit;
        }
    }

exit:
    if (sock != -1) {
        close(sock);
    }
    if (buf) {
        free(buf);
    }
    free_tls_context(&c);

    return ret;
}

static int make_certificate(FILE * restrict cert_out, FILE * restrict key_out, struct output p) {
    int ret = -1;

    mbedtls_pk_context key;
    const mbedtls_pk_info_t * restrict pk_info;
    mbedtls_x509write_cert cert;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&cert);

    // Generate the key

    pk_info = mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
    ret = mbedtls_pk_setup(&key, pk_info);
    if (ret != 0) {
        fprintf(stderr, "Failed to setup ecdsa key\n");
        p.write_error(INTERNAL_SSL_CONFIG);
        goto exit;
    }
    ret = mbedtls_ecp_gen_key(
        MBEDTLS_ECP_DP_SECP256R1,
        mbedtls_pk_ec(key),
        random_buffer,
        NULL
    );

    // Create the certificate

    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    ret = mbedtls_x509write_crt_set_serial_raw(&cert, (unsigned char *)"\x01", 1);
    if (ret != 0) {
        fprintf(stderr, "Failed to set static serial\n");
        p.write_error(INTERNAL_SSL_CONFIG);
        goto exit;
    }

    // Set validity to a year from now
    time_t now = time(NULL);
    time_t expires = now + 365 * 24 * 60 * 60;
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char not_before[15];
    strftime(not_before, sizeof(not_before), "%Y%m%d%H%M%S", &tm_buf);
    char not_after[15];
    gmtime_r(&expires, &tm_buf);
    strftime(not_after, sizeof(not_after), "%Y%m%d%H%M%S", &tm_buf);
    ret = mbedtls_x509write_crt_set_validity(&cert, not_before, not_after);
    if (ret != 0) {
        fprintf(stderr, "Failed to set cert validity\n");
        p.write_error(INTERNAL_SSL_CONFIG);
        goto exit;
    }

    const char dn[] = "CN=geminifox";
    ret = mbedtls_x509write_crt_set_subject_name(&cert, dn);
    if (ret != 0) {
        fprintf(stderr, "Failed to set cert dn for subject\n");
        p.write_error(INTERNAL_CERT_NAME);
        goto exit;
    }
    ret = mbedtls_x509write_crt_set_issuer_name(&cert, dn);
    if (ret != 0) {
        fprintf(stderr, "Failed to set cert dn for issuer\n");
        p.write_error(INTERNAL_CERT_NAME);
        goto exit;
    }

    mbedtls_x509write_crt_set_subject_key(&cert, &key);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key);
    ret = mbedtls_x509write_crt_set_basic_constraints(&cert, /* is_ca */ false, -1);
    if (ret != 0) {
        fprintf(stderr, "Failed to set basic constraints\n");
        p.write_error(INTERNAL_CERT_NAME);
        goto exit;
    }

    // A normal certificate is 1500 bytes, allocate с горкой
    unsigned char buffer[4096] = {};
    ret = mbedtls_x509write_crt_der(
        &cert,
        buffer,
        sizeof(buffer),
        random_buffer,
        NULL
    );
    if (ret <= 0) {
        fprintf(stderr, "Failed to create certificate bytes\n");
        p.write_error(INTERNAL_CERT_BYTES);
        goto exit;
    }
    // Der writer uses a crazy scheme where bytes are written from the end of
    // the buffer
    size_t der_bytes_len = (size_t)ret;
    char * der_bytes = (char *)buffer + sizeof(buffer) - der_bytes_len;

    ret = write_all(fileno(cert_out), (char *)der_bytes, der_bytes_len);
    if (ret != 0) {
        fprintf(stderr, "Failed to write cert bytes to file\n");
        p.write_error(INTERNAL_WRITE);
        goto exit;
    }

    ret = mbedtls_pk_write_key_der(&key, buffer, sizeof(buffer));
    if (ret <= 0) {
        fprintf(stderr, "Failed to create key bytes\n");
        p.write_error(INTERNAL_CERT_BYTES);
        goto exit;
    }
    der_bytes_len = (size_t)ret;
    der_bytes = (char *)buffer + sizeof(buffer) - der_bytes_len;

    ret = write_all(fileno(key_out), (char *)der_bytes, der_bytes_len);
    if (ret != 0) {
        fprintf(stderr, "Failed to write key bytes to file\n");
        p.write_error(INTERNAL_WRITE);
        goto exit;
    }

    ret = 0;
exit:
    mbedtls_x509write_crt_free(&cert);
    mbedtls_pk_free(&key);
    return ret;
}

static int keygen(const char * restrict domain, const struct output p) {
    int ret = -1;

    char path[1024] = {};
    size_t path_off = path_for_key(domain, path, sizeof(path));
    // Create a directory for this domain
    ret = mkdir(path, 0755);
    if (ret != 0) {
        if (errno == EEXIST) {
            fprintf(stderr, "Overwriting an existing key at %s\n", path);
            // Maybe we disallow this in the future
        } else {
            fprintf(stderr, "Failed to create %s\n", path);
            p.write_error(INTERNAL_MKDIR);
            return ret;
        }
    }

    // Open root.der and root.key.der - naming for future support of subpaths
    // if I'm ever arsed
    const char root[] = "root.der";
    const char root_key[] = "root.key.der";
    if (path_off + sizeof(root_key) >= sizeof(path)) {
        fprintf(stderr, "Internal: requested path is too large\n");
        p.write_error(INTERNAL_PATH_SIZE);
        return -1;
    }
    memcpy(path + path_off, root, sizeof(root));
    FILE * restrict cert_out = fopen(path, "w");
    if (cert_out == NULL) {
        fprintf(stderr, "Failed to open %s for writing\n", path);
        return -1;
    }
    memcpy(path + path_off, root_key, sizeof(root_key));
    FILE * restrict key_out = fopen(path, "w");
    if (key_out == NULL) {
        fprintf(stderr, "Failed to open %s for writing\n", path);
        p.write_error(INTERNAL_WRITE);
        fclose(cert_out);
        return -1;
    }

    ret = make_certificate(cert_out, key_out, p);
    fclose(cert_out);
    fclose(key_out);
    if (ret != 0) {
        fprintf(stderr, "Failed to make certificate\n");
        return -1;
    }

    p.write_progress(KEY_CREATED);
    return 0;
}

int main(int argc, const char ** argv) {
    int ret = -1;

    if (argc > 3) {
        fprintf(stderr, "Usage: %s REQ\nOr run with 0 or 2 arguments for stdio mode\n", argv[0]);
        return 1;
    }

    // TODO proper args, maybe unified with commands
    if (argc >= 2 && strcmp(argv[1], "--install") == 0) {
        interactive_install(argv[0], false);

        return 0;
    }

    // firefox gives the path to the manifest and the path to the script. I don't care
    bool stdio_mode = argc == 1 || argc == 3;

    struct output print = {
        .write_error = write_null,
        .write_progress = write_null,
        .write_result = write_verbatim,
    };
    if (stdio_mode) {
        print.write_error = write_error_json;
        print.write_progress = write_progress_json;
        print.write_result = write_result_json;
    }

    const char * restrict command = NULL;
    unsigned char command_buf[1024];
    if (stdio_mode) {
        fprintf(stderr, "stdio mode, awaiting command\n");

        // The message is preceded by its length in four bytes
        uint32_t length = 0;
        size_t length_read = fread(&length, sizeof(length), 1, stdin);
        if (length_read != 1) {
            fprintf(stderr, "error reading data\n");
            print.write_error(INTERNAL_COMMAND_READ);
            return 1;
        }

        unsigned char buf_b64_[1500];
        // We mutate the pointer later, and never touch the original buf
        unsigned char * restrict buf_b64 = buf_b64_;

        size_t b64_size = fread(buf_b64, 1, length, stdin);
        int err = ferror(stdin);
        if (err != 0) {
            fprintf(stderr, "error reading data\n");
            print.write_error(INTERNAL_COMMAND_READ);
            return 1;
        }
        // strip trailing whitespace
        while (b64_size > 0 && buf_b64[b64_size - 1] == '\n') {
            b64_size -= 1;
        }
        // skip json framing if present
        if (b64_size > 0 && buf_b64[0] == '"') {
            buf_b64 += 1;
            b64_size -= 2;
        }

        size_t size = 0;
        err = mbedtls_base64_decode(command_buf, sizeof(command_buf) - 1, &size, buf_b64, b64_size);
        command_buf[size] = '\0';
        if (err != 0) {
            fprintf(stderr, "failed to parse base64\n");
            print.write_error(INTERNAL_COMMAND_PARSE);
            return err;
        }

        command = (const char *)command_buf;
    } else {
        command = argv[1];
    }

    size_t action_length = 0;
    const char * restrict arg = NULL;
    for (; command[action_length] != '\0'; ++action_length) {
        if (command[action_length] == ' ') {
            arg = &command[action_length+1];
            break;
        }
    }

    if (matches_literal("fetch", command, action_length)) {
        struct connect_info c;
        ret = parse_url(arg, &c);
        if (ret != 0) {
            fprintf(stderr, "Failed to parse url\n");
            print.write_error(BAD_URL);
            free_connect_info(&c);
            return ret;
        }

        ret = fetch_gemini(&c, print);
        free_connect_info(&c);

    } else if (matches_literal("keygen", command, action_length)) {
        struct connect_info c;
        ret = parse_url(arg, &c);
        if (ret != 0) {
            fprintf(stderr, "Failed to parse url\n");
            print.write_error(BAD_URL);
            free_connect_info(&c);
            return ret;
        }

        ret = keygen(c.host, print);
        free_connect_info(&c);

    } else if (matches_literal("version", command, action_length)) {
        const char field[] = "lazy impl";
        const char message[] = "";
        write_json_body(field, sizeof(field) - 1, message, 0);

    } else if (matches_literal("debug", command, action_length)) {
        fprintf(stderr, "all works now, be happy\n");

    } else {
        fprintf(stderr, "Unknown command at %s\n", command);
        print.write_error(INTERNAL_COMMAND_PARSE);
    }

    return ret;
}
