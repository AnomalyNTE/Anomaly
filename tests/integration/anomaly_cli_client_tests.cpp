#include "analyzer.hpp"
#include "anomaly/diagnostic_pipe_service.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::size_t kPayloadBytes = 1024U * 1024U;
constexpr DWORD kTimeoutMilliseconds = 10U * 1000U;

using Json = nlohmann::json;

struct ServerResult final {
    std::string request;
    DWORD error{ERROR_SUCCESS};
    bool response_sent{};
};

bool Check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

std::filesystem::path CreateTemporaryOutputPath() {
    std::array<wchar_t, MAX_PATH> directory{};
    const DWORD directory_size = GetTempPathW(
        static_cast<DWORD>(directory.size()), directory.data());
    if (directory_size == 0 || directory_size >= directory.size()) return {};

    std::array<wchar_t, MAX_PATH> path{};
    if (GetTempFileNameW(directory.data(), L"acl", 0, path.data()) == 0) return {};
    return path.data();
}

std::string ReadOutputFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool MatchesResponse(const std::string_view output, const std::string_view response) {
    if (output == response) return true;
    return output.size() == response.size() + 1U && output.ends_with("\r\n") &&
        response.ends_with('\n') &&
        output.substr(0, output.size() - 2U) == response.substr(0, response.size() - 1U);
}

void ServeMessageResponse(
    const HANDLE pipe,
    const std::string& response,
    ServerResult& result) {
    const BOOL connected = ConnectNamedPipe(pipe, nullptr);
    if (connected == FALSE && GetLastError() != ERROR_PIPE_CONNECTED) {
        result.error = GetLastError();
        CloseHandle(pipe);
        return;
    }

    std::array<char, 1024> request_buffer{};
    DWORD read{};
    if (ReadFile(
            pipe, request_buffer.data(), static_cast<DWORD>(request_buffer.size()), &read, nullptr) ==
        FALSE) {
        result.error = GetLastError();
        CloseHandle(pipe);
        return;
    }
    result.request.assign(request_buffer.data(), read);

    const DWORD response_size = static_cast<DWORD>(response.size());
    DWORD written{};
    if (WriteFile(pipe, response.data(), response_size, &written, nullptr) == FALSE) {
        result.error = GetLastError();
        CloseHandle(pipe);
        return;
    }
    if (written != response_size) {
        result.error = ERROR_WRITE_FAULT;
        CloseHandle(pipe);
        return;
    }
    result.response_sent = true;
    if (FlushFileBuffers(pipe) == FALSE) result.error = GetLastError();
    static_cast<void>(DisconnectNamedPipe(pipe));
    CloseHandle(pipe);
}

bool RunCli(
    const std::wstring& cli_path,
    const DWORD pipe_pid,
    const std::filesystem::path& output_path,
    HANDLE& process_handle,
    DWORD& launch_error) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    const HANDLE output = CreateFileW(
        output_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        launch_error = GetLastError();
        return false;
    }
    const HANDLE input = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        launch_error = GetLastError();
        CloseHandle(output);
        return false;
    }

    std::wstring command_line =
        L"\"" + cli_path + L"\" --pid " + std::to_wstring(pipe_pid) + L" status";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(
        cli_path.c_str(), command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process);
    CloseHandle(input);
    CloseHandle(output);
    if (started == FALSE) {
        launch_error = GetLastError();
        return false;
    }
    CloseHandle(process.hThread);
    process_handle = process.hProcess;
    return true;
}

