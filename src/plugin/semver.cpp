#include "anomaly/semver.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace anomaly {
namespace {

bool IsDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

bool IsIdentifierCharacter(char value) noexcept {
    return IsDigit(value) || (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') || value == '-';
}

bool IsRangeWhitespace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void ResetError(SemVerParseError* error) {
    if (error != nullptr) *error = {};
}

void SetError(
    SemVerParseError* error, SemVerParseErrorCode code, std::size_t offset,
    std::string message) {
    if (error == nullptr) return;
    error->code = code;
    error->offset = offset;
    error->message = std::move(message);
}

}  // namespace

namespace detail {

class SemanticVersionParser final {
public:
    SemanticVersionParser(std::string_view text, SemVerParseError* error)
        : text_(text), error_(error) {}

    std::optional<SemanticVersion> Parse() {
        ResetError(error_);
        if (text_.empty()) {
            Fail(SemVerParseErrorCode::EmptyInput, 0, "semantic version is empty");
            return std::nullopt;
        }

        SemanticVersion version;
        if (!ParseCoreNumber(version.major_, "major") || !ConsumeCoreSeparator() ||
            !ParseCoreNumber(version.minor_, "minor") || !ConsumeCoreSeparator() ||
            !ParseCoreNumber(version.patch_, "patch")) {
            return std::nullopt;
        }

        if (position_ < text_.size() && text_[position_] == '-') {
            ++position_;
            if (!ParsePrerelease(version.prerelease_)) return std::nullopt;
        }
        if (position_ < text_.size() && text_[position_] == '+') {
            ++position_;
            if (!ParseBuildMetadata(version.build_metadata_)) return std::nullopt;
        }
        if (position_ != text_.size()) {
            Fail(SemVerParseErrorCode::InvalidCharacter, position_,
                 "unexpected character in semantic version");
            return std::nullopt;
        }
        return version;
    }

private:
    bool ParseCoreNumber(std::uint64_t& destination, const char* component) {
        const std::size_t start = position_;
        if (position_ >= text_.size() || !IsDigit(text_[position_])) {
            Fail(SemVerParseErrorCode::ExpectedCoreNumber, position_,
                 std::string("expected numeric ") + component + " version");
            return false;
        }
        while (position_ < text_.size() && IsDigit(text_[position_])) ++position_;
        if (position_ - start > 1 && text_[start] == '0') {
            Fail(SemVerParseErrorCode::LeadingZero, start,
                 std::string(component) + " version has a leading zero");
            return false;
        }

        std::uint64_t value{};
        for (std::size_t index = start; index < position_; ++index) {
            const std::uint64_t digit = static_cast<std::uint64_t>(text_[index] - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                Fail(SemVerParseErrorCode::NumericOverflow, start,
                     std::string(component) + " version exceeds uint64 range");
                return false;
            }
            value = value * 10 + digit;
        }
        destination = value;
        return true;
    }

    bool ConsumeCoreSeparator() {
        if (position_ >= text_.size() || text_[position_] != '.') {
            Fail(SemVerParseErrorCode::MissingCoreComponent, position_,
                 "semantic version must contain major.minor.patch");
            return false;
        }
        ++position_;
        return true;
    }

    bool ParsePrerelease(std::vector<std::string>& destination) {
        for (;;) {
            const std::size_t start = position_;
            while (position_ < text_.size() && IsIdentifierCharacter(text_[position_])) {
                ++position_;
            }
            if (start == position_) {
                Fail(SemVerParseErrorCode::EmptyIdentifier, position_,
                     "prerelease identifier is empty or invalid");
                return false;
            }

            const std::string value(text_.substr(start, position_ - start));
            const bool numeric = std::all_of(value.begin(), value.end(), IsDigit);
            if (numeric && value.size() > 1 && value.front() == '0') {
                Fail(SemVerParseErrorCode::LeadingZero, start,
                     "numeric prerelease identifier has a leading zero");
                return false;
            }
            destination.push_back(value);

            if (position_ >= text_.size() || text_[position_] == '+') return true;
            if (text_[position_] != '.') {
                Fail(SemVerParseErrorCode::InvalidCharacter, position_,
                     "invalid character in prerelease identifier");
                return false;
            }
            ++position_;
        }
    }

    bool ParseBuildMetadata(std::vector<std::string>& destination) {
        for (;;) {
            const std::size_t start = position_;
            while (position_ < text_.size() && IsIdentifierCharacter(text_[position_])) {
                ++position_;
            }
            if (start == position_) {
                Fail(SemVerParseErrorCode::EmptyIdentifier, position_,
                     "build metadata identifier is empty or invalid");
                return false;
            }
            destination.emplace_back(text_.substr(start, position_ - start));

            if (position_ >= text_.size()) return true;
            if (text_[position_] != '.') {
                Fail(SemVerParseErrorCode::InvalidCharacter, position_,
                     "invalid character in build metadata identifier");
                return false;
            }
            ++position_;
        }
    }

    void Fail(SemVerParseErrorCode code, std::size_t offset, std::string message) {
        SetError(error_, code, offset, std::move(message));
    }

