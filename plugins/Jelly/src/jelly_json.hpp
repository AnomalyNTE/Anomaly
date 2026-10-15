#pragma once
//
// A small JSON reader for the plugin's own bundled resources.
//
// The layout resource is plugin data that must be validated offline, so parsing
// cannot depend on the host's JSON service being published. The reader accepts
// the subset the resource schema uses (objects, arrays, strings, numbers,
// booleans, null), bounds its depth and document size, and never allocates
// unboundedly.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jelly::json {

enum class Kind { null_value, boolean, number, string, array, object };

struct Value {
    Kind kind{Kind::null_value};
    bool boolean{};
    double number{};
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    [[nodiscard]] const Value* Find(const std::string_view key) const noexcept {
        for (const auto& member : members) {
            if (member.first == key) return &member.second;
        }
        return nullptr;
    }

    [[nodiscard]] bool IsObject() const noexcept { return kind == Kind::object; }
    [[nodiscard]] bool IsArray() const noexcept { return kind == Kind::array; }
    [[nodiscard]] bool IsString() const noexcept { return kind == Kind::string; }
    [[nodiscard]] bool IsNumber() const noexcept { return kind == Kind::number; }
    [[nodiscard]] bool IsBool() const noexcept { return kind == Kind::boolean; }

    [[nodiscard]] std::string_view StringOr(const std::string_view fallback) const noexcept {
        return IsString() ? std::string_view(text) : fallback;
    }
};

inline constexpr std::size_t kMaximumDepth = 24;
inline constexpr std::size_t kMaximumDocumentBytes = 256U * 1024U;

// Parses one complete document. Trailing non-whitespace input is an error, so a
// truncated or concatenated resource cannot be silently accepted.
bool Parse(std::string_view document, Value& out, std::string& error) noexcept;

}  // namespace jelly::json

namespace jelly::json {
namespace detail {

class Reader final {
public:
    explicit Reader(const std::string_view document) noexcept : document_(document) {}

    bool ParseDocument(Value& out) noexcept {
        SkipWhitespace();
        if (!ParseValue(out, 0)) return false;
        SkipWhitespace();
        if (position_ != document_.size()) {
            error_ = "trailing content after the document";
            return false;
        }
        return true;
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    void SkipWhitespace() noexcept {
        while (position_ < document_.size()) {
            const char character = document_[position_];
            if (character == ' ' || character == '\t' || character == '\r' ||
                character == '\n') {
                ++position_;
                continue;
            }
            break;
        }
    }

    bool Fail(const char* message) noexcept {
        if (error_.empty()) error_ = message;
        return false;
    }

    bool ParseValue(Value& out, const std::size_t depth) noexcept {
        if (depth > kMaximumDepth) return Fail("nesting is too deep");
        if (position_ >= document_.size()) return Fail("unexpected end of document");
        switch (document_[position_]) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"':
                out.kind = Kind::string;
                return ParseString(out.text);
            case 't':
                if (document_.substr(position_, 4) != "true") return Fail("expected true");
                position_ += 4;
                out.kind = Kind::boolean;
                out.boolean = true;
                return true;
            case 'f':
                if (document_.substr(position_, 5) != "false") return Fail("expected false");
                position_ += 5;
                out.kind = Kind::boolean;
                out.boolean = false;
                return true;
            case 'n':
                if (document_.substr(position_, 4) != "null") return Fail("expected null");
                position_ += 4;
                out.kind = Kind::null_value;
                return true;
            default:
                return ParseNumber(out);
        }
    }

