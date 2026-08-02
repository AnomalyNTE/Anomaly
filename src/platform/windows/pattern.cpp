#include "pattern.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>

namespace ue5mem {
namespace {

int HexValue(char value) {
    const auto ch = static_cast<unsigned char>(value);
    if (std::isdigit(ch)) {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

PatternByte ParseToken(std::string_view token) {
    if (token.size() != 2) {
        throw std::invalid_argument("pattern tokens must contain two nibbles");
    }
    PatternByte result{};
    for (std::size_t index = 0; index < 2; ++index) {
        if (token[index] == '?') {
            continue;
        }
        const int nibble = HexValue(token[index]);
        if (nibble < 0) {
            throw std::invalid_argument("pattern contains a non-hex nibble");
        }
        const auto shift = static_cast<unsigned>((1 - index) * 4);
        result.value |= static_cast<std::uint8_t>(nibble << shift);
        result.mask |= static_cast<std::uint8_t>(0x0f << shift);
    }
    return result;
}

}  // namespace

Pattern Pattern::Parse(std::string_view text) {
    Pattern result;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }
        if (cursor == text.size()) {
            break;
        }
        const auto end = text.find_first_of(" \t\r\n", cursor);
        const auto token = text.substr(cursor, end == std::string_view::npos ? text.size() - cursor : end - cursor);
        result.bytes_.push_back(ParseToken(token));
        cursor = end == std::string_view::npos ? text.size() : end;
    }
    if (result.bytes_.empty()) {
        throw std::invalid_argument("pattern is empty");
    }

    std::size_t best_offset{};
    std::size_t best_size{};
    for (std::size_t anchor_cursor{}; anchor_cursor < result.bytes_.size();) {
        if (result.bytes_[anchor_cursor].mask != 0xffU) {
            ++anchor_cursor;
            continue;
        }
        const std::size_t offset = anchor_cursor;
        while (anchor_cursor < result.bytes_.size() &&
               result.bytes_[anchor_cursor].mask == 0xffU) {
            ++anchor_cursor;
        }
        const std::size_t size = anchor_cursor - offset;
        if (size > best_size) {
            best_offset = offset;
            best_size = size;
        }
    }
    if (best_size != 0) {
        result.anchor_offset_ = best_offset;
        result.anchor_.reserve(best_size);
        for (std::size_t index{}; index < best_size; ++index) {
            result.anchor_.push_back(result.bytes_[best_offset + index].value);
        }
    }
    return result;
}

std::vector<std::size_t> Pattern::FindAll(
    std::span<const std::uint8_t> bytes,
    std::size_t limit) const {
    std::vector<std::size_t> matches;
    if (bytes_.size() > bytes.size() || limit == 0) {
        return matches;
    }
    const auto matches_at = [&](const std::size_t offset) {
        for (std::size_t index = 0; index < bytes_.size(); ++index) {
            const auto& expected = bytes_[index];
            if ((bytes[offset + index] & expected.mask) != expected.value) {
                return false;
            }
        }
        return true;
    };

    if (anchor_.empty()) {
        for (std::size_t offset = 0; offset <= bytes.size() - bytes_.size(); ++offset) {
            if (matches_at(offset)) {
                matches.push_back(offset);
                if (matches.size() == limit) break;
            }
        }
        return matches;
    }

    const std::size_t trailing_bytes = bytes_.size() - anchor_offset_ - anchor_.size();
    const auto search_begin = bytes.begin() + anchor_offset_;
    const auto search_end = bytes.begin() + (bytes.size() - trailing_bytes);
    const std::boyer_moore_horspool_searcher searcher(anchor_.begin(), anchor_.end());
    for (auto candidate = std::search(search_begin, search_end, searcher);
         candidate != search_end;
         candidate = std::search(std::next(candidate), search_end, searcher)) {
        const std::size_t offset = static_cast<std::size_t>(candidate - bytes.begin()) - anchor_offset_;
        if (matches_at(offset)) {
            matches.push_back(offset);
            if (matches.size() == limit) break;
        }
    }
    return matches;
}

}  // namespace ue5mem
