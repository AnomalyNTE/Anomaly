#include "anomaly/build_profile.hpp"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

void PrintDiagnostics(const std::vector<anomaly::ProfileDiagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.source.string() << diagnostic.path << ": "
                  << diagnostic.message << '\n';
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::cerr << "usage: anomaly-profile <fingerprint|validate|catalog> <path> [game]\n";
        return 2;
    }
    const std::wstring command = argv[1];
    if (command == L"fingerprint") {
        std::string error;
        const auto fingerprint = anomaly::FingerprintPeFile(
            argv[2], argc > 3 ? Utf8(argv[3]) : "nte", &error);
        if (!fingerprint) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << "{\"game\":\"" << fingerprint->game << "\",\"id\":\""
                  << fingerprint->id << "\",\"module\":\"" << Utf8(fingerprint->module)
                  << "\",\"machine\":" << fingerprint->machine
                  << ",\"timestamp\":" << fingerprint->timestamp
                  << ",\"imageSize\":" << fingerprint->image_size
                  << ",\"textVirtualSize\":" << fingerprint->text_virtual_size
                  << ",\"textSha256\":\"" << fingerprint->text_sha256
                  << "\",\"fileVersion\":\"" << fingerprint->file_version << "\"}\n";
        return 0;
    }
    if (command == L"validate") {
        const auto parsed = anomaly::LoadBuildProfile(argv[2]);
        PrintDiagnostics(parsed.diagnostics);
        if (!parsed.Ok()) return 1;
        std::cout << "ok game=" << parsed.profile->game
                  << " symbols=" << parsed.profile->symbols.size()
                  << " hash=" << parsed.profile->source_hash << '\n';
        return 0;
    }
    if (command == L"catalog") {
        const std::string game = argc > 3 ? Utf8(argv[3]) : "nte";
        anomaly::BuildProfileCatalog catalog;
        const auto snapshot = catalog.Scan(argv[2]);
        PrintDiagnostics(snapshot.diagnostics);
        const auto profile = std::ranges::find_if(snapshot.profiles, [&](const auto& candidate) {
            return candidate.game == game;
        });
        std::cout << "game=" << game << " profile="
                  << (profile == snapshot.profiles.end() ? "none" : profile->source.string())
                  << '\n';
        return profile == snapshot.profiles.end() ? 3 : 0;
    }
    std::cerr << "invalid command\n";
    return 2;
}
