#include <Windows.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kMaximumRequestBytes = 64U * 1024U;
constexpr std::size_t kResponseChunkBytes = 64U * 1024U;

std::optional<std::string> Utf8(std::wstring_view value) {
    if (value.empty()) return std::string{};
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::nullopt;
    std::string output(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), length, nullptr, nullptr) != length) {
        return std::nullopt;
    }
    return output;
}

void Usage() {
    std::cerr << "usage: anomaly-cli --pid <pid> <command> [arguments...]\n"
                 "example: anomaly-cli --pid 1234 modules\n";
}

bool WriteRequest(const HANDLE pipe, const std::string_view request, DWORD& error) {
    const DWORD request_size = static_cast<DWORD>(request.size());
    DWORD written{};
    if (WriteFile(
            pipe, request.data(), request_size, &written, nullptr) == FALSE) {
        error = GetLastError();
        return false;
    }
    if (written != request_size) {
        error = ERROR_WRITE_FAULT;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

bool WriteResponseChunk(const char* data, const DWORD size) {
    if (size == 0) return true;
    std::cout.write(data, size);
    return static_cast<bool>(std::cout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4 || std::wstring_view(argv[1]) != L"--pid") {
        Usage();
        return 2;
    }
    wchar_t* end{};
    const unsigned long pid = wcstoul(argv[2], &end, 10);
    if (pid == 0 || end == argv[2] || *end != L'\0') {
        Usage();
        return 2;
    }

    std::string request;
    for (int index = 3; index < argc; ++index) {
        const auto argument = Utf8(argv[index]);
        if (!argument) {
            std::cerr << "argument cannot be encoded as UTF-8\n";
            return 2;
        }
        if (argument->find_first_of("\r\n") != std::string::npos) {
            std::cerr << "arguments must not contain line breaks\n";
            return 2;
        }
        const std::size_t separator = request.empty() ? 0U : 1U;
        const std::size_t maximum_command_bytes = kMaximumRequestBytes - 1U;
        if (request.size() > maximum_command_bytes - separator ||
            argument->size() > maximum_command_bytes - request.size() - separator) {
            std::cerr << "request exceeds the 64 KiB diagnostics pipe limit\n";
            return 2;
        }
        if (separator != 0) request.push_back(' ');
        request += *argument;
    }
    if (request.empty()) {
        Usage();
        return 2;
    }
    request.push_back('\n');
    const std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\Anomaly-" + std::to_wstring(pid);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD pipe_error = ERROR_FILE_NOT_FOUND;
    for (int attempt = 0; attempt < 50; ++attempt) {
        pipe = CreateFileW(
            pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        pipe_error = GetLastError();
        if (pipe_error == ERROR_PIPE_BUSY) {
            static_cast<void>(WaitNamedPipeW(pipe_name.c_str(), 100));
        } else if (pipe_error == ERROR_FILE_NOT_FOUND) {
            Sleep(100);
        } else {
            break;
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        std::cerr << "pipe connection failed: " << pipe_error << '\n';
        return 3;
    }

    DWORD read_mode = PIPE_READMODE_MESSAGE;
    const BOOL mode_configured = SetNamedPipeHandleState(pipe, &read_mode, nullptr, nullptr);
    const DWORD mode_error = mode_configured != FALSE ? ERROR_SUCCESS : GetLastError();
    if (mode_configured == FALSE && mode_error != ERROR_INVALID_PARAMETER) {
        std::cerr << "pipe read-mode configuration failed: " << mode_error << '\n';
        CloseHandle(pipe);
        return 4;
    }

    DWORD write_error{};
    if (!WriteRequest(pipe, request, write_error)) {
        std::cerr << "pipe write failed: " << write_error << '\n';
        CloseHandle(pipe);
        return 4;
    }

    std::array<char, kResponseChunkBytes> buffer{};
    bool received_response{};
    for (;;) {
        DWORD read{};
        const BOOL read_succeeded = ReadFile(
            pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
        const DWORD read_error = read_succeeded != FALSE ? ERROR_SUCCESS : GetLastError();

        if (!WriteResponseChunk(buffer.data(), read)) {
            std::cerr << "stdout write failed\n";
            CloseHandle(pipe);
            return 5;
        }
        received_response = received_response || read != 0;

        if (read_succeeded != FALSE) {
            if (read == 0) break;
            continue;
        }
        if (read_error == ERROR_MORE_DATA && read != 0) continue;
        if (read_error == ERROR_BROKEN_PIPE || read_error == ERROR_NO_DATA ||
            read_error == ERROR_PIPE_NOT_CONNECTED) {
            break;
        }
        std::cerr << "pipe read failed: " << read_error << '\n';
        CloseHandle(pipe);
        return 5;
    }
    CloseHandle(pipe);
    if (!received_response) {
        std::cerr << "pipe closed before returning a response\n";
        return 5;
    }
    return std::cout ? 0 : 5;
}
