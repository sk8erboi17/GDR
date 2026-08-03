#include "grd/config.h"
#include "grd/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define GRD_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define GRD_MKDIR(path) mkdir(path, 0700)
#endif

static void set_error(grd_error *error, grd_status code, const char *message)
{
    if (error == NULL) {
        return;
    }
    error->code = code;
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

static void uuid_generate_string(char output[37])
{
    uint8_t bytes[16];
    randombytes_buf(bytes, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = (uint8_t)((bytes[8] & 0x3FU) | 0x80U);
    (void)snprintf(
        output,
        37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]
    );
}

bool grd_config_directory(char *output, size_t capacity)
{
#if defined(_WIN32)
    const char *root = getenv("APPDATA");
    if (root == NULL) {
        return false;
    }
    (void)snprintf(output, capacity, "%s\\GRD", root);
#elif defined(__APPLE__)
    const char *root = getenv("HOME");
    if (root == NULL) {
        return false;
    }
    (void)snprintf(output, capacity, "%s/Library/Application Support/GRD", root);
#else
    const char *root = getenv("XDG_CONFIG_HOME");
    if (root != NULL) {
        (void)snprintf(output, capacity, "%s/grd", root);
    } else {
        root = getenv("HOME");
        if (root == NULL) {
            return false;
        }
        (void)snprintf(output, capacity, "%s/.config/grd", root);
    }
#endif
    return true;
}

static bool config_path(char *output, size_t capacity)
{
    char directory[1024];
    if (!grd_config_directory(directory, sizeof(directory))) {
        return false;
    }
#if defined(_WIN32)
    (void)snprintf(output, capacity, "%s\\config.ini", directory);
#else
    (void)snprintf(output, capacity, "%s/config.ini", directory);
#endif
    return true;
}

static void ensure_directory(void)
{
    char directory[1024];
    if (grd_config_directory(directory, sizeof(directory))) {
        if (GRD_MKDIR(directory) != 0 && errno != EEXIST) {
            GRD_WARN("Unable to create %s: %s", directory, strerror(errno));
        }
    }
}

void grd_config_defaults(grd_config *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    uuid_generate_string(config->device_id);

#if defined(_WIN32)
    DWORD size = (DWORD)sizeof(config->device_name);
    if (!GetComputerNameA(config->device_name, &size)) {
        (void)snprintf(config->device_name, sizeof(config->device_name), "GRD Computer");
    }
#else
    if (gethostname(config->device_name, sizeof(config->device_name) - 1U) != 0) {
        (void)snprintf(config->device_name, sizeof(config->device_name), "GRD Computer");
    }
#endif
    config->port = (uint16_t)GRD_DEFAULT_PORT;
    config->target_fps = 60U;
    /* Bitrates are TOTAL NETWORK wire budgets: the pacer spends the full
     * amount on the wire, while the encoder target is derived internally
     * (payload/wire ratio minus audio, FEC and retransmission margins).
     * The stream starts at the initial value, grows towards the ceiling and
     * never drops below the floor. */
    config->initial_bitrate_kbps = 20000U;
    config->target_bitrate_kbps = 24000U;
    config->min_bitrate_kbps = 10000U;
    config->abr_enabled = true;
    config->stream_profile = GRD_STREAM_BALANCED;
    config->video_codec = GRD_CODEC_H264;
    config->pixel_444 = false;
    config->gpu_preference = GRD_GPU_AUTOMATIC;
    config->client_target_fps = 120U;
    config->presentation_hz = 0U;
    config->client_max_height = 1440U;
    config->client_upscale_mode = GRD_CLIENT_UPSCALE_NATIVE;
    config->client_frame_pacing = true;
    config->client_cursor_prediction = true;
    config->show_advanced_stats = true;
    config->sharp_video_scaling = true;
    config->mouse_mode = GRD_MOUSE_AUTOMATIC;
    config->mouse_sensitivity_percent = 100U;
    config->remote_fullscreen = true;
    config->ssh_remote_access_enabled = false;
    config->ssh_remote_access_port = 22U;
}

static void parse_line(grd_config *config, char *line)
{
    char *separator = strchr(line, '=');
    if (separator == NULL) {
        return;
    }
    *separator = '\0';
    const char *key = line;
    char *value = separator + 1;
    value[strcspn(value, "\r\n")] = '\0';

    if (strcmp(key, "device_id") == 0) {
        (void)snprintf(config->device_id, sizeof(config->device_id), "%s", value);
    } else if (strcmp(key, "device_name") == 0) {
        (void)snprintf(config->device_name, sizeof(config->device_name), "%s", value);
    } else if (strcmp(key, "port") == 0) {
        config->port = (uint16_t)strtoul(value, NULL, 10);
    } else if (strcmp(key, "host_enabled") == 0) {
        config->host_enabled = strtoul(value, NULL, 10) != 0U;
    } else if (strcmp(key, "target_fps") == 0) {
        config->target_fps = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(key, "target_bitrate_kbps") == 0) {
        config->target_bitrate_kbps = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(key, "initial_bitrate_kbps") == 0) {
        config->initial_bitrate_kbps = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(key, "min_bitrate_kbps") == 0) {
        config->min_bitrate_kbps = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(key, "abr_enabled") == 0) {
        config->abr_enabled = strtoul(value, NULL, 10) != 0U;
    } else if (strcmp(key, "stream_profile") == 0) {
        config->stream_profile = (grd_stream_profile)strtoul(value, NULL, 10);
    } else if (strcmp(key, "video_codec") == 0) {
        config->video_codec = (grd_video_codec)strtoul(value, NULL, 10);
    } else if (strcmp(key, "pixel_444") == 0) {
        config->pixel_444 = strtoul(value, NULL, 10) != 0U;
    } else if (strcmp(key, "gpu_preference") == 0) {
        config->gpu_preference = (grd_gpu_preference)strtoul(value, NULL, 10);
    } else if (strcmp(key, "client_target_fps") == 0) {
        const uint32_t fps = (uint32_t)strtoul(value, NULL, 10);
        config->client_target_fps = fps < 30U
                                        ? 30U
                                        : fps > 120U ? 120U : fps;
    } else if (strcmp(key, "presentation_hz") == 0) {
        const uint32_t hz = (uint32_t)strtoul(value, NULL, 10);
        config->presentation_hz = hz == 0U
                                      ? 0U
                                      : hz < 30U
                                            ? 30U
                                            : hz > 120U ? 120U : hz;
    } else if (strcmp(key, "client_max_height") == 0) {
        const uint32_t height = (uint32_t)strtoul(value, NULL, 10);
        config->client_max_height =
            height == 0U || height == 1080U || height == 1440U ||
                    height == 2160U
                ? height
                : 1080U;
    } else if (strcmp(key, "client_upscale_mode") == 0) {
        const unsigned long mode = strtoul(value, NULL, 10);
        config->client_upscale_mode =
            mode <= (unsigned long)GRD_CLIENT_UPSCALE_PERFORMANCE
                ? (grd_client_upscale_mode)mode
                : GRD_CLIENT_UPSCALE_NATIVE;
    } else if (strcmp(key, "client_frame_pacing") == 0) {
        config->client_frame_pacing = strtoul(value, NULL, 10) != 0U;
    } else if (strcmp(key, "client_cursor_prediction") == 0) {
        config->client_cursor_prediction = strtoul(value, NULL, 10) != 0U;
    } else if (strcmp(key, "show_advanced_stats") == 0) {
        config->show_advanced_stats = strtoul(value, NULL, 10) != 0U;
    } else if (strcmp(key, "sharp_video_scaling") == 0) {
        config->sharp_video_scaling = strtoul(value, NULL, 10) != 0U;
    } else if (strcmp(key, "mouse_mode") == 0) {
        const unsigned long mode = strtoul(value, NULL, 10);
        config->mouse_mode = mode <= (unsigned long)GRD_MOUSE_RELATIVE
                                 ? (grd_mouse_mode)mode
                                 : GRD_MOUSE_AUTOMATIC;
    } else if (strcmp(key, "mouse_sensitivity_percent") == 0) {
        const uint32_t sensitivity = (uint32_t)strtoul(value, NULL, 10);
        config->mouse_sensitivity_percent = sensitivity < 25U
                                                ? 25U
                                                : sensitivity > 300U
                                                      ? 300U
                                                      : sensitivity;
    } else if (strcmp(key, "remote_fullscreen") == 0) {
        config->remote_fullscreen = strtoul(value, NULL, 10) != 0U;
    } else if (strcmp(key, "ssh_remote_access_enabled") == 0) {
        config->ssh_remote_access_enabled = strtoul(value, NULL, 10) != 0U;
    } else if (strcmp(key, "ssh_remote_access_port") == 0) {
        const unsigned long port = strtoul(value, NULL, 10);
        config->ssh_remote_access_port =
            port > 0UL && port <= 65535UL ? (uint16_t)port : 22U;
    } else if (strcmp(key, "remote_access_username") == 0) {
        if (grd_remote_access_username_valid(value)) {
            (void)snprintf(
                config->remote_access_username,
                sizeof(config->remote_access_username),
                "%s",
                value
            );
        }
    } else if (strcmp(key, "password_salt") == 0) {
        size_t length = 0U;
        if (sodium_hex2bin(
                config->password_salt, sizeof(config->password_salt),
                value, strlen(value), NULL, &length, NULL
            ) == 0 && length == sizeof(config->password_salt)) {
            config->password_configured = true;
        }
    } else if (strcmp(key, "password_verifier") == 0) {
        size_t length = 0U;
        if (sodium_hex2bin(
                config->password_verifier, sizeof(config->password_verifier),
                value, strlen(value), NULL, &length, NULL
            ) != 0 || length != sizeof(config->password_verifier)) {
            config->password_configured = false;
        }
    }
}

grd_status grd_config_load(grd_config *config, grd_error *error)
{
    if (config == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    grd_config_defaults(config);
    char path[1200];
    if (!config_path(path, sizeof(path))) {
        set_error(error, GRD_IO_ERROR, "Configuration directory is unavailable");
        return GRD_IO_ERROR;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return GRD_OK;
        }
        set_error(error, GRD_IO_ERROR, strerror(errno));
        return GRD_IO_ERROR;
    }
    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL) {
        parse_line(config, line);
    }
    (void)fclose(file);
    /* Migrate only the exact former Gaming preset. Hand-tuned ranges are
     * never touched. The old 16-18 Mbps ceiling is insufficient for
     * 1080p120 foliage and fast camera motion, so ABR had no room to improve
     * quality even on a lossless LAN. */
    if (config->stream_profile == GRD_STREAM_GAMING &&
        config->initial_bitrate_kbps == 16000U &&
        config->target_bitrate_kbps == 18000U &&
        config->min_bitrate_kbps == 12000U) {
        config->initial_bitrate_kbps = 20000U;
        config->target_bitrate_kbps = 30000U;
        config->min_bitrate_kbps = 14000U;
    }
    return GRD_OK;
}

