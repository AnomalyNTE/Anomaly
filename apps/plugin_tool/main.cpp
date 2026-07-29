#include "anomaly/plugin_manifest.hpp"
#include "anomaly/plugin_package.hpp"
#include "anomaly/artifact_bundle.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::optional<std::string> ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (value.size() > anomaly::kMaximumPluginManifestBytes) return std::nullopt;
    return value;
}

std::string Sha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD object_size{}, copied{};
    std::array<unsigned char, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
        !BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0))) return {};
    std::vector<unsigned char> object(object_size);
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0); return {};
    }
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count > 0 && !BCRYPT_SUCCESS(BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0))) {
            BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); return {};
        }
    }
    const bool ok = BCRYPT_SUCCESS(BCryptFinishHash(
        hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const auto value : digest) encoded << std::setw(2) << static_cast<unsigned>(value);
    return encoded.str();
}

struct ValidatedPackage {
    anomaly::PluginManifest manifest;
    std::filesystem::path root;
};

std::optional<ValidatedPackage> Validate(const std::filesystem::path& input, bool print_success) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(input, error);
    if (error || !std::filesystem::is_directory(root, error)) {
        std::cerr << "package directory is unavailable: " << input.string() << '\n';
        return std::nullopt;
    }
    const auto json = ReadText(root / L"manifest.json");
    if (!json) {
        std::cerr << "manifest.json is missing, unreadable, or too large\n";
        return std::nullopt;
    }
    auto parsed = anomaly::ParsePluginManifest(*json);
    for (const auto& diagnostic : parsed.diagnostics) {
        std::cerr << "manifest[" << static_cast<unsigned>(diagnostic.code) << "] "
                  << diagnostic.path << ": " << diagnostic.message << '\n';
    }
    if (!parsed.Ok()) return std::nullopt;
    const auto entry = anomaly::OpenConfinedPluginPackageFile(root, parsed.manifest->entry, true);
    if (!entry.Ok()) {
        std::cerr << "entry: " << entry.message << '\n';
        return std::nullopt;
    }
    if (print_success) {
        std::cout << "valid id=" << parsed.manifest->id << " version="
                  << parsed.manifest->version.ToString() << " entry="
                  << entry.path.filename().string() << '\n';
    }
    return ValidatedPackage{std::move(*parsed.manifest), root};
}

int Pack(const std::filesystem::path& input, const std::filesystem::path& output_root) {
    const auto package = Validate(input, false);
    if (!package) return 2;
    std::error_code error;
    const auto destination = std::filesystem::absolute(output_root) / package->manifest.id;
    const auto staging = destination.parent_path() /
        (destination.filename().wstring() + L".staging-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(staging, error); error.clear();
    std::filesystem::create_directories(staging, error);
    if (error) { std::cerr << "create staging failed: " << error.message() << '\n'; return 3; }
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(package->root, error), end;
         !error && it != end; it.increment(error)) {
        const auto relative = std::filesystem::relative(it->path(), package->root, error);
        if (error) break;
        if (relative.begin() != relative.end() && relative.begin()->wstring() == L".cache") {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        const auto target = staging / relative;
        if (it->is_directory()) std::filesystem::create_directories(target, error);
        else if (it->is_regular_file()) {
            std::filesystem::create_directories(target.parent_path(), error);
            if (!error) std::filesystem::copy_file(it->path(), target,
                std::filesystem::copy_options::overwrite_existing, error);
            if (!error) files.push_back(relative);
        }
        if (error) break;
    }
    if (error) { std::cerr << "copy failed: " << error.message() << '\n'; return 4; }
    std::sort(files.begin(), files.end());
    std::ofstream hashes(staging / L"package.sha256", std::ios::binary | std::ios::trunc);
    for (const auto& relative : files) {
        const auto digest = Sha256(staging / relative);
        if (digest.empty()) { std::cerr << "hash failed: " << relative.string() << '\n'; return 5; }
        hashes << digest << "  " << relative.generic_string() << '\n';
    }
    hashes.close();
    if (!hashes) { std::cerr << "hash manifest write failed\n"; return 5; }
    for (int attempt = 0; attempt < 20; ++attempt) {
        error.clear();
        std::filesystem::remove_all(destination, error);
        if (!error) std::filesystem::rename(staging, destination, error);
        if (!error) break;
        if (error.value() != ERROR_ACCESS_DENIED && error.value() != ERROR_SHARING_VIOLATION &&
            error.value() != ERROR_LOCK_VIOLATION) {
            break;
        }
        Sleep(25);
    }
    if (error) { std::cerr << "commit failed: " << error.message() << '\n'; return 6; }
    std::cout << "packed id=" << package->manifest.id << " output=" << destination.string()
              << " files=" << files.size() + 1 << '\n';
    return 0;
}

void Usage() {
    std::cerr << "usage:\n  anomaly-plugin validate <package-dir>\n"
                 "  anomaly-plugin pack <package-dir> --output <directory>\n"
                 "  anomaly-plugin bundle <packed-package-dir> --output <file.anomaly-package>\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { Usage(); return 1; }
    const std::wstring command = argv[1];
    if (command == L"validate") return Validate(argv[2], true) ? 0 : 2;
    if (command == L"pack") {
        std::filesystem::path output;
        for (int index = 3; index + 1 < argc; ++index) {
            if (std::wstring_view(argv[index]) == L"--output") output = argv[++index];
        }
        if (output.empty()) { Usage(); return 1; }
        return Pack(argv[2], output);
    }
    if (command == L"bundle") {
        std::filesystem::path output;
        for (int index = 3; index + 1 < argc; ++index) {
            if (std::wstring_view(argv[index]) == L"--output") output = argv[++index];
        }
        if (output.empty()) { Usage(); return 1; }
        if (!Validate(argv[2], false)) return 2;
        const auto result = anomaly::CreateArtifactBundle(argv[2], output);
        if (!result.Ok()) {
            std::cerr << "bundle failed: " << result.message << '\n';
            return 7;
        }
        std::cout << "bundled output=" << output.string() << " files=" << result.entries
                  << " bytes=" << result.bytes << '\n';
        return 0;
    }
    Usage();
    return 1;
}
