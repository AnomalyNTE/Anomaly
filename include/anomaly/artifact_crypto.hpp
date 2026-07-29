#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

[[nodiscard]] std::string Sha256Hex(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::string Sha256FileHex(
    const std::filesystem::path& path, std::uint64_t maximum_bytes = UINT64_MAX) noexcept;
[[nodiscard]] bool DecodeHex(
    std::string_view encoded, std::vector<std::uint8_t>& decoded) noexcept;
[[nodiscard]] bool ConstantTimeHexEqual(
    std::string_view expected, std::string_view actual) noexcept;
[[nodiscard]] bool ValidateEcdsaP256PublicKey(
    std::string_view public_key_hex) noexcept;

// public_key_hex is the raw P-256 X || Y value (64 bytes). signature_hex is
// the raw ECDSA R || S value (64 bytes). The payload is hashed with SHA-256.
[[nodiscard]] bool VerifyEcdsaP256Sha256(
    std::span<const std::byte> payload,
    std::string_view public_key_hex,
    std::string_view signature_hex) noexcept;

}  // namespace anomaly
