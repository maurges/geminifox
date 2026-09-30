#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include <string.h>

int main() {
    int ret = 1, len;
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
        strlen( pers )
    );
    if (ret != 0) {
        printf(" failed\n  ! mbedtls_ctr_drbg_seed returned %d\n", ret);
        goto exit;
    }

    // Initialize certificates - skipped

    // Start the connection
    const char * server_name = "transjovian.org";
    const char * server_port = "1965";
    ret = mbedtls_net_connect(
        &server_fd,
        server_name,
        server_port,
        MBEDTLS_NET_PROTO_TCP
    );
    if (ret != 0) {
        printf(" failed\n  ! mbedtls_net_connect returned %d\n\n", ret);
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
        printf(" failed\n  ! mbedtls_ssl_config_defaults returned %d\n\n", ret);
        goto exit;
    }

    // Disable ssl verification, as gemini certs are self-signed
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg); // wtf is this?

    // Apply the config to to the ssl struct
    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) {
        printf(" failed\n  ! mbedtls_ssl_setup returned %d\n\n", ret);
        goto exit;
    }
    // Set the hostname on ssl
    ret = mbedtls_ssl_set_hostname(&ssl, server_name);
    if (ret != 0) {
        printf(" failed\n  ! mbedtls_ssl_set_hostname returned %d\n\n", ret);
        goto exit;
    }
    // Apply the ssl to the connection
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    // Perform the handshake
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            printf(
                " failed\n  ! mbedtls_ssl_handshake returned -0x%x\n\n",
                (unsigned int) -ret
            );
        }
    }

    // Verify the certificate - skipped

    // Write the request
    const char * request = "gemini://transjovian.org/titan/index\r\n";
    size_t request_len = strlen(request);
    while ((ret = mbedtls_ssl_write(&ssl, (const unsigned char *)request, request_len)) <= 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            printf(" failed\n  ! mbedtls_ssl_write returned %d\n\n", ret);
            goto exit;
        }
    }

    // Read the response from server
    unsigned char buf[4096];
    do {
        size_t len = sizeof(buf) - 1;
        memset(buf, 0, sizeof(buf));

        ret = mbedtls_ssl_read(&ssl, buf, len);

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            // So how does this work? We don't update the buffer, do we just overwrite it?
            continue;
        }

        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            printf(
                "The return value %d from mbedtls_ssl_read() means that the server\n"
                "closed the connection first. We're ok with that.\n",
                ret
            );
            break;
        }

        // ret holds the length read
        if (ret == 0) {
            // EOF
            break;
        }

        printf("%s", buf);
    } while (1);
    printf("\n");
    fflush(stdout);

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

    mbedtls_exit(exit_code);
}
