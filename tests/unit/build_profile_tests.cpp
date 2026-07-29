#include "anomaly/build_profile.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::filesystem::path TemporaryDirectory() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"anomaly-profile-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(path);
    return path;
}

void WritePe(
    const std::filesystem::path& path,
    std::uint8_t marker,
    std::string_view code_section_name = ".text",
    std::optional<std::uint8_t> secondary_code_marker = std::nullopt) {
    std::vector<std::byte> bytes(secondary_code_marker ? 0xA00 : 0x600);
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    std::memcpy(bytes.data(), &dos, sizeof(dos));
    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = secondary_code_marker ? 2 : 1;
    nt.FileHeader.TimeDateStamp = 0x1234ABCD;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfImage = 0x5000;
    nt.OptionalHeader.SizeOfHeaders = 0x200;
    std::memcpy(bytes.data() + 0x80, &nt, sizeof(nt));
    IMAGE_SECTION_HEADER section{};
    std::memcpy(section.Name, code_section_name.data(), code_section_name.size());
    section.Misc.VirtualSize = 0x321;
    section.SizeOfRawData = 0x400;
    section.PointerToRawData = 0x200;
    section.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    const auto section_offset = 0x80 + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        sizeof(IMAGE_OPTIONAL_HEADER64);
    std::memcpy(bytes.data() + section_offset, &section, sizeof(section));
    std::fill(bytes.begin() + 0x200, bytes.begin() + 0x600, static_cast<std::byte>(marker));
    if (secondary_code_marker) {
        IMAGE_SECTION_HEADER secondary = section;
        secondary.Misc.VirtualSize = 0x80;
        secondary.VirtualAddress = 0x4000;
        secondary.PointerToRawData = 0x600;
        std::memcpy(bytes.data() + section_offset + sizeof(section), &secondary, sizeof(secondary));
        std::fill(
            bytes.begin() + 0x600,
            bytes.end(),
            static_cast<std::byte>(*secondary_code_marker));
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string ProfileJson() {
    return R"({
  "schemaVersion": 1,
  "game": "nte",
  "symbols": {
    "ue5.GWorld": {
      "module": "fixture.exe",
      "section": ".text",
      "pattern": "48 8B 1D ?? ?? ?? ??",
      "resolve": { "kind": "rip-rel32", "offset": 3, "instructionSize": 7 },
      "validators": ["readable-pointer"],
      "requiredBy": ["anomaly.ue5.world"]
    }
  },
  "features": {
    "ue5.world": ["ue5.GWorld"],
    "ue5.dependent": ["ue5.GWorld"]
  },
  "optionalFeatures": ["ue5.dependent"],
  "layout": { "world.persistentLevel": 48 }
})";
}

std::string ProfileJsonWithFeatureExtensions() {
    std::string json = ProfileJson();
    const auto closing = json.rfind('}');
    if (closing == std::string::npos) return json;
    json.insert(closing, R"(,
  "featureLayoutValidators": {
    "ue5.world": ["fixture-layout-v1"]
  },
  "featureDependencies": {
    "ue5.dependent": ["ue5.world"]
  }
)");
    return json;
}

