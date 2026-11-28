#include "dungeon.hpp"

#include "i18n.hpp"

#include <cstdio>
#include <cstring>

namespace daily {
namespace {

// 与 CloneEnter 端点的 schema 哈希必须逐字节一致，否则宿主按 SCHEMA_MISMATCH 拒绝。
constexpr std::uint32_t kEndpointMajor = 1;
constexpr std::uint32_t kEndpointMinor = 1;

constexpr AnomalyIpcSchemaHashV1 kRequestSchemaHash{{
    0x29, 0xef, 0xde, 0xaa, 0x2f, 0xb9, 0xb8, 0x46, 0x24, 0xf6, 0x80, 0xab, 0xff, 0x2f, 0x22, 0xeb,
    0x2a, 0x5a, 0xf0, 0xe3, 0xb4, 0xca, 0xdc, 0x0f, 0x45, 0x64, 0xce, 0x64, 0x6f, 0x29, 0x15, 0x6a}};
constexpr AnomalyIpcSchemaHashV1 kResponseSchemaHash{{
    0xff, 0x53, 0xa4, 0x53, 0x56, 0xd8, 0xd6, 0x3a, 0x9c, 0x5b, 0xb5, 0x0c, 0x65, 0xb8, 0xe5, 0x0f,
    0x78, 0x9f, 0x9e, 0x12, 0x71, 0x3f, 0xb4, 0xa8, 0x27, 0xb2, 0x46, 0xc6, 0x43, 0xef, 0x83, 0x14}};

AnomalyIpcEndpointSelectorV1 Selector(const char* endpoint) {
    AnomalyIpcEndpointSelectorV1 selector{};
    selector.struct_size = sizeof(selector);
    selector.endpoint_id = Str(endpoint, std::strlen(endpoint));
    selector.major_version = kEndpointMajor;
    selector.minimum_minor_version = kEndpointMinor;
    selector.request_schema = kRequestSchemaHash;
    selector.response_schema = kResponseSchemaHash;
    return selector;
}

void ANOMALY_CALL OnDungeonResponse(void* user, AnomalyGenerationHandleV1 /*pending*/,
                                    const AnomalyStatusV1 status,
                                    const AnomalyByteSpanV1 response) {
    static_cast<void>(user);
    State& state = S();
    std::lock_guard<std::mutex> guard(state.ipc_mutex);
    state.ipc_response.assign(
        response.data != nullptr ? reinterpret_cast<const char*>(response.data) : "", response.size);
    while (!state.ipc_response.empty() && state.ipc_response.back() == '\0') {
        state.ipc_response.pop_back();
    }
    state.ipc_status_code = status.code;
    state.ipc_has_response = true;
}

bool Send(const char* payload, const std::size_t length, const bool log_failure = true) {
    State& state = S();
    if (state.ipc == nullptr || state.ipc->invoke_async == nullptr) return false;
    {
        std::lock_guard<std::mutex> guard(state.ipc_mutex);
        if (state.ipc_in_flight) return false;
        state.ipc_has_response = false;
        state.ipc_response.clear();
        state.ipc_in_flight = true;
    }
    const AnomalyIpcEndpointSelectorV1 selector = Selector(kDungeonEndpointId);
    AnomalyGenerationHandleV1 call{};
    const AnomalyStatusV1 status = state.ipc->invoke_async(
        state.ipc->user, &selector,
        AnomalyByteSpanV1{reinterpret_cast<const std::uint8_t*>(payload), length},
        &OnDungeonResponse,
        nullptr, &call);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        std::lock_guard<std::mutex> guard(state.ipc_mutex);
        state.ipc_in_flight = false;
        state.ipc_status_code = status.code;
        if (log_failure) {
            WinLogf("ipc invoke_async failed: code=%d", static_cast<int>(status.code));
        }
        return false;
    }
    state.dungeon_call = call;
    return true;
}

bool ReadField(const AnomalyJsonServiceV1* json, AnomalyGenerationHandleV1 root, const char* key,
               AnomalyGenerationHandleV1& child) {
    child = {};
    return json->object_find(json->user, root, Str(key, std::strlen(key)), &child).code ==
           ANOMALY_STATUS_V1_OK;
}

}  // namespace

bool DungeonIpcAvailable() {
    const State& state = S();
    return state.ipc != nullptr && state.ipc->invoke_async != nullptr;
}

DungeonProbeResult DungeonProbe() {
    State& state = S();
    if (state.ipc == nullptr || state.ipc->invoke_async == nullptr) {
        return DungeonProbeResult::Missing;
    }
    bool in_flight = false;
    {
        std::lock_guard<std::mutex> guard(state.ipc_mutex);
        in_flight = state.ipc_in_flight;
    }
    if (in_flight) {
        // 上一次探测还在飞：读到响应就说明端点在线（响应内容对探测没有意义，直接丢掉）。
        DungeonStatus status{};
        return DungeonTakeStatus(status) ? DungeonProbeResult::Ready
                                         : DungeonProbeResult::Pending;
    }
    constexpr const char* kStatusPayload = R"({"op":"status"})";
    return Send(kStatusPayload, std::strlen(kStatusPayload), false)
               ? DungeonProbeResult::Pending
               : DungeonProbeResult::Missing;
}

