#pragma once

#if defined(ANOMALY_USE_CONFIGURED_VERSION)
#include <anomaly/sdk/configured_version.h>
#else
#define ANOMALY_SDK_VERSION_MAJOR 1u
#define ANOMALY_SDK_VERSION_MINOR 0u
#define ANOMALY_SDK_VERSION_PATCH 0u
#define ANOMALY_SDK_VERSION_STRING "1.0.0"

#define ANOMALY_PLUGIN_API_V1_MAJOR 1u
#define ANOMALY_PLUGIN_API_V1_MINOR 0u
#define ANOMALY_PLUGIN_V1_ENTRY_NAME "AnomalyPluginEntryV1"
#endif
