#include "anomaly/sdk/anomaly_sdk.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

_Static_assert(ANOMALY_PLUGIN_API_V1_MAJOR == 1u, "v1 API major");
_Static_assert(sizeof(AnomalyStringViewV1) == 16, "string view ABI");
_Static_assert(sizeof(AnomalyStatusV1) == 24, "status ABI");
_Static_assert(offsetof(AnomalyHostApiV1, allocator) == 16, "allocator offset");
_Static_assert(sizeof(AnomalyAllocatorV1) == 40, "allocator ABI");
_Static_assert(offsetof(AnomalyPluginDescriptorV1, on_load) == 72, "lifecycle offset");
_Static_assert(sizeof(AnomalyGenerationHandleV1) == 16, "generation handle ABI");
_Static_assert(sizeof(AnomalyUe5WorldSnapshotV1) == 40, "world snapshot ABI");
_Static_assert(sizeof(AnomalyNtePlayerSnapshotV1) == 56, "player snapshot ABI");

int main(void) {
    AnomalyPluginDescriptorV1 descriptor = {0};
    descriptor.struct_size = (uint32_t)sizeof(descriptor);
    descriptor.api_major = ANOMALY_PLUGIN_API_V1_MAJOR;
    descriptor.api_minor = ANOMALY_PLUGIN_API_V1_MINOR;
    if (descriptor.struct_size < 120u) return 1;
    puts("C plugin ABI v1 contract passed");
    return 0;
}
