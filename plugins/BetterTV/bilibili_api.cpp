// Native bilibili API client for the television source.
//
// Resolution order: unsigned view -> WBI-signed view (only when the unsigned
// endpoint rejects the request) -> nav WBI keys -> WBI-signed playurl ->
// unsigned playurl. Only one muxed durl segment at up to 720p (quality 64) is
// accepted; DASH-only and multi-segment pages are reported instead of being
// guessed at.
//
// The JSON reader and the WBI helpers live in this unit because it is also
// linked into the offline fixture, which has no third-party JSON dependency.
// MD5 comes from the Windows CNG provider. Every diagnostic string is English
// and only ever logged.

#include "bilibili_api.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace better_tv_video {
namespace {

constexpr std::string_view kApiOrigin = "https://api.bilibili.com";
constexpr std::string_view kViewPath = "/x/web-interface/view";
constexpr std::string_view kWbiViewPath = "/x/web-interface/wbi/view";
constexpr std::string_view kNavPath = "/x/web-interface/nav";
constexpr std::string_view kWbiPlayUrlPath = "/x/player/wbi/playurl";
constexpr std::string_view kPlayUrlPath = "/x/player/playurl";
// 720p is the highest quality the muxed MP4 profile accepts. qn=64 requests it;
// an unauthenticated account may still answer with a lower quality.
constexpr std::string_view kPlayUrlQuality = "64";
constexpr std::int64_t kQualityLimit = 64;
// Guard rails for the recursive JSON reader: real documents nest a few levels,
// and every response already has a byte cap on the transport.
constexpr int kMaxJsonDepth = 64;
constexpr std::size_t kMaxExactInteger = 9007199254740992;  // 2^53
// WBI mixes the 64 character "<img_key><sub_key>" source with the first 32
// entries of this table (bilibili's MIXIN_KEY_ENC_TAB).
constexpr std::array<std::uint8_t, 64> kMixinKeyEncTable{
    46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35,
    27, 43, 5,  49, 33, 9,  42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,  60, 51, 30, 4,
    22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52};
constexpr std::size_t kMixinSourceLength = 64;
constexpr std::size_t kMixinKeyLength = 32;

char ToLowerAscii(const char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool IsAsciiDigit(const char value) {
    return value >= '0' && value <= '9';
}

bool IsAsciiAlpha(const char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool IsAsciiAlnum(const char value) {
    return IsAsciiAlpha(value) || IsAsciiDigit(value);
}

bool EqualsIgnoreCase(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ToLowerAscii(left[index]) != ToLowerAscii(right[index])) return false;
    }
    return true;
}

bool StartsWithIgnoreCase(const std::string_view text, const std::string_view prefix) {
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

std::int64_t CurrentUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The JSON document is a tree; members are looked up by name. API documents are
// small enough that a linear search per member is not worth indexing.
struct JsonValue final {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind{Kind::Null};
    bool boolean{};
    double number{};
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* Find(const std::string_view key) const {
        if (kind != Kind::Object) return nullptr;
        for (const auto& [name, value] : object) {
            if (name == key) return &value;
        }
        return nullptr;
    }
};

class JsonReader final {
public:
    explicit JsonReader(const std::string_view text) : text_(text) {}

    bool Parse(JsonValue* root) {
        std::size_t position = 0;
        if (!ParseValue(&position, 0, root)) return false;
        SkipWhitespace(&position);
        return position == text_.size();
    }

private:
    void SkipWhitespace(std::size_t* position) const {
        while (*position < text_.size()) {
            const char value = text_[*position];
            if (value == ' ' || value == '\t' || value == '\n' || value == '\r') {
                ++*position;
                continue;
            }
            break;
        }
    }

    bool Consume(std::size_t* position, const char expected) const {
        if (*position >= text_.size() || text_[*position] != expected) return false;
        ++*position;
        return true;
    }

    bool ConsumeLiteral(std::size_t* position, const std::string_view literal) const {
        if (text_.substr(*position, literal.size()) != literal) return false;
        *position += literal.size();
        return true;
    }

    bool ReadHex4(std::size_t* position, std::uint32_t* value) const {
        if (text_.size() - *position < 4) return false;
        std::uint32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = text_[(*position)++];
            result <<= 4;
            if (digit >= '0' && digit <= '9') {
                result |= static_cast<std::uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                result |= static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                result |= static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
                return false;
            }
        }
        *value = result;
        return true;
    }

    static void AppendUtf8(const std::uint32_t code_point, std::string* text) {
        if (code_point <= 0x7F) {
            text->push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FF) {
            text->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            text->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point <= 0xFFFF) {
            text->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            text->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            text->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            text->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            text->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            text->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            text->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    bool ParseString(std::size_t* position, std::string* value) const {
        if (!Consume(position, '"')) return false;
        value->clear();
        while (*position < text_.size()) {
            const unsigned char byte = static_cast<unsigned char>(text_[*position]);
            if (byte == '"') {
                ++*position;
                return true;
            }
            if (byte == '\\') {
                ++*position;
                if (*position >= text_.size()) return false;
                const char escape = text_[(*position)++];
                switch (escape) {
                    case '"': value->push_back('"'); break;
                    case '\\': value->push_back('\\'); break;
                    case '/': value->push_back('/'); break;
                    case 'b': value->push_back('\b'); break;
                    case 'f': value->push_back('\f'); break;
                    case 'n': value->push_back('\n'); break;
                    case 'r': value->push_back('\r'); break;
                    case 't': value->push_back('\t'); break;
                    case 'u': {
                        std::uint32_t code_point = 0;
                        if (!ReadHex4(position, &code_point)) return false;
                        if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                            std::uint32_t trailing = 0;
                            if (!Consume(position, '\\') || !Consume(position, 'u') ||
                                !ReadHex4(position, &trailing) || trailing < 0xDC00 ||
                                trailing > 0xDFFF) {
                                return false;
                            }
                            code_point = 0x10000 + ((code_point - 0xD800) << 10) +
                                         (trailing - 0xDC00);
                        } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                            return false;
                        }
                        AppendUtf8(code_point, value);
                        break;
                    }
                    default: return false;
                }
                continue;
            }
            if (byte < 0x20) return false;
            value->push_back(static_cast<char>(byte));
            ++*position;
        }
        return false;
    }

    bool ParseNumber(std::size_t* position, double* value) const {
        const std::size_t start = *position;
        if (*position < text_.size() && text_[*position] == '-') ++*position;
        if (*position >= text_.size()) return false;
        if (text_[*position] == '0') {
            ++*position;
        } else if (IsAsciiDigit(text_[*position])) {
            while (*position < text_.size() && IsAsciiDigit(text_[*position])) ++*position;
        } else {
            return false;
        }
        if (*position < text_.size() && text_[*position] == '.') {
            ++*position;
            if (*position >= text_.size() || !IsAsciiDigit(text_[*position])) return false;
            while (*position < text_.size() && IsAsciiDigit(text_[*position])) ++*position;
        }
        if (*position < text_.size() &&
            (text_[*position] == 'e' || text_[*position] == 'E')) {
            ++*position;
            if (*position < text_.size() &&
                (text_[*position] == '+' || text_[*position] == '-')) {
                ++*position;
            }
            if (*position >= text_.size() || !IsAsciiDigit(text_[*position])) return false;
            while (*position < text_.size() && IsAsciiDigit(text_[*position])) ++*position;
        }
        const char* const begin = text_.data() + start;
        const char* const end = text_.data() + *position;
        double parsed = 0;
        const std::from_chars_result converted = std::from_chars(begin, end, parsed);
        if (converted.ec != std::errc() || converted.ptr != end) return false;
        *value = parsed;
        return true;
    }

    bool ParseArray(std::size_t* position, const int depth, JsonValue* value) const {
        value->kind = JsonValue::Kind::Array;
        value->array.clear();
        ++*position;  // '['
        SkipWhitespace(position);
        if (*position < text_.size() && text_[*position] == ']') {
            ++*position;
            return true;
        }
        for (;;) {
            JsonValue element;
            if (!ParseValue(position, depth + 1, &element)) return false;
            value->array.push_back(std::move(element));
            SkipWhitespace(position);
            if (*position >= text_.size()) return false;
            if (text_[*position] == ',') {
                ++*position;
                continue;
            }
            if (text_[*position] == ']') {
                ++*position;
                return true;
            }
            return false;
        }
    }

    bool ParseObject(std::size_t* position, const int depth, JsonValue* value) const {
        value->kind = JsonValue::Kind::Object;
        value->object.clear();
        ++*position;  // '{'
        SkipWhitespace(position);
        if (*position < text_.size() && text_[*position] == '}') {
            ++*position;
            return true;
        }
        for (;;) {
            SkipWhitespace(position);
            if (*position >= text_.size() || text_[*position] != '"') return false;
            std::string key;
            if (!ParseString(position, &key)) return false;
            SkipWhitespace(position);
            if (!Consume(position, ':')) return false;
            JsonValue member;
            if (!ParseValue(position, depth + 1, &member)) return false;
            value->object.emplace_back(std::move(key), std::move(member));
            SkipWhitespace(position);
            if (*position >= text_.size()) return false;
            if (text_[*position] == ',') {
                ++*position;
                continue;
            }
            if (text_[*position] == '}') {
                ++*position;
                return true;
            }
            return false;
        }
    }

    bool ParseValue(std::size_t* position, const int depth, JsonValue* value) const {
        if (depth > kMaxJsonDepth) return false;
        SkipWhitespace(position);
        if (*position >= text_.size()) return false;
        switch (text_[*position]) {
            case '{': return ParseObject(position, depth, value);
            case '[': return ParseArray(position, depth, value);
            case '"':
                value->kind = JsonValue::Kind::String;
                return ParseString(position, &value->string);
            case 't':
                value->kind = JsonValue::Kind::Bool;
                value->boolean = true;
                return ConsumeLiteral(position, "true");
            case 'f':
                value->kind = JsonValue::Kind::Bool;
                value->boolean = false;
                return ConsumeLiteral(position, "false");
            case 'n':
                value->kind = JsonValue::Kind::Null;
                return ConsumeLiteral(position, "null");
            default:
                value->kind = JsonValue::Kind::Number;
                return ParseNumber(position, &value->number);
        }
    }

    std::string_view text_;
};

bool ReadInteger(const JsonValue* value, std::int64_t* result) {
    if (value == nullptr || value->kind != JsonValue::Kind::Number) return false;
    const double number = value->number;
    if (!std::isfinite(number)) return false;
    if (number < -static_cast<double>(kMaxExactInteger) ||
        number > static_cast<double>(kMaxExactInteger)) {
        return false;
    }
    if (std::trunc(number) != number) return false;
    *result = static_cast<std::int64_t>(number);
    return true;
}

bool ReadString(const JsonValue* value, std::string_view* result) {
    if (value == nullptr || value->kind != JsonValue::Kind::String) return false;
    *result = value->string;
    return true;
}

bool ReadApiCode(const JsonValue& document, std::int64_t* code) {
    return ReadInteger(document.Find("code"), code);
}

std::string ReadApiMessage(const JsonValue& document) {
    std::string_view message;
    if (!ReadString(document.Find("message"), &message)) return {};
    return std::string(message);
}

std::string ApiUrl(const std::string_view path) {
    std::string url(kApiOrigin);
    url.append(path);
    return url;
}

BilibiliApiResult Failure(const BilibiliApiError error, std::string detail) {
    BilibiliApiResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

std::string UrlEncode(const std::string_view text) {
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(text.size());
    for (const char value : text) {
        const unsigned char byte = static_cast<unsigned char>(value);
        const bool unreserved = IsAsciiAlnum(value) || value == '-' || value == '_' ||
                                value == '.' || value == '~';
        if (unreserved) {
            encoded.push_back(value);
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHexDigits[byte >> 4]);
        encoded.push_back(kHexDigits[byte & 0x0F]);
    }
    return encoded;
}

// WBI drops these characters from parameter values before signing.
std::string RemoveWbiUnsafeCharacters(const std::string_view text) {
    std::string filtered;
    filtered.reserve(text.size());
    for (const char value : text) {
        if (value == '!' || value == '\'' || value == '(' || value == ')' || value == '*') {
            continue;
        }
        filtered.push_back(value);
    }
    return filtered;
}

using QueryParameters = std::vector<std::pair<std::string, std::string>>;

std::string JoinQuery(const QueryParameters& params) {
    std::string query;
    for (const auto& [name, value] : params) {
        if (!query.empty()) query.push_back('&');
        query += UrlEncode(name);
        query.push_back('=');
        query += UrlEncode(value);
    }
    return query;
}

QueryParameters SortedByKey(QueryParameters params) {
    std::sort(params.begin(), params.end(),
              [](const std::pair<std::string, std::string>& left,
                 const std::pair<std::string, std::string>& right) {
                  return left.first < right.first;
              });
    return params;
}

bool Md5Hex(const std::string_view input, std::string* hex) {
    hex->clear();
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(
            BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0))) {
        return false;
    }
    DWORD hash_length = 0;
    DWORD property_size = 0;
    const bool sized = BCRYPT_SUCCESS(BCryptGetProperty(
        algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_length),
        sizeof(hash_length), &property_size, 0));
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = sized && hash_length == 16 &&
              BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
    std::array<UCHAR, 16> digest{};
    if (ok) {
        ok = BCRYPT_SUCCESS(BCryptHashData(
                 hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                 static_cast<ULONG>(input.size()), 0)) &&
             BCRYPT_SUCCESS(
                 BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;

    static constexpr char kHexDigits[] = "0123456789abcdef";
    hex->reserve(digest.size() * 2);
    for (const UCHAR byte : digest) {
        hex->push_back(kHexDigits[byte >> 4]);
        hex->push_back(kHexDigits[byte & 0x0F]);
    }
    return true;
}

enum class DocumentOutcome { Ok, Rejected, Failed };

bool FetchJsonDocument(const ApiFetcher& fetch, const std::string& url, JsonValue* document,
                       std::string* detail, BilibiliApiError* error) {
    const HttpTextResult response = fetch(url);
    if (!response.ok || response.status < 200 || response.status >= 300) {
        *error = response.status != 0 ? BilibiliApiError::HttpStatus : BilibiliApiError::Network;
        if (!response.detail.empty()) {
            *detail = response.detail;
        } else if (response.status != 0) {
            *detail = "api request returned HTTP " + std::to_string(response.status);
        } else {
            *detail = "api request failed";
        }
        return false;
    }
    if (!JsonReader(response.body).Parse(document)) {
        *error = BilibiliApiError::InvalidDocument;
        *detail = "api response is not valid JSON";
        return false;
    }
    return true;
}

struct ViewData final {
    std::string bvid;
    std::uint64_t aid{};
    std::vector<std::int64_t> cids;
};

bool ParseViewPayload(const JsonValue& document, ViewData* view) {
    const JsonValue* data = document.Find("data");
    if (data == nullptr || data->kind != JsonValue::Kind::Object) return false;
    const JsonValue* pages = data->Find("pages");
    if (pages == nullptr || pages->kind != JsonValue::Kind::Array) return false;
    for (const JsonValue& item : pages->array) {
        std::int64_t cid = 0;
        if (!ReadInteger(item.Find("cid"), &cid) || cid <= 0) return false;
        view->cids.push_back(cid);
    }
    std::string_view bvid;
    if (ReadString(data->Find("bvid"), &bvid)) view->bvid.assign(bvid);
    std::int64_t aid = 0;
    if (ReadInteger(data->Find("aid"), &aid) && aid > 0) {
        view->aid = static_cast<std::uint64_t>(aid);
    }
    return true;
}

DocumentOutcome LoadViewDocument(const std::string& url, const ApiFetcher& fetch, ViewData* view,
                                 std::string* detail, BilibiliApiError* error) {
    JsonValue document;
    if (!FetchJsonDocument(fetch, url, &document, detail, error)) {
        return DocumentOutcome::Failed;
    }
    std::int64_t code = 0;
    if (!ReadApiCode(document, &code)) {
        *error = BilibiliApiError::InvalidDocument;
        *detail = "view document has no numeric code";
        return DocumentOutcome::Failed;
    }
    if (code != 0) {
        *detail = "view request rejected with code " + std::to_string(code);
        const std::string message = ReadApiMessage(document);
        if (!message.empty()) {
            *detail += ": ";
            *detail += message;
        }
        return DocumentOutcome::Rejected;
    }
    if (!ParseViewPayload(document, view)) {
        *error = BilibiliApiError::InvalidDocument;
        *detail = "view document has no usable page list";
        return DocumentOutcome::Failed;
    }
    return DocumentOutcome::Ok;
}

struct WbiKeys final {
    std::string img_key;
    std::string sub_key;
};

// The wbi key is the file name of the nav URL without its extension.
bool ExtractWbiKey(const std::string_view source_url, std::string* key) {
    const std::size_t slash = source_url.find_last_of('/');
    std::string_view name =
        slash == std::string_view::npos ? source_url : source_url.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return false;
    name = name.substr(0, dot);
    for (const char value : name) {
        if (!IsAsciiAlnum(value)) return false;
    }
    key->assign(name);
    return true;
}

bool LoadWbiKeys(const ApiFetcher& fetch, WbiKeys* keys, std::string* detail) {
    JsonValue document;
    BilibiliApiError ignored = BilibiliApiError::Network;
    if (!FetchJsonDocument(fetch, ApiUrl(kNavPath), &document, detail, &ignored)) return false;
    // The anonymous nav answer reports code -101 ("not logged in") but still
    // carries data.wbi_img, so only the key URLs decide success.
    const JsonValue* data = document.Find("data");
    const JsonValue* wbi_img = data == nullptr ? nullptr : data->Find("wbi_img");
    std::string_view img_url;
    std::string_view sub_url;
    if (wbi_img == nullptr || !ReadString(wbi_img->Find("img_url"), &img_url) ||
        !ReadString(wbi_img->Find("sub_url"), &sub_url) ||
        !ExtractWbiKey(img_url, &keys->img_key) ||
        !ExtractWbiKey(sub_url, &keys->sub_key)) {
        *detail = "nav response has no usable wbi_img keys";
        return false;
    }
    return true;
}

bool BuildSignedQuery(const QueryParameters& params, const WbiKeys& keys, std::string* query) {
    std::string mixin;
    if (!ComputeMixinKey(keys.img_key, keys.sub_key, &mixin)) return false;
    std::string signed_query = SignWbiQuery(params, mixin, CurrentUnixSeconds());
    if (signed_query.empty()) return false;
    *query = std::move(signed_query);
    return true;
}

struct PlayUrlData final {
    std::string media_url;
    std::uint64_t media_size{};
    std::int64_t duration_ms{};
    int quality{};
};

bool ParsePlayUrlPayload(const JsonValue& document, PlayUrlData* play, std::string* detail,
                         BilibiliApiError* error) {
    const JsonValue* data = document.Find("data");
    if (data == nullptr || data->kind != JsonValue::Kind::Object) {
        *error = BilibiliApiError::InvalidDocument;
        *detail = "playurl document has no data object";
        return false;
    }
    std::int64_t quality = 0;
    if (!ReadInteger(data->Find("quality"), &quality) || quality <= 0) {
        *error = BilibiliApiError::InvalidDocument;
        *detail = "playurl document has no positive quality";
        return false;
    }
    if (quality > kQualityLimit) {
        *error = BilibiliApiError::UnsupportedQuality;
        *detail = "playurl reported quality " + std::to_string(quality) +
                  ", above the supported 64";
        return false;
    }
    const JsonValue* durl = data->Find("durl");
    if (durl == nullptr || durl->kind != JsonValue::Kind::Array || durl->array.empty()) {
        *error = BilibiliApiError::NoPlayableStream;
        *detail = "playurl document has no durl segment (DASH-only page)";
        return false;
    }
    if (durl->array.size() != 1) {
        *error = BilibiliApiError::SegmentedStream;
        *detail = "playurl document has " + std::to_string(durl->array.size()) +
                  " durl segments, only one muxed segment is supported";
        return false;
    }
    const JsonValue& segment = durl->array.front();
    std::string_view media_url;
    if (segment.kind != JsonValue::Kind::Object ||
        !ReadString(segment.Find("url"), &media_url) || media_url.empty() ||
        !StartsWithIgnoreCase(media_url, "https://")) {
        *error = BilibiliApiError::InvalidDocument;
        *detail = "durl segment has no absolute HTTPS media URL";
        return false;
    }
    std::int64_t media_size = 0;
    if (!ReadInteger(segment.Find("size"), &media_size) || media_size <= 0) {
        *error = BilibiliApiError::InvalidDocument;
        *detail = "durl segment has no positive byte size";
        return false;
    }
    std::int64_t duration_ms = 0;
    if (!ReadInteger(data->Find("timelength"), &duration_ms) || duration_ms <= 0) {
        *error = BilibiliApiError::InvalidDocument;
        *detail = "playurl document has no positive timelength";
        return false;
    }
    play->media_url.assign(media_url);
    play->media_size = static_cast<std::uint64_t>(media_size);
    play->duration_ms = duration_ms;
    play->quality = static_cast<int>(quality);
    return true;
}

DocumentOutcome LoadPlayUrlDocument(const std::string& url, const ApiFetcher& fetch,
                                    PlayUrlData* play, std::string* detail,
                                    BilibiliApiError* error) {
    JsonValue document;
    if (!FetchJsonDocument(fetch, url, &document, detail, error)) {
        return DocumentOutcome::Failed;
    }
    std::int64_t code = 0;
    if (!ReadApiCode(document, &code)) {
        *error = BilibiliApiError::InvalidDocument;
        *detail = "playurl document has no numeric code";
        return DocumentOutcome::Failed;
    }
    if (code != 0) {
        *detail = "playurl request rejected with code " + std::to_string(code);
        const std::string message = ReadApiMessage(document);
        if (!message.empty()) {
            *detail += ": ";
            *detail += message;
        }
        return DocumentOutcome::Rejected;
    }
    if (!ParsePlayUrlPayload(document, play, detail, error)) return DocumentOutcome::Failed;
    return DocumentOutcome::Ok;
}

QueryParameters IdentityParameters(const BilibiliPage& page) {
    // The view endpoint takes the numeric identity as "aid"; playurl uses "avid".
    QueryParameters params;
    if (!page.bvid.empty()) {
        params.emplace_back("bvid", page.bvid);
    } else {
        params.emplace_back("aid", std::to_string(page.aid));
    }
    return params;
}

}  // namespace

