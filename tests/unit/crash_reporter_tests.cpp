#include "anomaly/crash_reporter.hpp"

#include <Windows.h>
#include <DbgHelp.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD kFixtureExceptionCode = 0xE0421001U;
constexpr DWORD kPreviousFilterExitCode = 197U;
constexpr wchar_t kFixtureSwitch[] = L"--unhandled-crash-fixture";

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

std::wstring CurrentExecutable() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

std::wstring QuoteArgument(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

LONG WINAPI FixturePreviousFilter(EXCEPTION_POINTERS*) noexcept {
    // A distinct exit code proves that CrashReporter chained to the filter that
    // was installed before it. TerminateProcess is intentional in this child only.
    TerminateProcess(GetCurrentProcess(), kPreviousFilterExitCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

int RunUnhandledCrashFixture(const std::filesystem::path& dump_directory) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(&FixturePreviousFilter);

    anomaly::CrashReporter reporter({dump_directory, "1.0.0-child-fixture"});
    std::string error;
    if (!reporter.Install(&error)) return 70;

    // Deliberately leave this application exception unhandled so Windows invokes
    // the process top-level exception filter rather than a test helper calling it.
    RaiseException(kFixtureExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 71;
}

bool LaunchUnhandledCrashFixture(const std::filesystem::path& dump_directory) {
    const std::wstring executable = CurrentExecutable();
    if (!Check(!executable.empty(), "test executable path is unavailable")) return false;

    std::wstring command_line = QuoteArgument(executable) + L" " + kFixtureSwitch +
        L" " + QuoteArgument(dump_directory.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!Check(
            CreateProcessW(
                executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
            "failed to launch unhandled-crash child fixture")) {
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(process.hProcess, 30000);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exit_code{};
    const bool queried = GetExitCodeProcess(process.hProcess, &exit_code) != FALSE;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return Check(wait_result == WAIT_OBJECT_0, "unhandled-crash child fixture timed out") &&
        Check(queried, "unhandled-crash child exit code is unavailable") &&
        Check(exit_code == kPreviousFilterExitCode,
              "crash reporter did not preserve the previous exception filter");
}

std::vector<std::filesystem::path> FindArtifacts(
    const std::filesystem::path& directory, const wchar_t* extension) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && iterator->path().extension() == extension) {
            result.push_back(iterator->path());
        }
    }
    return result;
}

bool VerifyMiniDumpNormal(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    MINIDUMP_HEADER header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    // Current DbgHelp versions may add AVX register context even when the caller
    // requests MiniDumpNormal. No broader memory/handle dump option is accepted.
    constexpr ULONG64 kPlatformContext = MiniDumpWithAvxXStateContext;
    return Check(
               input.gcount() == static_cast<std::streamsize>(sizeof(header)),
               "child minidump header is truncated") &&
        Check(header.Signature == MINIDUMP_SIGNATURE, "child artifact is not a minidump") &&
        Check((header.Flags & ~kPlatformContext) == static_cast<ULONG64>(MiniDumpNormal),
              "child minidump contains options beyond MiniDumpNormal");
}

bool VerifyUnhandledCrashArtifacts(const std::filesystem::path& directory) {
    const auto dumps = FindArtifacts(directory, L".dmp");
    const auto metadata_files = FindArtifacts(directory, L".json");
    bool result = Check(dumps.size() == 1, "child fixture did not create one minidump") &&
        Check(metadata_files.size() == 1, "child fixture did not create one metadata file");
    if (dumps.size() != 1 || metadata_files.size() != 1) return false;

    result = Check(std::filesystem::file_size(dumps.front()) > 0,
                   "child minidump file is empty") &&
        Check(std::filesystem::file_size(metadata_files.front()) > 0,
              "child metadata file is empty") &&
        Check(dumps.front().stem() == metadata_files.front().stem(),
              "child minidump and metadata names do not match") &&
        VerifyMiniDumpNormal(dumps.front()) && result;

    const std::string metadata = ReadText(metadata_files.front());
    result = Check(
                 metadata.find("\"runtimeVersion\": \"1.0.0-child-fixture\"") !=
                     std::string::npos,
                 "child metadata runtime version is missing") &&
        Check(metadata.find("\"exceptionCode\": " +
                            std::to_string(static_cast<std::uint32_t>(kFixtureExceptionCode))) !=
                  std::string::npos,
              "child metadata exception code is missing") &&
        Check(metadata.find("\"reasonCode\": \"unhandled-exception\"") != std::string::npos,
              "child metadata reason code is missing") &&
        Check(metadata.find("\"dumpType\": \"MiniDumpNormal\"") != std::string::npos,
              "child metadata dump type is missing") &&
        Check(metadata.find("\"containsFullMemory\": false") != std::string::npos,
              "child metadata full-memory privacy policy is missing") &&
        Check(metadata.find("\"containsHandleData\": false") != std::string::npos,
              "child metadata handle privacy policy is missing") &&
        Check(metadata.find("\"metadataContainsPaths\": false") != std::string::npos,
              "child metadata path privacy policy is missing") &&
        Check(metadata.find("\"metadataContainsPluginPrivateConfiguration\": false") !=
                  std::string::npos,
              "child metadata plugin privacy policy is missing") &&
        Check(metadata.find("\"dumpMayContainModulePathsAndStackMemory\": true") !=
                  std::string::npos,
              "child metadata dump disclosure is missing") && result;
    return result;
}

bool RunManualDumpChecks(const std::filesystem::path& root) {
    anomaly::CrashReporter reporter({root / L"manual-crashes", "1.0.0-test"});
    std::string error;
    bool result = Check(reporter.Install(&error), "crash reporter install failed") &&
        Check(reporter.Installed(), "crash reporter did not report installed state");

    anomaly::CrashReporter duplicate({root / L"duplicate", "1.0.0-test"});
    result = Check(!duplicate.Install(&error), "duplicate crash reporter was accepted") && result;

    const auto dump = reporter.WriteDump(nullptr, "manual-test");
    result = Check(dump.written && dump.error == ERROR_SUCCESS, "manual minidump failed") &&
        Check(std::filesystem::is_regular_file(dump.dump_path), "manual minidump is missing") &&
        Check(std::filesystem::file_size(dump.dump_path) > 0, "manual minidump is empty") &&
        Check(std::filesystem::is_regular_file(dump.metadata_path),
              "manual dump metadata is missing") && result;
    const std::string metadata = ReadText(dump.metadata_path);
    result = Check(metadata.find("\"runtimeVersion\": \"1.0.0-test\"") != std::string::npos,
                   "manual metadata version is missing") &&
        Check(metadata.find("\"dumpType\": \"MiniDumpNormal\"") != std::string::npos,
              "manual metadata type is missing") &&
        Check(metadata.find("\"containsFullMemory\": false") != std::string::npos,
              "manual metadata privacy policy is missing") && result;

    reporter.Uninstall();
    result = Check(!reporter.Installed(), "crash reporter uninstall failed") &&
        Check(duplicate.Install(&error), "crash reporter slot was not released") && result;
    duplicate.Uninstall();
    return result;
}

}  // namespace

int wmain(int argument_count, wchar_t** arguments) {
    if (argument_count == 3 && std::wstring_view(arguments[1]) == kFixtureSwitch) {
        return RunUnhandledCrashFixture(arguments[2]);
    }

    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-crash-reporter-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    bool result = RunManualDumpChecks(root);
    const auto child_directory = root / L"child-crashes";
    result = LaunchUnhandledCrashFixture(child_directory) && result;
    result = VerifyUnhandledCrashArtifacts(child_directory) && result;

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result ? 0 : 2;
}
