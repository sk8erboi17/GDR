#include "grd/gpu.h"

#import <Metal/Metal.h>

#include <stdio.h>
#include <string.h>

bool grd_metal_available(char *name, size_t name_capacity)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return false;
        }
        if (name != NULL && name_capacity != 0U) {
            const char *utf8 = [[device name] UTF8String];
            (void)snprintf(name, name_capacity, "%s", utf8 != NULL ? utf8 : "Metal");
        }
        return true;
    }
}