bool ComputeMixinKey(const std::string_view img_key, const std::string_view sub_key,
                     std::string* mixin) {
    if (mixin == nullptr) return false;
    mixin->clear();
    std::string source(img_key);
    source.append(sub_key);
    if (source.size() != kMixinSourceLength) return false;
    mixin->reserve(kMixinKeyLength);
    for (std::size_t index = 0; index < kMixinKeyLength; ++index) {
        mixin->push_back(source[kMixinKeyEncTable[index]]);
    }
    return true;
}

std::string SignWbiQuery(const std::span<const std::pair<std::string, std::string>> params,
                         const std::string_view mixin_key, const std::int64_t wts) {
    QueryParameters ordered;
    ordered.reserve(params.size() + 1);
    for (const auto& [name, value] : params) {
        ordered.emplace_back(name, RemoveWbiUnsafeCharacters(value));
    }
    ordered.emplace_back("wts", std::to_string(wts));
    const std::string query = JoinQuery(SortedByKey(std::move(ordered)));
    std::string signing_input(query);
    signing_input.append(mixin_key);
    std::string digest;
    if (!Md5Hex(signing_input, &digest)) return {};
    std::string signed_query(query);
    signed_query += "&w_rid=";
    signed_query += digest;
    return signed_query;
}

BilibiliApiResult ResolveMuxedPlayInfo(const BilibiliPage& page, const ApiFetcher& fetch) {
    if (!fetch) return Failure(BilibiliApiError::Network, "no api fetcher configured");

    const QueryParameters identity = IdentityParameters(page);
    std::string detail;
    BilibiliApiError error = BilibiliApiError::Network;
    ViewData view;
    DocumentOutcome outcome = LoadViewDocument(ApiUrl(kViewPath) + "?" + JoinQuery(identity),
                                               fetch, &view, &detail, &error);
    WbiKeys keys;
    bool keys_loaded = false;
    if (outcome == DocumentOutcome::Failed) return Failure(error, detail);
    if (outcome == DocumentOutcome::Rejected) {
        const std::string rejection_detail = detail;
        std::string keys_detail;
        if (!LoadWbiKeys(fetch, &keys, &keys_detail)) {
            return Failure(BilibiliApiError::ApiRejected,
                           rejection_detail +
                               " (wbi view fallback unavailable: " + keys_detail + ")");
        }
        keys_loaded = true;
        std::string signed_query;
        if (!BuildSignedQuery(identity, keys, &signed_query)) {
            return Failure(BilibiliApiError::ApiRejected,
                           rejection_detail + " (wbi signing failed)");
        }
        outcome = LoadViewDocument(ApiUrl(kWbiViewPath) + "?" + signed_query, fetch, &view,
                                   &detail, &error);
        if (outcome == DocumentOutcome::Failed) return Failure(error, detail);
        if (outcome == DocumentOutcome::Rejected) {
            return Failure(BilibiliApiError::ApiRejected, detail);
        }
    }

    if (page.page < 1 || static_cast<std::size_t>(page.page) > view.cids.size()) {
        return Failure(BilibiliApiError::PageOutOfRange,
                       "page " + std::to_string(page.page) + " is outside the video's " +
                           std::to_string(view.cids.size()) + " page(s)");
    }
    const std::int64_t cid = view.cids[static_cast<std::size_t>(page.page) - 1];

    QueryParameters play_params;
    if (!view.bvid.empty()) {
        play_params.emplace_back("bvid", view.bvid);
    } else if (!page.bvid.empty()) {
        play_params.emplace_back("bvid", page.bvid);
    } else {
        play_params.emplace_back(
            "avid", std::to_string(view.aid != 0 ? view.aid : page.aid));
    }
    play_params.emplace_back("cid", std::to_string(cid));
    play_params.emplace_back("qn", std::string(kPlayUrlQuality));
    play_params.emplace_back("fnver", "0");
    play_params.emplace_back("fnval", "1");
    play_params.emplace_back("fourk", "1");
    play_params.emplace_back("otype", "json");
    play_params.emplace_back("platform", "pc");
    play_params.emplace_back("high_quality", "1");

    PlayUrlData play;
    bool signed_resolved = false;
    if (!keys_loaded) {
        std::string keys_detail;
        keys_loaded = LoadWbiKeys(fetch, &keys, &keys_detail);
    }
    if (keys_loaded) {
        std::string signed_query;
        if (BuildSignedQuery(play_params, keys, &signed_query)) {
            outcome = LoadPlayUrlDocument(ApiUrl(kWbiPlayUrlPath) + "?" + signed_query, fetch,
                                          &play, &detail, &error);
            signed_resolved = outcome == DocumentOutcome::Ok;
        }
    }
    if (!signed_resolved) {
        // The unsigned endpoint still serves anonymous clients, and a refused
        // signed request is retried there before the error is reported.
        const std::string query = JoinQuery(SortedByKey(play_params));
        outcome = LoadPlayUrlDocument(ApiUrl(kPlayUrlPath) + "?" + query, fetch, &play, &detail,
                                      &error);
        if (outcome == DocumentOutcome::Failed) return Failure(error, detail);
        if (outcome == DocumentOutcome::Rejected) {
            return Failure(BilibiliApiError::ApiRejected, detail);
        }
    }

    BilibiliPlayInfo info;
    info.bvid = view.bvid.empty() ? page.bvid : view.bvid;
    info.aid = view.aid != 0 ? view.aid : page.aid;
    info.page = page.page;
    info.cid = cid;
    info.media_url = std::move(play.media_url);
    info.media_size = play.media_size;
    info.duration_ms = play.duration_ms;
    info.quality = play.quality;
    BilibiliApiResult result;
    result.info = std::move(info);
    return result;
}

}  // namespace better_tv_video
