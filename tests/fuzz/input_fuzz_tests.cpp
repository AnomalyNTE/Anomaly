#include "analyzer.hpp"
#include "anomaly/build_profile.hpp"
#include "anomaly/plugin_manifest.hpp"
#include "anomaly/plugin_package.hpp"
#include "config.hpp"
#include "pattern.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kMaximumMutationBytes = 16U * 1024U;

std::string Mutate(std::string seed, std::mt19937_64& random) {
    const std::size_t operations = 1U + static_cast<std::size_t>(random() % 8U);
    for (std::size_t operation = 0; operation < operations; ++operation) {
        const std::size_t position = seed.empty()
            ? 0U
            : static_cast<std::size_t>(random() % (seed.size() + 1U));
        switch (random() % 6U) {
        case 0:
            if (!seed.empty()) {
                seed[position % seed.size()] = static_cast<char>(random() & 0xffU);
            }
            break;
        case 1:
            if (seed.size() < kMaximumMutationBytes) {
                seed.insert(seed.begin() + static_cast<std::ptrdiff_t>(position),
                            static_cast<char>(random() & 0xffU));
            }
            break;
        case 2:
            if (!seed.empty()) {
                seed.erase(position % seed.size(), 1U + static_cast<std::size_t>(random() % 8U));
            }
            break;
        case 3:
            if (!seed.empty() && seed.size() < kMaximumMutationBytes) {
                const std::size_t begin = static_cast<std::size_t>(random() % seed.size());
                const std::size_t count = (std::min)(
                    1U + static_cast<std::size_t>(random() % 32U), seed.size() - begin);
                seed.insert(position, seed.substr(begin, count));
                if (seed.size() > kMaximumMutationBytes) seed.resize(kMaximumMutationBytes);
            }
            break;
        case 4:
            if (!seed.empty()) seed.resize(static_cast<std::size_t>(random() % (seed.size() + 1U)));
            break;
        default:
            if (seed.size() < kMaximumMutationBytes) {
                static constexpr std::string_view tokens =
                    "{}[],:/\\.?*\"0123456789ABCDEFabcdef \t\r\n";
                seed.insert(position, 1U, tokens[static_cast<std::size_t>(random() % tokens.size())]);
            }
            break;
        }
    }
    return seed;
}

std::string ManifestSeed() {
    return R"JSON({"schemaVersion":2,"id":"com.example.fuzz","name":"Fuzz","version":"1.0.0","entry":"plugin.dll","api":{"major":1,"minMinor":0,"maxMinor":0},"games":["nte"],"builds":["nte-win64-*"],"loadPhase":"game-ready","capabilities":[]})JSON";
}

std::string ProfileSeed() {
    return R"JSON({"schemaVersion":1,"game":"nte","symbols":{"ue5.GWorld":{"module":"fixture.exe","section":".text","pattern":"48 8B 1D ?? ?? ?? ??","resolve":{"kind":"rip-rel32","offset":3,"instructionSize":7},"validators":["readable-pointer"],"requiredBy":["anomaly.ue5.world"]}},"features":{"ue5.world":["ue5.GWorld"]},"layout":{"world.persistentLevel":48}})JSON";
}

bool ExerciseOne(std::string_view input, ue5mem::Analyzer& analyzer) {
    try {
        const auto manifest = anomaly::ParsePluginManifest(input);
        if (manifest.Ok() && !manifest.manifest) return false;

        const auto profile = anomaly::ParseBuildProfile(input, L"fuzz-input.json");
        if (profile.Ok() && !profile.profile) return false;

        try {
            const auto pattern = ue5mem::Pattern::Parse(input);
            const std::array<std::uint8_t, 64> bytes{};
            static_cast<void>(pattern.FindAll(bytes, 8));
        } catch (const std::exception&) {
        }

        const auto path = anomaly::ValidatePluginPackageRelativePath(input, false);
        if (path.Ok() && path.path.empty()) return false;

        // Keep fuzzed diagnostic commands read-only while still exercising tokenisation,
        // integer parsing, UTF-8 conversion, request-size boundaries, and error JSON.
        static constexpr std::array<std::string_view, 5> commands = {
            "read ", "chain ", "scan __anomaly_missing__ . ", "sections ", "unknown-"};
        const std::string command = std::string(commands[input.size() % commands.size()]) +
            std::string(input);
        const std::string response = analyzer.Execute(command);
        return response.starts_with('{') && response.ends_with('}');
    } catch (...) {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 1000;
    if (argc == 3 && std::string_view(argv[1]) == "--iterations") {
        const unsigned long parsed = std::strtoul(argv[2], nullptr, 10);
        if (parsed == 0 || parsed > 100000UL) return 2;
        iterations = static_cast<std::size_t>(parsed);
    } else if (argc != 1) {
        return 2;
    }

    ue5mem::AnalyzerConfig config;
    ue5mem::Analyzer analyzer(std::filesystem::current_path(), std::move(config));
    const std::vector<std::string> seeds = {
        ManifestSeed(), ProfileSeed(), "48 8B ?? ?F", "plugin.dll", "ping", "", "{}", "[]"};
    std::mt19937_64 random(0x414E4F4D414C5938ULL);
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        std::string input = Mutate(seeds[iteration % seeds.size()], random);
        if (!ExerciseOne(input, analyzer)) {
            std::cerr << "fuzz invariant failed at iteration " << iteration << '\n';
            return 1;
        }
    }
    std::cout << "deterministic input fuzz passed iterations=" << iterations << '\n';
    return 0;
}
