#include "anomaly/build_profile.hpp"

#include "build_profile_schema.hpp"
#include "pattern.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <winver.h>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace anomaly {
namespace {

using Json = nlohmann::json;

std::string Sha256(const void* data, std::size_t size) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::array<std::uint8_t, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return {};
    }
    const auto close_algorithm = [&] { BCryptCloseAlgorithmProvider(algorithm, 0); };
    DWORD object_size{};
    DWORD copied{};
    if (!BCRYPT_SUCCESS(BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0))) {
        close_algorithm();
        return {};
    }
    std::vector<std::uint8_t> object(object_size);
    if (!BCRYPT_SUCCESS(BCryptCreateHash(
            algorithm, &hash, object.data(), object_size, nullptr, 0, 0)) ||
        !BCRYPT_SUCCESS(BCryptHashData(
            hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
            static_cast<ULONG>(size), 0)) ||
        !BCRYPT_SUCCESS(BCryptFinishHash(
            hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        if (hash != nullptr) BCryptDestroyHash(hash);
        close_algorithm();
        return {};
    }
    BCryptDestroyHash(hash);
    close_algorithm();
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const auto value : digest) encoded << std::setw(2) << static_cast<unsigned>(value);
    return encoded.str();
}

std::string ReadFileVersion(const std::filesystem::path& path) {
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<std::byte> data(size);
    if (GetFileVersionInfoW(path.c_str(), 0, size, data.data()) == FALSE) return {};
    VS_FIXEDFILEINFO* info{};
    UINT info_size{};
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) == FALSE ||
        info == nullptr || info_size < sizeof(*info) || info->dwSignature != 0xFEEF04BD) {
        return {};
    }
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
        std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
        std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
        std::to_string(LOWORD(info->dwFileVersionLS));
}