bool DungeonStart(const Settings& settings) {
    char payload[256]{};
    const int length = std::snprintf(
        payload, sizeof(payload),
        // 平A模式固定开启（用户要求默认且不可关），所以不再读设置、也不暴露开关。
        R"({"op":"start","display":%u,"sub":%u,"level":%u,"runs":%u,"enterWait":%u,"exitWait":%u,"melee":true})",
        settings.dungeon.index, settings.dungeon.sub_index, settings.dungeon.level_index,
        settings.dungeon.runs, settings.dungeon.enter_wait, settings.dungeon.exit_wait);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(payload)) return false;
    return Send(payload, static_cast<std::size_t>(length));
}

bool DungeonStop() {
    constexpr const char* kPayload = R"({"op":"stop"})";
    return Send(kPayload, std::strlen(kPayload));
}

bool DungeonStatusRequest() {
    constexpr const char* kPayload = R"({"op":"status"})";
    return Send(kPayload, std::strlen(kPayload));
}

void DungeonCancelPending() {
    State& state = S();
    if (state.ipc != nullptr && state.ipc->cancel != nullptr && state.dungeon_call.id != 0) {
        static_cast<void>(state.ipc->cancel(state.ipc->user, state.dungeon_call));
    }
    state.dungeon_call = {};
    std::lock_guard<std::mutex> guard(state.ipc_mutex);
    state.ipc_in_flight = false;
}

bool DungeonTakeStatus(DungeonStatus& out) {
    State& state = S();
    std::string document;
    int code = 0;
    {
        std::lock_guard<std::mutex> guard(state.ipc_mutex);
        if (!state.ipc_has_response) return false;
        document.swap(state.ipc_response);
        code = state.ipc_status_code;
        state.ipc_has_response = false;
        state.ipc_in_flight = false;
    }
    state.dungeon_call = {};
    if (code != ANOMALY_STATUS_V1_OK || document.empty()) {
        WinLogf("dungeon ipc response rejected: code=%d", code);
        return false;
    }
    const AnomalyJsonServiceV1* json = state.json;
    if (json == nullptr || json->parse == nullptr) return false;
    AnomalyGenerationHandleV1 root{};
    if (json->parse(json->user, Str(document.data(), document.size()), &root).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    DungeonStatus status{};
    AnomalyGenerationHandleV1 child{};
    if (ReadField(json, root, "ok", child)) {
        int32_t value = 0;
        if (json->boolean_value(json->user, child, &value).code == ANOMALY_STATUS_V1_OK) {
            status.ok = value != 0;
        }
        static_cast<void>(json->release(json->user, child));
    }
    if (ReadField(json, root, "active", child)) {
        int32_t value = 0;
        if (json->boolean_value(json->user, child, &value).code == ANOMALY_STATUS_V1_OK) {
            status.active = value != 0;
        }
        static_cast<void>(json->release(json->user, child));
    }
    const struct {
        const char* key;
        int* target;
    } number_fields[] = {{"phase", &status.phase}, {"result", &status.result}};
    for (const auto& field : number_fields) {
        if (!ReadField(json, root, field.key, child)) continue;
        double value = 0.0;
        if (json->number_value(json->user, child, &value).code == ANOMALY_STATUS_V1_OK) {
            *field.target = static_cast<int>(value + 0.5);
        }
        static_cast<void>(json->release(json->user, child));
    }
    if (ReadField(json, root, "runsDone", child)) {
        double value = 0.0;
        if (json->number_value(json->user, child, &value).code == ANOMALY_STATUS_V1_OK) {
            status.runs_done = static_cast<unsigned>(value + 0.5);
        }
        static_cast<void>(json->release(json->user, child));
    }
    if (ReadField(json, root, "runsTotal", child)) {
        double value = 0.0;
        if (json->number_value(json->user, child, &value).code == ANOMALY_STATUS_V1_OK) {
            status.runs_total = static_cast<unsigned>(value + 0.5);
        }
        static_cast<void>(json->release(json->user, child));
    }
    if (ReadField(json, root, "reason", child)) {
        char buffer[96]{};
        std::size_t size = sizeof(buffer) - 1;
        if (json->string_value(json->user, child, buffer, &size).code == ANOMALY_STATUS_V1_OK) {
            status.reason.assign(buffer, std::min<std::size_t>(size, sizeof(buffer) - 1));
        }
        static_cast<void>(json->release(json->user, child));
    }
    static_cast<void>(json->release(json->user, root));
    out = status;
    return true;
}

}  // namespace daily
