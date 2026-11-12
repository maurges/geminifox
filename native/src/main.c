#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include "install.h"
#include "utils.h"


#ifndef VERSION
# define VERSION "0.0.1"
#endif


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


struct output {
    void (* write_progress) (const char * message, size_t length_ll);
    void (* write_error) (const char * message, size_t length_ll);
    void (* write_result) (const unsigned char * message, size_t length_ll);
};


static void write_json_body(const char * field, size_t field_len, const char * message, size_t message_len) {
    const char prefix[] = "{\"version\":\"" VERSION "\",\"";
    const char infix[] = "\":\"";
    const char suffix[] = "\"}";
    uint32_t length = sizeof(prefix) - 1 + field_len + sizeof(infix) - 1 + message_len + sizeof(suffix) - 1;
    fwrite(&length, sizeof(length), 1, stdout);
    fwrite(prefix, 1, sizeof(prefix) - 1, stdout);
    fwrite(field, 1, field_len, stdout);
    fwrite(infix, 1, sizeof(infix) - 1, stdout);
    fwrite(message, 1, message_len, stdout);
    fwrite(suffix, 1, sizeof(suffix) - 1, stdout);
}
/// length_ll includes the null terminator for convenience
static void write_progress_json(const char * message, size_t length_ll) {
    const char field[] = "progress";
    write_json_body(field, sizeof(field) - 1, message, length_ll - 1);
}
static void write_error_json(const char * message, size_t length_ll) {
    const char field[] = "error";
    write_json_body(field, sizeof(field) - 1, message, length_ll - 1);
}
static void write_result_json(const unsigned char * message_raw, size_t length_raw) {
    struct bytevec r = base64_encode(message_raw, length_raw);
    const char field[] = "part";
    write_json_body(field, sizeof(field) - 1, (const char *)r.data, r.length);
}

static void write_null(const char *, size_t) {}
static void write_verbatim(const unsigned char * message, size_t length) {
    fwrite(message, 1, length, stdout);
}


/// Request to a gemini server
struct connect_info {
    char * host;
    char * port;
    unsigned char * request; // Not null terminated
    size_t request_length;
};

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


static int connect_tcp(const struct connect_info * cinfo, struct output p) {
    int ret = -1;
    int sock = -1;
    struct addrinfo * addresses = NULL;

    // Lookup the domain
    p.write_progress(LOOKUP_DOMAIN, sizeof(LOOKUP_DOMAIN));
    struct addrinfo lookup_params = { 0 };
    lookup_params.ai_family = AF_UNSPEC; // both ipv6 and ipv4
    lookup_params.ai_socktype = SOCK_STREAM; // tcp
    ret = getaddrinfo(cinfo->host, cinfo->port ? cinfo->port : "1965", &lookup_params, &addresses);
    if (ret == EAI_SYSTEM) {
        fprintf(stderr, "system error in domain lookup\n");
            p.write_error(NETWORK_FAILED, sizeof(NETWORK_FAILED));
        goto exit;
    } else if (ret != 0) {
        fprintf(stderr, "getaddrinfo failed with %d\n", ret);
        p.write_error(LOOKUP_FAILED, sizeof(LOOKUP_FAILED));
        goto exit;
    }

    // Try to connect to all returned addrs in sequence
    p.write_progress(ESTABLISH_CONNECTION, sizeof(ESTABLISH_CONNECTION));
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
        p.write_error(CONNECT_FAILED, sizeof(CONNECT_FAILED));
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
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
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
    mbedtls_ctr_drbg_init(&c.ctr_drbg);
    mbedtls_entropy_init(&c.entropy);
    mbedtls_pk_init(&c.privkey);
    mbedtls_x509_crt_init(&c.pubkey);

    // Seed the rng
    const char pers[] = "geminifox";
    int ret = mbedtls_ctr_drbg_seed(
        &c.ctr_drbg,
        mbedtls_entropy_func,
        &c.entropy,
        (const unsigned char *)pers,
        sizeof(pers)
    );
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ctr_drbg_seed failed with %d\n", ret);
        abort();
    }

    return c;
}
static void free_tls_context(struct tls_context * c) {
    mbedtls_x509_crt_free(&c->pubkey);
    mbedtls_pk_free(&c->privkey);
    mbedtls_x509_crt_free(&c->cacert);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
}