std::string ToLowerAscii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool EqualWideInsensitive(std::wstring_view left, std::wstring_view right) noexcept {
    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring WideUtf8(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("profile contains invalid UTF-8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size);
    return result;
}

std::string Utf8Wide(std::wstring_view value) {
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

std::optional<std::string> ReadText(const std::filesystem::path& path, std::size_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!text.empty()) input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input && !text.empty()) return std::nullopt;
    return text;
}

void AddDiagnostic(
    BuildProfileParseResult& result,
    const std::filesystem::path& source,
    std::string path,
    std::string message) {
    result.diagnostics.push_back({source, std::move(path), std::move(message)});
}

bool ParseFeatureStringLists(
    const Json& document,
    std::string_view field,
    const std::map<std::string, std::vector<std::string>, std::less<>>& features,
    bool values_must_be_features,
    std::map<std::string, std::vector<std::string>, std::less<>>& output,
    BuildProfileParseResult& result,
    const std::filesystem::path& source) {
    const auto extension = document.find(std::string(field));
    if (extension == document.end()) return true;
    const std::string root = "/" + std::string(field);
    if (!extension->is_object()) {
        AddDiagnostic(result, source, root, "must be an object keyed by feature id");
        return false;
    }
    if (extension->size() > 128) {
        AddDiagnostic(result, source, root, "contains too many feature entries");
        return false;
    }
    for (auto iterator = extension->begin(); iterator != extension->end(); ++iterator) {
        const std::string path = root + "/" + iterator.key();
        if (!features.contains(iterator.key())) {
            AddDiagnostic(result, source, path, "references an unknown feature");
            continue;
        }
        if (!iterator.value().is_array()) {
            AddDiagnostic(result, source, path, "must be an array of strings");
            continue;
        }
        if (iterator.value().size() > 64) {
            AddDiagnostic(result, source, path, "contains too many entries");
            continue;
        }
        std::set<std::string, std::less<>> values;
        for (std::size_t index{}; index < iterator.value().size(); ++index) {
            const Json& value = iterator.value().at(index);
            const std::string item_path = path + "/" + std::to_string(index);
            if (!value.is_string()) {
                AddDiagnostic(result, source, item_path, "must be a string");
                continue;
            }
            const std::string item = value.get<std::string>();
            if (item.empty() || item.size() > 128) {
                AddDiagnostic(result, source, item_path, "must contain 1 to 128 characters");
                continue;
            }
            if (values_must_be_features && !features.contains(item)) {
                AddDiagnostic(result, source, item_path, "references an unknown feature " + item);
                continue;
            }
            if (!values.insert(item).second) {
                AddDiagnostic(result, source, item_path, "must not repeat an entry");
            }
        }
        output.emplace(iterator.key(), std::vector<std::string>(values.begin(), values.end()));
    }
    return result.diagnostics.empty();
}

[[nodiscard]] Ue5AckMode ParseAckMode(const std::string_view value) {
    if (value == "none") return Ue5AckMode::None;
    if (value == "always") return Ue5AckMode::Always;
    if (value == "presence-bit") return Ue5AckMode::PresenceBit;
    throw std::invalid_argument("unknown packet ACK mode");
}

[[nodiscard]] Ue5PacketCaptureStage ParseCaptureStage(const std::string_view value) {
    if (value == "post-packet-handler") return Ue5PacketCaptureStage::PostPacketHandler;
    throw std::invalid_argument("unknown packet capture stage");
}

[[nodiscard]] Ue5PacketProtection ParsePacketProtection(const std::string_view value) {
    if (value == "clear") return Ue5PacketProtection::Clear;
    throw std::invalid_argument("unknown packet protection state");
}

[[nodiscard]] Ue5PartialIdMode ParsePartialIdMode(const std::string_view value) {
    if (value == "none") return Ue5PartialIdMode::None;
    if (value == "explicit") return Ue5PartialIdMode::Explicit;
    throw std::invalid_argument("unknown packet partial identifier mode");
}

[[nodiscard]] Ue5SchemaFamily ParseSchemaFamily(const std::string_view value) {
    if (value == "legacy-rep-layout") return Ue5SchemaFamily::LegacyRepLayout;
    if (value == "iris") return Ue5SchemaFamily::Iris;
    throw std::invalid_argument("unknown UE packet schema family");
}

[[nodiscard]] Ue5FieldStreamEncoding ParseFieldStreamEncoding(const std::string_view value) {
    if (value == "verified-tagged-fields-v1") {
        return Ue5FieldStreamEncoding::VerifiedTaggedFieldsV1;
    }
    throw std::invalid_argument("unknown UE packet field stream encoding");
}

[[nodiscard]] Ue5FieldEncoding ParseFieldEncoding(const std::string_view value) {
    if (value == "boolean") return Ue5FieldEncoding::Boolean;
    if (value == "unsigned") return Ue5FieldEncoding::Unsigned;
    if (value == "signed") return Ue5FieldEncoding::Signed;
    if (value == "float32") return Ue5FieldEncoding::Float32;
    if (value == "utf8") return Ue5FieldEncoding::Utf8;
    if (value == "bytes") return Ue5FieldEncoding::Bytes;
    if (value == "vector3-float32") return Ue5FieldEncoding::Vector3Float32;
    throw std::invalid_argument("unknown UE packet field encoding");
}

[[nodiscard]] bool FeatureRequiresSymbol(
    const BuildProfile& profile,
    const std::string_view feature,
    const std::string_view symbol) {
    const auto found = profile.features.find(std::string(feature));
    if (found == profile.features.end()) return false;
    return std::find(found->second.begin(), found->second.end(), symbol) != found->second.end();
}

[[nodiscard]] bool FeatureRequiresDependency(
    const BuildProfile& profile,
    const std::string_view feature,
    const std::string_view dependency) {
    const auto found = profile.feature_dependencies.find(std::string(feature));
    return found != profile.feature_dependencies.end() &&
        std::find(found->second.begin(), found->second.end(), dependency) != found->second.end();
}

[[nodiscard]] bool FeatureDeclaresLayoutValidator(
    const BuildProfile& profile,
    const std::string_view feature,
    const std::string_view validator) {
    const auto found = profile.feature_layout_validators.find(std::string(feature));
    return found != profile.feature_layout_validators.end() &&
        std::find(found->second.begin(), found->second.end(), validator) != found->second.end();
}

void ValidateReflectionFeatureContract(
    const BuildProfile& profile,
    const std::string_view feature,
    const std::initializer_list<std::string_view> required_symbols,
    const std::initializer_list<std::string_view> required_dependencies,
    const std::string_view required_validator,
    const std::initializer_list<std::string_view> required_layout,
    BuildProfileParseResult& result,
    const std::filesystem::path& source) {
    if (!profile.features.contains(std::string(feature))) return;

    for (const std::string_view symbol : required_symbols) {
        if (FeatureRequiresSymbol(profile, feature, symbol)) continue;
        AddDiagnostic(
            result, source, "/features/" + std::string(feature),
            "must require " + std::string(symbol));
    }
    for (const std::string_view dependency : required_dependencies) {
        if (FeatureRequiresDependency(profile, feature, dependency)) continue;
        AddDiagnostic(
            result, source, "/featureDependencies/" + std::string(feature),
            "must require feature dependency " + std::string(dependency));
    }
    if (!FeatureDeclaresLayoutValidator(profile, feature, required_validator)) {
        AddDiagnostic(
            result, source, "/featureLayoutValidators/" + std::string(feature),
            "must require " + std::string(required_validator));
    }
    for (const std::string_view key : required_layout) {
        if (profile.layout.contains(std::string(key))) continue;
        AddDiagnostic(
            result, source, "/layout/" + std::string(key),
            std::string(feature) + " requires this layout field");
    }
}

void ValidateReflectionFeatureContracts(
    const BuildProfile& profile,
    BuildProfileParseResult& result,
    const std::filesystem::path& source) {
    ValidateReflectionFeatureContract(
        profile, "ue5.actors",
        {"ue5.GWorld", "ue5.GameTick", "ue5.FNamePool"},
        {"ue5.world", "ue5.names"},
        "ue5-actors-reflection-v1",
        {"world.persistentLevel", "level.actors", "object.class", "object.nameOffset",
         "object.outer", "names.blocksOffset", "names.blockBits", "names.entryStride",
         "names.headerLengthShift"},
        result, source);
    ValidateReflectionFeatureContract(
        profile, "ue5.functions",
        {"ue5.GObjects", "ue5.GameTick", "ue5.FNamePool"},
        {"ue5.objects", "ue5.names"},
        "ue5-functions-reflection-v1",
        {"object.class", "object.nameOffset", "object.outer", "ufunction.numParms",
         "ufunction.parmsSize", "ufunction.returnValueOffset", "names.blocksOffset",
         "names.blockBits", "names.entryStride", "names.headerLengthShift", "objects.itemsOffset",
         "objects.maxCountOffset", "objects.countOffset", "objects.maxChunksOffset",
         "objects.numChunksOffset", "objects.chunkCountSize", "objects.chunkSize",
         "objects.itemStride", "objects.objectOffset", "objects.serialOffset"},
        result, source);
}

void ParseNetworkProtocol(
    const Json& document,
    BuildProfile& profile,
    BuildProfileParseResult& result,
    const std::filesystem::path& source) {
    const auto network = document.find("network");
    if (network == document.end()) return;

    try {
        ProfileNetworkProtocol parsed;
        const Json& capture = network->at("capture");
        const auto parse_capture_boundary = [](const Json& value) {
            ProfileNetworkCaptureBoundary boundary;
            boundary.feature = value.at("feature").get<std::string>();
            boundary.boundary_symbol = value.at("boundarySymbol").get<std::string>();
            boundary.abi_validator = value.at("abiValidator").get<std::string>();
            boundary.capture_stage = ParseCaptureStage(value.at("stage").get<std::string>());
            boundary.capture_protection =
                ParsePacketProtection(value.at("protection").get<std::string>());
            return boundary;
        };
        parsed.inbound = parse_capture_boundary(capture.at("inbound"));
        parsed.outbound = parse_capture_boundary(capture.at("outbound"));

        Ue5PacketProtocolProfile& protocol = parsed.protocol;
        const Json& header = network->at("header");
        protocol.header.sequence_bits =
            static_cast<std::uint8_t>(header.at("sequenceBits").get<std::uint32_t>());
        protocol.header.ack_mode = ParseAckMode(header.at("ackMode").get<std::string>());
        protocol.header.ack_sequence_bits =
            static_cast<std::uint8_t>(header.at("ackSequenceBits").get<std::uint32_t>());
        protocol.header.ack_history_bits =
            static_cast<std::uint8_t>(header.at("ackHistoryBits").get<std::uint32_t>());
        protocol.header.bunch_count_bits =
            static_cast<std::uint8_t>(header.at("bunchCountBits").get<std::uint32_t>());

        const Json& bunch = network->at("bunch");
        protocol.bunch.maximum_channels = bunch.at("maximumChannels").get<std::uint32_t>();
        protocol.bunch.maximum_channel_types = bunch.at("maximumChannelTypes").get<std::uint32_t>();
        protocol.bunch.maximum_reliable_sequence =
            bunch.at("maximumReliableSequence").get<std::uint32_t>();
        protocol.bunch.maximum_partial_id = bunch.at("maximumPartialId").get<std::uint32_t>();
        protocol.bunch.maximum_payload_bits = bunch.at("maximumPayloadBits").get<std::uint32_t>();
        protocol.bunch.close_has_dormancy_bit = bunch.at("closeHasDormancyBit").get<bool>();
        protocol.bunch.has_package_map_export_flags =
            bunch.at("hasPackageMapExportFlags").get<bool>();
        protocol.bunch.partial_id_mode =
            ParsePartialIdMode(bunch.at("partialIdMode").get<std::string>());

        const Json& limits = network->at("limits");
        protocol.limits.maximum_packet_bits = limits.at("maximumPacketBits").get<std::size_t>();
        protocol.limits.maximum_connections = limits.at("maximumConnections").get<std::size_t>();
        protocol.limits.maximum_packet_history = limits.at("maximumPacketHistory").get<std::size_t>();
        protocol.limits.maximum_bunches_per_packet =
            limits.at("maximumBunchesPerPacket").get<std::size_t>();
        protocol.limits.maximum_open_partial_bunches =
            limits.at("maximumOpenPartialBunches").get<std::size_t>();
        protocol.limits.maximum_partial_fragments =
            limits.at("maximumPartialFragments").get<std::size_t>();
        protocol.limits.maximum_partial_packet_age =
            limits.at("maximumPartialPacketAge").get<std::size_t>();
        protocol.limits.maximum_partial_bunch_bits =
            limits.at("maximumPartialBunchBits").get<std::size_t>();
        protocol.limits.maximum_partial_state_bits =
            limits.at("maximumPartialStateBits").get<std::size_t>();
        protocol.limits.maximum_fields_per_bunch =
            limits.at("maximumFieldsPerBunch").get<std::size_t>();
        protocol.limits.maximum_field_bytes = limits.at("maximumFieldBytes").get<std::size_t>();

        for (const Json& schema : network->at("schemas")) {
            Ue5ChannelSchema parsed_schema;
            parsed_schema.channel_type = schema.at("channelType").get<std::uint32_t>();
            parsed_schema.id = schema.at("id").get<std::string>();
            parsed_schema.family = ParseSchemaFamily(schema.at("family").get<std::string>());
            parsed_schema.encoding =
                ParseFieldStreamEncoding(schema.at("encoding").get<std::string>());
            for (const Json& field : schema.at("fields")) {
                Ue5FieldDefinition parsed_field;
                parsed_field.handle = field.at("handle").get<std::uint32_t>();
                parsed_field.name = field.at("name").get<std::string>();
                parsed_field.encoding =
                    ParseFieldEncoding(field.at("encoding").get<std::string>());
                parsed_field.bit_width =
                    static_cast<std::uint8_t>(field.at("bitWidth").get<std::uint32_t>());
                parsed_field.maximum_length = field.at("maximumLength").get<std::uint32_t>();
                parsed_schema.fields.push_back(std::move(parsed_field));
            }
            protocol.schemas.push_back(std::move(parsed_schema));
        }

        const auto validate_capture = [&](const ProfileNetworkCaptureBoundary& boundary,
                                          const std::string_view direction) {
            const std::string path = "/network/capture/" + std::string(direction);
            if (!profile.features.contains(boundary.feature)) {
                AddDiagnostic(
                    result, source, path + "/feature",
                    "references an unknown profile feature");
                return false;
            }
            if (!profile.symbols.contains(boundary.boundary_symbol)) {
                AddDiagnostic(
                    result, source, path + "/boundarySymbol",
                    "references an unknown profile symbol");
                return false;
            }
            if (!FeatureRequiresSymbol(profile, boundary.feature, boundary.boundary_symbol)) {
                AddDiagnostic(
                    result, source, path + "/boundarySymbol",
                    "the capture feature does not require the capture boundary symbol");
                return false;
            }
            const auto validators = profile.feature_layout_validators.find(boundary.feature);
            if (validators == profile.feature_layout_validators.end() ||
                std::find(
                    validators->second.begin(), validators->second.end(),
                    boundary.abi_validator) == validators->second.end()) {
                AddDiagnostic(
                    result, source, path + "/abiValidator",
                    "the capture feature does not require the declared ABI validator");
                return false;
            }
            return true;
        };
        if (!validate_capture(parsed.inbound, "inbound") ||
            !validate_capture(parsed.outbound, "outbound")) {
            return;
        }

        if (protocol.header.ack_mode == Ue5AckMode::None &&
            (protocol.header.ack_sequence_bits != 0U || protocol.header.ack_history_bits != 0U)) {
            AddDiagnostic(
                result, source, "/network/header",
                "ACK-disabled packet profiles must not declare ACK widths");
            return;
        }
        if (protocol.bunch.partial_id_mode == Ue5PartialIdMode::None &&
            protocol.bunch.maximum_partial_id != 0U) {
            AddDiagnostic(
                result, source, "/network/bunch/maximumPartialId",
                "a non-explicit partial identifier mode requires a zero maximum");
            return;
        }
        if (protocol.bunch.partial_id_mode == Ue5PartialIdMode::Explicit &&
            protocol.bunch.maximum_partial_id == 0U) {
            AddDiagnostic(
                result, source, "/network/bunch/maximumPartialId",
                "an explicit partial identifier mode requires a nonzero maximum");
            return;
        }
        if (protocol.bunch.maximum_payload_bits > protocol.limits.maximum_packet_bits ||
            protocol.limits.maximum_partial_bunch_bits < protocol.bunch.maximum_payload_bits ||
            protocol.limits.maximum_partial_state_bits < protocol.limits.maximum_partial_bunch_bits) {
            AddDiagnostic(
                result, source, "/network/limits",
                "packet, payload, and partial-state limits are inconsistent");
            return;
        }

        profile.network_protocol = std::move(parsed);
    } catch (const std::exception& exception) {
        AddDiagnostic(result, source, "/network", exception.what());
    }
}

void ValidateFeatureDependencyCycles(
    const std::map<std::string, std::vector<std::string>, std::less<>>& features,
    const std::map<std::string, std::vector<std::string>, std::less<>>& dependencies,
    BuildProfileParseResult& result,
    const std::filesystem::path& source) {
    enum class VisitState : std::uint8_t { Unvisited, Visiting, Completed };

    std::map<std::string, VisitState, std::less<>> visits;
    std::function<void(const std::string&)> visit;
    visit = [&](const std::string& feature) {
        auto& state = visits[feature];
        if (state != VisitState::Unvisited) return;

        state = VisitState::Visiting;
        if (const auto found = dependencies.find(feature); found != dependencies.end()) {
            for (const auto& dependency : found->second) {
                const VisitState dependency_state = visits[dependency];
                if (dependency_state == VisitState::Visiting) {
                    AddDiagnostic(
                        result, source, "/featureDependencies/" + feature,
                        "feature dependency cycle through " + dependency);
                    continue;
                }
                if (dependency_state == VisitState::Unvisited) visit(dependency);
            }
        }
        state = VisitState::Completed;
    };

    for (const auto& [feature, unused] : features) {
        static_cast<void>(unused);
        visit(feature);
    }
}

class SchemaErrorHandler final : public nlohmann::json_schema::basic_error_handler {
public:
    void error(
        const nlohmann::json::json_pointer& pointer,
        const nlohmann::json& instance,
        const std::string& message) override {
        static_cast<void>(instance);
        basic_error_handler::error(pointer, instance, message);
        errors.push_back({pointer.to_string(), message});
    }

    std::vector<std::pair<std::string, std::string>> errors;
};

std::string CacheJson(const SymbolCacheRecord& record) {
    Json document;
    document["schemaVersion"] = 1;
    Json symbols = Json::object();
    for (const auto& [id, symbol] : record.symbols) {
        symbols[id] = {{"module", Utf8Wide(symbol.module)}, {"rva", symbol.rva}};
    }
    document["symbols"] = std::move(symbols);
    return document.dump(2) + "\n";
}

}  // namespace

