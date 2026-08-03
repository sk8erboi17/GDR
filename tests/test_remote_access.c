#include "test.h"

#include "grd/remote_access.h"

#include <string.h>

void test_remote_access(void)
{
    GRD_ASSERT(grd_remote_access_username_valid("giuseppe"));
    GRD_ASSERT(grd_remote_access_username_valid("user.name-2"));
    GRD_ASSERT(grd_remote_access_username_valid("_service"));
    GRD_ASSERT(!grd_remote_access_username_valid(""));
    GRD_ASSERT(!grd_remote_access_username_valid("-oProxyCommand=evil"));
    GRD_ASSERT(!grd_remote_access_username_valid("user@host"));
    GRD_ASSERT(!grd_remote_access_username_valid("user name"));

    GRD_ASSERT(grd_remote_access_address_valid("192.168.1.20"));
    GRD_ASSERT(grd_remote_access_address_valid("host.local"));
    GRD_ASSERT(grd_remote_access_address_valid("fe80::1%en0"));
    GRD_ASSERT(!grd_remote_access_address_valid(""));
    GRD_ASSERT(!grd_remote_access_address_valid("-oProxyCommand=evil"));
    GRD_ASSERT(!grd_remote_access_address_valid("host;touch_bad"));
    GRD_ASSERT(!grd_remote_access_address_valid("host/path"));

    char target[192];
    grd_error error = {0};
    GRD_ASSERT(
        grd_remote_access_target(
            "giuseppe",
            "192.168.1.20",
            target,
            sizeof(target),
            &error
        ) == GRD_OK
    );
    GRD_ASSERT(strcmp(target, "giuseppe@192.168.1.20") == 0);
    GRD_ASSERT(
        grd_remote_access_target(
            "user@bad",
            "192.168.1.20",
            target,
            sizeof(target),
            &error
        ) == GRD_INVALID_ARGUMENT
    );
}
