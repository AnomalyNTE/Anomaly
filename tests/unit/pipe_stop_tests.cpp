#include "analyzer.hpp"
#include "anomaly/diagnostic_pipe_service.hpp"
#include "pipe_server.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

bool Check(bool condition, std::string_view message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

template <typename Predicate>
bool WaitUntil(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        Sleep(10);
    }
    return predicate();
}

bool WaitUntilAvailable(const std::wstring& pipe_name) {
    return WaitUntil([&] {
        if (WaitNamedPipeW(pipe_name.c_str(), 50) != FALSE) return true;
        return false;
    });
}

bool WaitUntilUnavailable(const std::wstring& pipe_name) {
    return WaitUntil([&] {
        if (WaitNamedPipeW(pipe_name.c_str(), 0) != FALSE) return false;
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    });
}

HANDLE OpenPipe(const std::wstring& pipe_name, DWORD& open_error) {
    HANDLE result = INVALID_HANDLE_VALUE;
    const bool opened = WaitUntil([&] {
        result = CreateFileW(
            pipe_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (result != INVALID_HANDLE_VALUE) return true;

        open_error = GetLastError();
        if (open_error == ERROR_PIPE_BUSY) {
            static_cast<void>(WaitNamedPipeW(pipe_name.c_str(), 50));
        }
        return false;
    });
    return opened ? result : INVALID_HANDLE_VALUE;
}

bool ExchangeRequest(
    const std::wstring& pipe_name,
    std::string_view request,
    std::string& response,
    DWORD& client_error,
    std::chrono::milliseconds read_delay = 0ms) {
    const HANDLE client = OpenPipe(pipe_name, client_error);
    if (client == INVALID_HANDLE_VALUE) return false;

    DWORD written{};
    bool result = WriteFile(
                      client,
                      request.data(),
                      static_cast<DWORD>(request.size()),
                      &written,
                      nullptr) != FALSE &&
        written == request.size();
    if (result) {
        if (read_delay.count() > 0) {
            Sleep(static_cast<DWORD>(read_delay.count()));
        }
        std::array<char, 64 * 1024> buffer{};
        DWORD read{};
        for (;;) {
            if (ReadFile(
                    client,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr) == FALSE) {
                client_error = GetLastError();
                result = client_error == ERROR_BROKEN_PIPE ||
                    client_error == ERROR_NO_DATA ||
                    client_error == ERROR_PIPE_NOT_CONNECTED;
                break;
            }
            if (read == 0) break;
            response.append(buffer.data(), read);
        }
    } else {
        client_error = GetLastError();
    }
    CloseHandle(client);
    return result;
}

bool StopServiceWithinTwoSeconds(
    anomaly::DiagnosticPipeService& service,
    std::jthread& server,
    const std::atomic<DWORD>& run_result) {
    const auto started = std::chrono::steady_clock::now();
    service.Stop();
    service.Stop();
    server.join();
    return run_result.load() == ERROR_SUCCESS &&
        std::chrono::steady_clock::now() - started < 2s;
}

bool StopServerWithinTwoSeconds(std::jthread& server) {
    const auto started = std::chrono::steady_clock::now();
    server.request_stop();
    server.join();
    return std::chrono::steady_clock::now() - started < 2s;
}

bool CheckPrepareValidation(const std::shared_ptr<const ue5mem::Analyzer>& analyzer) {
    const std::wstring valid_name =
        ue5mem::BuildPipeName(L"AnomalyPipeInvalidOptions", GetCurrentProcessId());
    anomaly::DiagnosticPipeService missing_analyzer({{}, valid_name});
    anomaly::DiagnosticPipeService missing_name({analyzer, {}});
    return Check(
               missing_analyzer.Prepare() == ERROR_INVALID_PARAMETER,
               "Prepare accepted a missing analyzer") &&
        Check(
               missing_name.Prepare() == ERROR_INVALID_PARAMETER,
               "Prepare accepted an empty pipe name") &&
        Check(
               missing_name.Run() == ERROR_INVALID_STATE,
               "Run without a successful Prepare returned the wrong error");
}

bool CheckRepeatedPrepareAndEndpoint(
    const std::shared_ptr<const ue5mem::Analyzer>& analyzer,
    const std::wstring& pipe_name) {
    anomaly::DiagnosticPipeService service({analyzer, pipe_name});
    bool result = Check(service.Prepare() == ERROR_SUCCESS, "Prepare failed") &&
        Check(
            service.Prepare() == ERROR_ALREADY_INITIALIZED,
            "repeated Prepare returned the wrong error") &&
        Check(WaitUntilAvailable(pipe_name), "Prepare did not publish the first endpoint");

    service.Stop();
    result = Check(
                 service.Prepare() == ERROR_OPERATION_ABORTED,
                 "Prepare restarted a stopped single-use service") &&
        Check(WaitUntilUnavailable(pipe_name), "Stop left the prepared endpoint visible") &&
        result;
    return result;
}

bool CheckStopBeforeRun(
    const std::shared_ptr<const ue5mem::Analyzer>& analyzer,
    const std::wstring& pipe_name) {
    anomaly::DiagnosticPipeService service({analyzer, pipe_name});
    if (!Check(service.Prepare() == ERROR_SUCCESS, "stop-before-Run Prepare failed")) {
        return false;
    }
    service.Stop();
    const auto started = std::chrono::steady_clock::now();
    return Check(
               service.Run() == ERROR_SUCCESS,
               "Run after Stop did not converge to success") &&
        Check(
               std::chrono::steady_clock::now() - started < 2s,
               "Run after Stop did not return promptly") &&
        Check(
               WaitUntilUnavailable(pipe_name),
               "stop-before-Run left the endpoint visible");
}

bool CheckIdleConnectStop(
    const std::shared_ptr<const ue5mem::Analyzer>& analyzer,
    const std::wstring& pipe_name) {
    anomaly::DiagnosticPipeService service({analyzer, pipe_name});
    if (!Check(service.Prepare() == ERROR_SUCCESS, "idle-stop Prepare failed")) return false;

    std::atomic_bool run_entered{};
    std::atomic<DWORD> run_result{ERROR_GEN_FAILURE};
    std::jthread server([&] {
        run_entered.store(true);
        run_result.store(service.Run());
    });
    const bool entered = WaitUntil([&] { return run_entered.load(); });
    if (entered) Sleep(50);
    const bool stopped = StopServiceWithinTwoSeconds(service, server, run_result);
    return Check(entered, "idle-stop Run thread did not begin") &&
        Check(stopped, "idle ConnectNamedPipe stop exceeded two seconds");
}

bool CheckPartialRequestStop(
    const std::shared_ptr<const ue5mem::Analyzer>& analyzer,
    const std::wstring& pipe_name) {
    anomaly::DiagnosticPipeService service({analyzer, pipe_name});
    if (!Check(service.Prepare() == ERROR_SUCCESS, "partial-stop Prepare failed")) {
        return false;
    }

    std::atomic<DWORD> run_result{ERROR_GEN_FAILURE};
    std::jthread server([&] { run_result.store(service.Run()); });
    DWORD open_error{};
    const HANDLE client = OpenPipe(pipe_name, open_error);
    if (client == INVALID_HANDLE_VALUE) {
        service.Stop();
        server.join();
        std::cerr << "partial-request client open failed: " << open_error << '\n';
        return false;
    }

    constexpr char request[] = "partial request";
    DWORD written{};
    const bool request_consumed =
        WriteFile(client, request, sizeof(request) - 1, &written, nullptr) != FALSE &&
        written == sizeof(request) - 1 && FlushFileBuffers(client) != FALSE;
    const bool stopped = StopServiceWithinTwoSeconds(service, server, run_result);
    CloseHandle(client);
    return Check(request_consumed, "server did not consume the partial request") &&
        Check(stopped, "pending request ReadFile stop exceeded two seconds");
}

bool CheckCompletedRequestStop(
    const std::shared_ptr<const ue5mem::Analyzer>& analyzer,
    const std::wstring& pipe_name) {
    anomaly::DiagnosticPipeService service({analyzer, pipe_name});
    if (!Check(service.Prepare() == ERROR_SUCCESS, "completed-stop Prepare failed")) {
        return false;
    }

    std::atomic<DWORD> run_result{ERROR_GEN_FAILURE};
    std::jthread server([&] { run_result.store(service.Run()); });
    DWORD client_error{};
    std::string response;
    const bool exchanged = ExchangeRequest(pipe_name,
        "{\"protocol\":\"anomaly.diagnostics\",\"version\":1,\"id\":\"stop\",\"command\":\"ping\"}\n",
        response, client_error);
    const bool stopped = StopServiceWithinTwoSeconds(service, server, run_result);
    return Check(exchanged, "completed request exchange failed") &&
        Check(
            response.find("\"ok\":true") != std::string::npos,
            "completed request response was truncated") &&
        Check(stopped, "completed request stop exceeded two seconds");
}

bool JsonStringEquals(
    const Json& value,
    std::string_view name,
    std::string_view expected) {
    if (!value.is_object()) return false;
    const auto member = value.find(name);
    return member != value.end() && member->is_string() &&
        member->get_ref<const std::string&>() == expected;
}

bool JsonErrorCodeEquals(const Json& value, std::string_view expected) {
    if (!value.is_object()) return false;
    const auto error = value.find("error");
    return error != value.end() && JsonStringEquals(*error, "code", expected);
}

bool CheckJsonProtocol(
    const std::shared_ptr<const ue5mem::Analyzer>& analyzer,
    const std::wstring& pipe_name) {
    anomaly::DiagnosticPipeService service({analyzer, pipe_name});
    if (!Check(service.Prepare() == ERROR_SUCCESS, "JSON protocol Prepare failed")) {
        return false;
    }

    std::atomic<DWORD> run_result{ERROR_GEN_FAILURE};
    std::jthread server([&] { run_result.store(service.Run()); });

    DWORD client_error{};
    std::string status_response;
    const bool status_exchanged = ExchangeRequest(
        pipe_name,
        "{\"protocol\":\"anomaly.diagnostics\",\"version\":1,\"id\":\"status-1\",\"command\":\"status\"}\n",
        status_response,
        client_error);
    const Json status = Json::parse(status_response, nullptr, false);
    const bool status_valid = status_exchanged && status.is_object() &&
        JsonStringEquals(status, "protocol", "anomaly.diagnostics") &&
        status.value("version", 0U) == 1U &&
        JsonStringEquals(status, "type", "response") &&
        JsonStringEquals(status, "id", "status-1") &&
        status.value("ok", false) && status.contains("result") &&
        status["result"].is_object() && status["result"].contains("pid") &&
        status["result"].contains("runtime") && status["result"]["runtime"].is_object() &&
        status["result"]["runtime"].contains("plugin_diagnostics") &&
        status["result"]["runtime"]["plugin_diagnostics"].value("schemaVersion", 0U) == 1U &&
        status["result"]["runtime"]["plugin_diagnostics"].contains("plugins");

    std::string ping_response;
    const bool ping_exchanged = ExchangeRequest(
        pipe_name,
        "{\"protocol\":\"anomaly.diagnostics\",\"version\":1,\"id\":\"ping-1\",\"command\":\"ping\"}\n",
        ping_response,
        client_error);
    const Json ping = Json::parse(ping_response, nullptr, false);
    const bool ping_valid = ping_exchanged && ping.is_object() &&
        JsonStringEquals(ping, "id", "ping-1") && ping.value("ok", false) &&
        ping.contains("result") && ping["result"].is_object() &&
        ping["result"].contains("pid");

    std::string malformed_response;
    const bool malformed_exchanged = ExchangeRequest(
        pipe_name,
        "{\"protocol\":\"anomaly.diagnostics\",\"version\":1,\"id\":\"broken\",\"command\":\n",
        malformed_response,
        client_error);
    const Json malformed = Json::parse(malformed_response, nullptr, false);
    const bool malformed_valid = malformed_exchanged && malformed.is_object() &&
        malformed["id"].is_null() && !malformed.value("ok", true) &&
        JsonErrorCodeEquals(malformed, "malformed_json");

    std::string unsupported_response;
    const bool unsupported_exchanged = ExchangeRequest(
        pipe_name,
        "{\"protocol\":\"anomaly.diagnostics\",\"version\":2,\"id\":\"version-2\",\"command\":\"status\"}\n",
        unsupported_response,
        client_error);
    const Json unsupported = Json::parse(unsupported_response, nullptr, false);
    const bool unsupported_valid = unsupported_exchanged && unsupported.is_object() &&
        JsonStringEquals(unsupported, "id", "version-2") &&
        !unsupported.value("ok", true) &&
        JsonErrorCodeEquals(unsupported, "unsupported_version");

    std::string text_response;
    const bool text_exchanged =
        ExchangeRequest(pipe_name, "ping\n", text_response, client_error);
    const Json text = Json::parse(text_response, nullptr, false);
    const bool text_valid = text_exchanged && text.is_object() &&
        text.value("ok", false) && text.contains("pid") &&
        !text.contains("protocol") && !text.contains("result");

    const bool stopped = StopServiceWithinTwoSeconds(service, server, run_result);
    return Check(status_valid, "JSON status response did not match the v1 envelope") &&
        Check(ping_valid, "JSON ping response did not preserve the request ID") &&
        Check(malformed_valid, "malformed JSON request did not return a typed error") &&
        Check(unsupported_valid, "unsupported JSON version did not return a typed error") &&
        Check(text_valid, "text command response did not preserve the CLI contract") &&
        Check(stopped, "JSON protocol service did not stop promptly");
}

bool CheckFlushedLargeResponse(
    const std::wstring& pipe_name,
    const ue5mem::AnalyzerConfig& config) {
    const auto payload = std::make_shared<const std::string>(2U * 1024U * 1024U, 'x');
    const auto analyzer = std::make_shared<const ue5mem::Analyzer>(
        std::filesystem::current_path(),
        config,
        [payload] { return "{\"payload\":\"" + *payload + "\"}"; });
    anomaly::DiagnosticPipeService service({analyzer, pipe_name});
    if (!Check(service.Prepare() == ERROR_SUCCESS, "flush Prepare failed")) return false;

    std::atomic<DWORD> run_result{ERROR_GEN_FAILURE};
    std::jthread server([&] { run_result.store(service.Run()); });
    DWORD client_error{};
    std::string response;
    const bool exchanged = ExchangeRequest(
        pipe_name,
        "{\"protocol\":\"anomaly.diagnostics\",\"version\":1,\"id\":\"large\",\"command\":\"status\"}\n",
        response, client_error, 100ms);
    const bool stopped = StopServiceWithinTwoSeconds(service, server, run_result);
    return Check(exchanged, "delayed large response exchange failed") &&
        Check(response.size() > payload->size() && response.ends_with("\n"),
              "flushed large response was truncated") &&
        Check(stopped, "flushed response service did not stop promptly");
}

bool CheckPendingFlushStop(
    const std::shared_ptr<const ue5mem::Analyzer>& analyzer,
    const std::wstring& pipe_name) {
    anomaly::DiagnosticPipeService service({analyzer, pipe_name});
    if (!Check(service.Prepare() == ERROR_SUCCESS, "pending-flush Prepare failed")) {
        return false;
    }

    std::atomic<DWORD> run_result{ERROR_GEN_FAILURE};
    std::jthread server([&] { run_result.store(service.Run()); });
    DWORD open_error{};
    const HANDLE client = OpenPipe(pipe_name, open_error);
    if (client == INVALID_HANDLE_VALUE) {
        service.Stop();
        server.join();
        std::cerr << "pending-flush client open failed: " << open_error << '\n';
        return false;
    }

    constexpr char request[] =
        "{\"protocol\":\"anomaly.diagnostics\",\"version\":1,\"id\":\"partial\",\"command\":\"ping\"}\n";
    DWORD written{};
    const bool request_consumed =
        WriteFile(client, request, sizeof(request) - 1, &written, nullptr) != FALSE &&
        written == sizeof(request) - 1 && FlushFileBuffers(client) != FALSE;
    DWORD response_available{};
    const bool response_buffered = request_consumed && WaitUntil([&] {
        return PeekNamedPipe(
                   client, nullptr, 0, nullptr, &response_available, nullptr) != FALSE &&
            response_available != 0;
    });
    const bool stopped = StopServiceWithinTwoSeconds(service, server, run_result);
    CloseHandle(client);
    return Check(request_consumed, "pending-flush request was not consumed") &&
        Check(response_buffered, "pending-flush response was not buffered") &&
        Check(stopped, "pending FlushFileBuffers stop exceeded two seconds");
}

bool CheckClientDisconnectContinues(
    const std::shared_ptr<const ue5mem::Analyzer>& analyzer,
    const std::wstring& pipe_name) {
    anomaly::DiagnosticPipeService service({analyzer, pipe_name});
    if (!Check(service.Prepare() == ERROR_SUCCESS, "disconnect recovery Prepare failed")) {
        return false;
    }

    std::atomic<DWORD> run_result{ERROR_GEN_FAILURE};
    std::jthread server([&] { run_result.store(service.Run()); });
    DWORD open_error{};
    const HANDLE abandoned = OpenPipe(pipe_name, open_error);
    if (abandoned == INVALID_HANDLE_VALUE) {
        service.Stop();
        server.join();
        std::cerr << "disconnect recovery client open failed: " << open_error << '\n';
        return false;
    }
    constexpr char partial[] = "abandoned";
    DWORD written{};
    static_cast<void>(WriteFile(
        abandoned, partial, sizeof(partial) - 1, &written, nullptr));
    CloseHandle(abandoned);

    DWORD client_error{};
    std::string response;
    const bool exchanged = ExchangeRequest(pipe_name,
        "{\"protocol\":\"anomaly.diagnostics\",\"version\":1,\"id\":\"recover\",\"command\":\"ping\"}\n",
        response, client_error);
    const bool stopped = StopServiceWithinTwoSeconds(service, server, run_result);
    return Check(exchanged, "listener did not recover from a disconnected client") &&
        Check(
            response.find("\"ok\":true") != std::string::npos,
            "recovery request returned the wrong response") &&
        Check(stopped, "disconnect recovery service did not stop promptly");
}

bool CheckPipeServerEntry(
    const ue5mem::Analyzer& analyzer,
    const std::wstring& pipe_name) {
    using RunPipeServerFn = void (*)(
        const ue5mem::Analyzer&, const std::wstring&, std::stop_token);
    const RunPipeServerFn run_pipe_server = &ue5mem::RunPipeServer;

    std::jthread server([&](std::stop_token stop_token) {
        run_pipe_server(analyzer, pipe_name, stop_token);
    });
    if (!WaitUntilAvailable(pipe_name)) {
        server.request_stop();
        server.join();
        std::cerr << "pipe server did not publish an endpoint\n";
        return false;
    }

    DWORD client_error{};
    std::string response;
    const bool exchanged = ExchangeRequest(pipe_name,
        "{\"protocol\":\"anomaly.diagnostics\",\"version\":1,\"id\":\"overload\",\"command\":\"ping\"}\n",
        response, client_error);
    const bool stopped = StopServerWithinTwoSeconds(server);
    return Check(exchanged, "pipe server request exchange failed") &&
        Check(
            response.find("\"ok\":true") != std::string::npos,
            "pipe server returned the wrong response") &&
        Check(stopped, "pipe server did not stop promptly");
}

}  // namespace