std::optional<BuildFingerprint> FingerprintPeFile(
    const std::filesystem::path& path,
    std::string game,
    std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<BuildFingerprint> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };
    const auto bytes = ReadText(path, (std::numeric_limits<std::size_t>::max)());
    if (!bytes) return fail("failed to read PE file");
    if (bytes->size() < sizeof(IMAGE_DOS_HEADER)) return fail("truncated DOS header");
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, bytes->data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        return fail("invalid DOS header");
    }
    const std::size_t nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    if (nt_offset > bytes->size() || bytes->size() - nt_offset < sizeof(IMAGE_NT_HEADERS64)) {
        return fail("truncated NT headers");
    }
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, bytes->data() + nt_offset, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return fail("PE is not a 64-bit image");
    }
    const std::size_t section_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;
    const std::size_t section_bytes =
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (section_offset > bytes->size() || section_bytes > bytes->size() - section_offset) {
        return fail("truncated section table");
    }
    std::optional<IMAGE_SECTION_HEADER> text;
    std::optional<IMAGE_SECTION_HEADER> executable_code;
    for (std::size_t index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER section{};
        std::memcpy(
            &section,
            bytes->data() + section_offset + index * sizeof(section),
            sizeof(section));
        if (std::memcmp(section.Name, ".text", 5) == 0) {
            text = section;
            break;
        }
        if (!executable_code &&
            (section.Characteristics & IMAGE_SCN_CNT_CODE) != 0 &&
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            executable_code = section;
        }
    }
    if (!text) text = executable_code;
    if (!text) return fail("PE has no executable code section");
    const std::size_t raw_offset = text->PointerToRawData;
    const std::size_t raw_size = text->SizeOfRawData;
    if (raw_offset > bytes->size() || raw_size > bytes->size() - raw_offset) {
        return fail("truncated .text section");
    }
    const std::string text_hash = Sha256(bytes->data() + raw_offset, raw_size);
    if (text_hash.empty()) return fail("SHA-256 provider failed");

    BuildFingerprint result;
    result.game = ToLowerAscii(std::move(game));
    result.module = path.filename().wstring();
    result.canonical_path_tail = result.module;
    result.machine = nt.FileHeader.Machine;
    result.timestamp = nt.FileHeader.TimeDateStamp;
    result.image_size = nt.OptionalHeader.SizeOfImage;
    result.text_virtual_size = text->Misc.VirtualSize;
    result.text_sha256 = text_hash;
    result.file_version = ReadFileVersion(path);
    std::ostringstream id;
    id << result.game << "-win64-" << std::uppercase << std::hex << std::setfill('0')
       << std::setw(8) << result.timestamp << '-' << std::setw(8) << result.image_size << '-'
       << text_hash.substr(0, 16);
    result.id = ToLowerAscii(id.str());
    return result;
}

