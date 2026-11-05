#pragma once

// Test-only ABI transport. Parsing and package validation use Anomaly's
// production Profile service; no host internals enter the plugin DLL.
#include "../../apps/test_host/plugin_profile_fixture_host.hpp"
#include "awakening_profile.hpp"
#include <stdexcept>

inline void SetTestPackage(anomaly::testing::ProfileHost& host) {
    host.package = std::filesystem::path(__FILE__).parent_path();
}

inline void LoadTestProfile() {
    anomaly::testing::ProfileHost host("SkinChanger");
    SetTestPackage(host);
    if (!accessory::profile::LoadCompatibility(&host.api) ||
        !skin_awakening_profile::LoadLayout() || host.OutstandingHandles() != 0)
        throw std::runtime_error("production compatibility profile did not load: " + host.error);
}
