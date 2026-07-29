#pragma once

#include <Windows.h>

#include <memory>
#include <stop_token>
#include <string>

namespace ue5mem {
class Analyzer;
}

namespace anomaly {

struct PipeServiceOptions final {
    std::shared_ptr<const ue5mem::Analyzer> analyzer;
    std::wstring pipe_name;
};

class DiagnosticPipeService final {
public:
    explicit DiagnosticPipeService(PipeServiceOptions options);
    ~DiagnosticPipeService();

    DiagnosticPipeService(const DiagnosticPipeService&) = delete;
    DiagnosticPipeService& operator=(const DiagnosticPipeService&) = delete;
    DiagnosticPipeService(DiagnosticPipeService&&) = delete;
    DiagnosticPipeService& operator=(DiagnosticPipeService&&) = delete;

    // Prepares the ACL, stop event, and first listener without accepting clients.
    [[nodiscard]] DWORD Prepare() noexcept;

    // Runs on the calling thread. The owner must wait for this call before destruction.
    [[nodiscard]] DWORD Run(std::stop_token stop_token = {}) noexcept;

    // May be called repeatedly and from another thread. This call does not wait for Run.
    void Stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