BuildProfileParseResult ParseBuildProfile(
    std::string_view json,
    std::filesystem::path source) {
    BuildProfileParseResult result;
    if (json.size() > kMaximumBuildProfileBytes) {
        AddDiagnostic(result, source, {}, "profile exceeds maximum size");
        return result;
    }
    try {
        const Json document = Json::parse(json);
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(Json::parse(generated::kBuildProfileSchema));
        SchemaErrorHandler handler;
        validator.validate(document, handler);
        for (const auto& [path, message] : handler.errors) {
            AddDiagnostic(result, source, path, message);
        }
        if (!handler.errors.empty()) return result;

        BuildProfile profile;
        profile.schema_version = document.at("schemaVersion").get<std::uint32_t>();
        profile.game = document.at("game").get<std::string>();

        for (auto iterator = document.at("symbols").begin();
             iterator != document.at("symbols").end(); ++iterator) {
            ProfileSymbol symbol;
            symbol.id = iterator.key();
            symbol.module = WideUtf8(iterator.value().at("module").get<std::string>());
            symbol.section = iterator.value().at("section").get<std::string>();
            symbol.pattern = iterator.value().at("pattern").get<std::string>();
            const auto& resolve = iterator.value().at("resolve");
            symbol.resolve.kind = resolve.at("kind") == "direct"
                ? ProfileResolveKind::Direct
                : ProfileResolveKind::RipRelative32;
            symbol.resolve.offset = resolve.value("offset", std::size_t{});
            symbol.resolve.instruction_size = resolve.value("instructionSize", std::size_t{});
            symbol.resolve.addend = resolve.value("addend", std::ptrdiff_t{});
            symbol.validators = iterator.value().at("validators").get<std::vector<std::string>>();
            symbol.required_by = iterator.value().at("requiredBy").get<std::vector<std::string>>();
            try {
                static_cast<void>(ue5mem::Pattern::Parse(symbol.pattern));
            } catch (const std::invalid_argument& exception) {
                AddDiagnostic(result, source, "/symbols/" + symbol.id + "/pattern",
                              exception.what());
            }
            if (symbol.resolve.kind == ProfileResolveKind::RipRelative32 &&
                symbol.resolve.instruction_size == 0) {
                AddDiagnostic(result, source, "/symbols/" + symbol.id + "/resolve",
                              "rip-rel32 requires instructionSize");
            }
            if (symbol.resolve.kind == ProfileResolveKind::RipRelative32 &&
                (symbol.resolve.offset > symbol.resolve.instruction_size ||
                 symbol.resolve.instruction_size - symbol.resolve.offset < sizeof(std::int32_t))) {
                AddDiagnostic(result, source, "/symbols/" + symbol.id + "/resolve",
                              "RIP displacement must fit inside the instruction");
            }
            profile.symbols.emplace(symbol.id, std::move(symbol));
        }
        for (auto iterator = document.at("features").begin();
             iterator != document.at("features").end(); ++iterator) {
            auto requirements = iterator.value().get<std::vector<std::string>>();
            for (const auto& symbol : requirements) {
                if (!profile.symbols.contains(symbol)) {
                    AddDiagnostic(result, source, "/features/" + iterator.key(),
                                  "feature references unknown symbol " + symbol);
                }
            }
            profile.features.emplace(iterator.key(), std::move(requirements));
        }
        if (document.contains("optionalFeatures")) {
            for (std::size_t index{}; index < document.at("optionalFeatures").size(); ++index) {
                const std::string feature =
                    document.at("optionalFeatures").at(index).get<std::string>();
                if (!profile.features.contains(feature)) {
                    AddDiagnostic(
                        result, source, "/optionalFeatures/" + std::to_string(index),
                        "references an unknown feature " + feature);
                    continue;
                }
                profile.optional_features.insert(feature);
            }
        }
        if (document.contains("layout")) {
            for (auto iterator = document.at("layout").begin();
                 iterator != document.at("layout").end(); ++iterator) {
                profile.layout.emplace(iterator.key(), iterator.value().get<std::int64_t>());
            }
        }
        static_cast<void>(ParseFeatureStringLists(
            document, "featureLayoutValidators", profile.features, false,
            profile.feature_layout_validators, result, source));
        static_cast<void>(ParseFeatureStringLists(
            document, "featureDependencies", profile.features, true,
            profile.feature_dependencies, result, source));
        ValidateReflectionFeatureContracts(profile, result, source);
        ParseNetworkProtocol(document, profile, result, source);
        if (result.diagnostics.empty()) {
            ValidateFeatureDependencyCycles(
                profile.features, profile.feature_dependencies, result, source);
        }
        if (!result.diagnostics.empty()) return result;
        profile.source = std::move(source);
        profile.source_hash = Sha256(json.data(), json.size());
        if (profile.source_hash.empty()) {
            AddDiagnostic(result, profile.source, {}, "profile SHA-256 provider failed");
            return result;
        }
        result.profile = std::move(profile);
    } catch (const std::exception& exception) {
        AddDiagnostic(result, source, {}, exception.what());
    }
    return result;
}

