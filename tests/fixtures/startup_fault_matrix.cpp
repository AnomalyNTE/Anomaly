#include "anomaly/runtime_session.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

enum class Fault {
    None,
    StopBeforeStart,
    InvalidExternalHandle,
    InitializeError,
    InitializeException,
    StopDuringInitialize,
    WorkerError,
    WorkerException,
};

struct Case {
    std::string_view name;
    Fault fault;
    DWORD expected_error;
};

anomaly::RuntimeStartContext StartContext() {
    anomaly::RuntimeStartContext context;
    context.bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_EXTERNAL;
    context.bootstrap_module = GetModuleHandleW(nullptr);
    context.game_module = GetModuleHandleW(nullptr);
    context.runtime_root = L".";
    context.log_directory = L".";
    return context;
}

DWORD HandleCount() {
    DWORD count{};
    return GetProcessHandleCount(GetCurrentProcess(), &count) != FALSE ? count : 0;
}

bool RunCase(const Case& fixture) {
    const DWORD handles_before = HandleCount();
    std::atomic_bool initialize_entered{};
    std::atomic_bool shutdown_called{};
    anomaly::RuntimeSessionOptions options;
    options.initialize = [&](std::stop_token stop_token) -> DWORD {
        initialize_entered.store(true, std::memory_order_release);
        if (fixture.fault == Fault::InitializeError) return ERROR_BAD_CONFIGURATION;
        if (fixture.fault == Fault::InitializeException) throw std::runtime_error("fixture");
        if (fixture.fault == Fault::StopDuringInitialize) {
            while (!stop_token.stop_requested()) std::this_thread::sleep_for(1ms);
            return ERROR_CANCELLED;
        }
        return ERROR_SUCCESS;
    };
    options.shutdown = [&] { shutdown_called.store(true, std::memory_order_release); };
    if (fixture.fault == Fault::WorkerError || fixture.fault == Fault::WorkerException) {
        options.workers.push_back({"fault-matrix", [&](std::stop_token) -> DWORD {
            if (fixture.fault == Fault::WorkerException) throw std::runtime_error("worker");
            return ERROR_BAD_COMMAND;
        }});
    }

    auto start = StartContext();
    HANDLE invalid{};
    if (fixture.fault == Fault::InvalidExternalHandle) {
        invalid = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CloseHandle(invalid);
        start.external_stop_event = invalid;
    }

    {
        anomaly::RuntimeSession session(std::move(start), std::move(options));
        if (fixture.fault == Fault::StopBeforeStart) session.RequestStop();
        const DWORD accepted = session.Start();
        if (fixture.fault == Fault::InvalidExternalHandle) {
            if (accepted == ERROR_SUCCESS) {
                std::cerr << fixture.name << ": invalid handle startup was accepted\n";
                return false;
            }
        } else if (fixture.fault == Fault::StopBeforeStart) {
            if (accepted != ERROR_CANCELLED) {
                std::cerr << fixture.name << ": pre-start stop did not cancel\n";
                return false;
            }
        } else if (accepted != ERROR_SUCCESS) {
            std::cerr << fixture.name << ": asynchronous startup was rejected\n";
            return false;
        }

        if (fixture.fault == Fault::StopDuringInitialize) {
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!initialize_entered.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            session.RequestStop();
        } else if (fixture.fault == Fault::None) {
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (session.Snapshot().state != ANOMALY_RUNTIME_STATE_RUNNING &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            session.RequestStop();
        }

        if (!session.WaitForStop(3s)) {
            std::cerr << fixture.name << ": did not converge to Stopped\n";
            return false;
        }
        session.Join();
        const auto snapshot = session.Snapshot();
        if (snapshot.state != ANOMALY_RUNTIME_STATE_STOPPED ||
            snapshot.last_error != fixture.expected_error ||
            session.Dispatchers().IsAccepting()) {
            std::cerr << fixture.name << ": final state/error/dispatcher mismatch, error="
                      << snapshot.last_error << '\n';
            return false;
        }
        const bool initialization_owned = fixture.fault != Fault::StopBeforeStart &&
            fixture.fault != Fault::InvalidExternalHandle;
        if (shutdown_called.load(std::memory_order_acquire) != initialization_owned) {
            std::cerr << fixture.name << ": shutdown ownership mismatch\n";
            return false;
        }
    }

    const DWORD handles_after = HandleCount();
    if (handles_before != 0 && handles_after > handles_before + 1U) {
        std::cerr << fixture.name << ": leaked process handles\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const std::vector<Case> cases = {
        {"clean-start-stop", Fault::None, ERROR_SUCCESS},
        {"stop-before-start", Fault::StopBeforeStart, ERROR_CANCELLED},
        {"invalid-external-handle", Fault::InvalidExternalHandle, ERROR_INVALID_HANDLE},
        {"initialize-error", Fault::InitializeError, ERROR_BAD_CONFIGURATION},
        {"initialize-exception", Fault::InitializeException, ERROR_UNHANDLED_EXCEPTION},
        {"stop-during-initialize", Fault::StopDuringInitialize, ERROR_SUCCESS},
        {"worker-error", Fault::WorkerError, ERROR_BAD_COMMAND},
        {"worker-exception", Fault::WorkerException, ERROR_UNHANDLED_EXCEPTION},
    };
    for (const auto& fixture : cases) {
        if (!RunCase(fixture)) return 1;
    }
    std::cout << "startup fault matrix passed cases=" << cases.size() << '\n';
    return 0;
}
