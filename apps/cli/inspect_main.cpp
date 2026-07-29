#include <Windows.h>

#include <bit>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::uint64_t ParseInteger(const wchar_t* text, bool& valid) {
    wchar_t* end{};
    const auto value = _wcstoui64(text, &end, 0);
    valid = end != text && *end == L'\0';
    return value;
}

std::string Hex(std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << value;
    return output.str();
}

bool Read(HANDLE process, std::uint64_t address, void* output, std::size_t size) {
    SIZE_T read{};
    return ReadProcessMemory(
               process, reinterpret_cast<const void*>(address), output, size, &read) != FALSE &&
           read == size;
}

void Usage() {
    std::cerr << "usage:\n"
                 "  anomaly-inspect --pid PID read ADDRESS SIZE\n"
                 "  anomaly-inspect --pid PID rip INSTRUCTION DISP_OFFSET INSTRUCTION_SIZE\n"
                 "  anomaly-inspect --pid PID ptr ADDRESS\n"
                 "  anomaly-inspect --pid PID f32 ADDRESS COUNT\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 5 || std::wstring_view(argv[1]) != L"--pid") {
        Usage();
        return 2;
    }
    bool valid{};
    const auto pid_value = ParseInteger(argv[2], valid);
    if (!valid || pid_value == 0 || pid_value > MAXDWORD) return 2;

    const HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE,
        static_cast<DWORD>(pid_value));
    if (process == nullptr) {
        std::cerr << "OpenProcess failed: " << GetLastError() << '\n';
        return 3;
    }

    const std::wstring_view command = argv[3];
    const auto address = ParseInteger(argv[4], valid);
    if (!valid) {
        CloseHandle(process);
        return 2;
    }

    int result = 0;
    if (command == L"read" && argc == 6) {
        const auto size = ParseInteger(argv[5], valid);
        if (!valid || size == 0 || size > 4096) {
            result = 2;
        } else {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            if (!Read(process, address, bytes.data(), bytes.size())) {
                std::cerr << "ReadProcessMemory failed: " << GetLastError() << '\n';
                result = 4;
            } else {
                std::ostringstream hex;
                hex << std::hex << std::setfill('0');
                for (std::size_t index = 0; index < bytes.size(); ++index) {
                    if (index != 0) hex << ' ';
                    hex << std::setw(2) << static_cast<unsigned>(bytes[index]);
                }
                std::cout << "{\"ok\":true,\"address\":\"" << Hex(address)
                          << "\",\"bytes\":\"" << hex.str() << "\"}\n";
            }
        }
    } else if (command == L"rip" && argc == 7) {
        const auto displacement_offset = ParseInteger(argv[5], valid);
        bool size_valid{};
        const auto instruction_size = ParseInteger(argv[6], size_valid);
        std::int32_t displacement{};
        if (!valid || !size_valid || displacement_offset > 32 || instruction_size > 32) {
            result = 2;
        } else if (!Read(process, address + displacement_offset, &displacement, sizeof(displacement))) {
            std::cerr << "ReadProcessMemory failed: " << GetLastError() << '\n';
            result = 4;
        } else {
            const auto target = address + instruction_size + displacement;
            std::uint64_t pointer{};
            const bool has_pointer = Read(process, target, &pointer, sizeof(pointer));
            std::cout << "{\"ok\":true,\"instruction\":\"" << Hex(address)
                      << "\",\"displacement\":" << displacement
                      << ",\"target\":\"" << Hex(target) << "\"";
            if (has_pointer) std::cout << ",\"pointer\":\"" << Hex(pointer) << "\"";
            std::cout << "}\n";
        }
    } else if (command == L"ptr" && argc == 5) {
        std::uint64_t pointer{};
        if (!Read(process, address, &pointer, sizeof(pointer))) {
            std::cerr << "ReadProcessMemory failed: " << GetLastError() << '\n';
            result = 4;
        } else {
            std::cout << "{\"ok\":true,\"address\":\"" << Hex(address)
                      << "\",\"pointer\":\"" << Hex(pointer) << "\"}\n";
        }
    } else if (command == L"f32" && argc == 6) {
        const auto count = ParseInteger(argv[5], valid);
        if (!valid || count == 0 || count > 256) {
            result = 2;
        } else {
            std::vector<float> values(static_cast<std::size_t>(count));
            if (!Read(process, address, values.data(), values.size() * sizeof(float))) {
                std::cerr << "ReadProcessMemory failed: " << GetLastError() << '\n';
                result = 4;
            } else {
                std::cout << "{\"ok\":true,\"address\":\"" << Hex(address) << "\",\"values\":[";
                for (std::size_t index = 0; index < values.size(); ++index) {
                    if (index != 0) std::cout << ',';
                    std::cout << std::setprecision(9) << values[index];
                }
                std::cout << "]}\n";
            }
        }
    } else {
        Usage();
        result = 2;
    }

    CloseHandle(process);
    return result;
}
