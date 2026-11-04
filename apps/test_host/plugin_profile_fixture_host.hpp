#pragma once

// Test transport for the public Profile/JSON ABI. Documents still pass through
// the production package reader and schema validator, then the SDK parser.
#include "anomaly/plugin_profile.hpp"
#include "anomaly/sdk/profile.hpp"
#include "pattern.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <map>

namespace anomaly::testing {
class ProfileHost final {
public:
    AnomalyHostApiV1 api{};
    AnomalyCoreServiceV1 core{};
    AnomalySignatureServiceV1 signatures{};
    AnomalyProfileServiceV1 profiles{};
    AnomalyJsonServiceV1 json{};
    std::filesystem::path package;
    std::string error;
    // Independent bytes observed at GetAppearance in PID 44236. Tests may
    // replace the displacement with the independently observed 1.5 value.
    std::vector<std::uint8_t> image{
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
        0x48,0x89,0x7C,0x24,0x20,0x41,0x56,0x48,0x83,0xEC,0x20,0x80,0xB9,0x50,0x2A,
        0,0,0,0x48,0x8B,0xF2,0x48,0x8B,0xF9,0x0F,0x84,0xA4,0,0,0};

    explicit ProfileHost(std::string_view plugin)
        : package(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "plugins" / plugin) {
        api.struct_size = sizeof(api); api.api_major = ANOMALY_PLUGIN_API_V1_MAJOR;
        api.host_context = this; api.query_service = Query;
        core.struct_size = sizeof(core); core.service_version = 1; core.user = this;
        core.read_memory = [](void* u, std::uintptr_t address, AnomalyMutableByteSpanV1 out) {
            auto& self = *static_cast<ProfileHost*>(u);
            if (address < 0x10000 || address - 0x10000 > self.image.size() ||
                out.size > self.image.size() - (address - 0x10000)) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
            std::memcpy(out.data, self.image.data() + address - 0x10000, out.size); return sdk::Ok();
        };
        signatures.struct_size = sizeof(signatures); signatures.service_version = 1; signatures.user = this;
        signatures.resolve = [](void* u, AnomalyStringViewV1, AnomalyStringViewV1,
                                AnomalyStringViewV1 pattern, std::uintptr_t* address) {
            *address = 0;
            try {
                auto& self = *static_cast<ProfileHost*>(u);
                const auto hits = ue5mem::Pattern::Parse({pattern.data, pattern.size}).FindAll(self.image, 2);
                if (hits.size() > 1) return Status(ANOMALY_STATUS_V1_CONFLICT);
                if (hits.empty()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
                *address = 0x10000 + hits[0]; return sdk::Ok();
            } catch (...) { return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT); }
        };
        profiles.struct_size = sizeof(profiles); profiles.service_version = 1; profiles.user = this;
        profiles.load = [](void* u, AnomalyStringViewV1 filename, AnomalyGenerationHandleV1* out) {
            auto& self = *static_cast<ProfileHost*>(u);
            auto loaded = LoadPluginProfile(self.package, {filename.data, filename.size});
            self.error = loaded.error;
            if (!loaded) return Status(loaded.code);
            *out = self.Add(*loaded.document); return sdk::Ok();
        };
        json.struct_size = sizeof(json); json.service_version = 1; json.user = this;
        json.release = [](void* u, AnomalyGenerationHandleV1 h) {
            static_cast<ProfileHost*>(u)->nodes.erase(h.id); return sdk::Ok();
        };
        json.object_find = [](void* u, AnomalyGenerationHandleV1 h, AnomalyStringViewV1 key, AnomalyGenerationHandleV1* out) {
            auto& self = *static_cast<ProfileHost*>(u);
            const auto& node = self.nodes.at(h.id);
            const auto found = node.find(std::string(key.data, key.size));
            if (found == node.end()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
            *out = self.Add(*found); return sdk::Ok();
        };
        json.array_item = [](void* u, AnomalyGenerationHandleV1 h, std::size_t i, AnomalyGenerationHandleV1* out) {
            auto& self = *static_cast<ProfileHost*>(u); *out = self.Add(self.nodes.at(h.id).at(i)); return sdk::Ok();
        };
        json.array_size = json.object_size = [](void* u, AnomalyGenerationHandleV1 h, std::size_t* size) {
            *size = static_cast<ProfileHost*>(u)->nodes.at(h.id).size(); return sdk::Ok();
        };
        json.number_value = [](void* u, AnomalyGenerationHandleV1 h, double* value) {
            *value = static_cast<ProfileHost*>(u)->nodes.at(h.id).get<double>(); return sdk::Ok();
        };
        json.string_value = [](void* u, AnomalyGenerationHandleV1 h, char* out, std::size_t* size) {
            return Copy(static_cast<ProfileHost*>(u)->nodes.at(h.id).get_ref<const std::string&>(), out, size);
        };
        json.object_key_at = [](void* u, AnomalyGenerationHandleV1 h, std::size_t i, char* out, std::size_t* size) {
            const auto& node = static_cast<ProfileHost*>(u)->nodes.at(h.id);
            auto it = node.begin(); std::advance(it, i); return Copy(it.key(), out, size);
        };
    }
    static AnomalyStatusV1 ANOMALY_CALL Query(void* u, AnomalyStringViewV1 id, std::uint32_t, const void** out) {
        auto& self = *static_cast<ProfileHost*>(u); const std::string_view name(id.data, id.size);
        if (name == ANOMALY_CORE_SERVICE_V1_ID) *out = &self.core;
        else if (name == ANOMALY_SIGNATURE_SERVICE_V1_ID) *out = &self.signatures;
        else if (name == ANOMALY_PROFILE_SERVICE_V1_ID) *out = &self.profiles;
        else if (name == ANOMALY_JSON_SERVICE_V1_ID) *out = &self.json;
        else { *out = nullptr; return Status(ANOMALY_STATUS_V1_NOT_FOUND); }
        return sdk::Ok();
    }
    [[nodiscard]] std::size_t OutstandingHandles() const { return nodes.size(); }
private:
    std::map<std::uint64_t, nlohmann::json> nodes;
    std::uint64_t next{};
    AnomalyGenerationHandleV1 Add(const nlohmann::json& value) {
        nodes.emplace(++next, value); return {next, 1};
    }
    static AnomalyStatusV1 Status(std::uint32_t code) { return {code, 0, {}}; }
    static AnomalyStatusV1 Copy(std::string_view text, char* out, std::size_t* size) {
        const auto capacity = *size; *size = text.size() + 1;
        if (!out) return sdk::Ok();
        if (capacity < *size) return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
        std::memcpy(out, text.data(), text.size()); out[text.size()] = '\0'; return sdk::Ok();
    }
};
} // namespace anomaly::testing

namespace anomaly::testing {

inline AnomalyStatusV1 RecordedProcessEventEntry(AnomalyMutableByteSpanV1 bytes) {
    constexpr std::uint8_t entry[]{0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0x00,0x01,0x00,0x00,0x48,0x8D,0x6C,0x24,0x30,0x48,0x89,0x9D,0x28,0x01,0x00,0x00,0x48,0x8B,0x05};
    if (bytes.size > sizeof(entry)) return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND};
    std::memcpy(bytes.data,entry,bytes.size);
    return anomaly::sdk::Ok();
}
}
