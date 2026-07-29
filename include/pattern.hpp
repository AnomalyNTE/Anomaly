#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ue5mem {

struct PatternByte {
    std::uint8_t value{};
    std::uint8_t mask{};
};

class Pattern {
public:
    static Pattern Parse(std::string_view text);

    [[nodiscard]] std::vector<std::size_t> FindAll(
        std::span<const std::uint8_t> bytes,
        std::size_t limit = 256) const;
    [[nodiscard]] std::size_t Size() const noexcept { return bytes_.size(); }

private:
    std::vector<PatternByte> bytes_;
};

}  // namespace ue5mem