int main() {
    ue5mem::AnalyzerConfig config;
    const auto analyzer = std::make_shared<const ue5mem::Analyzer>(
        std::filesystem::current_path(), config);
    const auto diagnostic_analyzer = std::make_shared<const ue5mem::Analyzer>(
        std::filesystem::current_path(), config,
        [] {
            return std::string{
                "{\"built\":true,\"plugin_diagnostics\":{\"schemaVersion\":1,"
                "\"plugins\":[{\"id\":\"fixture.plugin\",\"capabilities\":[\"diagnostics\"]}]}}"};
        });
    const auto pipe_name = [](std::wstring_view test_name) {
        return ue5mem::BuildPipeName(test_name, GetCurrentProcessId());
    };

    if (!CheckPrepareValidation(analyzer)) return 1;
    if (!CheckRepeatedPrepareAndEndpoint(analyzer, pipe_name(L"AnomalyPipePreparedTests"))) {
        return 2;
    }
    if (!CheckStopBeforeRun(analyzer, pipe_name(L"AnomalyPipeStopBeforeRunTests"))) return 3;
    if (!CheckIdleConnectStop(analyzer, pipe_name(L"AnomalyPipeIdleStopTests"))) return 4;
    if (!CheckPartialRequestStop(analyzer, pipe_name(L"AnomalyPipePartialStopTests"))) {
        return 5;
    }
    if (!CheckCompletedRequestStop(analyzer, pipe_name(L"AnomalyPipeCompletedStopTests"))) {
        return 6;
    }
    if (!CheckJsonProtocol(
            diagnostic_analyzer, pipe_name(L"AnomalyPipeJsonProtocolTests"))) return 7;
    if (!CheckFlushedLargeResponse(
            pipe_name(L"AnomalyPipeFlushTests"), config)) {
        return 8;
    }
    if (!CheckPendingFlushStop(
            analyzer, pipe_name(L"AnomalyPipePendingFlushTests"))) {
        return 9;
    }
    if (!CheckClientDisconnectContinues(
            analyzer, pipe_name(L"AnomalyPipeDisconnectRecoveryTests"))) {
        return 10;
    }
    if (!CheckPipeServerEntry(*analyzer, pipe_name(L"AnomalyPipeServerTests"))) return 11;
    return 0;
}
