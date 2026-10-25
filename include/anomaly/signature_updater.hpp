#pragma once

#include "memory.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace anomaly {

// Synchronous read/scan ports shared by the Profile resolver and plugin signature
// service. The caller supplies a declared executable section, never a game object.
struct SignatureScanContext {
    ue5mem::ModuleInfo module;
    ue5mem::SectionInfo section;
    std::function<bool(std::uintptr_t, void*, std::size_t)> read;
    std::function<std::vector<std::uintptr_t>(std::string_view, std::size_t)> scan;
};

struct SignatureRecovery {
    std::uintptr_t instruction{};
    std::string pattern;
    std::string diagnostic;
};

struct SignatureLayoutField {
    std::string id;
    std::uint32_t seed_offset{};
    std::uint32_t alignment{1};
};

struct SignatureLayoutMember {
    std::size_t field_index{};
    std::int32_t addend{};
    std::uint32_t width{};
    std::uint32_t access{};
};

struct SignatureLayoutWitness {
    std::string id;
    std::string pattern;
    std::vector<SignatureLayoutMember> members;
};

struct SignatureLayoutDefinition {
    std::vector<SignatureLayoutField> fields;
    std::vector<SignatureLayoutWitness> witnesses;
};

struct SignatureLayoutResolution {
    std::vector<std::uintptr_t> addresses;
    std::vector<std::uint32_t> offsets;
    std::string diagnostic;
    bool conflict{};
    [[nodiscard]] bool Ok() const noexcept { return !addresses.empty(); }
};

// Learns instruction structure from successful resolutions. Relative branches
// and RIP addresses can move; member and stack displacements stay fixed so
// existing consumers can keep using their original field offsets.
// Recovery requires an exact, unique match of the remaining instruction bytes;
// the caller must still run its address/ABI/semantic validators before Learn.
// Call from Worker/Lifecycle only: these operations scan memory and persist state.
class SignatureUpdater final {
public:
    explicit SignatureUpdater(std::filesystem::path state_directory);
    ~SignatureUpdater();
    SignatureUpdater(const SignatureUpdater&) = delete;
    SignatureUpdater& operator=(const SignatureUpdater&) = delete;

    [[nodiscard]] SignatureRecovery Recover(
        const SignatureScanContext& context, std::string_view contract,
        std::string_view original_pattern, std::stop_token stop = {});
    [[nodiscard]] std::string Learn(
        const SignatureScanContext& context, std::string_view contract,
        std::string_view original_pattern, std::uintptr_t instruction,
        std::stop_token stop = {});

    // Opt-in member learning. Resolves the entire declaration before publishing
    // any field; unlike the address-only API, declared member operands may move.
    [[nodiscard]] SignatureLayoutResolution ResolveLayout(
        const SignatureScanContext& context, const SignatureLayoutDefinition& definition,
        std::stop_token stop = {});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