std::string NetworkProfileJson() {
    return R"({
  "schemaVersion": 1,
  "game": "nte",
  "symbols": {
    "ue5.LowLevelReceive": {
      "module": "fixture.exe",
      "section": ".text",
      "pattern": "48 89 5C 24 ?? 57",
      "resolve": { "kind": "direct" },
      "validators": ["address-in-module", "executable"],
      "requiredBy": ["ue5.network.inbound-capture"]
    },
    "ue5.LowLevelSend": {
      "module": "fixture.exe",
      "section": ".text",
      "pattern": "48 89 5C 24 ??",
      "resolve": { "kind": "direct" },
      "validators": ["address-in-module", "executable"],
      "requiredBy": ["ue5.network.outbound-capture"]
    }
  },
  "features": {
    "ue5.network.inbound-capture": ["ue5.LowLevelReceive"],
    "ue5.network.outbound-capture": ["ue5.LowLevelSend"]
  },
  "featureLayoutValidators": {
    "ue5.network.inbound-capture": ["ue5-network-inbound-capture-abi-v1"],
    "ue5.network.outbound-capture": ["ue5-network-outbound-capture-abi-v1"]
  },
  "network": {
    "capture": {
      "inbound": {
        "feature": "ue5.network.inbound-capture",
        "boundarySymbol": "ue5.LowLevelReceive",
        "abiValidator": "ue5-network-inbound-capture-abi-v1",
        "stage": "post-packet-handler",
        "protection": "clear"
      },
      "outbound": {
        "feature": "ue5.network.outbound-capture",
        "boundarySymbol": "ue5.LowLevelSend",
        "abiValidator": "ue5-network-outbound-capture-abi-v1",
        "stage": "post-packet-handler",
        "protection": "clear"
      }
    },
    "header": {
      "sequenceBits": 8,
      "ackMode": "presence-bit",
      "ackSequenceBits": 8,
      "ackHistoryBits": 4,
      "bunchCountBits": 3
    },
    "bunch": {
      "maximumChannels": 16,
      "maximumChannelTypes": 8,
      "maximumReliableSequence": 16,
      "maximumPartialId": 32,
      "maximumPayloadBits": 512,
      "closeHasDormancyBit": true,
      "hasPackageMapExportFlags": true,
      "partialIdMode": "explicit"
    },
    "limits": {
      "maximumPacketBits": 4096,
      "maximumConnections": 4,
      "maximumPacketHistory": 128,
      "maximumBunchesPerPacket": 7,
      "maximumOpenPartialBunches": 4,
      "maximumPartialFragments": 16,
      "maximumPartialPacketAge": 64,
      "maximumPartialBunchBits": 1024,
      "maximumPartialStateBits": 4096,
      "maximumFieldsPerBunch": 7,
      "maximumFieldBytes": 32
    },
    "schemas": [
      {
        "channelType": 2,
        "id": "legacy.fixture",
        "family": "legacy-rep-layout",
        "encoding": "verified-tagged-fields-v1",
        "fields": [
          {
            "handle": 0,
            "name": "ready",
            "encoding": "boolean",
            "bitWidth": 0,
            "maximumLength": 0
          }
        ]
      }
    ]
  }
})";
}

