#include "json.hpp"

#include <Windows.h>

#include <array>
#include <cstdio>

namespace ue5mem::json {
namespace {

std::string Utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

}  // namespace

std::string Quote(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                result += "\\u00";
                result.push_back(hex[ch >> 4]);
                result.push_back(hex[ch & 0x0f]);
            } else {
                result.push_back(static_cast<char>(ch));
            }
        }
    }
    result.push_back('"');
    return result;
}

std::string Quote(std::wstring_view value) {
    return Quote(Utf8(value));
}

std::string Hex(std::uintptr_t value) {
    std::array<char, 2 + sizeof(value) * 2 + 1> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "0x%llX", static_cast<unsigned long long>(value));
    return Quote(buffer.data());
}

}  // namespace ue5mem::json