BuildProfileParseResult LoadBuildProfile(const std::filesystem::path& path) {
    const auto text = ReadText(path, kMaximumBuildProfileBytes);
    if (!text) {
        BuildProfileParseResult result;
        AddDiagnostic(result, path, {}, "failed to read profile");
        return result;
    }
    return ParseBuildProfile(*text, path);
}

std::string_view BuildProfileSchemaJson() noexcept {
    return generated::kBuildProfileSchema;
}

BuildProfileCatalogSnapshot BuildProfileCatalog::Scan(
    const std::filesystem::path& directory) const {
    return ScanLayered({{directory, "bundled", 0, false}});
}

BuildProfileCatalogSnapshot BuildProfileCatalog::ScanLayered(
    std::vector<BuildProfileCatalogLayer> layers) const {
    BuildProfileCatalogSnapshot snapshot;
    std::ranges::sort(layers, [](const auto& left, const auto& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        return left.channel < right.channel;
    });
    std::set<std::string, std::less<>> games;
    for (const auto& layer : layers) {
        std::error_code error;
        if (!std::filesystem::is_directory(layer.directory, error)) {
            if (!layer.optional) {
                snapshot.diagnostics.push_back({
                    layer.directory, {}, "profile directory is unavailable"});
            }
            continue;
        }
        std::vector<std::filesystem::path> files;
        for (std::filesystem::recursive_directory_iterator iterator(
                 layer.directory, std::filesystem::directory_options::skip_permission_denied,
                 error), end;
             iterator != end; iterator.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (iterator->is_regular_file(error) && iterator->path().extension() == L".json") {
                files.push_back(iterator->path());
            }
        }
        std::ranges::sort(files);
        for (const auto& file : files) {
            auto parsed = LoadBuildProfile(file);
            snapshot.diagnostics.insert(
                snapshot.diagnostics.end(),
                std::make_move_iterator(parsed.diagnostics.begin()),
                std::make_move_iterator(parsed.diagnostics.end()));
            if (!parsed.profile) continue;
            parsed.profile->source_channel = layer.channel;
            parsed.profile->source_priority = layer.priority;
            if (!games.insert(parsed.profile->game).second) {
                snapshot.diagnostics.push_back({
                    file, "/game", "profile shadowed by a higher-priority source"});
                continue;
            }
            snapshot.profiles.push_back(std::move(*parsed.profile));
        }
    }
    return snapshot;
}

