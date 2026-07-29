#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

enum class SemVerParseErrorCode {
    None,
    EmptyInput,
    ExpectedCoreNumber,
    MissingCoreComponent,
    LeadingZero,
    NumericOverflow,
    EmptyIdentifier,
    InvalidCharacter,
    MissingRangeVersion,
};

struct SemVerParseError {
    SemVerParseErrorCode code{SemVerParseErrorCode::None};
    std::size_t offset{};
    std::string message;
};

namespace detail {
class SemanticVersionParser;
}

class SemanticVersion final {
public:
    SemanticVersion() = default;

    [[nodiscard]] std::string ToString() const;
    [[nodiscard]] std::uint64_t Major() const noexcept { return major_; }
    [[nodiscard]] std::uint64_t Minor() const noexcept { return minor_; }
    [[nodiscard]] std::uint64_t Patch() const noexcept { return patch_; }
    [[nodiscard]] const std::vector<std::string>& Prerelease() const noexcept {
        return prerelease_;
    }
    [[nodiscard]] const std::vector<std::string>& BuildMetadata() const noexcept {
        return build_metadata_;
    }

private:
    friend class detail::SemanticVersionParser;

    // Core numbers are intentionally bounded so compatibility checks cannot
    // depend on platform-specific arbitrary-precision arithmetic.
    std::uint64_t major_{};
    std::uint64_t minor_{};
    std::uint64_t patch_{};
    std::vector<std::string> prerelease_;
    std::vector<std::string> build_metadata_;
};

enum class SemVerComparatorOperator {
    Equal,
    Less,
    LessOrEqual,
    Greater,
    GreaterOrEqual,
};

struct SemVerComparator {
    SemVerComparatorOperator operation{SemVerComparatorOperator::Equal};
    SemanticVersion version;
};

class SemanticVersionRange final {
public:
    SemanticVersionRange() = default;
    explicit SemanticVersionRange(std::vector<SemVerComparator> comparators);

    [[nodiscard]] bool Matches(const SemanticVersion& version) const noexcept;
    [[nodiscard]] const std::vector<SemVerComparator>& Comparators() const noexcept {
        return comparators_;
    }

private:
    std::vector<SemVerComparator> comparators_;
};

// Returns -1, 0 or 1. Build metadata is deliberately excluded from precedence.
[[nodiscard]] int CompareSemanticVersionPrecedence(
    const SemanticVersion& left, const SemanticVersion& right) noexcept;

[[nodiscard]] std::optional<SemanticVersion> ParseSemanticVersion(
    std::string_view text, SemVerParseError* error = nullptr);

// The initial range grammar is a whitespace-separated AND of exact or
// =, <, <=, > and >= comparators, for example: ">=1.0.0 <2.0.0". Prerelease
// candidates require a comparator with a prerelease on the same core version.
[[nodiscard]] std::optional<SemanticVersionRange> ParseSemanticVersionRange(
    std::string_view text, SemVerParseError* error = nullptr);

}  // namespace anomaly
