#pragma once

#include "anomaly/build_profile.hpp"
#include "anomaly/pattern_service.hpp"

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

struct SymbolMemoryRegion {
    std::uintptr_t base{};
    std::size_t size{};
    DWORD state{};
    DWORD protection{};
    DWORD type{};
};

class SymbolMemory {
public:
    virtual ~SymbolMemory() = default;
    [[nodiscard]] virtual std::optional<ue5mem::ModuleInfo> FindModule(
        std::wstring_view name) const = 0;
    [[nodiscard]] virtual std::vector<ue5mem::SectionInfo> Sections(
        const ue5mem::ModuleInfo& module) const = 0;
    [[nodiscard]] virtual std::vector<std::uintptr_t> Scan(
        const ue5mem::ModuleInfo& module,
        std::string_view section,
        std::string_view pattern,
        std::size_t limit) const = 0;
    [[nodiscard]] virtual bool Read(
        std::uintptr_t address,
        void* destination,
        std::size_t size) const = 0;
    // Resolution is read-only by default. A narrowly scoped semantic service may
    // opt into this operation after its own exact-Profile and thread checks.
    [[nodiscard]] virtual bool Write(
        std::uintptr_t,
        const void*,
        std::size_t) const {
        return false;
    }
    [[nodiscard]] virtual std::optional<SymbolMemoryRegion> Query(
        std::uintptr_t address) const = 0;
};

class LiveSymbolMemory final : public SymbolMemory {
public:
    explicit LiveSymbolMemory(CoreMemoryServices services = {});

    [[nodiscard]] std::optional<ue5mem::ModuleInfo> FindModule(
        std::wstring_view name) const override;
    [[nodiscard]] std::vector<ue5mem::SectionInfo> Sections(
        const ue5mem::ModuleInfo& module) const override;
    [[nodiscard]] std::vector<std::uintptr_t> Scan(
        const ue5mem::ModuleInfo& module,
        std::string_view section,
        std::string_view pattern,
        std::size_t limit) const override;
    [[nodiscard]] bool Read(
        std::uintptr_t address,
        void* destination,
        std::size_t size) const override;
    [[nodiscard]] bool Write(
        std::uintptr_t address,
        const void* source,
        std::size_t size) const override;
    [[nodiscard]] std::optional<SymbolMemoryRegion> Query(
        std::uintptr_t address) const override;

private:
    CoreMemoryServices services_;
};

struct SymbolValidationResult {
    bool valid{};
    std::string message;
};

class SymbolValidatorRegistry final {
public:
    using Validator = std::function<SymbolValidationResult(
        const BuildProfile&,
        const ProfileSymbol&,
        const ue5mem::ModuleInfo&,
        std::uintptr_t,
        const SymbolMemory&)>;

    SymbolValidatorRegistry();
    void Register(std::string id, Validator validator);
    [[nodiscard]] SymbolValidationResult Validate(
        std::string_view id,
        const BuildProfile& profile,
        const ProfileSymbol& symbol,
        const ue5mem::ModuleInfo& module,
        std::uintptr_t address,
        const SymbolMemory& memory) const;

private:
    std::map<std::string, Validator, std::less<>> validators_;
};

enum class SymbolResolutionState : std::uint8_t {
    Unavailable,
    Resolved,
    ModuleMissing,
    SectionMissing,
    PatternInvalid,
    NotFound,
    Ambiguous,
    AddressResolutionFailed,
    ValidationFailed,
};

struct ResolvedSymbol {
    std::string id;
    SymbolResolutionState state{SymbolResolutionState::Unavailable};
    std::wstring module;
    std::uintptr_t instruction{};
    std::uintptr_t address{};
    std::uint64_t rva{};
    std::size_t candidate_count{};
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool Available() const noexcept {
        return state == SymbolResolutionState::Resolved;
    }
};

struct FeatureResolution {
    std::string id;
    bool available{};
    // The profile contract is structurally valid, but a game-owned object
    // needed by a layout validator has not been initialized yet. The adapter
    // retries these validators on the game thread.
    bool deferred_validation{};
    std::vector<std::string> missing_symbols;
    std::vector<std::string> unavailable_dependencies;
    std::vector<std::string> validation_diagnostics;
};

enum class ProfileResolutionState : std::uint8_t {
    NoProfile,
    ProfileLoaded,
    Degraded,
    Ready,
};

struct ProfileResolutionSnapshot {
    ProfileResolutionState state{ProfileResolutionState::NoProfile};
    std::string build_id;
    std::string profile_hash;
    std::chrono::microseconds duration{};
    std::map<std::string, ResolvedSymbol, std::less<>> symbols;
    std::map<std::string, FeatureResolution, std::less<>> features;

    [[nodiscard]] const ResolvedSymbol* FindSymbol(std::string_view id) const noexcept;
    [[nodiscard]] bool FeatureAvailable(std::string_view id) const noexcept;
};

struct FeatureValidationResult {
    bool valid{};
    std::string message;
    bool deferred{};
};

class FeatureLayoutValidatorRegistry final {
public:
    using Validator = std::function<FeatureValidationResult(
        const BuildProfile&,
        std::string_view,
        const ProfileResolutionSnapshot&,
        const SymbolMemory&)>;

    FeatureLayoutValidatorRegistry();
    void Register(std::string id, Validator validator);
    [[nodiscard]] FeatureValidationResult Validate(
        std::string_view id,
        const BuildProfile& profile,
        std::string_view feature_id,
        const ProfileResolutionSnapshot& snapshot,
        const SymbolMemory& memory) const;

private:
    std::map<std::string, Validator, std::less<>> validators_;
};

struct SymbolResolverOptions {
    std::size_t maximum_candidates{16};
};

class SymbolResolver final {
public:
    SymbolResolver(
        std::shared_ptr<const SymbolMemory> memory,
        SymbolValidatorRegistry validators = {},
        SymbolResolverOptions options = {},
        FeatureLayoutValidatorRegistry feature_layout_validators = {});

    [[nodiscard]] ProfileResolutionSnapshot Resolve(
        const BuildFingerprint& fingerprint,
        const BuildProfile* profile) const;

    // Retries validators for an already unique, resolved candidate without
    // scanning module text. This supports UE
    // globals whose storage is allocated after the initial profile startup.
    [[nodiscard]] bool RevalidateDeferredCandidates(
        const BuildProfile& profile,
        ProfileResolutionSnapshot& snapshot) const;

private:
    std::shared_ptr<const SymbolMemory> memory_;
    SymbolValidatorRegistry validators_;
    SymbolResolverOptions options_;
    FeatureLayoutValidatorRegistry feature_layout_validators_;
};

}  // namespace anomaly
