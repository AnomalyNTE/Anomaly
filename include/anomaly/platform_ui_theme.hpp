#pragma once

#include <filesystem>

namespace ue5mem {

// Requires an active ImGui context.
void ApplyPlatformUiStyle() noexcept;
[[nodiscard]] bool ConfigurePlatformUiFontAtlas(
    const std::filesystem::path& runtime_root = {}) noexcept;
[[nodiscard]] bool ApplyPlatformUiFontScale(float scale) noexcept;

}  // namespace ue5mem
