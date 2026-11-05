#pragma once
// Test-only transport: production package reader/schema, then the public SDK.
#include "plugin_profile_fixture_host.hpp"
#include "profile.hpp"

inline void PrepareProfileHost(anomaly::testing::ProfileHost& host) {
    host.package=std::filesystem::path(__FILE__).parent_path();
    host.image.assign(8192,0);
    // Independently observed game instructions; only RIP displacements are
    // relocated into this controlled test address space.
    constexpr std::uint8_t objects[]{
        0x48,0x8B,0x41,0x08,0x8B,0x08,0x3B,0x0D,0x88,0x22,0xAA,0x0E,0x7D,0x1B,
        0x0F,0xB7,0xC1,0x48,0xC1,0xE9,0x10,0x48,0x8D,0x14,0x40,0x48,0x8B,0x05,
        0x60,0x22,0xAA,0x0E,0x48,0x8B,0x0C,0xC8,0x48,0x8B,0x04,0xD1,0xC3,
        0x33,0xC0,0x48,0x8B,0x00,0xC3,0xCC};
    constexpr std::uint8_t world[]{0x48,0x8B,0x1D,0x45,0x22,0x9C,0x0B,0x48,0x85,0xDB,0x74,0x33,0x41,0xB0,0x01};
    std::memcpy(host.image.data()+32,objects,sizeof(objects));
    std::memcpy(host.image.data()+128,world,sizeof(world));
    const std::int32_t objects_relative=4096+16-(32+25+7),world_relative=6144-(128+7);
    std::memcpy(host.image.data()+32+28,&objects_relative,sizeof(objects_relative));
    std::memcpy(host.image.data()+128+3,&world_relative,sizeof(world_relative));
}
inline void LoadTestProfile() {
    anomaly::testing::ProfileHost host("Autotaxi");
    PrepareProfileHost(host);
    if (!taxi::profile::Load(&host.api) || host.OutstandingHandles())
        throw std::runtime_error("taxi production Profile failed: "+host.error);
}