static int tls_handshake(
    int sock,
    const char * hostname,
    struct tls_context * c,
    bool use_key,
    struct output p
) {
    int ret = -1;

    mbedtls_net_context server_fd = { .fd = sock };

    p.write_progress(ESTABLISH_HANDSHAKE, sizeof(ESTABLISH_HANDSHAKE));

    // Setup tls config
    ret = mbedtls_ssl_config_defaults(
        &c->conf,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT
    );
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ssl_config_defaults failed with %d\n", ret);
        p.write_error(INTERNAL_SSL_CONFIG, sizeof(INTERNAL_SSL_CONFIG));
        goto exit;
    }

    // Setup rng for this config
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);
    // Disable ssl verification, as gemini certs are self-signed
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);

    if (use_key) {
        ret = mbedtls_ssl_conf_own_cert(&c->conf, &c->pubkey, &c->privkey);
        if (ret != 0) {
            fprintf(stderr, "mbedtls_ssl_conf_own_cert failed with %d\n", ret);
            p.write_error(INTERNAL_SSL_CONFIG, sizeof(INTERNAL_SSL_CONFIG));
            goto exit;
        }
    }

    // Apply the config to to the ssl struct
    ret = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ssl_setup failed with %d\n", ret);
        p.write_error(INTERNAL_SSL_SETUP, sizeof(INTERNAL_SSL_SETUP));
        goto exit;
    }
    // Set the hostname on ssl
    ret = mbedtls_ssl_set_hostname(&c->ssl, hostname);
    if (ret != 0) {
        fprintf(stderr, "mbedtls_ssl_set_hostname failed with code %d\n", ret);
        p.write_error(INTERNAL_SSL_HOSTNAME, sizeof(INTERNAL_SSL_HOSTNAME));
        goto exit;
    }
    // Apply the ssl to the connection
    mbedtls_ssl_set_bio(&c->ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    // Perform the handshake
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            fprintf(
                stderr,
                "mbedtls_ssl_handshake failed with %d\n",
                ret
            );
            p.write_error(HANDSHAKE_FAILED, sizeof(HANDSHAKE_FAILED));
            goto exit;
        }
    }

exit:
    if (ret != 0) {
        free_tls_context(c);
    }
    return ret;
}