std::optional<std::filesystem::path> FindNteProfile(std::string_view file_name) {
    std::error_code error;
    auto directory = std::filesystem::current_path(error);
    const std::filesystem::path relative =
        std::filesystem::path("profiles") / "nte" / std::string(file_name);
    while (!directory.empty()) {
        const auto candidate = directory / relative;
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return std::nullopt;
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
    const auto root = TemporaryDirectory();
    const auto pe = root / L"fixture.exe";
    WritePe(pe, 0x5A);
    std::string error;
    const auto fingerprint = anomaly::FingerprintPeFile(pe, "NTE", &error);
    bool result = Check(fingerprint.has_value(), "PE fingerprint failed") &&
        Check(fingerprint && fingerprint->game == "nte" &&
                  fingerprint->module == L"fixture.exe" &&
                  fingerprint->machine == IMAGE_FILE_MACHINE_AMD64 &&
                  fingerprint->timestamp == 0x1234ABCD &&
                  fingerprint->image_size == 0x5000 &&
                  fingerprint->text_virtual_size == 0x321 &&
                  fingerprint->text_sha256.size() == 64 &&
                  fingerprint->id.starts_with("nte-win64-1234abcd-00005000-"),
              "PE fingerprint fields changed");
    if (!fingerprint) return 1;

    const auto renamed_pe = root / L"renamed-code-section.exe";
    WritePe(renamed_pe, 0xA5, ".std", static_cast<std::uint8_t>(0x3C));
    const auto renamed_fingerprint = anomaly::FingerprintPeFile(renamed_pe, "NTE", &error);
    const auto renamed_primary_only = root / L"renamed-primary-code-section.exe";
    WritePe(renamed_primary_only, 0xA5, ".std");
    const auto primary_fingerprint =
        anomaly::FingerprintPeFile(renamed_primary_only, "NTE", &error);
    result = Check(
                 renamed_fingerprint &&
                     primary_fingerprint &&
                     renamed_fingerprint->text_virtual_size == 0x321 &&
                     renamed_fingerprint->text_sha256 == primary_fingerprint->text_sha256,
                 "executable code section fallback did not select the primary code section") && result;

    const auto json = ProfileJson();
    const auto parsed = anomaly::ParseBuildProfile(json, root / L"valid.json");
    result = Check(parsed.Ok(), "valid profile was rejected") &&
        Check(parsed.profile && parsed.profile->source_hash.size() == 64 &&
                  parsed.profile->symbols.contains("ue5.GWorld") &&
                  parsed.profile->layout.at("world.persistentLevel") == 48 &&
                  parsed.profile->game == "nte",
              "profile semantic fields changed") && result;

    auto profile_with_build = json;
    const auto game_key = profile_with_build.find("\"game\"");
    if (game_key != std::string::npos) {
        profile_with_build.insert(game_key, "\"build\": {},\n  ");
    }
    result = Check(
                 !anomaly::ParseBuildProfile(profile_with_build).Ok(),
                 "Profile schema accepted removed build metadata") &&
        result;

    const auto extended_json = ProfileJsonWithFeatureExtensions();
    const auto extended = anomaly::ParseBuildProfile(extended_json, root / L"extended.json");
    result = Check(extended.Ok() && extended.profile &&
                       extended.profile->feature_layout_validators.at("ue5.world") ==
                           std::vector<std::string>{"fixture-layout-v1"} &&
                       extended.profile->feature_dependencies.at("ue5.dependent") ==
                           std::vector<std::string>{"ue5.world"},
                   "feature-level profile extensions were not parsed") && result;

    const auto network_json = NetworkProfileJson();
    const auto network_profile = anomaly::ParseBuildProfile(network_json, root / L"network.json");
    result = Check(
                 network_profile.Ok() && network_profile.profile &&
                     network_profile.profile->network_protocol &&
                     network_profile.profile->network_protocol->inbound.feature ==
                         "ue5.network.inbound-capture" &&
                     network_profile.profile->network_protocol->inbound.capture_stage ==
                         anomaly::Ue5PacketCaptureStage::PostPacketHandler &&
                     network_profile.profile->network_protocol->inbound.capture_protection ==
                         anomaly::Ue5PacketProtection::Clear &&
                     network_profile.profile->network_protocol->outbound.feature ==
                         "ue5.network.outbound-capture" &&
                     network_profile.profile->network_protocol->outbound.capture_stage ==
                         anomaly::Ue5PacketCaptureStage::PostPacketHandler &&
                     network_profile.profile->network_protocol->outbound.capture_protection ==
                         anomaly::Ue5PacketProtection::Clear &&
                     network_profile.profile->network_protocol->protocol.schemas.size() == 1 &&
                     network_profile.profile->network_protocol->protocol.schemas.front().fields.size() == 1,
                 "network protocol declaration did not preserve bidirectional capture evidence") &&
        result;

    auto unknown_network_boundary_json = network_json;
    const std::string known_boundary{"\"boundarySymbol\": \"ue5.LowLevelSend\""};
    const auto known_boundary_offset = unknown_network_boundary_json.find(known_boundary);
    if (known_boundary_offset != std::string::npos) {
        unknown_network_boundary_json.replace(
            known_boundary_offset, known_boundary.size(),
            "\"boundarySymbol\": \"ue5.UnknownBoundary\"");
    }
    const auto unknown_network_boundary = anomaly::ParseBuildProfile(unknown_network_boundary_json);
    result = Check(!unknown_network_boundary.Ok() && std::ranges::any_of(
                       unknown_network_boundary.diagnostics, [](const auto& diagnostic) {
                           return diagnostic.path == "/network/capture/outbound/boundarySymbol";
                       }),
                   "network protocol accepted a capture boundary outside the active profile") &&
        result;

    const auto generated_schema = anomaly::BuildProfileSchemaJson();
    result = Check(
                 generated_schema.find("\"featureLayoutValidators\"") != std::string_view::npos &&
                      generated_schema.find("\"featureDependencies\"") != std::string_view::npos &&
                      generated_schema.find("\"optionalFeatures\"") != std::string_view::npos &&
                      generated_schema.find("\"network\"") != std::string_view::npos &&
                      generated_schema.find("\"build\"") == std::string_view::npos,
                 "generated build-profile schema omitted network profile fields") &&
        result;
    const auto current_nte_profile_path = FindNteProfile("nte-current.json");
    if (!current_nte_profile_path) {
        result = Check(
                     false,
                     "current NTE profile was not found from the test working directory") &&
            result;
    } else {
        const auto current_nte_profile = anomaly::LoadBuildProfile(*current_nte_profile_path);
        const auto* const profile = current_nte_profile.profile
            ? &*current_nte_profile.profile
            : nullptr;
        const auto contains = [](
                                  const std::vector<std::string>& values,
                                  const std::string_view expected) {
            return std::ranges::any_of(values, [expected](const std::string& value) {
                return value == expected;
            });
        };
        const auto feature_symbols = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->features.find(
                      "nte.player-teleport");
                  return found == profile->features.end() ? nullptr : &found->second;
              }();
        const auto feature_validators = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->feature_layout_validators.find(
                      "nte.player-teleport");
                  return found == profile->feature_layout_validators.end() ? nullptr : &found->second;
              }();
        const auto feature_dependencies = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->feature_dependencies.find(
                      "nte.player-teleport");
                  return found == profile->feature_dependencies.end() ? nullptr : &found->second;
               }();
        const auto actor_feature_symbols = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->features.find("ue5.actors");
                  return found == profile->features.end() ? nullptr : &found->second;
              }();
        const auto actor_feature_validators = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->feature_layout_validators.find("ue5.actors");
                  return found == profile->feature_layout_validators.end() ? nullptr : &found->second;
              }();
        const auto actor_feature_dependencies = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->feature_dependencies.find("ue5.actors");
                  return found == profile->feature_dependencies.end() ? nullptr : &found->second;
              }();
        const auto function_feature_symbols = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->features.find("ue5.functions");
                  return found == profile->features.end() ? nullptr : &found->second;
              }();
        const auto function_feature_validators = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->feature_layout_validators.find("ue5.functions");
                  return found == profile->feature_layout_validators.end() ? nullptr : &found->second;
              }();
        const auto function_feature_dependencies = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->feature_dependencies.find("ue5.functions");
                  return found == profile->feature_dependencies.end() ? nullptr : &found->second;
              }();
        const auto entity_feature_symbols = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->features.find("nte.entities");
                  return found == profile->features.end() ? nullptr : &found->second;
              }();
        const auto entity_feature_validators = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->feature_layout_validators.find("nte.entities");
                  return found == profile->feature_layout_validators.end() ? nullptr : &found->second;
              }();
        const auto entity_feature_dependencies = profile == nullptr
            ? nullptr
            : [&]() -> const std::vector<std::string>* {
                  const auto found = profile->feature_dependencies.find("nte.entities");
                  return found == profile->feature_dependencies.end() ? nullptr : &found->second;
              }();
        const anomaly::ProfileSymbol* process_event{};
        if (profile != nullptr) {
            const auto found = profile->symbols.find("ue5.ProcessEvent");
            if (found != profile->symbols.end()) process_event = &found->second;
        }
        const auto requires_teleport_service = [&](const std::string_view symbol) {
            if (profile == nullptr) return false;
            const auto found = profile->symbols.find(symbol);
            return found != profile->symbols.end() &&
                contains(found->second.required_by, "anomaly.nte.player-teleport");
        };
        const bool excludes_rob_bank_contract = profile != nullptr &&
            !profile->symbols.contains("nte.HTRobBankContainerActor.Pickup") &&
            !profile->features.contains("nte.rob-bank-pickup") &&
            !profile->features.contains("nte.rob-bank-pickability") &&
            !profile->feature_layout_validators.contains("nte.rob-bank-pickup") &&
            !profile->feature_layout_validators.contains("nte.rob-bank-pickability") &&
            !profile->feature_dependencies.contains("nte.rob-bank-pickup") &&
            !profile->feature_dependencies.contains("nte.rob-bank-pickability") &&
            !profile->optional_features.contains("nte.rob-bank-pickup") &&
            !profile->optional_features.contains("nte.rob-bank-pickability") &&
            std::ranges::none_of(profile->layout, [](const auto& entry) {
                return std::string_view(entry.first).starts_with("robBank.");
            }) &&
            std::ranges::all_of(profile->symbols, [](const auto& entry) {
                return std::ranges::find(
                    entry.second.required_by, "anomaly.nte.rob-bank-pickup") ==
                    entry.second.required_by.end();
            });
        const bool excludes_fake_uid_contract = profile != nullptr &&
            std::ranges::all_of(
                std::array<std::string_view, 5>{
                    "ue5.TextBlockSetText",
                    "ue5.FStringAssign",
                    "ue5.FTextFromString",
                    "ue5.FStringFree",
                    "ue5.FTextToString"},
                [profile](const std::string_view symbol) {
                    return !profile->symbols.contains(std::string(symbol));
                }) &&
            std::ranges::none_of(profile->layout, [](const auto& entry) {
                return std::string_view(entry.first).starts_with("textBlock.");
            }) &&
            std::ranges::all_of(profile->symbols, [](const auto& entry) {
                return std::ranges::find(
                    entry.second.required_by, "anomaly.local.nte.fake-uid") ==
                    entry.second.required_by.end();
            });
        const bool retains_teleport_reflection_layout = profile != nullptr && std::ranges::all_of(
            std::array<std::string_view, 15>{
                "object.outer",
                "ustruct.propertyLink",
                "ufunction.numParms",
                "ufunction.parmsSize",
                "ufunction.returnValueOffset",
                "ffield.name",
                "fproperty.arrayDim",
                "fproperty.elementSize",
                "fproperty.offsetInternal",
                "fproperty.propertyLinkNext",
                "fstructProperty.struct",
                "fboolProperty.fieldSize",
                "fboolProperty.byteOffset",
                "fboolProperty.byteMask",
                "fboolProperty.fieldMask"},
            [profile](const std::string_view key) {
                return profile->layout.contains(std::string(key));
            });
        const bool retains_function_reflection_layout = profile != nullptr && std::ranges::all_of(
            std::array<std::string_view, 20>{
                "object.class",
                "object.nameOffset",
                "object.outer",
                "ufunction.numParms",
                "ufunction.parmsSize",
                "ufunction.returnValueOffset",
                "names.blocksOffset",
                "names.blockBits",
                "names.entryStride",
                "names.headerLengthShift",
                "objects.itemsOffset",
                "objects.maxCountOffset",
                "objects.countOffset",
                "objects.maxChunksOffset",
                "objects.numChunksOffset",
                "objects.chunkCountSize",
                "objects.chunkSize",
                "objects.itemStride",
                "objects.objectOffset",
                "objects.serialOffset"},
            [profile](const std::string_view key) {
                return profile->layout.contains(std::string(key));
            });
        result = Check(
                     current_nte_profile.Ok() && profile != nullptr &&
                          profile->feature_layout_validators.at("nte.player") ==
                              std::vector<std::string>{"nte-player-layout-v1"} &&
                          profile->feature_layout_validators.at("nte.player-esp") ==
                              std::vector<std::string>{"nte-player-esp-layout-v1"} &&
                          feature_symbols != nullptr && feature_symbols->size() == 5 &&
                          contains(*feature_symbols, "ue5.GWorld") &&
                          contains(*feature_symbols, "ue5.GameTick") &&
                          contains(*feature_symbols, "ue5.FNamePool") &&
                          contains(*feature_symbols, "ue5.GObjects") &&
                          contains(*feature_symbols, "ue5.ProcessEvent") &&
                          feature_validators != nullptr && feature_validators->size() == 2 &&
                          contains(*feature_validators, "nte-player-teleport-layout-v1") &&
                          contains(*feature_validators, "nte-player-teleport-process-event-abi-v1") &&
                          feature_dependencies != nullptr && feature_dependencies->size() == 3 &&
                          contains(*feature_dependencies, "nte.player") &&
                           contains(*feature_dependencies, "ue5.names") &&
                           contains(*feature_dependencies, "ue5.objects") &&
                           actor_feature_symbols != nullptr && actor_feature_symbols->size() == 3 &&
                           contains(*actor_feature_symbols, "ue5.GWorld") &&
                           contains(*actor_feature_symbols, "ue5.GameTick") &&
                           contains(*actor_feature_symbols, "ue5.FNamePool") &&
                           actor_feature_validators != nullptr &&
                           *actor_feature_validators ==
                               std::vector<std::string>{"ue5-actors-reflection-v1"} &&
                           actor_feature_dependencies != nullptr &&
                           actor_feature_dependencies->size() == 2 &&
                           contains(*actor_feature_dependencies, "ue5.world") &&
                           contains(*actor_feature_dependencies, "ue5.names") &&
                           function_feature_symbols != nullptr && function_feature_symbols->size() == 3 &&
                           contains(*function_feature_symbols, "ue5.GObjects") &&
                           contains(*function_feature_symbols, "ue5.GameTick") &&
                           contains(*function_feature_symbols, "ue5.FNamePool") &&
                           function_feature_validators != nullptr &&
                           *function_feature_validators ==
                               std::vector<std::string>{"ue5-functions-reflection-v1"} &&
                           function_feature_dependencies != nullptr &&
                           function_feature_dependencies->size() == 2 &&
                           contains(*function_feature_dependencies, "ue5.objects") &&
                           contains(*function_feature_dependencies, "ue5.names") &&
                           entity_feature_symbols != nullptr && entity_feature_symbols->size() == 3 &&
                           contains(*entity_feature_symbols, "ue5.GWorld") &&
                           contains(*entity_feature_symbols, "ue5.GameTick") &&
                           contains(*entity_feature_symbols, "ue5.FNamePool") &&
                           entity_feature_validators != nullptr &&
                           *entity_feature_validators ==
                               std::vector<std::string>{"nte-entities-layout-v2"} &&
                           entity_feature_dependencies != nullptr &&
                           *entity_feature_dependencies == std::vector<std::string>{"ue5.names"} &&
                            profile->layout.at("world.levels") == 464 &&
                            profile->layout.at("entities.maxLevels") == 4096 &&
                            profile->layout.at("entities.maxCount") == 32768 &&
                            profile->layout.at("controller.playerState") == 720 &&
                            profile->optional_features.contains("ue5.actors") &&
                            profile->optional_features.contains("ue5.functions") &&
                            profile->optional_features.contains("nte.player-esp") &&
                            profile->optional_features.contains("nte.player-teleport") &&
                            excludes_rob_bank_contract &&
                           process_event != nullptr && process_event->module == L"HTGame.exe" &&
                          process_event->section == ".text" &&
                          process_event->resolve.kind == anomaly::ProfileResolveKind::Direct &&
                          process_event->pattern.starts_with("40 55 56 57") &&
                          process_event->validators.size() == 2 &&
                          contains(process_event->validators, "address-in-module") &&
                          contains(process_event->validators, "executable") &&
                          requires_teleport_service("ue5.GWorld") &&
                          requires_teleport_service("ue5.FNamePool") &&
                            requires_teleport_service("ue5.GObjects") &&
                            requires_teleport_service("ue5.GameTick") &&
                            requires_teleport_service("ue5.ProcessEvent") &&
                            excludes_fake_uid_contract &&
                            retains_teleport_reflection_layout &&
                           retains_function_reflection_layout &&
                          !profile->layout.contains("object.vtable") &&
                          !profile->layout.contains("uobject.processEventVtableIndex") &&
                          !profile->features.contains("nte.player-root-location-override") &&
                          !profile->optional_features.contains("nte.player-root-location-override") &&
                          !profile->feature_layout_validators.contains(
                              "nte.player-root-location-override") &&
                          !profile->feature_dependencies.contains(
                              "nte.player-root-location-override") &&
                          !profile->layout.contains("sceneComponent.rootLocation") &&
                          !profile->symbols.contains("nte.BidKing.ClientResponseDispatch") &&
                          !profile->features.contains("nte.bidking-response-probe") &&
                          !profile->network_protocol,
                      "current NTE profile did not declare its trusted interop contracts or "
                        "excluded an unverified route") &&
            result;

        auto invalid_reflection_contract = ReadTextFile(*current_nte_profile_path);
        const std::string validator{"ue5-functions-reflection-v1"};
        const auto validator_offset = invalid_reflection_contract.find(validator);
        if (validator_offset != std::string::npos) {
            invalid_reflection_contract.replace(
                validator_offset, validator.size(), "unverified-functions-layout-v1");
        }
        const auto invalid_reflection_profile = anomaly::ParseBuildProfile(
            invalid_reflection_contract, *current_nte_profile_path);
        result = Check(
                     validator_offset != std::string::npos && !invalid_reflection_profile.Ok() &&
                         std::ranges::any_of(
                             invalid_reflection_profile.diagnostics,
                             [](const auto& diagnostic) {
                                 return diagnostic.path ==
                                        "/featureLayoutValidators/ue5.functions";
                             }),
                     "reflection feature accepted an unverified layout validator") &&
            result;

        auto incomplete_actor_reflection_contract = ReadTextFile(*current_nte_profile_path);
        const std::string actor_name_layout{"\"names.blocksOffset\""};
        const auto actor_name_layout_offset = incomplete_actor_reflection_contract.find(
            actor_name_layout);
        if (actor_name_layout_offset != std::string::npos) {
            incomplete_actor_reflection_contract.replace(
                actor_name_layout_offset, actor_name_layout.size(), "\"names.unusedBlocks\"");
        }
        const auto incomplete_actor_reflection_profile = anomaly::ParseBuildProfile(
            incomplete_actor_reflection_contract, *current_nte_profile_path);
        result = Check(
                     actor_name_layout_offset != std::string::npos &&
                         !incomplete_actor_reflection_profile.Ok() &&
                         std::ranges::any_of(
                             incomplete_actor_reflection_profile.diagnostics,
                             [](const auto& diagnostic) {
                                 return diagnostic.path == "/layout/names.blocksOffset";
                             }),
                     "actor reflection feature accepted an incomplete name layout contract") &&
            result;

    }

    const auto example_nte_profile_path = FindNteProfile("nte-build-profile.json.example");
    auto example_nte_profile_json = example_nte_profile_path
        ? ReadTextFile(*example_nte_profile_path)
        : std::string{};
    const auto name_pattern = example_nte_profile_json.find("FNAME_POOL_PATTERN");
    if (name_pattern != std::string::npos) {
        example_nte_profile_json.replace(
            name_pattern, sizeof("FNAME_POOL_PATTERN") - 1, "48 8D 05 ?? ?? ?? ??");
    }
    const auto tick_pattern = example_nte_profile_json.find("GAME_TICK_PATTERN");
    if (tick_pattern != std::string::npos) {
        example_nte_profile_json.replace(
            tick_pattern, sizeof("GAME_TICK_PATTERN") - 1, "40 53 48 83 EC 20");
    }
    const auto example_nte_profile = anomaly::ParseBuildProfile(
        example_nte_profile_json,
        example_nte_profile_path ? *example_nte_profile_path : std::filesystem::path{});
    result = Check(
                 example_nte_profile_path.has_value() && name_pattern != std::string::npos &&
                     tick_pattern != std::string::npos && example_nte_profile.Ok() &&
                     example_nte_profile.profile &&
                     example_nte_profile.profile->features.contains("ue5.actors") &&
                     example_nte_profile.profile->features.contains("ue5.functions"),
                 "NTE profile template did not retain the reflection feature contracts") &&
        result;

    auto unknown_dependency_json = extended_json;
    const auto dependency = unknown_dependency_json.find("\"ue5.world\"]");
    if (dependency != std::string::npos) {
        unknown_dependency_json.replace(
            dependency, sizeof("\"ue5.world\"") - 1, "\"missing.feature\"");
    }
    const auto unknown_dependency = anomaly::ParseBuildProfile(unknown_dependency_json);
    result = Check(!unknown_dependency.Ok() && std::ranges::any_of(
                       unknown_dependency.diagnostics, [](const auto& diagnostic) {
                           return diagnostic.path.starts_with("/featureDependencies/");
                       }),
                   "feature dependency accepted an unknown feature") && result;

    auto unknown_optional_json = extended_json;
    const auto optional_feature = unknown_optional_json.find("\"ue5.dependent\"");
    if (optional_feature != std::string::npos) {
        unknown_optional_json.replace(
            optional_feature, sizeof("\"ue5.dependent\"") - 1,
            "\"missing.optional\"");
    }
    const auto unknown_optional = anomaly::ParseBuildProfile(unknown_optional_json);
    result = Check(!unknown_optional.Ok() && std::ranges::any_of(
                       unknown_optional.diagnostics, [](const auto& diagnostic) {
                           return diagnostic.path.starts_with("/optionalFeatures/");
                       }),
                   "optional feature list accepted an unknown feature") && result;

    auto self_cycle_json = extended_json;
    const auto self_cycle_dependency = self_cycle_json.find("\"ue5.world\"]");
    if (self_cycle_dependency != std::string::npos) {
        self_cycle_json.replace(
            self_cycle_dependency, sizeof("\"ue5.world\"") - 1, "\"ue5.dependent\"");
    }
    const auto self_cycle = anomaly::ParseBuildProfile(self_cycle_json);
    result = Check(!self_cycle.Ok() && std::ranges::any_of(
                       self_cycle.diagnostics, [](const auto& diagnostic) {
                           return diagnostic.path.starts_with("/featureDependencies/") &&
                               diagnostic.message.find("cycle") != std::string::npos;
                       }),
                   "feature dependency accepted a self-cycle") && result;

    auto mutual_cycle_json = extended_json;
    const std::string dependency_entry{"\"ue5.dependent\": [\"ue5.world\"]"};
    const auto dependency_entry_offset = mutual_cycle_json.find(dependency_entry);
    if (dependency_entry_offset != std::string::npos) {
        mutual_cycle_json.replace(
            dependency_entry_offset, dependency_entry.size(),
            "\"ue5.dependent\": [\"ue5.world\"],\n"
            "    \"ue5.world\": [\"ue5.dependent\"]");
    }
    const auto mutual_cycle = anomaly::ParseBuildProfile(mutual_cycle_json);
    result = Check(!mutual_cycle.Ok() && std::ranges::any_of(
                       mutual_cycle.diagnostics, [](const auto& diagnostic) {
                           return diagnostic.path.starts_with("/featureDependencies/") &&
                               diagnostic.message.find("cycle") != std::string::npos;
                       }),
                   "feature dependency accepted a multi-node cycle") && result;

    const auto invalid = anomaly::ParseBuildProfile(R"({"schemaVersion":1})");
    result = Check(!invalid.Ok() && !invalid.diagnostics.empty(),
                   "invalid profile passed schema validation") && result;
    auto invalid_pattern_json = json;
    const auto pattern_offset = invalid_pattern_json.find("48 8B 1D ?? ?? ?? ??");
    invalid_pattern_json.replace(pattern_offset, sizeof("48 8B 1D ?? ?? ?? ??") - 1, "NOT_A_PATTERN");
    const auto invalid_pattern = anomaly::ParseBuildProfile(invalid_pattern_json);
    result = Check(!invalid_pattern.Ok() && std::ranges::any_of(
                       invalid_pattern.diagnostics, [](const auto& diagnostic) {
                           return diagnostic.path.ends_with("/pattern");
                       }),
                   "profile tool accepted invalid pattern syntax") && result;

    const auto profiles = root / L"profiles";
    std::filesystem::create_directories(profiles);
    std::ofstream(profiles / L"known.json") << json;
    anomaly::BuildProfileCatalog catalog;
    const auto snapshot = catalog.Scan(profiles);
    const auto* selected = snapshot.profiles.empty() ? nullptr : &snapshot.profiles.front();
    result = Check(snapshot.profiles.size() == 1 && selected != nullptr,
                   "catalog did not load the profile recipe") && result;

    const auto malformed_profile_path = profiles / L"malformed.json";
    std::ofstream(malformed_profile_path) << "{ not valid JSON";
    const auto malformed_snapshot = catalog.Scan(profiles);
    result = Check(
                 malformed_snapshot.profiles.size() == 1 &&
                     std::filesystem::exists(malformed_profile_path) &&
                     std::ranges::any_of(
                         malformed_snapshot.diagnostics, [&](const auto& diagnostic) {
                             return diagnostic.source == malformed_profile_path;
                         }),
                 "malformed profile changed the active recipe catalog") &&
        result;

    const auto bundled_profiles = root / L"layered" / L"bundled";
    const auto managed_profiles = root / L"layered" / L"managed";
    const auto local_profiles = root / L"layered" / L"local";
    std::filesystem::create_directories(bundled_profiles);
    std::filesystem::create_directories(managed_profiles);
    std::filesystem::create_directories(local_profiles);
    std::ofstream(bundled_profiles / L"profile.json") << json;
    std::ofstream(managed_profiles / L"profile.json") << json;
    std::ofstream(local_profiles / L"profile.json") << json;
    const auto layered = catalog.ScanLayered({
        {local_profiles, "local-override", 300, true},
        {managed_profiles, "repository", 200, true},
        {bundled_profiles, "bundled", 100, false}});
    result = Check(layered.profiles.size() == 1 &&
                   layered.profiles.front().source_channel == "local-override" &&
                   layered.profiles.front().source_priority == 300 &&
                   std::ranges::count_if(layered.diagnostics, [](const auto& diagnostic) {
                       return diagnostic.message.find("shadowed") != std::string::npos;
                   }) == 2,
                   "layered Profile priority did not prefer local override") && result;

    const auto cache_file = root / L"state" / L"profile-symbol-cache.json";
    anomaly::SymbolCache cache(cache_file);
    anomaly::SymbolCacheRecord record;
    record.symbols.emplace("ue5.GWorld", anomaly::CachedSymbol{L"fixture.exe", 0x1234});
    result = Check(cache.Store(record, &error),
                   "profile cache store failed") && result;
    const auto loaded = cache.Load();
    result = Check(cache.File() == std::filesystem::absolute(cache_file) && loaded &&
                       loaded->symbols.at("ue5.GWorld").rva == 0x1234,
                   "profile cache round-trip failed") && result;

    auto replacement = record;
    replacement.symbols.at("ue5.GWorld").rva = 0x4321;
    result = Check(cache.Store(replacement, &error) && cache.Load() &&
                       cache.Load()->symbols.at("ue5.GWorld").rva == 0x4321 &&
                       std::ranges::distance(
                           std::filesystem::directory_iterator(cache_file.parent_path()),
                           std::filesystem::directory_iterator{}) == 1,
                   "trusted RVA cache did not atomically replace its single fixed record") && result;

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result ? 0 : 2;
}
