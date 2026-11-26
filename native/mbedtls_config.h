// What we want:
// - Tls 1.2 and 1.3, with support for rsa and ecdsa (eddsa is apparently not
//   supported by this version)
// - Certificate and key generation on p256, and writing of the DER format
// - Reading of the DER format key
// - Base64
//
// See vendor/mbedtls/include/mbedtls/mbedtls_config.h for documentation on options

///// Hardware /////

#define MBEDTLS_HAVE_ASM
// Runtime-detected hardware aes support
#define MBEDTLS_AESNI_C
#define MBEDTLS_AESCE_C

///// TLS /////

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ALL_ALERT_MESSAGES

// TLS 1.2: ephemeral ECDH, server signs with RSA or ECDSA
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET

// TLS 1.3: certificate-based handshake only, no PSK and no session resumption
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
// As said by the small note, always required for tls 1.3
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

///// Certificates /////

#define MBEDTLS_X509_USE_C
// Parsing
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
// Writing
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
// This one needs to be defined to /disable/, funny
#define MBEDTLS_X509_REMOVE_INFO

///// Ciphers and other cryptography /////

#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BIGNUM_C

// Supported elliptic curves
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

// RSA for server certificates
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15 // for tls 1.2
#define MBEDTLS_PKCS1_V21 // for tls 1.3


// AEAD
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

// Hashes. Need to enable all because a server certificate with an unknown hash
// will fail to parse completely
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_HKDF_C

// Base64
#define MBEDTLS_BASE64_C

///// PSA crypto, required by TLS 1.3 /////

#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC
// Use our defined random instead of mbedtls
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
