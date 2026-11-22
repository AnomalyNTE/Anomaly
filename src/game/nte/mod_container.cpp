#include "anomaly/mod_container.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace anomaly {
namespace {
constexpr std::array<std::uint8_t, 8> magic{'A','N','O','M','O','D',1,0};
constexpr std::size_t nonce_offset = magic.size() + 3 * sizeof(std::uint64_t);
constexpr std::size_t tag_offset = nonce_offset + 12;
constexpr std::size_t header_size = tag_offset + 16;
struct Handle {
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Mapping {
    const std::uint8_t* data;
    ~Mapping() { if (data) UnmapViewOfFile(data); }
};
struct Provider {
    BCRYPT_ALG_HANDLE value{};
    ~Provider() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
};
struct Binding {
    BCRYPT_KEY_HANDLE value{};
    ~Binding() { if (value) BCryptDestroyKey(value); }
};
struct PayloadBuffer {
    std::vector<std::uint8_t> bytes;
    ~PayloadBuffer() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
};
void RequireStatus(NTSTATUS status) {
    if (!BCRYPT_SUCCESS(status)) throw std::runtime_error("invalid mod container payload");
}
} // namespace

ModContainerView::ModContainerView(const std::filesystem::path& package) {
    Handle file{CreateFileW(package.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    LARGE_INTEGER size{};
    // The native reader accepts an ULONG payload size.
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart <= static_cast<LONGLONG>(header_size) ||
        size.QuadPart - header_size > UINT32_MAX) throw std::runtime_error("invalid Anomaly mod size");
    Handle mapping{CreateFileMappingW(file.value, nullptr, PAGE_READONLY, 0, 0, nullptr)};
    if (!mapping.value) throw std::runtime_error("cannot create Anomaly mod mapping");
    Mapping view{static_cast<const std::uint8_t*>(MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, 0))};
    if (!view.data) throw std::runtime_error("cannot map Anomaly mod");
    if (!std::equal(magic.begin(), magic.end(), view.data))
        throw std::runtime_error("unknown Anomaly mod signature or version");

    const auto payload_size = static_cast<std::size_t>(size.QuadPart) - header_size;
    std::array<std::size_t, 3> lengths{};
    std::size_t total{};
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        std::uint64_t length{};
        std::memcpy(&length, view.data + magic.size() + i * sizeof(length), sizeof(length));
        if (!length || length > payload_size - total) throw std::runtime_error("invalid Anomaly mod section");
        lengths[i] = static_cast<std::size_t>(length);
        total += lengths[i];
    }
    if (total != payload_size) throw std::runtime_error("unexpected Anomaly mod payload size");

    Provider provider;
    RequireStatus(BCryptOpenAlgorithmProvider(&provider.value, BCRYPT_AES_ALGORITHM, nullptr, 0));
    RequireStatus(BCryptSetProperty(provider.value, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
    Binding binding;
    {
        container_detail::ReadContext state(view.data + nonce_offset);
        container_detail::ApplyCatalogContext(state);
        container_detail::ApplyGroupContext(state);
        container_detail::ResolveContext(state);
        RequireStatus(BCryptGenerateSymmetricKey(provider.value, &binding.value, nullptr, 0,
            reinterpret_cast<PUCHAR>(state.words.data()), static_cast<ULONG>(sizeof(state.words)), 0));
    }
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = const_cast<PUCHAR>(view.data + nonce_offset);
    auth.cbNonce = 12;
    auth.pbTag = const_cast<PUCHAR>(view.data + tag_offset);
    auth.cbTag = 16;
    auth.pbAuthData = const_cast<PUCHAR>(view.data);
    auth.cbAuthData = static_cast<ULONG>(tag_offset);
    auto payload = std::make_shared<PayloadBuffer>();
    payload->bytes.resize(payload_size);
    ULONG written{};
    RequireStatus(BCryptDecrypt(binding.value, const_cast<PUCHAR>(view.data + header_size),
        static_cast<ULONG>(payload_size), &auth, nullptr, 0, payload->bytes.data(),
        static_cast<ULONG>(payload_size), &written, 0));
    if (written != payload_size) throw std::runtime_error("invalid mod container payload size");
    // Publish section spans only after whole-container validation.
    storage_ = payload;
    std::size_t offset{};
    for (std::size_t i = 0; i < parts_.size(); ++i) {
        parts_[i] = {payload->bytes.data() + offset, lengths[i]};
        offset += lengths[i];
    }
}

std::span<const std::uint8_t> ModContainerView::Bytes(ModContainerPart part) const {
    return parts_.at(static_cast<std::size_t>(part));
}
} // namespace anomaly

namespace anomaly::container_detail {
ReadContext::ReadContext(const std::uint8_t* input) noexcept {
    std::memcpy(&seed, input, sizeof(seed));
    std::memcpy(&tail, input + sizeof(seed), sizeof(tail));
}
ReadContext::~ReadContext() { SecureZeroMemory(this, sizeof(*this)); }

__declspec(noinline) void ResolveContext(ReadContext& state) noexcept {
    state.words[0] ^= AdjustWord(0xf953fb31ba378ea0ull, 0x61c312fddf2dfeabull, 13) ^ state.masks[0];
    state.words[2] ^= AdjustWord(0x523b4b77b72cb589ull, 0x57c5c85396c15509ull, 17) ^ state.masks[2];
    state.words[1] ^= AdjustWord(0xca4fe305f3202803ull, 0x49f55048116ea691ull, 43) ^ state.masks[1];
    state.words[3] ^= AdjustWord(0x88ff9f585cd7b24dull, 0x5a574d8cfb39e59cull, 31) ^ state.masks[3];
}
} // namespace anomaly::container_detail
