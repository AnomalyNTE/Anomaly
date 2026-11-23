#pragma once

#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace equipment_manager {
inline constexpr std::size_t kAccountArchiveLimit = 256U * 1024U * 1024U;
inline constexpr std::size_t kAccountTransferChunk = 32768;
std::vector<std::uint8_t> BuildAccountArchive(const nlohmann::json& account);
nlohmann::json ReadAccountArchive(std::span<const std::uint8_t> archive);
std::string EncodeAccountChunk(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> DecodeAccountChunk(std::string_view text);
} // namespace equipment_manager
