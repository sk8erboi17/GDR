#include "platform/remote_access_internal.h"
#include "grd/log.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>

static void set_error(grd_error *error, grd_status code, const char *message)
{
    if (error == NULL) {
        return;
    }
    error->code = code;
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

static bool executable(const char *path)
{
    return path != NULL && path[0] != '\0' && access(path, X_OK) == 0;
}

static bool find_in_path(
    const char *name,
    char *output,
    size_t capacity
)
{
    if (name == NULL || output == NULL || capacity == 0U) {
        return false;
    }
    if (strchr(name, '/') != NULL) {
        if (!executable(name) || strlen(name) >= capacity) {
            return false;
        }
        (void)snprintf(output, capacity, "%s", name);
        return true;
    }
    const char *path = getenv("PATH");
    if (path == NULL) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    const char *cursor = path;
    while (*cursor != '\0') {
        const char *separator = strchr(cursor, ':');
        const size_t directory_length = separator != NULL
                                            ? (size_t)(separator - cursor)
                                            : strlen(cursor);
        const int written = directory_length == 0U
                                ? snprintf(output, capacity, "./%s", name)
                                : snprintf(
                                      output,
                                      capacity,
                                      "%.*s/%s",
                                      (int)directory_length,
                                      cursor,
                                      name
                                  );
        if (written > 0 && (size_t)written < capacity && executable(output)) {
            return true;
        }
        if (separator == NULL) {
            break;
        }
        cursor = separator + 1;
    }
    output[0] = '\0';
    return false;
}

static bool openssh_path(
    grd_remote_access_kind kind,
    char *output,
    size_t capacity
)
{
    const char *name = kind == GRD_REMOTE_ACCESS_SFTP ? "sftp" : "ssh";
#if defined(__APPLE__)
    char system_path[32];
    (void)snprintf(system_path, sizeof(system_path), "/usr/bin/%s", name);
    if (executable(system_path)) {
        (void)snprintf(output, capacity, "%s", system_path);
        return true;
    }
#endif
    return find_in_path(name, output, capacity);
}

#if !defined(__APPLE__)
typedef enum terminal_argument_style {
    TERMINAL_ARGUMENT_DASH_E,
    TERMINAL_ARGUMENT_SEPARATOR
} terminal_argument_style;

static bool terminal_path(
    char *output,
    size_t capacity,
    terminal_argument_style *style
)
{
    const char *configured = getenv("TERMINAL");
    if (configured != NULL && configured[0] == '/' &&
        strchr(configured, ' ') == NULL && executable(configured)) {
        (void)snprintf(output, capacity, "%s", configured);
        *style = TERMINAL_ARGUMENT_DASH_E;
        return true;
    }
    static const struct {
        const char *name;
        terminal_argument_style style;
    } candidates[] = {
        {"x-terminal-emulator", TERMINAL_ARGUMENT_DASH_E},
        {"gnome-terminal", TERMINAL_ARGUMENT_SEPARATOR},
        {"konsole", TERMINAL_ARGUMENT_DASH_E},
        {"kitty", TERMINAL_ARGUMENT_SEPARATOR},
        {"alacritty", TERMINAL_ARGUMENT_DASH_E},
        {"xterm", TERMINAL_ARGUMENT_DASH_E}
    };
    for (size_t index = 0U;
         index < sizeof(candidates) / sizeof(candidates[0]);
         ++index) {
        if (find_in_path(candidates[index].name, output, capacity)) {
            *style = candidates[index].style;
            return true;
        }
    }
    return false;
}
#endif

static bool spawn_detached(const char *path, char *const arguments[])
{
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        const pid_t grandchild = fork();
        if (grandchild < 0) {
            _exit(126);
        }
        if (grandchild > 0) {
            _exit(0);
        }
        (void)setsid();
        execv(path, arguments);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#if defined(__APPLE__)
static bool shell_quote(FILE *file, const char *value)
{
    if (fputc('\'', file) == EOF) {
        return false;
    }
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor == '\'') {
            if (fputs("'\\''", file) == EOF) {
                return false;
            }
        } else if (fputc((unsigned char)*cursor, file) == EOF) {
            return false;
        }
    }
    return fputc('\'', file) != EOF;
}

