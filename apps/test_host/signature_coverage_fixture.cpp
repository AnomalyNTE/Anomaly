#include "anomaly/signature_updater.hpp"
#include "pattern.hpp"
#include "plugin_profile_fixture_host.hpp"
#include "../../plugins/HiFiVehicleMusic/hifi_vehicle_music_profile.hpp"
#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

// Explicit offline measurement against complete .text sections. Never attach
// to or modify a process; the caller supplies independently captured inputs.
namespace {
struct Image {
    std::vector<std::uint8_t> bytes;
    std::map<std::string, std::vector<std::uintptr_t>, std::less<>> scans;
    static constexpr std::uintptr_t base = 0x140001000;
    explicit Image(const char* path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input || input.tellg() <= 0) throw std::runtime_error("cannot read section input");
        bytes.resize(static_cast<std::size_t>(input.tellg()));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (!input) throw std::runtime_error("truncated section input");
    }
    std::vector<std::uintptr_t> Scan(std::string_view pattern, std::size_t limit) {
        auto [it, added] = scans.try_emplace(std::string(pattern));
        if (added) for (auto offset : ue5mem::Pattern::Parse(pattern).FindAll(bytes, 256))
            it->second.push_back(base + offset);
        auto result = it->second;
        if (result.size() > limit) result.resize(limit);
        return result;
    }
    anomaly::SignatureScanContext Context() {
        return {{L"HTGame.exe", L"", base, bytes.size()},
            {".text", base, bytes.size(), IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ},
            [this](auto address, void* out, auto size) {
                if (address < base || address - base > bytes.size() || size > bytes.size() - (address - base)) return false;
                std::memcpy(out, bytes.data() + address - base, size);
                return true;
            }, [this](auto pattern, auto limit) { return Scan(pattern, limit); }};
    }
};

anomaly::SignatureLayoutDefinition Definition(const anomaly::sdk::LayoutSchema& schema) {
    anomaly::SignatureLayoutDefinition result;
    for (const auto& field : schema.fields)
        result.fields.push_back({std::string(field.id.data, field.id.size), field.seed_offset, field.alignment});
    for (const auto& witness : schema.witnesses) {
        anomaly::SignatureLayoutWitness row{std::string(witness.id.data, witness.id.size),
            std::string(witness.pattern.data, witness.pattern.size), {}};
        for (std::uint32_t i{}; i < witness.member_count; ++i) {
            const auto& member = witness.members[i];
            row.members.push_back({member.field_index, member.addend, member.width, member.access});
        }
        result.witnesses.push_back(std::move(row));
    }
    return result;
}

