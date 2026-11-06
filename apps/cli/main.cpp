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

// Generated for a caller that has never seen this tool: it states the output contract, the exact
// command grammar of the runtime, the argument syntax, and what every exit code means.
constexpr char kUsageText[] =
    "anomaly-cli - one-shot client for the Anomaly runtime diagnostics pipe\n"
    "\n"
    "usage:\n"
    "  anomaly-cli --pid <pid> <command> [arguments...]\n"
    "  anomaly-cli --help\n"
    "\n"
    "output contract:\n"
    "  stdout carries exactly one line: the UTF-8 JSON reply of the runtime, for example\n"
    "    {\"ok\":true,\"pid\":1234}\n"
    "    {\"ok\":false,\"error\":\"unknown command; use help\"}\n"
    "  Always parse stdout as JSON and branch on \"ok\". stderr carries client-side failures only\n"
    "  (argument problems, pipe problems); it is never mixed into the JSON line.\n"
    "  Arguments are joined with single spaces, so \"scan . .text 48 8B ?? 48 85 C0\" may be quoted\n"
    "  as one argument or passed as several. Line breaks are rejected, and the request may not\n"
    "  exceed 64 KiB.\n"
    "\n"
    "target:\n"
    "  --pid <pid>   PID of the game process hosting the Anomaly runtime, usually HTGame.exe.\n"
    "                The runtime logs its PID and pipe name to Anomaly\\logs\\anomaly-runtime.log.\n"
    "                The pipe is \\\\.\\pipe\\LOCAL\\Anomaly-<pid>, so [Analyzer] PipePrefix must stay\n"
    "                \"Anomaly\". An ordinary, non-elevated console is enough.\n"
    "\n"
    "commands:\n"
    "  discovery\n"
    "    ping                        liveness only: {\"ok\":true,\"pid\":<runtime pid>}\n"
    "    help                        the runtime's own command list as JSON\n"
    "    status                      runtime root, profile game id, platform flags, service health\n"
    "    modules                     loaded modules with name, path, base address and image size\n"
    "    sections <module|.>         PE sections of a module: base, size, characteristics\n"
    "    regions <module|.>          memory regions: state, protection, type\n"
    "\n"
    "  signatures\n"
    "    scan <module|.> <section> <pattern>\n"
    "                                pattern scan inside one section; the pattern is hex bytes with\n"
    "                                wildcards, e.g. \"48 8B ?? ?? 48 85 C0\" or \"4? ?F\"\n"
    "    xrefs <module|.> <target>   code references to an absolute target address\n"
    "\n"
    "  unreal engine state (depends on the active Profile and its feature gates)\n"
    "    ue                          active profile, resolved symbols, feature matrix\n"
    "    ue actors <filter|*> [limit] [cursor]\n"
    "    ue objects <filter|*> [limit] [cursor]\n"
    "    ue functions <filter|*> [limit] [cursor]\n"
    "                                paged lists; filter is a name/class substring or *\n"
    "    ue fname <id>               resolve an FName id to text\n"
    "    ue ftext <address>          read the FText living at an address\n"
    "    ue combat                   combat bridge status\n"
    "    ue buffs                    buff snapshot\n"
    "\n"
    "  reading memory\n"
    "    read <address> <size>       size <= 1048576; returns the bytes and decoded values\n"
    "    chain <base> [offset ...]   follow a pointer chain, at most 64 offsets, return the end address\n"
    "    ptr <address>               read one pointer\n"
    "    rip <instruction> <disp_offset> <instruction_size>\n"
    "                                resolve a RIP-relative operand to its absolute target\n"
    "    f32 <address> [count]       float32 values, count defaults to 3\n"
    "    f64 <address> [count]       float64 values, count defaults to 3\n"
    "    snapshot [filename]         write a full JSON snapshot into the Anomaly directory\n"
    "\n"
    "  runtime pak mods (mutates the mounted container set)\n"
    "    pak                         same as \"pak status\": every container the runtime knows about\n"
    "    pak status                  id, path, read order, size and state of each container\n"
    "    pak reload                  rescan ~mods and the optional config/pak-mods.json now\n"
    "    pak mount <path> [order]    mount one container; order defaults to the runtime base order\n"
    "    pak release <id>            unmount one container; pending engine reads may retain handles\n"
    "    pak unmountall              unmount everything the runtime mounted\n"
    "    pak prime <id>              explicitly probe one mounted container and report the read stage\n"
    "\n"
    "  writing memory (mutates the running game)\n"
    "    write <address> <hex bytes>       write into a page that is already writable\n"
    "    patch <address> <hex bytes>       switch to RWX, write, flush the instruction cache, restore\n"
    "    protect <address> <size> <r|rw|x|rx|rwx>\n"
    "    alloc <size> [protection]         allocate in the target, size <= 268435456, default rw\n"
    "    free <address>                    release an address returned by alloc\n"
    "    hex bytes are space separated, e.g. \"90 90\" or \"E9 1B 00 00 00\"; every mutation reply\n"
    "    includes the bytes that were replaced\n"
    "\n"
    "arguments:\n"
    "  <module>      \".\" is the game main module, otherwise a module name such as HTGame.exe\n"
    "  <address>     0x-prefixed hex or decimal, e.g. 0x147e40ba0; sizes and offsets use the same\n"
    "                parser, so 0x10 and 16 are equivalent\n"
    "  <section>     PE section name, e.g. .text\n"
    "  <pattern>     hex bytes; ?? is a wildcard byte, 4? fixes the high nibble, ?F the low nibble\n"
    "  protection    r, rw, x, rx, rwx, or a numeric PAGE_* value\n"
    "\n"
    "exit codes:\n"
    "  0   the runtime answered; read the JSON \"ok\" field to learn the outcome\n"
    "  2   client-side usage error: missing or malformed --pid, unencodable argument, oversized request\n"
    "  3   could not connect: 2 = no pipe for that PID, 5 = access denied, 231 = every instance busy\n"
    "  4   the pipe could not be configured or the request could not be written\n"
    "  5   the response could not be read, or stdout failed\n"
    "\n"
    "typical use:\n"
    "  anomaly-cli --pid 1234 status\n"
    "  anomaly-cli --pid 1234 modules\n"
    "  anomaly-cli --pid 1234 sections .\n"
    "  anomaly-cli --pid 1234 scan . .text \"48 8B ?? ?? ?? ?? ?? 48 85 C0\"\n"
    "  anomaly-cli --pid 1234 xrefs . 0x1418aacf0\n"
    "  anomaly-cli --pid 1234 read 0x14fb292f0 16\n"
    "  anomaly-cli --pid 1234 ue actors * 20\n"
    "  anomaly-cli --pid 1234 snapshot\n"
    "\n"
    "recommended sequence for an unfamiliar target: status, modules, scan or xrefs, then\n"
    "read/ptr/chain to confirm a layout before any write/patch/protect/alloc/free.\n";

