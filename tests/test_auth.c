#include "test.h"

#include "grd/auth.h"

#include <sodium.h>

void test_authentication(void)
{
    static const char password[] = "very-strong-test-password";
    grd_config config;
    grd_error error = {0};
    grd_config_defaults(&config);
    GRD_ASSERT(config.target_fps == 60U);
    GRD_ASSERT(config.client_target_fps == 120U);
    GRD_ASSERT(config.presentation_hz == 0U);
    GRD_ASSERT(config.client_upscale_mode == GRD_CLIENT_UPSCALE_NATIVE);
    GRD_ASSERT(config.client_frame_pacing);
    GRD_ASSERT(config.client_cursor_prediction);
    GRD_ASSERT(config.client_max_height == 1440U);
    GRD_ASSERT(config.show_advanced_stats);
    GRD_ASSERT(config.sharp_video_scaling);
    GRD_ASSERT(config.mouse_mode == GRD_MOUSE_AUTOMATIC);
    GRD_ASSERT(!config.ssh_remote_access_enabled);
    GRD_ASSERT(config.ssh_remote_access_port == 22U);
    GRD_ASSERT(grd_config_set_password(&config, password, &error) == GRD_OK);

    grd_auth_context server;
    grd_auth_context client;
    grd_auth_challenge challenge;
    grd_auth_response response;
    GRD_ASSERT(grd_auth_server_begin(
                   &config, &server, &challenge, &error
               ) == GRD_OK);
    GRD_ASSERT(grd_auth_client_respond(
                   password, &challenge, &client, &response, &error
               ) == GRD_OK);
    GRD_ASSERT(grd_auth_server_finish(
                   &config, &server, &challenge, &response, &error
               ) == GRD_OK);
    GRD_ASSERT(sodium_memcmp(
                   server.session_key,
                   client.session_key,
                   GRD_SESSION_KEY_BYTES
               ) == 0);
    grd_auth_context_clear(&server);
    grd_auth_context_clear(&client);

    GRD_ASSERT(grd_auth_server_begin(
                   &config, &server, &challenge, &error
               ) == GRD_OK);
    GRD_ASSERT(grd_auth_client_respond(
                   "wrong-password-123",
                   &challenge,
                   &client,
                   &response,
                   &error
               ) == GRD_OK);
    GRD_ASSERT(grd_auth_server_finish(
                   &config, &server, &challenge, &response, &error
               ) == GRD_AUTH_FAILED);
    grd_auth_context_clear(&server);
    grd_auth_context_clear(&client);
}