bool CheckMusicMembers(Image& previous, Image& current, const std::filesystem::path& plugins) {
    anomaly::testing::ProfileHost host("HiFiVehicleMusic");
    host.package = plugins / "HiFiVehicleMusic";
    host.core.user = &previous;
    host.core.read_memory = [](void* user, std::uintptr_t address, AnomalyMutableByteSpanV1 out) {
        return AnomalyStatusV1{static_cast<std::uint32_t>(static_cast<Image*>(user)->Context().read(address, out.data, out.size)
            ? ANOMALY_STATUS_V1_OK : ANOMALY_STATUS_V1_NOT_FOUND)};
    };
    host.signatures.user = &previous;
    host.signatures.resolve = [](void* user, AnomalyStringViewV1, AnomalyStringViewV1,
                                 AnomalyStringViewV1 pattern, std::uintptr_t* out) {
        const auto matches = static_cast<Image*>(user)->Scan({pattern.data, pattern.size}, 2);
        *out = matches.size() == 1 ? matches.front() : 0;
        return AnomalyStatusV1{static_cast<std::uint32_t>(matches.size() == 1 ? ANOMALY_STATUS_V1_OK :
            matches.empty() ? ANOMALY_STATUS_V1_NOT_FOUND : ANOMALY_STATUS_V1_CONFLICT)};
    };
    if (!hifi_vehicle_music_profile::Load(&host.api)) throw std::runtime_error("cannot load the production music Profile against the old image");
    const hifi_vehicle_music_profile::PlaybackSchema playback;
    const hifi_vehicle_music_profile::AlbumSchema album;
    // Independently checked HTMusicSubsystem fields in both supplied SDK dumps.
    const std::array<std::vector<std::uint32_t>, 2> expected{{{1080,1084,1088},{1048,1016,1088}}};
    const std::array definitions{Definition(playback.View()), Definition(album.View())};
    anomaly::SignatureUpdater updater({});
    bool correct = true;
    for (std::size_t i{}; i < definitions.size(); ++i) {
        const auto old = updater.ResolveLayout(previous.Context(), definitions[i]);
        const auto next = updater.ResolveLayout(current.Context(), definitions[i]);
        const bool ok = old.Ok() && next.Ok() && old.offsets == expected[i] && next.offsets == expected[i];
        correct &= ok;
        std::cout << "HiFi member layout " << i << ": " << (ok ? "PASS" : "FAIL")
            << " old=" << old.diagnostic << " new=" << next.diagnostic << '\n';
    }
    return correct;
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: signature_coverage_fixture <old.text> <new.text> <plugins-directory>");
        Image previous(argv[1]), current(argv[2]);
        const auto before = previous.Context(), after = current.Context();
        anomaly::SignatureUpdater updater({});
        std::set<std::string> tested;
        std::size_t total{}, exact{}, learned{}, recovered{}, wrong{}, excluded{};
        for (const auto& plugin : std::filesystem::directory_iterator(argv[3])) {
            const auto directory = plugin.path() / "profile";
            if (!std::filesystem::is_directory(directory)) continue;
            for (const auto& file : std::filesystem::directory_iterator(directory)) {
                if (file.path().extension() != ".json") continue;
                nlohmann::json profile;
                std::ifstream input(file.path()); input >> profile;
                for (const auto& symbol : profile.at("symbols")) {
                    std::vector<std::pair<std::string, std::uintptr_t>> seeds;
                    std::set<std::uintptr_t> expected;
                    for (const auto& candidate : symbol.at("candidates")) {
                        const auto pattern = candidate.at("pattern").get<std::string>();
                        const auto old_matches = previous.Scan(pattern, 2);
                        const auto new_matches = current.Scan(pattern, 2);
                        if (old_matches.size() == 1) seeds.emplace_back(pattern, old_matches.front());
                        if (new_matches.size() == 1) expected.insert(new_matches.front());
                    }
                    if (seeds.size() != 1 || expected.size() != 1) { ++excluded; continue; }
                    const auto& [pattern, address] = seeds.front();
                    if (!tested.insert(pattern).second) continue;
                    ++total;
                    const auto direct = current.Scan(pattern, 2);
                    const bool direct_ok = direct.size() == 1 && direct.front() == *expected.begin();
                    exact += direct_ok;
                    const auto diagnostic = updater.Learn(before, "coverage", pattern, address);
                    learned += diagnostic.empty();
                    const auto result = updater.Recover(after, "coverage", pattern);
                    const bool correct = result.instruction == *expected.begin();
                    recovered += correct;
                    wrong += result.instruction != 0 && !correct;
                    std::cout << plugin.path().filename().string() << '/' << symbol.at("name").get<std::string>()
                        << ": exact=" << direct_ok << " learned=" << diagnostic.empty() << " recovered=" << correct;
                    if (!correct) std::cout << " (" << result.diagnostic << "; " << diagnostic << ')';
                    std::cout << '\n' << std::flush;
                }
            }
        }
        std::cout << "TOTAL unique=" << total << " exact=" << exact << " learned=" << learned
            << " recovered=" << recovered << " wrong=" << wrong << " unpaired-symbols=" << excluded << '\n';
        const auto members = CheckMusicMembers(previous, current, argv[3]);
        return wrong == 0 && total != 0 && members ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
