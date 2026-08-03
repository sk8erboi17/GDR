#include "grd/auth.h"

#include <stdio.h>
#include <string.h>

static void set_error(grd_error *error, grd_status code, const char *message)
{
    if (error != NULL) {
        error->code = code;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
}

static void auth_transcript(
    const grd_auth_challenge *challenge,
    const uint8_t client_public_key[GRD_AUTH_PUBLIC_KEY_BYTES],
    uint8_t output[GRD_AUTH_NONCE_BYTES + 2U * GRD_AUTH_PUBLIC_KEY_BYTES]
)
{
    memcpy(output, challenge->nonce, GRD_AUTH_NONCE_BYTES);
    memcpy(
        output + GRD_AUTH_NONCE_BYTES,
        challenge->server_public_key,
        GRD_AUTH_PUBLIC_KEY_BYTES
    );
    memcpy(
        output + GRD_AUTH_NONCE_BYTES + GRD_AUTH_PUBLIC_KEY_BYTES,
        client_public_key,
        GRD_AUTH_PUBLIC_KEY_BYTES
    );
}

static void derive_session_key(
    const uint8_t shared[crypto_scalarmult_BYTES],
    const uint8_t verifier[32],
    const uint8_t nonce[GRD_AUTH_NONCE_BYTES],
    uint8_t output[GRD_SESSION_KEY_BYTES]
)
{
    crypto_generichash_state state;
    (void)crypto_generichash_init(&state, verifier, 32U, GRD_SESSION_KEY_BYTES);
    (void)crypto_generichash_update(&state, shared, crypto_scalarmult_BYTES);
    (void)crypto_generichash_update(&state, nonce, GRD_AUTH_NONCE_BYTES);
    (void)crypto_generichash_final(&state, output, GRD_SESSION_KEY_BYTES);
}

grd_status grd_auth_server_begin(
    const grd_config *config,
    grd_auth_context *context,
    grd_auth_challenge *challenge,
    grd_error *error
)
{
    if (config == NULL || context == NULL || challenge == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    if (!config->password_configured) {
        set_error(error, GRD_AUTH_FAILED, "Host password is not configured");
        return GRD_AUTH_FAILED;
    }
    memset(context, 0, sizeof(*context));
    randombytes_buf(context->nonce, sizeof(context->nonce));
    if (crypto_kx_keypair(context->public_key, context->secret_key) != 0) {
        set_error(error, GRD_ERROR, "Session-key generation failed");
        return GRD_ERROR;
    }
    memcpy(challenge->salt, config->password_salt, sizeof(challenge->salt));
    memcpy(challenge->nonce, context->nonce, sizeof(challenge->nonce));
    memcpy(challenge->server_public_key, context->public_key, sizeof(challenge->server_public_key));
    return GRD_OK;
}

grd_status grd_auth_client_respond(
    const char *password,
    const grd_auth_challenge *challenge,
    grd_auth_context *context,
    grd_auth_response *response,
    grd_error *error
)
{
    if (password == NULL || challenge == NULL || context == NULL || response == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    uint8_t verifier[32];
    if (crypto_pwhash(
            verifier, sizeof(verifier), password, strlen(password), challenge->salt,
            crypto_pwhash_OPSLIMIT_MODERATE,
            crypto_pwhash_MEMLIMIT_MODERATE,
            crypto_pwhash_ALG_ARGON2ID13
        ) != 0) {
        set_error(error, GRD_OUT_OF_MEMORY, "Insufficient memory for Argon2id");
        return GRD_OUT_OF_MEMORY;
    }
    memset(context, 0, sizeof(*context));
    memcpy(context->nonce, challenge->nonce, sizeof(context->nonce));
    (void)crypto_kx_keypair(context->public_key, context->secret_key);
    memcpy(response->client_public_key, context->public_key, sizeof(response->client_public_key));

    uint8_t transcript[GRD_AUTH_NONCE_BYTES + 2U * GRD_AUTH_PUBLIC_KEY_BYTES];
    auth_transcript(challenge, response->client_public_key, transcript);
    crypto_auth_hmacsha256(response->proof, transcript, sizeof(transcript), verifier);

    uint8_t shared[crypto_scalarmult_BYTES];
    if (crypto_scalarmult(
            shared, context->secret_key, challenge->server_public_key
        ) != 0) {
        grd_secure_zero(verifier, sizeof(verifier));
        set_error(error, GRD_AUTH_FAILED, "Invalid server key");
        return GRD_AUTH_FAILED;
    }
    derive_session_key(shared, verifier, challenge->nonce, context->session_key);
    grd_secure_zero(shared, sizeof(shared));
    grd_secure_zero(verifier, sizeof(verifier));
    return GRD_OK;
}

grd_status grd_auth_server_finish(
    const grd_config *config,
    grd_auth_context *context,
    const grd_auth_challenge *challenge,
    const grd_auth_response *response,
    grd_error *error
)
{
    if (config == NULL || context == NULL || challenge == NULL || response == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    uint8_t transcript[GRD_AUTH_NONCE_BYTES + 2U * GRD_AUTH_PUBLIC_KEY_BYTES];
    uint8_t expected[GRD_AUTH_PROOF_BYTES];
    auth_transcript(challenge, response->client_public_key, transcript);
    crypto_auth_hmacsha256(
        expected, transcript, sizeof(transcript), config->password_verifier
    );
    if (sodium_memcmp(expected, response->proof, sizeof(expected)) != 0) {
        grd_secure_zero(expected, sizeof(expected));
        set_error(error, GRD_AUTH_FAILED, "Incorrect password");
        return GRD_AUTH_FAILED;
    }
    uint8_t shared[crypto_scalarmult_BYTES];
    if (crypto_scalarmult(
            shared, context->secret_key, response->client_public_key
        ) != 0) {
        set_error(error, GRD_AUTH_FAILED, "Invalid client key");
        return GRD_AUTH_FAILED;
    }
    derive_session_key(
        shared, config->password_verifier, challenge->nonce, context->session_key
    );
    grd_secure_zero(shared, sizeof(shared));
    grd_secure_zero(expected, sizeof(expected));
    return GRD_OK;
}

void grd_auth_context_clear(grd_auth_context *context)
{
    if (context != NULL) {
        grd_secure_zero(context, sizeof(*context));
    }
}
