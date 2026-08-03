#include "test.h"

#include <sodium.h>
#include <stdio.h>

int grd_test_failures = 0;

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "libsodium initialization failed\n");
        return 1;
    }
    fprintf(stderr, "[test] protocol\n");
    test_protocol();
    fprintf(stderr, "[test] authentication\n");
    test_authentication();
    fprintf(stderr, "[test] audio Opus\n");
    test_audio();
    fprintf(stderr, "[test] codec\n");
    test_codec();
    fprintf(stderr, "[test] stream policy\n");
    test_stream_policy();
    fprintf(stderr, "[test] transport\n");
    test_transport();
    fprintf(stderr, "[test] Unix SSH/SFTP access\n");
    test_remote_access();
    if (grd_test_failures != 0) {
        fprintf(stderr, "%d tests failed\n", grd_test_failures);
        return 1;
    }
    puts("All GRD tests passed");
    return 0;
}
