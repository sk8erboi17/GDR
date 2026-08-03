#ifndef GRD_NET_INTERNAL_H
#define GRD_NET_INTERNAL_H

#include "grd/common.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET grd_socket;
typedef HANDLE grd_thread;
typedef CRITICAL_SECTION grd_mutex;
typedef CONDITION_VARIABLE grd_cond;
typedef int grd_socklen;
#define GRD_INVALID_SOCKET INVALID_SOCKET
#define grd_socket_close closesocket
static inline void grd_mutex_init(grd_mutex *mutex) { InitializeCriticalSection(mutex); }
static inline void grd_mutex_destroy(grd_mutex *mutex) { DeleteCriticalSection(mutex); }
static inline void grd_mutex_lock(grd_mutex *mutex) { EnterCriticalSection(mutex); }
static inline void grd_mutex_unlock(grd_mutex *mutex) { LeaveCriticalSection(mutex); }
static inline void grd_cond_init(grd_cond *condition) { InitializeConditionVariable(condition); }
static inline void grd_cond_destroy(grd_cond *condition) { (void)condition; }
static inline void grd_cond_signal(grd_cond *condition) { WakeConditionVariable(condition); }
static inline void grd_cond_broadcast(grd_cond *condition) { WakeAllConditionVariable(condition); }
static inline void grd_cond_wait(grd_cond *condition, grd_mutex *mutex)
{
    (void)SleepConditionVariableCS(condition, mutex, INFINITE);
}
static inline bool grd_net_initialize(void)
{
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
static inline void grd_net_shutdown(void) { WSACleanup(); }
static inline bool grd_socket_set_nonblocking(grd_socket socket_value)
{
    u_long mode = 1U;
    return ioctlsocket(socket_value, FIONBIO, &mode) == 0;
}

static inline bool grd_socket_would_block(void)
{
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

static inline void grd_sleep_millis(unsigned milliseconds)
{
    Sleep(milliseconds);
}

/* Sub-millisecond sleep for the UDP pacer. Below 1 ms Windows sleeps are
 * unreliable, so a short busy spin (with yields) refines the deadline. */
static inline void grd_sleep_micros(unsigned microseconds)
{
#if defined(_WIN32)
    if (microseconds >= 1000U) {
        Sleep(microseconds / 1000U);
        return;
    }
    if (microseconds == 0U) {
        return;
    }
    Sleep(0);
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    (void)QueryPerformanceFrequency(&frequency);
    (void)QueryPerformanceCounter(&start);
    for (unsigned spin = 0U;; ++spin) {
        LARGE_INTEGER now;
        (void)QueryPerformanceCounter(&now);
        const double elapsed_us =
            (double)(now.QuadPart - start.QuadPart) * 1000000.0 /
            (double)frequency.QuadPart;
        if (elapsed_us >= (double)microseconds) {
            break;
        }
        if ((spin & 15U) == 0U) {
            Sleep(0);
        }
    }
#else
    struct timespec interval = {
        .tv_sec = microseconds / 1000000U,
        .tv_nsec = (long)(microseconds % 1000000U) * 1000L
    };
    (void)nanosleep(&interval, NULL);
#endif
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
typedef int grd_socket;
typedef pthread_t grd_thread;
typedef pthread_mutex_t grd_mutex;
typedef pthread_cond_t grd_cond;
typedef socklen_t grd_socklen;
#define GRD_INVALID_SOCKET (-1)
#define grd_socket_close close
static inline void grd_mutex_init(grd_mutex *mutex) { (void)pthread_mutex_init(mutex, NULL); }
static inline void grd_mutex_destroy(grd_mutex *mutex) { (void)pthread_mutex_destroy(mutex); }
static inline void grd_mutex_lock(grd_mutex *mutex) { (void)pthread_mutex_lock(mutex); }
static inline void grd_mutex_unlock(grd_mutex *mutex) { (void)pthread_mutex_unlock(mutex); }
static inline void grd_cond_init(grd_cond *condition) { (void)pthread_cond_init(condition, NULL); }
static inline void grd_cond_destroy(grd_cond *condition) { (void)pthread_cond_destroy(condition); }
static inline void grd_cond_signal(grd_cond *condition) { (void)pthread_cond_signal(condition); }
static inline void grd_cond_broadcast(grd_cond *condition) { (void)pthread_cond_broadcast(condition); }
static inline void grd_cond_wait(grd_cond *condition, grd_mutex *mutex)
{
    (void)pthread_cond_wait(condition, mutex);
}
static inline bool grd_net_initialize(void) { return true; }
static inline void grd_net_shutdown(void) {}
static inline bool grd_socket_set_nonblocking(grd_socket socket_value)
{
    const int flags = fcntl(socket_value, F_GETFL, 0);
    return flags >= 0 && fcntl(socket_value, F_SETFL, flags | O_NONBLOCK) == 0;
}

static inline bool grd_socket_would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static inline void grd_sleep_millis(unsigned milliseconds)
{
    struct timespec interval = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L
    };
    (void)nanosleep(&interval, NULL);
}

static inline void grd_sleep_micros(unsigned microseconds)
{
    struct timespec interval = {
        .tv_sec = microseconds / 1000000U,
        .tv_nsec = (long)(microseconds % 1000000U) * 1000L
    };
    (void)nanosleep(&interval, NULL);
}
#endif

static inline void grd_thread_join(grd_thread thread)
{
#if defined(_WIN32)
    (void)WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    (void)pthread_join(thread, NULL);
#endif
}

static inline void grd_socket_shutdown(grd_socket socket_value)
{
#if defined(_WIN32)
    (void)shutdown(socket_value, SD_BOTH);
#else
    (void)shutdown(socket_value, SHUT_RDWR);
#endif
}

static inline bool grd_socket_set_timeout(grd_socket socket_value, unsigned milliseconds)
{
#if defined(_WIN32)
    const DWORD timeout = milliseconds;
    return setsockopt(
               socket_value, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&timeout, sizeof(timeout)
           ) == 0 &&
           setsockopt(
               socket_value, SOL_SOCKET, SO_SNDTIMEO,
               (const char *)&timeout, sizeof(timeout)
           ) == 0;
#else
    const struct timeval timeout = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_usec = (suseconds_t)((milliseconds % 1000U) * 1000U)
    };
    return setsockopt(
               socket_value, SOL_SOCKET, SO_RCVTIMEO,
               &timeout, sizeof(timeout)
           ) == 0 &&
           setsockopt(
               socket_value, SOL_SOCKET, SO_SNDTIMEO,
               &timeout, sizeof(timeout)
           ) == 0;
#endif
}

/* Mouse packets are tiny and latency-sensitive. Disable Nagle coalescing so
 * a relative motion is delivered immediately instead of waiting for a later
 * video/audio packet. */
static inline bool grd_socket_set_low_latency(grd_socket socket_value)
{
    const int enabled = 1;
#if defined(_WIN32)
    return setsockopt(
               socket_value, IPPROTO_TCP, TCP_NODELAY,
               (const char *)&enabled, sizeof(enabled)
           ) == 0;
#else
    return setsockopt(
               socket_value, IPPROTO_TCP, TCP_NODELAY,
               &enabled, sizeof(enabled)
           ) == 0;
#endif
}

#endif
