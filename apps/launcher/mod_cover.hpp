#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>

namespace anomaly::launcher {

inline constexpr std::array<const wchar_t*, 5> kModCoverExtensions{
    L".png", L".jpg", L".jpeg", L".webp", L".bmp"};

inline std::wstring LowerModCoverText(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

inline bool IsModCoverImageFile(const std::filesystem::path& file) {
    const std::wstring extension = LowerModCoverText(file.extension().wstring());
    for (const wchar_t* supported : kModCoverExtensions) {
        if (extension == supported) return true;
    }
    return false;
}

inline std::filesystem::path ModCoverRelativePath(
    const std::filesystem::path& mod_root, const std::filesystem::path& file) {
    auto normalized_root = mod_root.lexically_normal();
    if (!normalized_root.empty() && normalized_root.filename().empty()) {
        normalized_root = normalized_root.parent_path();
    }
    return file.lexically_normal().lexically_relative(normalized_root);
}

inline int ModCoverNamePriority(const std::filesystem::path& file) {
    const std::wstring stem = LowerModCoverText(file.stem().wstring());
    if (stem == L"icon") return 0;
    if (stem == L"cover" || stem == L"preview" || stem == L"thumbnail") return 1;
    return 2;
}

inline std::size_t ModCoverDepth(const std::filesystem::path& relative) {
    if (relative.empty()) return 0;
    std::size_t depth = 0;
    for (const auto& part : relative.parent_path()) {
        if (!part.empty() && part != L".") ++depth;
    }
    return depth;
}

// Selects a cover image from any supported image inside a directory mod without
// requiring a specific filename. Shallower images win over nested ones,
// conventional cover names (icon / cover / preview / thumbnail) still take
// precedence within the same depth, and relative path order breaks ties so
// directory iteration order never changes the result.
inline bool IsBetterDirectoryModCover(
    const std::filesystem::path& mod_root,
    const std::filesystem::path& candidate,
    const std::filesystem::path& current) {
    if (!IsModCoverImageFile(candidate)) return false;
    if (current.empty()) return true;
    const auto rank = [&mod_root](const std::filesystem::path& file) {
        const std::filesystem::path relative = ModCoverRelativePath(mod_root, file);
        return std::tuple<std::size_t, int, std::wstring>{
            ModCoverDepth(relative),
            ModCoverNamePriority(file),
            LowerModCoverText(relative.generic_wstring())};
    };
    return rank(candidate) < rank(current);
}

inline bool IsLooseModCover(
    const std::wstring_view mod_name, const std::filesystem::path& candidate) {
    if (!IsModCoverImageFile(candidate)) return false;
    return LowerModCoverText(candidate.stem().wstring()) ==
        LowerModCoverText(std::wstring(mod_name));
}

}  // namespace anomaly::launcher