SymbolCache::SymbolCache(std::filesystem::path file)
    : file_(std::filesystem::absolute(std::move(file))) {}

std::optional<SymbolCacheRecord> SymbolCache::Load() const {
    const auto text = ReadText(file_, kMaximumBuildProfileBytes);
    if (!text) return std::nullopt;
    try {
        const Json document = Json::parse(*text);
        if (document.value("schemaVersion", 0U) != 1 ||
            !document.contains("symbols") || !document.at("symbols").is_object()) {
            return std::nullopt;
        }
        SymbolCacheRecord record;
        for (auto iterator = document.at("symbols").begin();
             iterator != document.at("symbols").end(); ++iterator) {
            if (!iterator.value().is_object() ||
                !iterator.value().contains("module") ||
                !iterator.value().contains("rva")) return std::nullopt;
            CachedSymbol symbol;
            symbol.module = WideUtf8(iterator.value().at("module").get<std::string>());
            symbol.rva = iterator.value().at("rva").get<std::uint64_t>();
            record.symbols.emplace(iterator.key(), std::move(symbol));
        }
        return record;
    } catch (...) {
        return std::nullopt;
    }
}

bool SymbolCache::Store(
    const SymbolCacheRecord& record,
    std::string* error) const {
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };
    const auto& target = file_;
    std::error_code filesystem_error;
    if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path(), filesystem_error);
    }
    if (filesystem_error) return fail(filesystem_error.message());
    const auto temporary = target.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return fail("failed to create temporary cache");
        output << CacheJson(record);
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, filesystem_error);
            return fail("failed to write temporary cache");
        }
    }
    if (MoveFileExW(
            temporary.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD code = GetLastError();
        std::filesystem::remove(temporary, filesystem_error);
        return fail("failed to atomically replace cache: " + std::to_string(code));
    }
    return true;
}

}  // namespace anomaly