/// Writes directory to buf and returns its length. Returns 0 on error
static size_t path_for_key(const char * domain, char * buf, size_t buf_len) {
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
    const struct connect_info * cinfo,
    mbedtls_pk_context * privkey,
    mbedtls_x509_crt * pubkey,
    /// A seeded rng for use with mbedtls_ctr_drbg_random
    void * p_rng
) {
    int ret = -1;

    (void)cinfo;

    char path[1024] = {};
    const char root[] = "root.pem";
    const char root_key[] = "root.key.pem";
    size_t path_off = path_for_key(cinfo->host, path, sizeof(path));
    if (path_off == 0) {
        fprintf(stderr, "Failed to access the directory with certificates\n");
        ret = 1;
        goto exit;
    }
    if (path_off + sizeof(root_key) >= sizeof(path)) {
        fprintf(stderr, "Internal: requested path is too large\n");
        return -1;
    }
    if (is_writable(path) <= -2) {
        fprintf(stderr, "Key for %s doesn't exist\n", cinfo->host);
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
    size_t buf_len = ret;
    ret = mbedtls_pk_parse_key(
        privkey,
        (unsigned char *)buf,
        buf_len + 1, // including the terminator
        NULL,
        0,
        mbedtls_ctr_drbg_random,
        p_rng
    );
    if (ret != 0) {
        mbedtls_strerror(ret, buf, sizeof(buf));
        fprintf(stderr, "Failed to parse private key at %s, code %d: %s\n", path, ret, buf);
        goto exit;
    }

    // Read the cert
    memcpy(path + path_off, root, sizeof(root));
    ret = read_all_file(path, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        goto exit;
    }
    buf_len = ret;
    ret = mbedtls_x509_crt_parse(
        pubkey,
        (unsigned char *)buf,
        buf_len + 1 // including the terminator
    );
    if (ret != 0) {
        mbedtls_strerror(ret, buf, sizeof(buf));
        fprintf(
            stderr,
            "Failed to parse public certificate at %s, code %d: %s\n",
            path,
            ret,
            buf
        );
        goto exit;
    }

    fprintf(stderr, "Loaded key for %s\n", cinfo->host);
exit:
    return ret;
}

static int fetch_gemini(const struct connect_info * cinfo, const struct output p) {
    int ret = 1;

    struct tls_context c = new_tls_context();
    int sock = -1;
    unsigned char * buf = NULL;

    sock = connect_tcp(cinfo, p);
    if (sock < 0) {
        goto exit;
    }

    ret = load_key(cinfo, &c.privkey, &c.pubkey, &c.ctr_drbg);
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
    p.write_progress(SEND_REQUEST, sizeof(SEND_REQUEST));
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
            p.write_error(REQUEST_FAILED, sizeof(REQUEST_FAILED));
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
            p.write_result(buf, ret);
        } else {
            fprintf(stderr, "mbedtls_ssl_read failed with code %d\n", ret);
            p.write_error(RESPONSE_FAILED, sizeof(RESPONSE_FAILED));
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

static int make_certificate(FILE * cert_out, FILE * key_out) {
    int ret = -1;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_pk_context key;
    const mbedtls_pk_info_t * pk_info;
    mbedtls_x509write_cert cert;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&cert);

    // Generate the key

    const char pers[] = "ecdsa-p256-pk";
    ret = mbedtls_ctr_drbg_seed(
        &ctr_drbg,
        mbedtls_entropy_func,
        &entropy,
        (const unsigned char *) pers,
        sizeof(pers)
    );
    if (ret != 0) {
        fprintf(stderr, "Failed to init randomness\n");
        goto exit;
    }

    pk_info = mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
    ret = mbedtls_pk_setup(&key, pk_info);
    if (ret != 0) {
        fprintf(stderr, "Failed to setup ecdsa key\n");
        goto exit;
    }
    ret = mbedtls_ecp_gen_key(
        MBEDTLS_ECP_DP_SECP256R1,
        mbedtls_pk_ec(key),
        mbedtls_ctr_drbg_random,
        &ctr_drbg
    );

    // Create the certificate

    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    ret = mbedtls_x509write_crt_set_serial_raw(&cert, (unsigned char *)"\x01", 1);
    if (ret != 0) {
        fprintf(stderr, "Failed to set static serial\n");
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
        goto exit;
    }

    const char dn[] = "CN=geminifox";
    ret = mbedtls_x509write_crt_set_subject_name(&cert, dn);
    if (ret != 0) {
        fprintf(stderr, "Failed to set cert dn for subject\n");
        goto exit;
    }
    ret = mbedtls_x509write_crt_set_issuer_name(&cert, dn);
    if (ret != 0) {
        fprintf(stderr, "Failed to set cert dn for issuer\n");
        goto exit;
    }

    mbedtls_x509write_crt_set_subject_key(&cert, &key);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key);
    ret = mbedtls_x509write_crt_set_basic_constraints(&cert, /* is_ca */ false, -1);
    if (ret != 0) {
        fprintf(stderr, "Failed to set basic constraints\n");
        goto exit;
    }

    // A normal certificate is 1500 bytes, allocate с горкой
    unsigned char pem_bytes[4096] = {};
    ret = mbedtls_x509write_crt_pem(
        &cert,
        pem_bytes,
        sizeof(pem_bytes),
        mbedtls_ctr_drbg_random,
        &ctr_drbg
    );
    if (ret != 0) {
        fprintf(stderr, "Failed to create certificate bytes\n");
        goto exit;
    }
    size_t pem_bytes_len = strlen((char *)pem_bytes);

    ret = write_all(fileno(cert_out), (char *)pem_bytes, pem_bytes_len);
    if (ret != 0) {
        fprintf(stderr, "Failed to write cert bytes to file\n");
        goto exit;
    }

    ret = mbedtls_pk_write_key_pem(&key, pem_bytes, sizeof(pem_bytes));
    if (ret != 0) {
        fprintf(stderr, "Failed to create key bytes\n");
        goto exit;
    }
    pem_bytes_len = strlen((char *)pem_bytes);

    ret = write_all(fileno(key_out), (char *)pem_bytes, pem_bytes_len);
    if (ret != 0) {
        fprintf(stderr, "Failed to write key bytes to file\n");
        goto exit;
    }

    ret = 0;
exit:
    mbedtls_x509write_crt_free(&cert);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

static int keygen(const char * domain) {
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
            return ret;
        }
    }

    // Open root.pem and root.pem.key - naming for future support of subpaths
    // if I'm ever arsed
    const char root[] = "root.pem";
    const char root_key[] = "root.key.pem";
    if (path_off + sizeof(root_key) >= sizeof(path)) {
        fprintf(stderr, "Internal: requested path is too large\n");
        return -1;
    }
    memcpy(path + path_off, root, sizeof(root));
    FILE * cert_out = fopen(path, "w");
    if (cert_out == NULL) {
        fprintf(stderr, "Failed to open %s for writing\n", path);
        return -1;
    }
    memcpy(path + path_off, root_key, sizeof(root_key));
    FILE * key_out = fopen(path, "w");
    if (key_out == NULL) {
        fprintf(stderr, "Failed to open %s for writing\n", path);
        fclose(cert_out);
        return -1;
    }

    ret = make_certificate(cert_out, key_out);
    fclose(cert_out);
    fclose(key_out);
    if (ret != 0) {
        fprintf(stderr, "Failed to make certificate\n");
        return -1;
    }

    return 0;
}

