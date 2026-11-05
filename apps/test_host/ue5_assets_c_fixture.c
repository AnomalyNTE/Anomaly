#include <anomaly/sdk/services/ue5.h>

_Static_assert(sizeof(AnomalyUe5AssetsServiceV1) == 48, "asset service ABI changed");
_Static_assert(offsetof(AnomalyUe5AssetsServiceV1, prepare_class) == 16, "asset preparation ABI changed");
_Static_assert(offsetof(AnomalyUe5AssetsServiceV1, enumerate_string_table) == 40, "string table ABI changed");
int AssetsCContract(void) {
    AnomalyUe5AssetsServiceV1 service = {0};
    service.struct_size = sizeof(service);
    service.service_version = ANOMALY_UE5_ASSETS_SERVICE_V1_VERSION;
    return service.struct_size == 48 && service.service_version == 1;
}
