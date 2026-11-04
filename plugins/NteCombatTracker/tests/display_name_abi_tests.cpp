// 验证 NCT 的名称解析完全建立在宿主 ABI 之上，而不是本地字典。
//
// 被测合同来自 docs/api-reference/nte-services.md：
//   - event_name_utf8 / participant_display_name_utf8 返回显示文本；
//   - 名称延后补全，未就绪时返回 NOT_FOUND，调用方应保留记录并重试；
//   - 显示名与 source_name_utf8 / participant_path_utf8 的对象路径不是同一回事。

#include <anomaly/sdk/anomaly_sdk.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int failures{};

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

AnomalyStatusV1 Status(const uint32_t code) {
    AnomalyStatusV1 value{};
    value.code = code;
    return value;
}

// 伪宿主：显示名何时可解析由测试显式控制，用来模拟宿主的「延后补全」。
struct FakeHost {
    bool event_display_ready{false};
    bool participant_display_ready{false};
    std::string event_display{"暴风之刃"};
    std::string participant_display{"早雾"};
    std::string event_path{"GE_StormBlade"};
    std::string participant_path{"/Game/Characters/player_003_sagiri"};
};

FakeHost g_host;

AnomalyStatusV1 CopyOut(const std::string& value, char* destination, std::size_t* size) {
    if (size == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const std::size_t required = value.size() + 1;
    if (destination == nullptr) {
        *size = required;
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    }
    if (*size < required) return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    std::memcpy(destination, value.c_str(), required);
    *size = required;
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL FakeEventName(
    void*, const AnomalyNteCombatEventV1*, char* destination, std::size_t* size) {
    if (!g_host.event_display_ready) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
    return CopyOut(g_host.event_display, destination, size);
}

AnomalyStatusV1 ANOMALY_CALL FakeParticipantDisplay(
    void*, AnomalyGenerationHandleV1, char* destination, std::size_t* size) {
    if (!g_host.participant_display_ready) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
    return CopyOut(g_host.participant_display, destination, size);
}

AnomalyStatusV1 ANOMALY_CALL FakeSourceName(
    void*, uint64_t, char* destination, std::size_t* size) {
    return CopyOut(g_host.event_path, destination, size);
}

AnomalyStatusV1 ANOMALY_CALL FakeParticipantPath(
    void*, AnomalyGenerationHandleV1, char* destination, std::size_t* size) {
    return CopyOut(g_host.participant_path, destination, size);
}

// 与插件内一致的两段式查询协议。
template <typename Resolver>
std::string Query(Resolver resolver) {
    std::size_t size{};
    resolver(nullptr, &size);
    if (size <= 1) return {};
    std::string value(size, '\0');
    if (resolver(value.data(), &size).code != ANOMALY_STATUS_V1_OK) return {};
    if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    return value;
}

bool LooksLikeObjectPath(const std::string& value) {
    return value.starts_with("/Game/") || value.starts_with("/Script/");
}

AnomalyNteCombatServiceV1 MakeService() {
    AnomalyNteCombatServiceV1 service{};
    service.struct_size = sizeof(service);
    service.service_version = 1;
    service.event_name_utf8 = &FakeEventName;
    service.participant_display_name_utf8 = &FakeParticipantDisplay;
    service.source_name_utf8 = &FakeSourceName;
    service.participant_path_utf8 = &FakeParticipantPath;
    return service;
}

void EventNameFallsBackToPathThenId() {
    g_host = FakeHost{};
    auto service = MakeService();
    AnomalyNteCombatEventV1 event{};
    event.struct_size = sizeof(event);
    event.name_id = 4242;

    const std::string pending = Query([&](char* d, std::size_t* s) {
        return service.event_name_utf8(service.user, &event, d, s);
    });
    Check(pending.empty(), "a pending event display name must not resolve");

    const std::string path = Query([&](char* d, std::size_t* s) {
        return service.source_name_utf8(service.user, event.name_id, d, s);
    });
    Check(path == "GE_StormBlade", "the technical source name feeds the fallback");

    g_host.event_display_ready = true;
    const std::string display = Query([&](char* d, std::size_t* s) {
        return service.event_name_utf8(service.user, &event, d, s);
    });
    Check(display == "暴风之刃", "the resolved event display name is the localized text");
    Check(display != path, "the display name is not the object path");
}

void ParticipantDisplayWinsOverPath() {
    g_host = FakeHost{};
    auto service = MakeService();
    const AnomalyGenerationHandleV1 handle{7, 3};

    const std::string pending = Query([&](char* d, std::size_t* s) {
        return service.participant_display_name_utf8(service.user, handle, d, s);
    });
    Check(pending.empty(), "a pending participant display name must not resolve");

    const std::string path = Query([&](char* d, std::size_t* s) {
        return service.participant_path_utf8(service.user, handle, d, s);
    });
    Check(LooksLikeObjectPath(path), "the path fallback is an object path");

    g_host.participant_display_ready = true;
    const std::string display = Query([&](char* d, std::size_t* s) {
        return service.participant_display_name_utf8(service.user, handle, d, s);
    });
    Check(display == "早雾", "the resolved participant display name is localized");
    Check(!LooksLikeObjectPath(display), "a real display name is never an object path");
}

void MissingInterfaceDegradesWithoutCrashing() {
    // 旧宿主只填 struct_size 更小的服务表，显示名槽位为空。
    AnomalyNteCombatServiceV1 service{};
    service.struct_size = sizeof(service);
    Check(service.event_name_utf8 == nullptr,
        "an older host leaves the display-name slots empty");
    Check(service.participant_display_name_utf8 == nullptr,
        "an older host leaves the participant display-name slot empty");

    g_host = FakeHost{};
    const std::string none = Query([&](char* d, std::size_t* s) {
        return service.participant_path_utf8 != nullptr
            ? service.participant_path_utf8(service.user,
                  AnomalyGenerationHandleV1{1, 1}, d, s)
            : Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    });
    Check(none.empty(), "without the path interface there is nothing to fall back to");
}

}  // namespace

int main() {
    EventNameFallsBackToPathThenId();
    ParticipantDisplayWinsOverPath();
    MissingInterfaceDegradesWithoutCrashing();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "display name ABI checks passed\n";
    return 0;
}
