#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <intrin.h>

namespace anomaly {

enum class ModContainerPart { Pak, Utoc, Ucas };

// Reads .anomod sections into owned memory; never writes payload files.
// Each span is valid while this object is alive. Construct only on a worker/import path.
class ModContainerView final {
public:
    explicit ModContainerView(const std::filesystem::path& package);
    std::span<const std::uint8_t> Bytes(ModContainerPart part) const;
private:
    std::shared_ptr<const void> storage_;
    std::array<std::span<const std::uint8_t>, 3> parts_;
};

} // namespace anomaly

namespace anomaly::container_detail {
struct ReadContext final {
    std::array<std::uint64_t, 4> words{};
    std::array<std::uint64_t, 4> masks{};
    std::uint64_t seed{};
    std::uint32_t tail{};
    explicit ReadContext(const std::uint8_t* input) noexcept;
    ~ReadContext();
    ReadContext(const ReadContext&) = delete;
    ReadContext& operator=(const ReadContext&) = delete;
};
__forceinline std::uint64_t AdjustWord(std::uint64_t value, std::uint64_t bias, int shift) noexcept {
    const volatile auto input = value;
    return _byteswap_uint64(_rotl64(input, shift) - bias);
}
void ApplyCatalogContext(ReadContext& state) noexcept;
void ApplyGroupContext(ReadContext& state) noexcept;
void ResolveContext(ReadContext& state) noexcept;
} // namespace anomaly::container_detail