    std::string_view text_;
    SemVerParseError* error_{};
    std::size_t position_{};
};

}  // namespace detail

namespace {

int CompareUnsigned(std::uint64_t left, std::uint64_t right) noexcept {
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

int CompareIdentifier(std::string_view left, std::string_view right) noexcept {
    const bool left_numeric = std::all_of(left.begin(), left.end(), IsDigit);
    const bool right_numeric = std::all_of(right.begin(), right.end(), IsDigit);
    if (left_numeric != right_numeric) return left_numeric ? -1 : 1;
    if (left_numeric) {
        if (left.size() < right.size()) return -1;
        if (left.size() > right.size()) return 1;
    }
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

}  // namespace

std::string SemanticVersion::ToString() const {
    std::string result = std::to_string(major_) + "." + std::to_string(minor_) + "." +
        std::to_string(patch_);
    if (!prerelease_.empty()) {
        result.push_back('-');
        for (std::size_t index = 0; index < prerelease_.size(); ++index) {
            if (index != 0) result.push_back('.');
            result += prerelease_[index];
        }
    }
    if (!build_metadata_.empty()) {
        result.push_back('+');
        for (std::size_t index = 0; index < build_metadata_.size(); ++index) {
            if (index != 0) result.push_back('.');
            result += build_metadata_[index];
        }
    }
    return result;
}

int CompareSemanticVersionPrecedence(
    const SemanticVersion& left, const SemanticVersion& right) noexcept {
    if (const int comparison = CompareUnsigned(left.Major(), right.Major()); comparison != 0) {
        return comparison;
    }
    if (const int comparison = CompareUnsigned(left.Minor(), right.Minor()); comparison != 0) {
        return comparison;
    }
    if (const int comparison = CompareUnsigned(left.Patch(), right.Patch()); comparison != 0) {
        return comparison;
    }

    if (left.Prerelease().empty() != right.Prerelease().empty()) {
        return left.Prerelease().empty() ? 1 : -1;
    }
    const std::size_t shared = std::min(left.Prerelease().size(), right.Prerelease().size());
    for (std::size_t index = 0; index < shared; ++index) {
        if (const int comparison = CompareIdentifier(
                left.Prerelease()[index], right.Prerelease()[index]); comparison != 0) {
            return comparison;
        }
    }
    if (left.Prerelease().size() < right.Prerelease().size()) return -1;
    if (left.Prerelease().size() > right.Prerelease().size()) return 1;
    return 0;
}

SemanticVersionRange::SemanticVersionRange(std::vector<SemVerComparator> comparators)
    : comparators_(std::move(comparators)) {}

bool SemanticVersionRange::Matches(const SemanticVersion& version) const noexcept {
    if (comparators_.empty()) return false;
    if (!version.Prerelease().empty()) {
        const bool prerelease_opt_in = std::any_of(
            comparators_.begin(), comparators_.end(), [&](const SemVerComparator& comparator) {
                return !comparator.version.Prerelease().empty() &&
                    comparator.version.Major() == version.Major() &&
                    comparator.version.Minor() == version.Minor() &&
                    comparator.version.Patch() == version.Patch();
            });
        if (!prerelease_opt_in) return false;
    }
    for (const SemVerComparator& comparator : comparators_) {
        const int comparison = CompareSemanticVersionPrecedence(version, comparator.version);
        switch (comparator.operation) {
        case SemVerComparatorOperator::Equal:
            if (comparison != 0) return false;
            break;
        case SemVerComparatorOperator::Less:
            if (comparison >= 0) return false;
            break;
        case SemVerComparatorOperator::LessOrEqual:
            if (comparison > 0) return false;
            break;
        case SemVerComparatorOperator::Greater:
            if (comparison <= 0) return false;
            break;
        case SemVerComparatorOperator::GreaterOrEqual:
            if (comparison < 0) return false;
            break;
        }
    }
    return true;
}

std::optional<SemanticVersion> ParseSemanticVersion(
    std::string_view text, SemVerParseError* error) {
    return detail::SemanticVersionParser(text, error).Parse();
}

std::optional<SemanticVersionRange> ParseSemanticVersionRange(
    std::string_view text, SemVerParseError* error) {
    ResetError(error);
    std::size_t position{};
    while (position < text.size() && IsRangeWhitespace(text[position])) ++position;
    if (position == text.size()) {
        SetError(error, SemVerParseErrorCode::EmptyInput, position,
                 "semantic version range is empty");
        return std::nullopt;
    }

    std::vector<SemVerComparator> comparators;
    while (position < text.size()) {
        const std::size_t token_start = position;
        while (position < text.size() && !IsRangeWhitespace(text[position])) ++position;
        const std::string_view token = text.substr(token_start, position - token_start);

        SemVerComparator comparator;
        std::size_t version_offset{};
        if (token.starts_with("<=")) {
            comparator.operation = SemVerComparatorOperator::LessOrEqual;
            version_offset = 2;
        } else if (token.starts_with(">=")) {
            comparator.operation = SemVerComparatorOperator::GreaterOrEqual;
            version_offset = 2;
        } else if (token.starts_with('<')) {
            comparator.operation = SemVerComparatorOperator::Less;
            version_offset = 1;
        } else if (token.starts_with('>')) {
            comparator.operation = SemVerComparatorOperator::Greater;
            version_offset = 1;
        } else if (token.starts_with('=')) {
            comparator.operation = SemVerComparatorOperator::Equal;
            version_offset = 1;
        }

        if (version_offset == token.size()) {
            SetError(error, SemVerParseErrorCode::MissingRangeVersion,
                     token_start + version_offset, "range comparator is missing a version");
            return std::nullopt;
        }
        SemVerParseError version_error;
        auto version = ParseSemanticVersion(token.substr(version_offset), &version_error);
        if (!version) {
            SetError(error, version_error.code,
                     token_start + version_offset + version_error.offset,
                     "invalid range version: " + version_error.message);
            return std::nullopt;
        }
        comparator.version = std::move(*version);
        comparators.push_back(std::move(comparator));

        while (position < text.size() && IsRangeWhitespace(text[position])) ++position;
    }
    return SemanticVersionRange(std::move(comparators));
}

}  // namespace anomaly
