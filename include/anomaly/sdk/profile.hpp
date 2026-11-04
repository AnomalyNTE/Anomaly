#pragma once

#include "anomaly/sdk/cpp.hpp"
#include <array>
#include <charconv>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace anomaly::sdk {

using ProfileValues = std::map<std::string, std::uint32_t, std::less<>>;

struct ProfileCandidate final {
    std::string pattern;
    std::vector<std::uint8_t> entry;
    std::optional<std::uint32_t> call_offset;
    ProfileValues values;
    std::string provenance;
};

struct ProfileSymbol final {
    std::string name;
    std::vector<ProfileCandidate> candidates;
};

// Plugin-owned snapshot. No file I/O or host handles remain after LoadProfile.
struct Profile final {
    std::string game, module, section, contract, provenance, unavailable;
    ProfileValues layout;
    std::vector<ProfileSymbol> symbols;

    [[nodiscard]] const ProfileSymbol& symbol(std::string_view name) const {
        for (const auto& item : symbols) if (item.name == name) return item;
        throw std::out_of_range("undeclared profile symbol");
    }
};

namespace detail {

class ProfileJsonNode final {
public:
    ProfileJsonNode(const AnomalyJsonServiceV1* service, AnomalyGenerationHandleV1 handle)
        : service_(service), handle_(handle) {}
    ~ProfileJsonNode() { if (handle_.id != 0) service_->release(service_->user, handle_); }
    ProfileJsonNode(const ProfileJsonNode&) = delete;
    ProfileJsonNode& operator=(const ProfileJsonNode&) = delete;
    ProfileJsonNode(ProfileJsonNode&& other) noexcept
        : service_(other.service_), handle_(std::exchange(other.handle_, {})) {}

    static void Check(const AnomalyStatusV1 status) {
        if (!Succeeded(status)) throw std::runtime_error(status.message.data != nullptr
            ? std::string(status.message.data, status.message.size) : "profile service failed");
    }
    [[nodiscard]] ProfileJsonNode member(std::string_view key) const {
        AnomalyGenerationHandleV1 child{};
        Check(service_->object_find(service_->user, handle_, StringView(key), &child));
        return {service_, child};
    }
    [[nodiscard]] std::optional<ProfileJsonNode> optional_member(std::string_view key) const {
        AnomalyGenerationHandleV1 child{};
        const auto status = service_->object_find(service_->user, handle_, StringView(key), &child);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) return std::nullopt;
        Check(status);
        return ProfileJsonNode(service_, child);
    }
    [[nodiscard]] ProfileJsonNode item(std::size_t index) const {
        AnomalyGenerationHandleV1 child{};
        Check(service_->array_item(service_->user, handle_, index, &child));
        return {service_, child};
    }
    [[nodiscard]] std::size_t size() const {
        std::size_t count{};
        Check(service_->array_size(service_->user, handle_, &count));
        return count;
    }
    [[nodiscard]] std::string string() const {
        return CopyString([&](char* out, std::size_t* size) {
            return service_->string_value(service_->user, handle_, out, size);
        });
    }
    [[nodiscard]] std::uint32_t number() const {
        double value{};
        Check(service_->number_value(service_->user, handle_, &value));
        // The host schema limits numeric profile values to uint32 integers.
        return static_cast<std::uint32_t>(value);
    }
    [[nodiscard]] ProfileValues values() const {
        std::size_t count{};
        Check(service_->object_size(service_->user, handle_, &count));
        ProfileValues result;
        for (std::size_t i{}; i < count; ++i) {
            auto key = CopyString([&](char* out, std::size_t* size) {
                return service_->object_key_at(service_->user, handle_, i, out, size);
            });
            const auto value = member(key).number();
            result.emplace(std::move(key), value);
        }
        return result;
    }

private:
    template <typename Copy> static std::string CopyString(Copy&& copy) {
        std::size_t size{};
        Check(copy(nullptr, &size));
        if (size == 0 || size > 1024U * 1024U)
            throw std::runtime_error("profile string exceeds service limits");
        std::string value(size, '\0');
        Check(copy(value.data(), &size));
        value.resize(size - 1);
        return value;
    }
    const AnomalyJsonServiceV1* service_;
    AnomalyGenerationHandleV1 handle_;
};

}  // namespace detail

