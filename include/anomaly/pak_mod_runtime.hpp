#pragma once

#include "anomaly/structured_logger.hpp"
#include "anomaly/ue5_pak.hpp"
#include "anomaly/sdk/services/mods.h"

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

inline constexpr std::string_view kPakModRuntimeServiceId = "anomaly.internal.pak-mods";
inline constexpr std::string_view kPakModConfigName = "pak-mods.json";
inline constexpr std::string_view kPakModStatusName = "pak-mods.json";

enum class PakModState : std::uint8_t {
    // Discovered, but not mounted yet (engine not ready, or the host refused the container).
    Pending,
    Mounted,
    // Released on request so the container file can be replaced; the next change to its
    // fingerprint mounts the replacement without a restart.
    Released,
    Failed,
};

struct PakModEntrySnapshot {
    std::string id;
    std::filesystem::path file;
    std::uint32_t order{};
    std::uint64_t bytes{};
    std::uint64_t stamp{};
    std::string fingerprint;
    PakModState state{PakModState::Pending};
    std::string message;
};

// The engine's mount verdict and transferred strong reference.
struct PakModMountOutcome {
    bool mounted{};
    std::string detail;
    std::uintptr_t container{};
};

// Verdict of a repeat priming pass requested through the diagnostic pipe.
struct PakModPrimeOutcome {
    bool primed{};
    std::string stage{"unavailable"};
    std::string detail;
};

struct PakModRuntimeOptions {
    std::filesystem::path runtime_root;
    std::vector<std::filesystem::path> settings_paths;
    // Compatibility mode reports status only; the startup bypass and engine own all mounts.
    bool compatibility_mode{};
    bool startup_enabled{true};
    // Directory the engine scans for paks; mod configs and the watcher work under it.
    std::filesystem::path mod_directory;
    // UE resolves relative mounted filenames against the executable directory.
    std::filesystem::path engine_base_directory;
    std::uint32_t base_order{100000};
    // Empty mounts a container at the mount point stored inside it.
    std::wstring mount_point;
    std::chrono::milliseconds poll_interval{750};
    // Engine access. Returning 0 means "not available yet"; the watcher retries instead of
    // caching the failure, because the platform file chain only exists after engine start-up.
    std::function<std::uintptr_t()> resolve_pak_platform_file;
    std::function<PakModMountOutcome(std::uintptr_t, const Ue5PakMountRequest&)> mount_pak;
    std::function<bool(std::uintptr_t, const std::wstring&)> unmount_pak;
    std::function<std::optional<std::vector<Ue5MountedPakFile>>(std::uintptr_t)> mounted_files;
    std::function<void(std::uintptr_t)> release_container;
    // Detaches and drains the reporting sink before this runtime is destroyed.
    std::function<void()> remove_cache_verify;
    // Re-runs the post-mount priming read for one already mounted container. Diagnostic only: it
    // never mounts or unmounts, so it can be driven from the pipe without restarting the game.
    std::function<PakModPrimeOutcome(std::uintptr_t, std::uintptr_t)> prime_pak;
    // Installs the block-cache verifier detour once per session and reports its status through the
    // engine's own vocabulary. The sink is invoked from the verifier's thread for every request the
    // detour sees; the runtime ignores healthy samples and caps first-failure diagnostics.
    std::function<Ue5PakCacheVerifyStatus(Ue5PakCacheVerifySink, void*, std::uintptr_t&)>
        apply_cache_verify;
    // Clears the engine's container signature requirement once per session, before the first
    // mount. Without it an unsigned mod container is rejected as "pak is invalid", which is what
    // the pattern-based startup bypass used to cover.
    std::function<bool(std::uintptr_t)> clear_signature_requirement;
    std::function<void(LogLevel, std::string_view)> log;
};

// Owns runtime pak mods: discovers containers, mounts them, and keeps the mounted set in step with
// the directory and with `config/pak-mods.json`.
//
// Hot replacement is a release/mount handshake rather than a rewrite in place: a mounted container
// keeps its file handle open, so the file is unmounted first, the caller replaces it, and the next
// scan mounts the new fingerprint. Every path that mounts or unmounts serializes on one mutex, so
// the watcher and the diagnostic pipe can drive it concurrently without interleaving engine calls.
class PakModRuntime final {
public:
    explicit PakModRuntime(PakModRuntimeOptions options);
    ~PakModRuntime();

    PakModRuntime(const PakModRuntime&) = delete;
    PakModRuntime& operator=(const PakModRuntime&) = delete;

    // Scans once, then keeps polling until Stop. Returns false only when the watcher thread could
    // not start; an unavailable engine is a degraded start, not a failure.
    bool Start();
    // Joins the watcher and attempts to unmount every owned container. Native engine calls are
    // synchronous and cannot be cancelled; this operation has no bounded timeout.
    void Stop() noexcept;

    [[nodiscard]] PakModRuntimeOptions Options() const;
    [[nodiscard]] std::string StatusJson() const;
    [[nodiscard]] const AnomalyModsServiceV1* Service() const noexcept;
    // "status", "reload", "mount <path> [order]", "release <id>", "unmount <id>", "prime <id>". Runs
    // the engine calls on the calling thread; used by the diagnostic pipe.
    [[nodiscard]] std::string Execute(std::string_view arguments) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
