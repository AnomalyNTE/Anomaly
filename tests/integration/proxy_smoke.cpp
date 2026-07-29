#include "anomaly/core_api.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

namespace {

std::string Query(const std::wstring& pipe_name, std::string request) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 60; ++attempt) {
        pipe = CreateFileW(
            pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        Sleep(100);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        std::cerr << "CreateFileW pipe error: " << GetLastError() << '\n';
        return {};
    }

    request = nlohmann::json{
        {"protocol", "anomaly.diagnostics"},
        {"version", 1},
        {"type", "request"},
        {"id", "proxy-smoke"},
        {"command", request},
    }.dump();
    request.push_back('\n');
    DWORD written{};
    if (!WriteFile(pipe, request.data(), static_cast<DWORD>(request.size()), &written, nullptr)) {
        CloseHandle(pipe);
        return {};
    }

    std::string response;
    std::array<char, 4096> buffer{};
    DWORD read{};
    while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read != 0) {
        response.append(buffer.data(), read);
    }
    CloseHandle(pipe);
    return response;
}

bool IsOk(const std::string& response) {
    return response.find("\"ok\":true") != std::string::npos;
}

bool Check(const std::wstring& pipe_name, const char* command) {
    const auto response = Query(pipe_name, command);
    if (IsOk(response)) return true;
    std::cerr << command << " response: " << (response.empty() ? "<empty>" : response) << '\n';
    return false;
}

std::optional<std::uintptr_t> AddressFrom(const std::string& response) {
    constexpr std::string_view marker = "\"address\":\"0x";
    const auto begin = response.find(marker);
    if (begin == std::string::npos) return std::nullopt;
    const auto digits = begin + marker.size();
    const auto end = response.find('"', digits);
    if (end == std::string::npos) return std::nullopt;
    try {
        return static_cast<std::uintptr_t>(std::stoull(response.substr(digits, end - digits), nullptr, 16));
    } catch (...) {
        return std::nullopt;
    }
}

bool WaitForState(
    AnomalyGetStateFn get_state, AnomalyRuntimeState expected, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        AnomalyRuntimeStateInfo state_info{
            .struct_size = ANOMALY_RUNTIME_STATE_INFO_V1_SIZE,
            .state_info_version = ANOMALY_RUNTIME_STATE_INFO_VERSION,
        };
        const DWORD result = get_state(&state_info);
        if (result != ERROR_SUCCESS) {
            std::cerr << "AnomalyGetState error: " << result << '\n';
            return false;
        }
        if (state_info.state == expected) return true;
        if (GetTickCount64() >= deadline) {
            std::cerr << "runtime state: " << state_info.state
                      << ", expected: " << expected << '\n';
            return false;
        }
        Sleep(10);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const HMODULE proxy = LoadLibraryW(argv[1]);
    if (proxy == nullptr) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return 3;
    }

    using IsCompositionEnabled = HRESULT(WINAPI*)(BOOL*);
    const auto function = reinterpret_cast<IsCompositionEnabled>(
        GetProcAddress(proxy, "DwmIsCompositionEnabled"));
    if (function == nullptr) return 4;
    BOOL enabled{};
    static_cast<void>(function(&enabled));

    const std::wstring pipe_name =
        L"\\\\.\\pipe\\LOCAL\\Anomaly-" + std::to_wstring(GetCurrentProcessId());
    if (!Check(pipe_name, "ping")) return 5;
    if (!Check(pipe_name, "modules")) return 6;
    if (!Check(pipe_name, "sections .")) return 7;
    if (!Check(pipe_name, "regions .")) return 8;
    if (!Check(pipe_name, "scan . .text 4?")) return 9;
    if (!Check(pipe_name, "ue")) return 10;
    const auto allocation_response = Query(pipe_name, "alloc 64 rw");
    const auto allocation = AddressFrom(allocation_response);
    if (!allocation) return 11;
    const auto address = std::to_string(*allocation);
    if (!Check(pipe_name, ("write " + address + " DE AD BE EF").c_str())) return 12;
    if (Query(pipe_name, "read " + address + " 4").find("de ad be ef") == std::string::npos) return 13;
    if (!Check(pipe_name, ("protect " + address + " 64 r").c_str())) return 14;
    if (!Check(pipe_name, ("patch " + address + " AA BB").c_str())) return 15;
    if (Query(pipe_name, "read " + address + " 2").find("aa bb") == std::string::npos) return 16;
    if (!Check(pipe_name, ("free " + address).c_str())) return 17;

    const HMODULE core = GetModuleHandleW(L"Anomaly.Core.dll");
    if (core == nullptr) {
        std::cerr << "Anomaly.Core.dll is not loaded: " << GetLastError() << '\n';
        return 18;
    }
    const auto get_state = reinterpret_cast<AnomalyGetStateFn>(
        GetProcAddress(core, ANOMALY_CORE_GET_STATE_ENTRY));
    const auto request_stop = reinterpret_cast<AnomalyRequestStopFn>(
        GetProcAddress(core, ANOMALY_CORE_REQUEST_STOP_ENTRY));
    const auto wait_for_stop = reinterpret_cast<AnomalyWaitForStopFn>(
        GetProcAddress(core, ANOMALY_CORE_WAIT_FOR_STOP_ENTRY));
    if (get_state == nullptr || request_stop == nullptr || wait_for_stop == nullptr) {
        std::cerr << "runtime lifecycle export is missing\n";
        return 19;
    }
    if (!WaitForState(get_state, ANOMALY_RUNTIME_STATE_RUNNING, 2000)) return 20;
    const DWORD stop_result = request_stop();
    if (stop_result != ERROR_SUCCESS) {
        std::cerr << "AnomalyRequestStop error: " << stop_result << '\n';
        return 21;
    }
    const DWORD wait_result = wait_for_stop(2000);
    if (wait_result != ERROR_SUCCESS) {
        std::cerr << "AnomalyWaitForStop error: " << wait_result << '\n';
        return 22;
    }
    if (!WaitForState(get_state, ANOMALY_RUNTIME_STATE_STOPPED, 0)) return 23;
    return 0;
}
