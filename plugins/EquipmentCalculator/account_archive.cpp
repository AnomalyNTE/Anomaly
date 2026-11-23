#include "account_archive.hpp"
#include "workspace_store.hpp"

#include <miniz.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <memory>
#include <set>

namespace equipment_calculator {
namespace {
using Json = nlohmann::json;
[[noreturn]] void Invalid() { throw InvalidWorkspace{WorkspaceFailure::invalid_archive}; }
constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
struct Zip {
    mz_zip_archive value{};
    bool writer{};
    ~Zip() { if (writer) mz_zip_writer_end(&value); else mz_zip_reader_end(&value); }
};
}
std::string EncodeAccountChunk(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > kAccountTransferChunk) Invalid();
    std::string result; result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto count = (std::min)(std::size_t{3}, bytes.size() - index);
        const auto value = (std::uint32_t(bytes[index]) << 16) |
            (count > 1 ? std::uint32_t(bytes[index + 1]) << 8 : 0) | (count > 2 ? bytes[index + 2] : 0);
        result += kAlphabet[(value >> 18) & 63]; result += kAlphabet[(value >> 12) & 63];
        result += count > 1 ? kAlphabet[(value >> 6) & 63] : '=';
        result += count > 2 ? kAlphabet[value & 63] : '=';
    }
    return result;
}
std::vector<std::uint8_t> DecodeAccountChunk(std::string_view text) {
    if (text.empty() || text.size() % 4 || text.size() > ((kAccountTransferChunk + 2) / 3) * 4) Invalid();
    std::vector<std::uint8_t> bytes; bytes.reserve(text.size() / 4 * 3);
    for (std::size_t index = 0; index < text.size(); index += 4) {
        std::array<std::uint32_t, 4> values{}; std::size_t padding{};
        for (std::size_t digit = 0; digit < 4; ++digit) {
            const auto character = text[index + digit];
            if (character == '=') { if (digit < 2 || index + 4 != text.size()) Invalid(); ++padding; }
            else {
                const auto found = kAlphabet.find(character);
                if (found == kAlphabet.npos || padding) Invalid();
                values[digit] = static_cast<std::uint32_t>(found);
            }
        }
        if (padding > 2 || (padding == 2 && (values[1] & 15)) || (padding == 1 && (values[2] & 3))) Invalid();
        const auto value = (values[0] << 18) | (values[1] << 12) | (values[2] << 6) | values[3];
        bytes.push_back(static_cast<std::uint8_t>(value >> 16));
        if (padding < 2) bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        if (!padding) bytes.push_back(static_cast<std::uint8_t>(value));
    }
    if (bytes.size() > kAccountTransferChunk) Invalid();
    return bytes;
}
std::vector<std::uint8_t> BuildAccountArchive(const Json& account) {
    const auto document = account.dump();
    if (document.size() > kAccountArchiveLimit) Invalid();
    const auto manifest = Json{{"format", "anomaly.equipment-workspace"}, {"schema_version", 1}}.dump();
    Zip zip; zip.writer = true;
    if (!mz_zip_writer_init_heap(&zip.value, 0, 0) ||
        !mz_zip_writer_add_mem(&zip.value, "manifest.json", manifest.data(), manifest.size(), MZ_BEST_SPEED) ||
        !mz_zip_writer_add_mem(&zip.value, "account.json", document.data(), document.size(), MZ_BEST_SPEED)) Invalid();
    void* data{}; std::size_t size{};
    if (!mz_zip_writer_finalize_heap_archive(&zip.value, &data, &size)) Invalid();
    const std::unique_ptr<void, decltype(&mz_free)> owner(data, mz_free);
    if (!size || size > kAccountArchiveLimit) Invalid();
    return {static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + size};
}
Json ReadAccountArchive(std::span<const std::uint8_t> archive) {
    if (archive.empty() || archive.size() > kAccountArchiveLimit) Invalid();
    Zip zip;
    if (!mz_zip_reader_init_mem(&zip.value, archive.data(), archive.size(), 0) ||
        mz_zip_reader_get_num_files(&zip.value) != 2) Invalid();
    std::set<std::string> names; Json manifest, account;
    for (mz_uint index = 0; index < 2; ++index) {
        mz_zip_archive_file_stat file{};
        if (!mz_zip_reader_file_stat(&zip.value, index, &file) || file.m_is_directory || file.m_is_encrypted ||
            ((file.m_external_attr >> 16) & 0170000) == 0120000 || !names.insert(file.m_filename).second) Invalid();
        const std::string_view name(file.m_filename);
        if (name != "manifest.json" && name != "account.json") Invalid();
        const auto maximum = name == "manifest.json" ? std::size_t{4096} : kAccountArchiveLimit;
        if (!file.m_uncomp_size || file.m_uncomp_size > maximum) Invalid();
        std::string document(static_cast<std::size_t>(file.m_uncomp_size), '\0');
        if (!mz_zip_reader_extract_to_mem(&zip.value, index, document.data(), document.size(), 0)) Invalid();
        auto parsed = Json::parse(document, nullptr, false);
        if (!parsed.is_object()) Invalid();
        if (name == "manifest.json") manifest = std::move(parsed); else account = std::move(parsed);
    }
    if (manifest.value("format", std::string{}) != "anomaly.equipment-workspace" ||
        manifest.value("schema_version", 0) != 1 || account.is_null()) Invalid();
    return account;
}
} // namespace equipment_calculator