void Usage(std::ostream& stream) { stream << kUsageText; }


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
    if (argc >= 2) {
        const std::wstring_view first(argv[1]);
        if (first == L"--help" || first == L"-h" || first == L"/?" || first == L"help") {
            Usage(std::cout);
            return 0;
        }
    }
    if (argc < 4 || std::wstring_view(argv[1]) != L"--pid") {
        Usage(std::cerr);
        return 2;
    }
    wchar_t* end{};
    const unsigned long pid = wcstoul(argv[2], &end, 10);
    if (pid == 0 || end == argv[2] || *end != L'\0') {
        Usage(std::cerr);
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
        // The shell removes quotes around a pak id/path. Preserve that argument boundary on the
        // diagnostics wire, whose pak parser accepts quoted tokens. Other command grammars differ.
        const bool quote_pak_argument = std::wstring_view(argv[3]) == L"pak" && index >= 5 &&
            argument->find_first_of(" \t") != std::string::npos;
        if (quote_pak_argument && argument->find('"') != std::string::npos) {
            std::cerr << "quoted pak arguments must not contain a quote\n";
            return 2;
        }
        const auto encoded = quote_pak_argument ? '"' + *argument + '"' : *argument;
        const std::size_t separator = request.empty() ? 0U : 1U;
        const std::size_t maximum_command_bytes = kMaximumRequestBytes - 1U;
        if (request.size() > maximum_command_bytes - separator ||
            encoded.size() > maximum_command_bytes - request.size() - separator) {
            std::cerr << "request exceeds the 64 KiB diagnostics pipe limit\n";
            return 2;
        }
        if (separator != 0) request.push_back(' ');
        request += encoded;
    }
    if (request.empty()) {
        Usage(std::cerr);
        return 2;
    }
    request.push_back('\n');
    const std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\Anomaly-" + std::to_wstring(pid);
    const std::string pipe_name_utf8 = Utf8(pipe_name).value_or(std::string{});
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
        std::cerr << "pipe connection failed: " << pipe_error << " (" << pipe_name_utf8 << ")\n";
        if (pipe_error == ERROR_FILE_NOT_FOUND) {
            std::cerr << "hint: no diagnostics pipe for that PID; confirm the PID is the running "
                         "game process and that the Anomaly runtime is loaded\n";
        } else if (pipe_error == ERROR_ACCESS_DENIED) {
            std::cerr << "hint: access denied; the pipe belongs to a runtime of a higher integrity "
                         "level, or to another Windows user\n";
        }
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