grd_status grd_config_save(const grd_config *config, grd_error *error)
{
    if (config == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    ensure_directory();
    char path[1200];
    char temporary[1220];
    if (!config_path(path, sizeof(path))) {
        set_error(error, GRD_IO_ERROR, "Configuration directory is unavailable");
        return GRD_IO_ERROR;
    }
    (void)snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *file = fopen(temporary, "w");
    if (file == NULL) {
        set_error(error, GRD_IO_ERROR, strerror(errno));
        return GRD_IO_ERROR;
    }
    char salt_hex[sizeof(config->password_salt) * 2U + 1U];
    char verifier_hex[sizeof(config->password_verifier) * 2U + 1U];
    sodium_bin2hex(salt_hex, sizeof(salt_hex), config->password_salt, sizeof(config->password_salt));
    sodium_bin2hex(
        verifier_hex, sizeof(verifier_hex),
        config->password_verifier, sizeof(config->password_verifier)
    );
    (void)fprintf(file, "device_id=%s\n", config->device_id);
    (void)fprintf(file, "device_name=%s\n", config->device_name);
    (void)fprintf(file, "port=%u\n", (unsigned)config->port);
    (void)fprintf(file, "host_enabled=%u\n", config->host_enabled ? 1U : 0U);
    (void)fprintf(file, "target_fps=%u\n", config->target_fps);
    (void)fprintf(file, "initial_bitrate_kbps=%u\n", config->initial_bitrate_kbps);
    (void)fprintf(file, "target_bitrate_kbps=%u\n", config->target_bitrate_kbps);
    (void)fprintf(file, "min_bitrate_kbps=%u\n", config->min_bitrate_kbps);
    (void)fprintf(file, "abr_enabled=%u\n", config->abr_enabled ? 1U : 0U);
    (void)fprintf(file, "stream_profile=%u\n", (unsigned)config->stream_profile);
    (void)fprintf(file, "video_codec=%u\n", (unsigned)config->video_codec);
    (void)fprintf(file, "pixel_444=%u\n", config->pixel_444 ? 1U : 0U);
    (void)fprintf(file, "gpu_preference=%u\n", (unsigned)config->gpu_preference);
    (void)fprintf(file, "client_target_fps=%u\n", config->client_target_fps);
    (void)fprintf(file, "presentation_hz=%u\n", config->presentation_hz);
    (void)fprintf(file, "client_max_height=%u\n", config->client_max_height);
    (void)fprintf(
        file,
        "client_upscale_mode=%u\n",
        (unsigned)config->client_upscale_mode
    );
    (void)fprintf(
        file,
        "client_frame_pacing=%u\n",
        config->client_frame_pacing ? 1U : 0U
    );
    (void)fprintf(
        file,
        "client_cursor_prediction=%u\n",
        config->client_cursor_prediction ? 1U : 0U
    );
    (void)fprintf(
        file,
        "show_advanced_stats=%u\n",
        config->show_advanced_stats ? 1U : 0U
    );
    (void)fprintf(
        file,
        "sharp_video_scaling=%u\n",
        config->sharp_video_scaling ? 1U : 0U
    );
    (void)fprintf(file, "mouse_mode=%u\n", (unsigned)config->mouse_mode);
    (void)fprintf(
        file,
        "mouse_sensitivity_percent=%u\n",
        config->mouse_sensitivity_percent
    );
    (void)fprintf(
        file,
        "remote_fullscreen=%u\n",
        config->remote_fullscreen ? 1U : 0U
    );
    (void)fprintf(
        file,
        "ssh_remote_access_enabled=%u\n",
        config->ssh_remote_access_enabled ? 1U : 0U
    );
    (void)fprintf(
        file,
        "ssh_remote_access_port=%u\n",
        (unsigned)config->ssh_remote_access_port
    );
    if (grd_remote_access_username_valid(config->remote_access_username)) {
        (void)fprintf(
            file,
            "remote_access_username=%s\n",
            config->remote_access_username
        );
    }
    if (config->password_configured) {
        (void)fprintf(file, "password_salt=%s\n", salt_hex);
        (void)fprintf(file, "password_verifier=%s\n", verifier_hex);
    }
    if (fclose(file) != 0) {
        set_error(error, GRD_IO_ERROR, strerror(errno));
        return GRD_IO_ERROR;
    }
#if defined(_WIN32)
    /* rename() on Windows cannot replace an existing destination, which left
     * a stale .tmp file and kept the old settings after every save. */
    if (MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING) == 0) {
        set_error(error, GRD_IO_ERROR, "Failed to save configuration");
        (void)DeleteFileA(temporary);
        return GRD_IO_ERROR;
    }
#else
    if (rename(temporary, path) != 0) {
        set_error(error, GRD_IO_ERROR, strerror(errno));
        return GRD_IO_ERROR;
    }
#endif
#if !defined(_WIN32)
    (void)chmod(path, 0600);
#endif
    return GRD_OK;
}

grd_status grd_config_set_password(
    grd_config *config,
    const char *password,
    grd_error *error
)
{
    if (config == NULL || password == NULL) {
        return GRD_INVALID_ARGUMENT;
    }
    if (strlen(password) < 12U) {
        set_error(error, GRD_INVALID_ARGUMENT, "The password must contain at least 12 characters");
        return GRD_INVALID_ARGUMENT;
    }
    randombytes_buf(config->password_salt, sizeof(config->password_salt));
    if (crypto_pwhash(
            config->password_verifier,
            sizeof(config->password_verifier),
            password,
            strlen(password),
            config->password_salt,
            crypto_pwhash_OPSLIMIT_MODERATE,
            crypto_pwhash_MEMLIMIT_MODERATE,
            crypto_pwhash_ALG_ARGON2ID13
        ) != 0) {
        set_error(error, GRD_OUT_OF_MEMORY, "Insufficient memory for Argon2id");
        return GRD_OUT_OF_MEMORY;
    }
    config->password_configured = true;
    return GRD_OK;
}
