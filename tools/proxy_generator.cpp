#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Export {
    unsigned ordinal{};
    std::string name;
};

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "usage: proxy_generator <exports> <output-directory>\n";
        return 2;
    }

    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "failed to open export list\n";
        return 3;
    }

    std::vector<Export> exports;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        Export item;
        fields >> item.ordinal >> item.name;
        if (item.ordinal != 0) {
            exports.push_back(std::move(item));
        }
    }
    if (exports.empty()) {
        std::cerr << "export list is empty\n";
        return 4;
    }

    const fs::path output = argv[2];
    fs::create_directories(output);

    std::ofstream def(output / "dwmapi.def");
    def << "LIBRARY dwmapi\nEXPORTS\n";
    for (size_t index = 0; index < exports.size(); ++index) {
        const auto& item = exports[index];
        const auto public_name = item.name.empty() ? "ordinal" + std::to_string(item.ordinal) : item.name;
        def << "  " << public_name << "=proxy_stub_" << index << " @" << item.ordinal;
        if (item.name.empty()) {
            def << " NONAME";
        }
        def << '\n';
    }

    std::ofstream assembly(output / "dwmapi.asm");
    assembly << ".code\nextern g_proxy_procs:QWORD\n";
    for (size_t index = 0; index < exports.size(); ++index) {
        assembly << "proxy_stub_" << index << " proc\n"
                 << "  jmp g_proxy_procs[8*" << index << "]\n"
                 << "proxy_stub_" << index << " endp\n";
    }
    assembly << "end\n";

    std::ofstream source(output / "proxy_exports.cpp");
    source << "#include <Windows.h>\n#include <cstdint>\n#include \"proxy_exports.hpp\"\n\n";
    source << "extern \"C\" uintptr_t WINAPI ProxyMissingExport();\n";
    source << "extern \"C\" { uintptr_t g_proxy_procs[" << exports.size() << "]{}; }\n";
    source << "bool ResolveProxyExports(HMODULE module) {\n  bool complete = true;\n";
    for (size_t index = 0; index < exports.size(); ++index) {
        const auto& item = exports[index];
        if (item.name.empty()) {
            source << "  g_proxy_procs[" << index << "] = reinterpret_cast<uintptr_t>(GetProcAddress(module, MAKEINTRESOURCEA(" << item.ordinal << ")));\n";
        } else {
            source << "  g_proxy_procs[" << index << "] = reinterpret_cast<uintptr_t>(GetProcAddress(module, \"" << item.name << "\"));\n";
        }
        source << "  complete = complete && g_proxy_procs[" << index << "] != 0;\n";
        source << "  if (g_proxy_procs[" << index << "] == 0) g_proxy_procs[" << index << "] = reinterpret_cast<uintptr_t>(&ProxyMissingExport);\n";
    }
    source << "  return complete;\n}\n";

    std::cout << "generated " << exports.size() << " proxy exports\n";
    return 0;
}
