#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

// Worker-side cooked data inspection. These callbacks use the running engine's
// registered codecs and keys; neither keys nor game addresses are stored on disk.
struct Ue5ContainerCodecs {
    std::function<bool(const std::array<std::uint8_t, 16>&, std::array<std::uint8_t, 32>&)> key;
    std::function<bool(std::string_view, std::span<const std::uint8_t>, std::span<std::uint8_t>)> decompress;
};

struct Ue5ModCompatibilityPlan {
    // Empty for a compatible container. Otherwise this is a generated container
    // containing the current game's incompatible Blueprint dependency closure.
    std::filesystem::path overlay;
    std::vector<std::string> retained_packages;
};

class Ue5ModCompatibility final {
public:
    Ue5ModCompatibility();
    ~Ue5ModCompatibility();
    Ue5ModCompatibility(const Ue5ModCompatibility&) = delete;
    Ue5ModCompatibility& operator=(const Ue5ModCompatibility&) = delete;

    // Official containers must be in engine resolution order, highest first.
    // Inspection never loads a UObject from the candidate. Unsafe or unreadable
    // dependency closures fail before either container becomes visible to UE.
    Ue5ModCompatibilityPlan Prepare(const std::filesystem::path& candidate,
        const std::vector<std::filesystem::path>& official_containers,
        const std::filesystem::path& output_directory, const Ue5ContainerCodecs& codecs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace anomaly
