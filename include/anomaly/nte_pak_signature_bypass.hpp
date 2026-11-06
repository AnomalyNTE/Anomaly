#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace anomaly {

// Optional pak-signature bypass for NTE.
//
// The game refuses to mount pak mods that carry no signature, and the check sits behind a jmp
// thunk. This module locates that thunk from a pattern that lives in
// `<runtime>/config/pak-signature-bypass.json` (data, not code), follows the jump to the real
// function and installs a hook that always reports "valid". The hook is owned by a private
// MinHook backend so it can be removed again, unlike the byte patch the community loaders use.
//
// Timing matters: the engine mounts its paks during start-up, so Start() has to be called as early
// as possible — it is invoked from the core entry point, before configuration, services and
// plugins come up. The scan runs on its own thread and polls until the packed image is decrypted.
class NtePakSignatureBypass final {
public:
    NtePakSignatureBypass();
    ~NtePakSignatureBypass();

    NtePakSignatureBypass(const NtePakSignatureBypass&) = delete;
    NtePakSignatureBypass& operator=(const NtePakSignatureBypass&) = delete;

    // Creates the default config file (disabled) when it is missing and starts the worker when it
    // is enabled. `log_directory` receives pak-signature-bypass.log until the main logger is up.
    void Start(const std::filesystem::path& runtime_root, HMODULE game_module,
        const std::filesystem::path& log_directory);

    // Removes the hook and stops the worker.
    void Stop();

    [[nodiscard]] bool Patched() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
