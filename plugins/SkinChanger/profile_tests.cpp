#include "profile_test_setup.hpp"
#include <chrono>
#include <fstream>
#include <iostream>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Package {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("lite-profile-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Package() { std::filesystem::create_directories(path / "profile"); }
    ~Package() {
        std::error_code error;
        std::filesystem::remove(path / "profile" / "compatibility.profile.json", error);
        std::filesystem::remove(path / "profile", error);
        std::filesystem::remove(path, error);
    }
    void Write(const std::string& text) const {
        std::ofstream(path / "profile" / "compatibility.profile.json", std::ios::binary) << text;
    }
};
}
int main() {
    try {
        anomaly::testing::ProfileHost host("SkinChanger");
        SetTestPackage(host);
        const auto original_package = host.package;
        const auto original_image = host.image;
        const auto load = [&] {
            const bool loaded = accessory::profile::LoadCompatibility(&host.api);
            Check(host.OutstandingHandles() == 0, "Profile load leaked JSON handles");
            return loaded;
        };
        Check(load() && skin_awakening_profile::LoadLayout(), "old candidate did not load through production Profile service");
        Check(accessory::profile::native_layout.DisplayCharacter == 10724 &&
            skin_awakening_profile::native_layout.PlayerStateInventory == 9656,
            "old candidate lost its independently recorded SDK offsets");
        host.image[28] = 0x48; host.image[29] = 0x2B;
        Check(load() && skin_awakening_profile::LoadLayout(), "new candidate did not load");
        Check(accessory::profile::native_layout.DisplayCharacter == 10956 &&
            accessory::profile::native_layout.GliderActor == 15560 &&
            skin_awakening_profile::native_layout.PlayerStateInventory == 10080 &&
            skin_awakening_profile::native_layout.ActorFullAwaken == 11351,
            "new SDK fields were not selected together");
        Check(accessory::profile::compatibility.Address("GliderSpawn") == 0,
            "an unmatched optional symbol published a usable address");

        host.image = original_image;
        host.image.insert(host.image.end(), original_image.begin(), original_image.end());
        Check(!load() && !accessory::profile::native_layout.DisplayCharacter &&
            !accessory::profile::compatibility.Address("layout"), "ambiguous image reused the previous layout");
        host.image.assign(original_image.size(), 0xCC);
        Check(!load(), "unknown native image activated a layout");
        host.image = original_image;

        nlohmann::json source;
        std::ifstream(original_package / "profile" / "compatibility.profile.json") >> source;
        Package package;
        host.package = package.path;
        const auto reject = [&](const nlohmann::json& value, const char* message) {
            package.Write(value.dump());
            Check(!load() && !accessory::profile::compatibility.Address("layout"), message);
        };
        auto modified = source;
        modified["layout"].erase("MapEntryStride");
        reject(modified, "missing required field fell back to an offset in code");
        modified = source; modified["layout"]["ProcessEventSlot"] = 1;
        reject(modified, "unaligned vtable slot was accepted");
        modified = source; modified["layout"]["awakening.AwakenDataStride"] = 16;
        package.Write(modified.dump());
        Check(load() && !skin_awakening_profile::LoadLayout() &&
            !skin_awakening_profile::native_layout.AwakenDataStride,
            "unsupported awakening representation retained a usable snapshot");
        modified = source; modified["contract"] = "unsupported-contract";
        reject(modified, "unsupported semantic contract was accepted");
        modified = source; modified["contract"] = 42;
        reject(modified, "invalid schema bypassed production validation");
        modified = source;
        for (auto& symbol : modified["symbols"]) if (symbol["name"] == "layout")
            for (auto& candidate : symbol["candidates"]) candidate["entry"] = "CC" + candidate["entry"].get<std::string>().substr(2);
        reject(modified, "matching pattern with wrong entry bytes was accepted");
        modified = source;
        auto& symbols = modified["symbols"];
        symbols.erase(std::remove_if(symbols.begin(), symbols.end(), [](const auto& symbol) {
            return symbol["name"] == "layout";
        }), symbols.end());
        reject(modified, "missing layout symbol activated common offsets");
        package.Write("{broken");
        Check(!load(), "malformed JSON activated a fallback");
        host.package = package.path / "absent";
        Check(!load(), "missing package activated a fallback");

        host.package = original_package;
        Check(load(), "valid package did not recover after rejected loads");
        host.image.resize(34);
        Check(anomaly::sdk::Succeeded(anomaly::testing::RecordedProcessEventEntry({host.image.data(),host.image.size()})),
            "recorded ProcessEvent entry did not fit");
        Check(accessory::profile::compatibility.MatchesEntry(&host.core,"ProcessEvent.entry",0x10000),
            "controlled ProcessEvent entry failed validation");
        host.image[0] = 0xE9;
        Check(!accessory::profile::compatibility.MatchesEntry(&host.core,"ProcessEvent.entry",0x10000) &&
            accessory::profile::compatibility.MatchesEntry(&host.core,"ProcessEvent.entry",0x10000,true),
            "ProcessEvent observer detour ignored its explicit contract");
        host.image[7] ^= 1;
        Check(!accessory::profile::compatibility.MatchesEntry(&host.core,"ProcessEvent.entry",0x10000,true),
            "corrupt ProcessEvent tail was accepted");
        std::cout << "PASS production Profile parsing, old/new layouts, mismatch rejection and handle cleanup\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
