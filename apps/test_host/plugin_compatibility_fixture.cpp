#include "plugin_profile_fixture_host.hpp"
#include "../../plugins/SkinChanger/profile.hpp"
#include "../../plugins/FakeUID/fake_uid_profile.hpp"
#include "../../plugins/BetterPose/better_pose_profile.hpp"
#include <iostream>
#include <fstream>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}
int main() {
    try {
        anomaly::testing::ProfileHost host("SkinChanger");
        std::size_t count{};
        for (const auto& plugin : std::filesystem::directory_iterator(host.package.parent_path())) {
            const auto profiles = plugin.path() / "profile";
            if (!std::filesystem::is_directory(profiles)) continue;
            for (const auto& file : std::filesystem::directory_iterator(profiles)) {
                if (file.path().extension() != ".json") continue;
                host.package = plugin.path();
                const auto loaded = anomaly::sdk::LoadProfile(anomaly::sdk::Host(&host.api), file.path().filename().string());
                if (!loaded) throw std::runtime_error(file.path().string() + ": " + loaded.error() + ": " + host.error);
                Require(host.OutstandingHandles() == 0, "SDK must release all Profile JSON handles");
                ++count;
            }
        }
        host.package = host.package.parent_path() / "SkinChanger";
        Require(accessory::profile::LoadCompatibility(&host.api), "1.4 skin layout must load");
        Require(accessory::profile::native_layout.GliderActor == 15168 &&
                accessory::profile::native_layout.DisplayCharacter == 10724, "1.4 SDK layout mismatch");
        host.image[28] = 0x48; host.image[29] = 0x2B;
        Require(accessory::profile::LoadCompatibility(&host.api), "1.5 skin layout must load");
        Require(accessory::profile::native_layout.GliderActor == 15560 &&
                accessory::profile::native_layout.DisplayCharacter == 10956, "1.5 SDK layout mismatch");
        host.image[0] = 0xCC;
        Require(!accessory::profile::LoadCompatibility(&host.api), "unknown image must not publish a fallback layout");
        Require(accessory::profile::native_layout.DisplayCharacter == 0 &&
                accessory::profile::compatibility.Address("layout") == 0, "failed load must not expose the previous layout or addresses");
        host.package = host.package.parent_path() / "FakeUID";
        Require(fake_uid_profile::LoadCompatibility(&host.api), "shared widget layout must load");
        Require(fake_uid_profile::native_layout.kWidgetSlotOffset == 48 &&
                fake_uid_profile::native_layout.kObjectItemStride == 24, "independent widget and registry ABI");
        host.package = host.package.parent_path() / "BetterPose";
        Require(better_pose_profile::LoadCompatibility(&host.api), "pose layout must load");
        Require(better_pose_profile::native_layout.kCharacterMeshOffset == 840 &&
                better_pose_profile::native_layout.kKawaiiCapsuleLimitSize == 176, "independent character and capsule SDK ABI");
        host.image.resize(34);
        Require(anomaly::sdk::Succeeded(anomaly::testing::RecordedProcessEventEntry({host.image.data(), host.image.size()})), "recorded entry must fit");
        Require(better_pose_profile::compatibility.MatchesEntry(&host.core, "ProcessEvent.entry", 0x10000), "recorded ProcessEvent must pass");
        host.image[0] = 0xE9;
        Require(!better_pose_profile::compatibility.MatchesEntry(&host.core, "ProcessEvent.entry", 0x10000), "detour requires an explicit observer contract");
        Require(better_pose_profile::compatibility.MatchesEntry(&host.core, "ProcessEvent.entry", 0x10000, true), "observer detour with intact tail must pass");
        host.image[7] ^= 1;
        Require(!better_pose_profile::compatibility.MatchesEntry(&host.core, "ProcessEvent.entry", 0x10000, true), "wrong entry tail must fail even for an observer");
        Require(host.OutstandingHandles() == 0, "load failures must release handles too");
        std::cout << "PASS " << count << " production profiles, SDK parsing, two native layouts, no fallback, handle cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
