#include "anomaly/artifact_crypto.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <fstream>
#include <limits>

namespace anomaly {
namespace {

class Algorithm final {
public:
    explicit Algorithm(const wchar_t* name) noexcept {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&value_, name, nullptr, 0))) {
            value_ = nullptr;
        }
    }
    ~Algorithm() {
        if (value_ != nullptr) BCryptCloseAlgorithmProvider(value_, 0);
    }
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return value_; }
private:
    BCRYPT_ALG_HANDLE value_{};
};

std::array<std::uint8_t, 32> Sha256Bytes(
    std::span<const std::byte> bytes, bool& success) noexcept {
    success = false;
    std::array<std::uint8_t, 32> digest{};
    Algorithm algorithm(BCRYPT_SHA256_ALGORITHM);
    if (algorithm.get() == nullptr) return digest;
    DWORD object_size{};
    DWORD copied{};
    if (!BCRYPT_SUCCESS(BCryptGetProperty(
            algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0))) {
        return digest;
    }
    std::vector<std::uint8_t> object(object_size);
    BCRYPT_HASH_HANDLE hash{};
    if (!BCRYPT_SUCCESS(BCryptCreateHash(
            algorithm.get(), &hash, object.data(), object_size, nullptr, 0, 0))) {
        return digest;
    }
    const auto close_hash = [&] { BCryptDestroyHash(hash); };
    if (bytes.size() > (std::numeric_limits<ULONG>::max)() ||
        !BCRYPT_SUCCESS(BCryptHashData(
            hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
            static_cast<ULONG>(bytes.size()), 0)) ||
        !BCRYPT_SUCCESS(BCryptFinishHash(
            hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        close_hash();
        return digest;
    }
    close_hash();
    success = true;
    return digest;
}

std::string EncodeHex(std::span<const std::uint8_t> bytes) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2] = alphabet[bytes[index] >> 4];
        result[index * 2 + 1] = alphabet[bytes[index] & 0x0f];
    }
    return result;
}

int HexValue(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    if (value >= 'A' && value <= 'F') return 10 + value - 'A';
    return -1;
}

bool ImportEcdsaP256PublicKey(
    BCRYPT_ALG_HANDLE algorithm, std::string_view public_key_hex,
    BCRYPT_KEY_HANDLE& imported) noexcept {
    std::vector<std::uint8_t> key;
    if (algorithm == nullptr || !DecodeHex(public_key_hex, key) || key.size() != 64) {
        return false;
    }
    BCRYPT_ECCKEY_BLOB header{};
    header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header.cbKey = 32;
    std::vector<std::uint8_t> blob(sizeof(header) + key.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), key.data(), key.size());
    return BCRYPT_SUCCESS(BCryptImportKeyPair(
        algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &imported,
        blob.data(), static_cast<ULONG>(blob.size()), 0));
}

}  // namespace

std::string Sha256Hex(std::span<const std::byte> bytes) noexcept {
    try {
        bool success{};
        const auto digest = Sha256Bytes(bytes, success);
        return success ? EncodeHex(digest) : std::string{};
    } catch (...) {
        return {};
    }
}

std::string Sha256FileHex(
    const std::filesystem::path& path, std::uint64_t maximum_bytes) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        Algorithm algorithm(BCRYPT_SHA256_ALGORITHM);
        if (algorithm.get() == nullptr) return {};
        DWORD object_size{};
        DWORD copied{};
        if (!BCRYPT_SUCCESS(BCryptGetProperty(
                algorithm.get(), BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0))) {
            return {};
        }
        std::vector<std::uint8_t> object(object_size);
        BCRYPT_HASH_HANDLE hash{};
        if (!BCRYPT_SUCCESS(BCryptCreateHash(
                algorithm.get(), &hash, object.data(), object_size, nullptr, 0, 0))) {
            return {};
        }
        std::array<char, 64U * 1024U> buffer{};
        std::uint64_t total{};
        bool ok = true;
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count <= 0) break;
            total += static_cast<std::uint64_t>(count);
            if (total > maximum_bytes || !BCRYPT_SUCCESS(BCryptHashData(
                    hash, reinterpret_cast<PUCHAR>(buffer.data()),
                    static_cast<ULONG>(count), 0))) {
                ok = false;
                break;
            }
        }
        std::array<std::uint8_t, 32> digest{};
        ok = ok && input.eof() && BCRYPT_SUCCESS(BCryptFinishHash(
            hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
        BCryptDestroyHash(hash);
        return ok ? EncodeHex(digest) : std::string{};
    } catch (...) {
        return {};
    }
}

bool DecodeHex(std::string_view encoded, std::vector<std::uint8_t>& decoded) noexcept {
    try {
        if ((encoded.size() & 1U) != 0) return false;
        std::vector<std::uint8_t> result(encoded.size() / 2);
        for (std::size_t index = 0; index < result.size(); ++index) {
            const int high = HexValue(encoded[index * 2]);
            const int low = HexValue(encoded[index * 2 + 1]);
            if (high < 0 || low < 0) return false;
            result[index] = static_cast<std::uint8_t>((high << 4) | low);
        }
        decoded = std::move(result);
        return true;
    } catch (...) {
        return false;
    }
}

bool ConstantTimeHexEqual(
    std::string_view expected, std::string_view actual) noexcept {
    if (expected.size() != actual.size()) return false;
    unsigned difference{};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        char left = expected[index];
        char right = actual[index];
        if (left >= 'A' && left <= 'F') left = static_cast<char>(left + ('a' - 'A'));
        if (right >= 'A' && right <= 'F') right = static_cast<char>(right + ('a' - 'A'));
        difference |= static_cast<unsigned>(static_cast<unsigned char>(left) ^
                                            static_cast<unsigned char>(right));
    }
    return difference == 0;
}

bool ValidateEcdsaP256PublicKey(std::string_view public_key_hex) noexcept {
    try {
        Algorithm algorithm(BCRYPT_ECDSA_P256_ALGORITHM);
        if (algorithm.get() == nullptr) return false;
        BCRYPT_KEY_HANDLE imported{};
        if (!ImportEcdsaP256PublicKey(algorithm.get(), public_key_hex, imported)) {
            return false;
        }
        BCryptDestroyKey(imported);
        return true;
    } catch (...) {
        return false;
    }
}

bool VerifyEcdsaP256Sha256(
    std::span<const std::byte> payload,
    std::string_view public_key_hex,
    std::string_view signature_hex) noexcept {
    try {
        std::vector<std::uint8_t> signature;
        if (!DecodeHex(signature_hex, signature) || signature.size() != 64) {
            return false;
        }
        bool digest_ok{};
        const auto digest = Sha256Bytes(payload, digest_ok);
        if (!digest_ok) return false;
        Algorithm algorithm(BCRYPT_ECDSA_P256_ALGORITHM);
        if (algorithm.get() == nullptr) return false;
        BCRYPT_KEY_HANDLE imported{};
        if (!ImportEcdsaP256PublicKey(algorithm.get(), public_key_hex, imported)) {
            return false;
        }
        const bool valid = BCRYPT_SUCCESS(BCryptVerifySignature(
            imported, nullptr, const_cast<PUCHAR>(digest.data()),
            static_cast<ULONG>(digest.size()), signature.data(),
            static_cast<ULONG>(signature.size()), 0));
        BCryptDestroyKey(imported);
        return valid;
    } catch (...) {
        return false;
    }
}

}  // namespace anomaly