bool CheckResponseMode(
    const std::wstring& cli_path,
    const DWORD pipe_pid,
    const std::string& response,
    const DWORD pipe_mode,
    const std::string_view mode_name) {
    const std::wstring pipe_name =
        L"\\\\.\\pipe\\LOCAL\\Anomaly-" + std::to_wstring(pipe_pid);
    const HANDLE pipe = CreateNamedPipeW(
        pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
        pipe_mode | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, static_cast<DWORD>(response.size()), 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::cerr << mode_name << " CreateNamedPipeW failed: " << GetLastError() << '\n';
        return false;
    }

    const auto output_path = CreateTemporaryOutputPath();
    if (output_path.empty()) {
        std::cerr << mode_name << " GetTempFileNameW failed: " << GetLastError() << '\n';
        CloseHandle(pipe);
        return false;
    }

    HANDLE process{};
    DWORD launch_error{};
    const bool started = RunCli(cli_path, pipe_pid, output_path, process, launch_error);
    if (!started) {
        std::cerr << mode_name << " anomaly-cli launch failed: " << launch_error << '\n';
        CloseHandle(pipe);
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        return false;
    }

    ServerResult server_result;
    std::thread server(ServeMessageResponse, pipe, std::cref(response), std::ref(server_result));
    const DWORD wait = WaitForSingleObject(process, kTimeoutMilliseconds);
    if (wait != WAIT_OBJECT_0) {
        launch_error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        static_cast<void>(TerminateProcess(process, ERROR_TIMEOUT));
        static_cast<void>(WaitForSingleObject(process, kTimeoutMilliseconds));
    }
    DWORD exit_code{};
    const BOOL exit_obtained = GetExitCodeProcess(process, &exit_code);
    if (exit_obtained == FALSE) launch_error = GetLastError();
    CloseHandle(process);
    server.join();

    const std::string output = ReadOutputFile(output_path);
    std::error_code ignored;
    std::filesystem::remove(output_path, ignored);
    const std::string prefix = std::string(mode_name) + " ";
    return Check(server_result.error == ERROR_SUCCESS, prefix + "server failed") &&
        Check(server_result.request == "status\n", prefix + "CLI did not write a complete request") &&
        Check(server_result.response_sent, prefix + "server did not send a response") &&
        Check(wait == WAIT_OBJECT_0 && exit_obtained != FALSE, prefix + "CLI did not exit cleanly") &&
        Check(exit_code == 0, prefix + "CLI rejected a complete response") &&
        Check(MatchesResponse(output, response), prefix + "CLI truncated a multi-chunk response");
}

bool CheckRealService(const std::wstring& cli_path, const DWORD pipe_pid) {
    ue5mem::AnalyzerConfig config;
    const auto analyzer = std::make_shared<const ue5mem::Analyzer>(
        std::filesystem::current_path(), config);
    const std::wstring pipe_name =
        L"\\\\.\\pipe\\LOCAL\\Anomaly-" + std::to_wstring(pipe_pid);
    anomaly::DiagnosticPipeService service({analyzer, pipe_name});
    if (!Check(service.Prepare() == ERROR_SUCCESS, "real service Prepare failed")) {
        return false;
    }

    const auto output_path = CreateTemporaryOutputPath();
    if (output_path.empty()) {
        std::cerr << "real service GetTempFileNameW failed: " << GetLastError() << '\n';
        service.Stop();
        return false;
    }

    std::atomic<DWORD> run_result{ERROR_GEN_FAILURE};
    std::jthread server([&] { run_result.store(service.Run()); });
    HANDLE process{};
    DWORD launch_error{};
    const bool started = RunCli(cli_path, pipe_pid, output_path, process, launch_error);
    if (!started) {
        std::cerr << "real service anomaly-cli launch failed: " << launch_error << '\n';
        service.Stop();
        server.join();
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        return false;
    }

    const DWORD wait = WaitForSingleObject(process, kTimeoutMilliseconds);
    if (wait != WAIT_OBJECT_0) {
        launch_error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        static_cast<void>(TerminateProcess(process, ERROR_TIMEOUT));
        static_cast<void>(WaitForSingleObject(process, kTimeoutMilliseconds));
    }
    DWORD exit_code{};
    const BOOL exit_obtained = GetExitCodeProcess(process, &exit_code);
    if (exit_obtained == FALSE) launch_error = GetLastError();
    CloseHandle(process);

    service.Stop();
    server.join();
    const std::string output = ReadOutputFile(output_path);
    std::error_code ignored;
    std::filesystem::remove(output_path, ignored);
    const Json response = Json::parse(output, nullptr, false);
    const bool response_valid = response.is_object() &&
        response.value("ok", false) && response.value("pid", 0U) == pipe_pid &&
        !response.contains("protocol") && !response.contains("result");

    return Check(wait == WAIT_OBJECT_0 && exit_obtained != FALSE,
                 "CLI did not exit after calling the real service") &&
        Check(exit_code == 0, "CLI rejected the real service response") &&
        Check(run_result.load() == ERROR_SUCCESS, "real service did not stop cleanly") &&
        Check(response_valid, "real service response did not preserve the CLI JSON contract");
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "usage: anomaly_cli_client_tests <anomaly-cli path>\n";
        return 2;
    }

    const DWORD pipe_pid = GetCurrentProcessId();
    const bool real_service = CheckRealService(argv[1], pipe_pid);
    const std::string response =
        "{\"ok\":true,\"payload\":\"" + std::string(kPayloadBytes, 'x') + "\"}\n";
    const bool byte_mode = CheckResponseMode(
        argv[1], pipe_pid, response, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, "byte-mode");
    const bool message_mode = CheckResponseMode(
        argv[1], pipe_pid, response, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE, "message-mode");
    return real_service && byte_mode && message_mode ? 0 : 6;
}
