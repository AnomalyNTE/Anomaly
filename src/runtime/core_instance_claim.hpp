#pragma once

#include <Windows.h>

#include <string>

namespace anomaly {

// A Core image has its own C++ globals, including RuntimeControl's mutex. The
// named kernel object arbitrates ownership across images in the same process.
// Retain it across session stops: process-lifetime hooks still belong to Core.
class CoreInstanceClaim final {
public:
    CoreInstanceClaim() = default;
    CoreInstanceClaim(const CoreInstanceClaim&) = delete;
    CoreInstanceClaim& operator=(const CoreInstanceClaim&) = delete;
    ~CoreInstanceClaim() { if (handle_) CloseHandle(handle_); }

    // Serialized by RuntimeControl; called outside Loader Lock before startup.
    DWORD Acquire() {
        if (handle_) return ERROR_SUCCESS;
        const auto name = L"Local\\Anomaly.Core.Owner." + std::to_wstring(GetCurrentProcessId());
        const HANDLE candidate = CreateMutexW(nullptr, FALSE, name.c_str());
        const DWORD error = GetLastError();
        if (!candidate) return error;
        if (error == ERROR_ALREADY_EXISTS) {
            CloseHandle(candidate);
            return ERROR_ALREADY_INITIALIZED;
        }
        handle_ = candidate;
        return ERROR_SUCCESS;
    }

private:
    HANDLE handle_{};
};

} // namespace anomaly
