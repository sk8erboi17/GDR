#ifndef GRD_AUTH_H
#define GRD_AUTH_H

#include "grd/common.h"
#include "grd/config.h"
#include <sodium.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRD_AUTH_PUBLIC_KEY_BYTES crypto_kx_PUBLICKEYBYTES
#define GRD_AUTH_SECRET_KEY_BYTES crypto_kx_SECRETKEYBYTES
#define GRD_AUTH_NONCE_BYTES 32U
#define GRD_AUTH_PROOF_BYTES crypto_auth_hmacsha256_BYTES
#define GRD_SESSION_KEY_BYTES crypto_aead_xchacha20poly1305_ietf_KEYBYTES

typedef struct grd_auth_challenge {
    uint8_t salt[crypto_pwhash_SALTBYTES];
    uint8_t nonce[GRD_AUTH_NONCE_BYTES];
    uint8_t server_public_key[GRD_AUTH_PUBLIC_KEY_BYTES];
} grd_auth_challenge;

typedef struct grd_auth_response {
    uint8_t client_public_key[GRD_AUTH_PUBLIC_KEY_BYTES];
    uint8_t proof[GRD_AUTH_PROOF_BYTES];
} grd_auth_response;

typedef struct grd_auth_context {
    uint8_t public_key[GRD_AUTH_PUBLIC_KEY_BYTES];
    uint8_t secret_key[GRD_AUTH_SECRET_KEY_BYTES];
    uint8_t nonce[GRD_AUTH_NONCE_BYTES];
    uint8_t session_key[GRD_SESSION_KEY_BYTES];
} grd_auth_context;

grd_status grd_auth_server_begin(
    const grd_config *config,
    grd_auth_context *context,
    grd_auth_challenge *challenge,
    grd_error *error
);
grd_status grd_auth_client_respond(
    const char *password,
    const grd_auth_challenge *challenge,
    grd_auth_context *context,
    grd_auth_response *response,
    grd_error *error
);
grd_status grd_auth_server_finish(
    const grd_config *config,
    grd_auth_context *context,
    const grd_auth_challenge *challenge,
    const grd_auth_response *response,
    grd_error *error
);
void grd_auth_context_clear(grd_auth_context *context);

#ifdef __cplusplus
}
#endif

#endif