static grd_status launch_macos(
    grd_remote_access_kind kind,
    const char *client_path,
    const char *target,
    uint16_t port,
    grd_error *error
)
{
    char script_path[] = "/tmp/grd-openssh-XXXXXX.command";
    const int descriptor = mkstemps(script_path, 8);
    if (descriptor < 0) {
        set_error(error, GRD_IO_ERROR, "Unable to create the OpenSSH launcher");
        return GRD_IO_ERROR;
    }
    if (fchmod(descriptor, S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
        (void)close(descriptor);
        (void)unlink(script_path);
        set_error(error, GRD_IO_ERROR, "Invalid OpenSSH launcher permissions");
        return GRD_IO_ERROR;
    }
    FILE *script = fdopen(descriptor, "w");
    if (script == NULL) {
        (void)close(descriptor);
        (void)unlink(script_path);
        set_error(error, GRD_IO_ERROR, "The OpenSSH launcher is not writable");
        return GRD_IO_ERROR;
    }
    bool written = fputs("#!/bin/sh\nself=$0\nrm -f -- \"$self\"\nexec ", script) != EOF;
    written = written && shell_quote(script, client_path);
    if (kind == GRD_REMOTE_ACCESS_SFTP) {
        written = written && fputs(" -P ", script) != EOF;
    } else {
        written = written && fputs(" -p ", script) != EOF;
    }
    char port_text[8];
    (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    written = written && shell_quote(script, port_text);
    written = written && fputc(' ', script) != EOF;
    written = written && shell_quote(script, target);
    written = written && fputc('\n', script) != EOF;
    if (fclose(script) != 0) {
        written = false;
    }
    if (!written) {
        (void)unlink(script_path);
        set_error(error, GRD_IO_ERROR, "Failed to write the OpenSSH launcher");
        return GRD_IO_ERROR;
    }
    char open_path[32];
    if (!find_in_path("open", open_path, sizeof(open_path))) {
        (void)unlink(script_path);
        set_error(error, GRD_NOT_SUPPORTED, "Terminal.app is unavailable");
        return GRD_NOT_SUPPORTED;
    }
    char *const arguments[] = {
        open_path,
        "-b",
        "com.apple.Terminal",
        script_path,
        NULL
    };
    if (!spawn_detached(open_path, arguments)) {
        (void)unlink(script_path);
        set_error(error, GRD_IO_ERROR, "Failed to start Terminal.app");
        return GRD_IO_ERROR;
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    GRD_INFO(
        "Unix remote access: started %s to %s on port %u",
        kind == GRD_REMOTE_ACCESS_SFTP ? "SFTP" : "SSH",
        target,
        (unsigned)port
    );
    return GRD_OK;
}
#endif

bool grd_platform_remote_access_client_available(grd_remote_access_kind kind)
{
    char client_path[512];
    if (!openssh_path(kind, client_path, sizeof(client_path))) {
        return false;
    }
#if defined(__APPLE__)
    return executable("/usr/bin/open");
#else
    char terminal[512];
    terminal_argument_style style = TERMINAL_ARGUMENT_DASH_E;
    return terminal_path(terminal, sizeof(terminal), &style);
#endif
}

bool grd_platform_remote_access_default_username(
    char *output,
    size_t capacity
)
{
    if (output == NULL || capacity == 0U) {
        return false;
    }
    const struct passwd *entry = getpwuid(getuid());
    if (entry == NULL || entry->pw_name == NULL ||
        strlen(entry->pw_name) >= capacity) {
        output[0] = '\0';
        return false;
    }
    (void)snprintf(output, capacity, "%s", entry->pw_name);
    return true;
}

grd_status grd_platform_remote_access_launch(
    grd_remote_access_kind kind,
    const char *address,
    uint16_t port,
    const char *username,
    grd_error *error
)
{
    char target[GRD_REMOTE_USERNAME_MAX + GRD_MAX_ADDRESS + 2U];
    if (grd_remote_access_target(
            username, address, target, sizeof(target), error
        ) != GRD_OK) {
        return GRD_INVALID_ARGUMENT;
    }
    char client_path[512];
    if (!openssh_path(kind, client_path, sizeof(client_path))) {
        set_error(error, GRD_NOT_SUPPORTED, "OpenSSH client is unavailable");
        return GRD_NOT_SUPPORTED;
    }
#if defined(__APPLE__)
    return launch_macos(kind, client_path, target, port, error);
#else
    char terminal[512];
    terminal_argument_style style = TERMINAL_ARGUMENT_DASH_E;
    if (!terminal_path(terminal, sizeof(terminal), &style)) {
        set_error(error, GRD_NOT_SUPPORTED, "Terminal emulator is unavailable");
        return GRD_NOT_SUPPORTED;
    }
    char port_text[8];
    (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    const char *terminal_separator = style == TERMINAL_ARGUMENT_SEPARATOR
                                         ? "--"
                                         : "-e";
    const char *port_option = kind == GRD_REMOTE_ACCESS_SFTP ? "-P" : "-p";
    char *const arguments[] = {
        terminal,
        (char *)terminal_separator,
        client_path,
        (char *)port_option,
        port_text,
        target,
        NULL
    };
    if (!spawn_detached(terminal, arguments)) {
        set_error(error, GRD_IO_ERROR, "Failed to start the OpenSSH terminal");
        return GRD_IO_ERROR;
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    GRD_INFO(
        "Unix remote access: started %s to %s on port %u",
        kind == GRD_REMOTE_ACCESS_SFTP ? "SFTP" : "SSH",
        target,
        (unsigned)port
    );
    return GRD_OK;
#endif
}

grd_status grd_platform_remote_access_probe_local_ssh(
    uint16_t port,
    uint32_t timeout_millis,
    grd_error *error
)
{
    const int socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_value < 0) {
        set_error(error, GRD_IO_ERROR, "SSH probe socket is unavailable");
        return GRD_IO_ERROR;
    }
    const int original_flags = fcntl(socket_value, F_GETFL, 0);
    if (original_flags < 0 ||
        fcntl(socket_value, F_SETFL, original_flags | O_NONBLOCK) != 0) {
        (void)close(socket_value);
        set_error(error, GRD_IO_ERROR, "Unable to configure the SSH probe");
        return GRD_IO_ERROR;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int result = connect(
        socket_value,
        (const struct sockaddr *)&address,
        sizeof(address)
    );
    if (result != 0 && errno != EINPROGRESS) {
        (void)close(socket_value);
        set_error(error, GRD_NOT_SUPPORTED, "Local OpenSSH server is unreachable");
        return GRD_NOT_SUPPORTED;
    }
    if (result != 0) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_value, &write_set);
        struct timeval timeout = {
            .tv_sec = (time_t)(timeout_millis / 1000U),
            .tv_usec = (suseconds_t)((timeout_millis % 1000U) * 1000U)
        };
        result = select(
            socket_value + 1, NULL, &write_set, NULL, &timeout
        );
        int socket_error = 0;
        socklen_t error_length = (socklen_t)sizeof(socket_error);
        if (result <= 0 ||
            getsockopt(
                socket_value,
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &error_length
            ) != 0 || socket_error != 0) {
            (void)close(socket_value);
            set_error(error, GRD_NOT_SUPPORTED, "Local OpenSSH server is unreachable");
            return GRD_NOT_SUPPORTED;
        }
    }
    (void)fcntl(socket_value, F_SETFL, original_flags);
    struct timeval receive_timeout = {
        .tv_sec = (time_t)(timeout_millis / 1000U),
        .tv_usec = (suseconds_t)((timeout_millis % 1000U) * 1000U)
    };
    (void)setsockopt(
        socket_value,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &receive_timeout,
        sizeof(receive_timeout)
    );
    char banner[256];
    const ssize_t received = recv(socket_value, banner, sizeof(banner) - 1U, 0);
    (void)close(socket_value);
    if (received <= 0) {
        set_error(error, GRD_NOT_SUPPORTED, "The local service did not provide an SSH banner");
        return GRD_NOT_SUPPORTED;
    }
    banner[received] = '\0';
    if (received < 4 || memcmp(banner, "SSH-", 4U) != 0) {
        set_error(error, GRD_NOT_SUPPORTED, "The configured port does not expose OpenSSH");
        return GRD_NOT_SUPPORTED;
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    return GRD_OK;
}
