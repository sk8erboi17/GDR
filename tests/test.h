#ifndef GRD_TEST_H
#define GRD_TEST_H

#include <stdio.h>

extern int grd_test_failures;

#define GRD_ASSERT(condition)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: assertion failed: %s\n",              \
                    __FILE__, __LINE__, #condition);                         \
            ++grd_test_failures;                                             \
            return;                                                          \
        }                                                                    \
    } while (0)

void test_protocol(void);
void test_authentication(void);
void test_audio(void);
void test_codec(void);
void test_stream_policy(void);
void test_transport(void);
void test_remote_access(void);

#endif
