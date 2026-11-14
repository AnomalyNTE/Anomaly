#pragma once
// Reads the objects the game keeps its names in. Every offset comes from the plugin profile and is
// validated once in Load, so nothing here assumes a build: a mismatched profile leaves Ready() false
// and the caller exports nothing rather than reading the wrong bytes.
#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/profile.hpp>
#include <anomaly/sdk/services/core.h>
#include <anomaly/sdk/services/interop.h>
#include <anomaly/sdk/services/ue5.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mod_name_export {
using Handle = AnomalyGenerationHandleV1;

class Access final {
public:
    bool Load(const AnomalyHostApiV1* api) {
        const anomaly::sdk::Host host(api);
        core_ = host.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
        names_ = host.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get();
        objects_ = host.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        signatures_ = host.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
        if (!core_ || !core_->read_memory || !names_ || !names_->resolve_utf8 || !objects_ ||
            !objects_->snapshot_by_handle || !objects_->find_exact || !signatures_ ||
            !signatures_->resolve)
            return Fail("services");
        // The document is read here and its symbol is scanned by the host: the pattern stays in the
        // profile, and a pattern that no longer matches is a reported failure rather than a guess.
        const auto loaded = anomaly::sdk::LoadProfile(host, "compatibility.profile.json",
            {"nte", "HTGame.exe", ".text", "mod-name-export-v1"});
        if (!loaded) return Fail("profile");
        const auto& document = loaded.value();
        objectClass_ = document.Value("ObjectClass", 65536);
        objectName_ = document.Value("ObjectName", 65536);
        objectIndex_ = document.Value("ObjectIndex", 65536);
        objectOuter_ = document.Value("ObjectOuter", 65536);
        objectFlags_ = document.Value("ObjectFlags", 65536);
        structSuper_ = document.Value("StructSuper", 65536);
        structProperties_ = document.Value("StructProperties", 65536);
        propertyName_ = document.Value("PropertyName", 65536);
        propertyNext_ = document.Value("PropertyNext", 65536);
        propertyClass_ = document.Value("PropertyClass", 65536);
        fieldClassName_ = document.Value("FieldClassName", 65536);
        propertyOffset_ = document.Value("PropertyOffset", 65536);
        propertySize_ = document.Value("PropertySize", 65536);
        registryCount_ = document.Value("RegistryCount", 65536);
        registryItems_ = document.Value("RegistryItems", 65536);
        chunkSize_ = document.Value("ChunkSize", 1 << 20);
        itemStride_ = document.Value("ItemStride", 256);
        itemSerial_ = document.Value("ItemSerial", 128);
        registrySubtract_ = document.Value("RegistrySubtract", 256);
        rowStruct_ = document.Value("DataTableRowStruct", 256);
        rowMap_ = document.Value("DataTableRowMap", 256);
        mapEntryStride_ = document.Value("MapEntryStride", 256);
        mapValue_ = document.Value("MapValueOffset", 128);
        setDataCount_ = document.Value("SetDataCount", 256);
        setAllocationBits_ = document.Value("SetAllocationBits", 256);
        setIndirectBits_ = document.Value("SetIndirectBits", 256);
        setBitCount_ = document.Value("SetBitCount", 256);
        setFreeCount_ = document.Value("SetFreeCount", 256);
        if (!chunkSize_ || itemStride_ < sizeof(std::uintptr_t) ||
            itemSerial_ + sizeof(std::uint32_t) > itemStride_ || !rowStruct_ || !rowMap_ ||
            mapEntryStride_ < sizeof(std::uintptr_t) + mapValue_)
            return Fail("layout");
        const auto& candidates = document.symbol("Objects").candidates;
        if (candidates.empty()) return Fail("objects-symbol");
        std::uintptr_t instruction{};
        const auto scanned = signatures_->resolve(signatures_->user,
            anomaly::sdk::StringView("HTGame.exe"), anomaly::sdk::StringView(".text"),
            anomaly::sdk::StringView(candidates.front().pattern), &instruction);
        if (scanned.code != ANOMALY_STATUS_V1_OK || !instruction) return Fail("objects-scan");
        std::int32_t displacement{};
        // x64 MOV reg, [RIP+rel32] decoding; the instruction shape is checked by the profile.
        const auto raw = core_->read_memory(core_->user, instruction + 3,
            {reinterpret_cast<std::uint8_t*>(&displacement), sizeof(displacement)});
        if (raw.code != ANOMALY_STATUS_V1_OK)
            return Fail("objects-decode@" + Hex(instruction) + ":" + std::to_string(raw.code));
        registry_ = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(instruction) + 7 +
            displacement - registrySubtract_);
        ready_ = registry_ != 0;
        return ready_;
    }

    [[nodiscard]] bool Ready() const { return ready_; }
    [[nodiscard]] const std::string& Error() const { return error_; }

    [[nodiscard]] Handle Find(const std::string_view path) const {
        Handle handle{};
        if (!ready_ || objects_->find_exact(objects_->user, anomaly::sdk::StringView(path), &handle)
                .code != ANOMALY_STATUS_V1_OK)
            return {};
        return handle;
    }

    [[nodiscard]] std::uintptr_t Resolve(const Handle handle) const {
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (!ready_ || !handle.id ||
            objects_->snapshot_by_handle(objects_->user, handle, &snapshot).code !=
                ANOMALY_STATUS_V1_OK)
            return 0;
        const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
        std::uint32_t count{};
        std::uintptr_t chunks{};
        if (!Read(registry_ + registryCount_, count) || index >= count || !count ||
            !Read(registry_ + registryItems_, chunks))
            return 0;
        std::uintptr_t chunk{};
        if (!Read(chunks + sizeof(std::uintptr_t) * (index / chunkSize_), chunk) || !chunk) return 0;
        const auto item = chunk + static_cast<std::uint64_t>(itemStride_) * (index % chunkSize_);
        std::uintptr_t object{};
        std::uint32_t serial{};
        std::uint32_t actual{};
        std::uint32_t flags{};
        if (!Read(item, object) || !Read(item + itemSerial_, serial) ||
            serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) || !object ||
            !Read(object + objectIndex_, actual) || actual != index ||
            !Read(object + objectFlags_, flags) || (flags & 0x18000u))
            return 0;
        return object;
    }

    bool Read(const std::uintptr_t at, void* out, const std::size_t size) const {
        if (!core_ || at <= 65536 || !size) return false;
        return core_->read_memory(core_->user, at,
            {static_cast<std::uint8_t*>(out), size}).code == ANOMALY_STATUS_V1_OK;
    }
    template <class T> bool Read(const std::uintptr_t at, T& out) const {
        return Read(at, &out, sizeof(out));
    }

    [[nodiscard]] std::string NameAt(const std::uintptr_t at) const {
        struct Name { std::uint32_t id{}, number{}; } name{};
        if (!Read(at, name) || !name.id) return {};
        std::array<char, 512> text{};
        std::size_t size = text.size();
        if (names_->resolve_utf8(names_->user, name.id, text.data(), &size).code !=
            ANOMALY_STATUS_V1_OK || size == 0)
            return {};
        std::string value(text.data(), std::min<std::size_t>(size, text.size()));
        if (const auto nul = value.find('\0'); nul != std::string::npos) value.resize(nul);
        if (name.number) value += "_" + std::to_string(name.number - 1);
        return value;
    }

    // The host resolves FText only on its Game domain; anything else returns an empty string.
    [[nodiscard]] std::string TextAt(const std::uintptr_t at) const {
        if (!names_ || !names_->resolve_ftext_utf8) return {};
        std::array<char, 1024> text{};
        std::size_t size = text.size();
        if (names_->resolve_ftext_utf8(names_->user, at, text.data(), &size).code !=
            ANOMALY_STATUS_V1_OK || size == 0)
            return {};
        std::string value(text.data(), std::min<std::size_t>(size, text.size()));
        if (const auto nul = value.find('\0'); nul != std::string::npos) value.resize(nul);
        return value;
    }

    // An FString is a TArray<TCHAR>: data, count, capacity.
    [[nodiscard]] std::string StringAt(const std::uintptr_t at) const {
        struct Array { std::uintptr_t data{}; std::int32_t count{}, capacity{}; } array{};
        if (!Read(at, array) || !array.data || array.count <= 0 || array.count > 4096 ||
            array.capacity < array.count)
            return {};
        std::wstring wide(static_cast<std::size_t>(array.count), L'\0');
        if (!Read(array.data, wide.data(), wide.size() * sizeof(wchar_t))) return {};
        if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
        std::string out;
        for (const wchar_t unit : wide) {
            const auto code = static_cast<std::uint32_t>(unit);
            if (code < 0x80) out += static_cast<char>(code);
            else if (code < 0x800) {
                out += static_cast<char>(0xc0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3f));
            } else {
                out += static_cast<char>(0xe0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
                out += static_cast<char>(0x80 | (code & 0x3f));
            }
        }
        return out;
    }

    struct Field final {
        std::string name, type;
        std::uint32_t offset{}, size{};
    };

    [[nodiscard]] std::vector<Field> Fields(const std::uintptr_t table) const {
        std::vector<Field> fields;
        std::uintptr_t row_struct{};
        std::uintptr_t field{};
        if (!Read(table + rowStruct_, row_struct) || !row_struct ||
            !Read(row_struct + structProperties_, field))
            return fields;
        for (int depth = 0; field && depth < 512; ++depth) {
            Field entry;
            entry.name = NameAt(field + propertyName_);
            std::uintptr_t property_class{};
            if (Read(field + propertyClass_, property_class) && property_class)
                entry.type = NameAt(property_class + fieldClassName_);
            if (!Read(field + propertyOffset_, entry.offset) ||
                !Read(field + propertySize_, entry.size))
                break;
            fields.push_back(std::move(entry));
            std::uintptr_t next{};
            if (!Read(field + propertyNext_, next)) break;
            field = next;
        }
        return fields;
    }

    struct Row final {
        std::string name;
        std::uintptr_t data{};
    };

    [[nodiscard]] std::vector<Row> Rows(const std::uintptr_t table) const {
        std::vector<Row> rows;
        // Num is the sparse array's used index range, including free slots. Capacity may extend
        // beyond the allocation bitmap; only Num - NumFreeIndices rows are live.
        const auto map = table + rowMap_;
        std::uintptr_t data{}, extra_bits{};
        std::int32_t count{}, bit_count{}, free_count{};
        if (!Read(map, data) || !data || !Read(map + setDataCount_, count) ||
            !Read(map + setIndirectBits_, extra_bits) || !Read(map + setBitCount_, bit_count) ||
            !Read(map + setFreeCount_, free_count) || count <= 0 || count > 100000 ||
            bit_count < count || free_count < 0 || free_count > count)
            return rows;
        std::vector<std::uint32_t> bits(static_cast<std::size_t>((count + 31) / 32));
        const auto bits_address = extra_bits ? extra_bits : map + setAllocationBits_;
        if (!Read(bits_address, bits.data(), bits.size() * sizeof(std::uint32_t))) return rows;
        for (std::int32_t index = 0; index < count && rows.size() < static_cast<std::size_t>(count - free_count); ++index) {
            if ((bits[static_cast<std::size_t>(index) / 32] & (1u << (index % 32))) == 0) continue;
            const auto element = data + static_cast<std::uint64_t>(mapEntryStride_) *
                static_cast<std::size_t>(index);
            Row row;
            row.name = NameAt(element);
            if (!Read(element + mapValue_, row.data)) continue;
            rows.push_back(std::move(row));
        }
        return rows;
    }