class ProfileResult final {
public:
    [[nodiscard]] explicit operator bool() const noexcept { return profile_.has_value(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const Profile& value() const & { return profile_.value(); }
    [[nodiscard]] Profile&& value() && { return std::move(profile_.value()); }
private:
    friend ProfileResult LoadProfile(const Host&, std::string_view);
    std::optional<Profile> profile_;
    std::string error_;
};

[[nodiscard]] inline ProfileResult LoadProfile(const Host& host, std::string_view filename) {
    ProfileResult result;
    try {
        const auto profiles = host.Query<AnomalyProfileServiceV1>(ANOMALY_PROFILE_SERVICE_V1_ID);
        const auto json = host.Query<AnomalyJsonServiceV1>(ANOMALY_JSON_SERVICE_V1_ID);
        if (!profiles || profiles->struct_size < sizeof(AnomalyProfileServiceV1) ||
            profiles->load == nullptr || !json || json->struct_size < sizeof(AnomalyJsonServiceV1) ||
            json->release == nullptr || json->object_find == nullptr || json->array_item == nullptr ||
            json->array_size == nullptr || json->object_size == nullptr || json->object_key_at == nullptr ||
            json->number_value == nullptr || json->string_value == nullptr)
            throw std::runtime_error("profile and JSON services are unavailable");
        AnomalyGenerationHandleV1 handle{};
        detail::ProfileJsonNode::Check(profiles->load(profiles->user, StringView(filename), &handle));
        const detail::ProfileJsonNode root(json.get(), handle);
        Profile profile;
        profile.game = root.member("game").string();
        profile.module = root.member("module").string();
        profile.section = root.member("section").string();
        profile.contract = root.member("contract").string();
        profile.provenance = root.member("provenance").string();
        profile.unavailable = root.member("unavailable").string();
        profile.layout = root.member("layout").values();
        const auto symbols = root.member("symbols");
        const auto symbol_count = symbols.size();
        for (std::size_t i{}; i < symbol_count; ++i) {
            const auto source = symbols.item(i);
            ProfileSymbol symbol;
            symbol.name = source.member("name").string();
            const auto candidates = source.member("candidates");
            const auto candidate_count = candidates.size();
            for (std::size_t c{}; c < candidate_count; ++c) {
                const auto candidate_source = candidates.item(c);
                ProfileCandidate candidate;
                candidate.pattern = candidate_source.member("pattern").string();
                candidate.provenance = candidate_source.member("provenance").string();
                candidate.values = candidate_source.member("values").values();
                if (const auto call = candidate_source.optional_member("callOffset"))
                    candidate.call_offset = call->number();
                const auto entry = candidate_source.member("entry").string();
                for (std::size_t byte{}; byte < entry.size(); byte += 3) {
                    unsigned value{};
                    const auto parsed = std::from_chars(entry.data() + byte, entry.data() + byte + 2, value, 16);
                    if (parsed.ec != std::errc{} || parsed.ptr != entry.data() + byte + 2)
                        throw std::runtime_error("profile entry is not hexadecimal bytes");
                    candidate.entry.push_back(static_cast<std::uint8_t>(value));
                }
                symbol.candidates.push_back(std::move(candidate));
            }
            profile.symbols.push_back(std::move(symbol));
        }
        result.profile_ = std::move(profile);
    } catch (const std::exception& error) {
        result.error_ = error.what();
    }
    return result;
}

struct ProfileSymbolResult final {
    std::uintptr_t address{};
    const ProfileCandidate* candidate{};  // Borrowed from the supplied Profile.
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return address != 0; }
};

// Uses existing scoped signature and memory services. A match is published only
// when exactly one candidate passes its declared entry-byte check.
[[nodiscard]] inline ProfileSymbolResult ResolveProfileSymbol(
    const AnomalySignatureServiceV1* signatures, const AnomalyCoreServiceV1* core,
    const Profile& profile, std::string_view name) {
    if (signatures == nullptr || signatures->struct_size < sizeof(AnomalySignatureServiceV1) ||
        signatures->service_version < ANOMALY_SIGNATURE_SERVICE_V1_VERSION || signatures->resolve == nullptr ||
        core == nullptr || core->struct_size < offsetof(AnomalyCoreServiceV1, read_memory) + sizeof(core->read_memory) ||
        core->service_version < ANOMALY_CORE_SERVICE_V1_VERSION || core->read_memory == nullptr)
        return {0, nullptr, "signature or memory service is unavailable"};
    const ProfileSymbol* symbol{};
    for (const auto& item : profile.symbols) if (item.name == name) { symbol = &item; break; }
    if (symbol == nullptr) return {0, nullptr, "undeclared profile symbol"};
    ProfileSymbolResult result;
    for (const auto& candidate : symbol->candidates) {
        std::uintptr_t address{};
        const auto status = signatures->resolve(signatures->user, StringView(profile.module),
            StringView(profile.section), StringView(candidate.pattern), &address);
        if (status.code == ANOMALY_STATUS_V1_CONFLICT)
            return {0, nullptr, "profile signature is ambiguous"};
        if (!Succeeded(status) || address == 0) continue;
        if (candidate.call_offset) {
            std::array<std::uint8_t, 5> call{};
            if (!Succeeded(core->read_memory(core->user, address + *candidate.call_offset,
                    {call.data(), call.size()})) || call[0] != 0xE8) continue;
            std::int32_t displacement{};
            std::memcpy(&displacement, call.data() + 1, sizeof(displacement));
            address += *candidate.call_offset + call.size() + displacement;
        }
        if (candidate.entry.empty()) continue;
        std::vector<std::uint8_t> actual(candidate.entry.size());
        if (!Succeeded(core->read_memory(core->user, address, {actual.data(), actual.size()})) ||
            actual != candidate.entry) continue;
        if (result.address != 0) return {0, nullptr, "multiple profile candidates matched"};
        result.address = address;
        result.candidate = &candidate;
    }
    if (!result) result.error = "no profile candidate passed validation";
    return result;
}

}  // namespace anomaly::sdk