int main(int argc, const char ** argv) {
    int ret = -1;

    if (argc > 3) {
        fprintf(stderr, "Usage: %s REQ\nOr run with 0 or 2 arguments for stdio mode\n", argv[0]);
        return 1;
    }

    // XXX
    if (argc >= 1 && strcmp(argv[1], "--install") == 0) {
        interactive_install(argv[0], false);

        return 0;
    }

    // firefox gives the path to the manifest and the path to the script. I don't care
    bool stdio_mode = argc == 1 || argc == 3;

    const char * command = NULL;
    if (stdio_mode) {
        fprintf(stderr, "stdio mode, awaiting command\n");

        // The message is preceded by its length in four bytes
        uint32_t length = 0;
        size_t length_read = fread(&length, sizeof(length), 1, stdin);
        if (length_read != 1) {
            fprintf(stderr, "error reading data\n");
            return 1;
        }

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

        command = (const char *)buf;
    } else {
        command = argv[1];
    }

    size_t action_length = 0;
    const char * arg = NULL;
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
            free_connect_info(&c);
            return ret;
        }

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

        ret = fetch_gemini(&c, print);
        free_connect_info(&c);

    } else if (matches_literal("keygen", command, action_length)) {
        ret = keygen(arg);

    } else if (matches_literal("version", command, action_length)) {
        const char field[] = "lazy impl";
        const char message[] = "";
        write_json_body(field, sizeof(field) - 1, message, 0);

    } else if (matches_literal("debug", command, action_length)) {
        char buf[1024] = {};
        size_t off = data_home(buf, sizeof(buf));
        printf("%lu: %s\n", off, buf);

    } else {
        fprintf(stderr, "Unknown command at %s\n", command);
    }

    return ret;
}