    bool ParseObject(Value& out, const std::size_t depth) noexcept {
        ++position_;  // '{'
        out.kind = Kind::object;
        SkipWhitespace();
        if (position_ < document_.size() && document_[position_] == '}') {
            ++position_;
            return true;
        }
        for (;;) {
            SkipWhitespace();
            if (position_ >= document_.size() || document_[position_] != '"') {
                return Fail("expected a member name");
            }
            std::string key;
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (position_ >= document_.size() || document_[position_] != ':') {
                return Fail("expected ':'");
            }
            ++position_;
            SkipWhitespace();
            Value member;
            if (!ParseValue(member, depth + 1U)) return false;
            out.members.emplace_back(std::move(key), std::move(member));
            SkipWhitespace();
            if (position_ >= document_.size()) return Fail("unterminated object");
            if (document_[position_] == ',') {
                ++position_;
                continue;
            }
            if (document_[position_] == '}') {
                ++position_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(Value& out, const std::size_t depth) noexcept {
        ++position_;  // '['
        out.kind = Kind::array;
        SkipWhitespace();
        if (position_ < document_.size() && document_[position_] == ']') {
            ++position_;
            return true;
        }
        for (;;) {
            SkipWhitespace();
            Value item;
            if (!ParseValue(item, depth + 1U)) return false;
            out.items.push_back(std::move(item));
            SkipWhitespace();
            if (position_ >= document_.size()) return Fail("unterminated array");
            if (document_[position_] == ',') {
                ++position_;
                continue;
            }
            if (document_[position_] == ']') {
                ++position_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseString(std::string& out) noexcept {
        ++position_;  // '"'
        for (;;) {
            if (position_ >= document_.size()) return Fail("unterminated string");
            const char character = document_[position_++];
            if (character == '"') return true;
            if (character != '\\') {
                out.push_back(character);
                continue;
            }
            if (position_ >= document_.size()) return Fail("unterminated escape");
            const char escape = document_[position_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (position_ + 4U > document_.size()) return Fail("truncated \\u escape");
                    unsigned code{};
                    for (int digit = 0; digit < 4; ++digit) {
                        const char hex = document_[position_ + static_cast<std::size_t>(digit)];
                        code <<= 4U;
                        if (hex >= '0' && hex <= '9') {
                            code |= static_cast<unsigned>(hex - '0');
                        } else if (hex >= 'a' && hex <= 'f') {
                            code |= static_cast<unsigned>(hex - 'a' + 10);
                        } else if (hex >= 'A' && hex <= 'F') {
                            code |= static_cast<unsigned>(hex - 'A' + 10);
                        } else {
                            return Fail("invalid \\u escape");
                        }
                    }
                    position_ += 4U;
                    // The resources are ASCII; a non-ASCII escape is encoded as UTF-8.
                    if (code < 0x80U) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800U) {
                        out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
                        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
                    } else {
                        out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
                        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
                        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
                    }
                    break;
                }
                default: return Fail("unknown escape");
            }
        }
    }

    bool ParseNumber(Value& out) noexcept {
        const std::size_t begin = position_;
        if (position_ < document_.size() &&
            (document_[position_] == '-' || document_[position_] == '+')) {
            ++position_;
        }
        bool digits{};
        while (position_ < document_.size() && document_[position_] >= '0' &&
               document_[position_] <= '9') {
            ++position_;
            digits = true;
        }
        if (position_ < document_.size() && document_[position_] == '.') {
            ++position_;
            while (position_ < document_.size() && document_[position_] >= '0' &&
                   document_[position_] <= '9') {
                ++position_;
                digits = true;
            }
        }
        if (!digits) return Fail("expected a value");
        if (position_ < document_.size() &&
            (document_[position_] == 'e' || document_[position_] == 'E')) {
            ++position_;
            if (position_ < document_.size() &&
                (document_[position_] == '-' || document_[position_] == '+')) {
                ++position_;
            }
            bool exponent_digits{};
            while (position_ < document_.size() && document_[position_] >= '0' &&
                   document_[position_] <= '9') {
                ++position_;
                exponent_digits = true;
            }
            if (!exponent_digits) return Fail("expected exponent digits");
        }
        const std::string_view slice = document_.substr(begin, position_ - begin);
        out.kind = Kind::number;
        out.number = std::strtod(std::string(slice).c_str(), nullptr);
        return true;
    }

    std::string_view document_;
    std::size_t position_{};
    std::string error_;
};

}  // namespace detail

inline bool Parse(
    const std::string_view document, Value& out, std::string& error) noexcept {
    error.clear();
    out = Value{};
    if (document.empty()) {
        error = "document is empty";
        return false;
    }
    if (document.size() > kMaximumDocumentBytes) {
        error = "document is too large";
        return false;
    }
    detail::Reader reader(document);
    if (!reader.ParseDocument(out)) {
        error = reader.error();
        return false;
    }
    return true;
}

}  // namespace jelly::json