private:
    const AnomalyCoreServiceV1* core_{};
    const AnomalyUe5NamesServiceV1* names_{};
    const AnomalyUe5ObjectsServiceV1* objects_{};
    const AnomalySignatureServiceV1* signatures_{};
    std::uintptr_t registry_{};
    std::uint32_t objectClass_{}, objectName_{}, objectIndex_{}, objectOuter_{}, objectFlags_{};
    std::uint32_t structSuper_{}, structProperties_{}, propertyName_{}, propertyNext_{},
        propertyClass_{}, fieldClassName_{}, propertyOffset_{}, propertySize_{};
    std::uint32_t registryCount_{}, registryItems_{}, chunkSize_{}, itemStride_{}, itemSerial_{},
        registrySubtract_{};
    std::uint32_t rowStruct_{}, rowMap_{}, mapEntryStride_{}, mapValue_{};
    std::uint32_t setDataCount_{}, setAllocationBits_{}, setIndirectBits_{}, setBitCount_{},
        setFreeCount_{};
    bool ready_{};
    std::string error_{"unset"};
    static std::string Hex(const std::uintptr_t value) {
        char text[32]{};
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
        return text;
    }
    bool Fail(std::string_view reason) {
        error_ = std::string(reason);
        return false;
    }
};

} // namespace mod_name_export
