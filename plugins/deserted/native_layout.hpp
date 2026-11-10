#pragma once
#include <anomaly/sdk/profile.hpp>
#include <array>
#include <cstddef>
namespace deserted_profile {
// 与版本无关的字段：对象表布局、类/名字偏移、Profile 记录形状、车辆数量旋钮。
// 全部来自 Profile 的 layout 块，代码里不保留第二份数值。
struct Layout {
    std::uint32_t kClassPrivateOffset{};
    std::uint32_t kNamePrivateOffset{};
    std::uint32_t kDefaultProfileOffset{};   // 由选中的候选填充
    std::uint32_t kDensityProfilesOffset{};  // 由选中的候选填充
    std::uint32_t kProfileStride{};
    std::uint32_t kSpawnCountOffset{};
    std::uint32_t kSpawnScaleOffset{};
    std::uint32_t kVehMinActiveOffset{};     // 由选中的候选填充
    std::uint32_t kObjectChunkSize{};
    std::uint32_t kObjectItemStride{};
    std::uint32_t kArrayNumOffset{};
    std::array<std::uint32_t, 5> kScaleOffsets{};
};
// 随游戏版本变化的三处偏移：1.4b 与 1.5a 的基类尺寸不同，这三处整体后移。
struct Candidate {
    std::uint32_t kDefaultProfileOffset{};
    std::uint32_t kDensityProfilesOffset{};
    std::uint32_t kVehMinActiveOffset{};
};
inline Layout base_layout;                 // 已校验的版本无关字段
inline Layout native_layout;               // 当前生效布局；候选未选定前三处为 0
inline std::array<Candidate, 4> candidates{};
inline std::size_t candidate_count{};
inline int selected_candidate{-1};         // -1 = 尚未选定，禁止用版本相关偏移访问内存
[[nodiscard]] inline bool LayoutSelected() noexcept { return selected_candidate >= 0; }
inline bool SelectCandidate(std::size_t index) noexcept {
    if (index >= candidate_count) return false;
    const Candidate& candidate = candidates[index];
    if (candidate.kDefaultProfileOffset + base_layout.kProfileStride > candidate.kDensityProfilesOffset)
        return false;
    Layout layout = base_layout;
    layout.kDefaultProfileOffset = candidate.kDefaultProfileOffset;
    layout.kDensityProfilesOffset = candidate.kDensityProfilesOffset;
    layout.kVehMinActiveOffset = candidate.kVehMinActiveOffset;
    native_layout = layout;
    selected_candidate = static_cast<int>(index);
    return true;
}
namespace detail {
[[nodiscard]] inline std::uint32_t CandidateValue(const anomaly::sdk::ProfileValues& values,
                                                  const char* key) {
    const auto found = values.find(key);
    if (found == values.end()) throw std::runtime_error(std::string("missing profile value: ") + key);
    return found->second;
}
// 候选自身的关系校验：默认档必须整体落在 Profile 数组之前，车辆保底字段在合理区间内。
[[nodiscard]] inline bool CandidateShape(const Candidate& candidate, const Layout& base) noexcept {
    if (candidate.kDefaultProfileOffset == 0 || candidate.kDensityProfilesOffset == 0) return false;
    if (base.kProfileStride < sizeof(float) * 5) return false;
    if (candidate.kDefaultProfileOffset + base.kProfileStride > candidate.kDensityProfilesOffset) return false;
    if (candidate.kVehMinActiveOffset < base.kSpawnScaleOffset ||
        candidate.kVehMinActiveOffset > 0x8000u) return false;
    return true;
}
}  // namespace detail
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        // layout 候选由插件自己挑选：签名命中先用签名，未命中时留给实际对象形状判定，
        // 因此这里不把 layout 声明成加载期必须解析成功的符号。
        if (!compatibility.Load(host, "compatibility.profile.json",
                                {"nte", "HTGame.exe", ".text", "deserted-density-v1"}, {}, false, {})) return false;
        Layout base;
        base.kClassPrivateOffset = compatibility.Value("kClassPrivateOffset", 65536, 4);
        base.kNamePrivateOffset = compatibility.Value("kNamePrivateOffset", 65536, 4);
        base.kProfileStride = compatibility.Value("kProfileStride", 65536, 4);
        base.kSpawnCountOffset = compatibility.Value("kSpawnCountOffset", 65536, 4);
        base.kSpawnScaleOffset = compatibility.Value("kSpawnScaleOffset", 65536, 4);
        base.kObjectChunkSize = compatibility.Value("kObjectChunkSize", 65536, 4);
        base.kObjectItemStride = compatibility.Value("kObjectItemStride", 65536, 4);
        base.kArrayNumOffset = compatibility.Value("kArrayNumOffset", 65536, 4);
        base.kScaleOffsets[0] = compatibility.Value("scale0", 65536, 4);
        base.kScaleOffsets[1] = compatibility.Value("scale1", 65536, 4);
        base.kScaleOffsets[2] = compatibility.Value("scale2", 65536, 4);
        base.kScaleOffsets[3] = compatibility.Value("scale3", 65536, 4);
        base.kScaleOffsets[4] = compatibility.Value("scale4", 65536, 4);
        if (!base.kObjectChunkSize || base.kObjectItemStride < sizeof(void*) ||
            base.kProfileStride < sizeof(float)) return false;
        for (std::size_t i{}; i < base.kScaleOffsets.size(); ++i) {
            if (base.kScaleOffsets[i] + sizeof(float) > base.kProfileStride ||
                (i && base.kScaleOffsets[i - 1] + sizeof(float) > base.kScaleOffsets[i])) return false;
        }
        const auto& symbol = compatibility.Document().symbol("layout");
        candidate_count = 0;
        for (const auto& source : symbol.candidates) {
            if (candidate_count >= candidates.size()) break;
            Candidate parsed;
            parsed.kDefaultProfileOffset = detail::CandidateValue(source.values, "kDefaultProfileOffset");
            parsed.kDensityProfilesOffset = detail::CandidateValue(source.values, "kDensityProfilesOffset");
            parsed.kVehMinActiveOffset = detail::CandidateValue(source.values, "kVehMinActiveOffset");
            if (!detail::CandidateShape(parsed, base)) return false;
            candidates[candidate_count++] = parsed;
        }
        if (candidate_count == 0) return false;
        base_layout = base;
        native_layout = base;
        selected_candidate = -1;
        // 签名已命中的候选直接生效；签名全部失配时 selected_candidate 保持 -1，
        // 由实际生成器对象的形状在扫描后判定唯一候选。
        const auto* selected = compatibility.Selected("layout");
        if (selected != nullptr) {
            const auto& all = compatibility.Document().symbol("layout").candidates;
            for (std::size_t i{}; i < candidate_count && i < all.size(); ++i) {
                if (selected != &all[i]) continue;
                if (!SelectCandidate(i)) return false;
                break;
            }
        }
        return true;
    } catch (...) { return false; }
}
}  // namespace deserted_profile
