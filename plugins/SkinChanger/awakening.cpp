#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <anomaly/sdk/cpp.hpp>
#include "awakening.hpp"
#include "awakening_data.hpp"
#include "awakening_profile.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace anomaly::sdk;
using Clock = std::chrono::steady_clock;
struct FName { uint32_t id{}, number{}; };
struct RowArray { uintptr_t data{}; int32_t count{}, capacity{}; };
static_assert(sizeof(FName) == 8 && sizeof(RowArray) == 16);
const AnomalyHostApiV1* api{};
const AnomalyCoreServiceV1* core{};
uintptr_t registry{};
Clock::time_point retry_init{};
void Log(std::string value) {
    if (core && core->log) core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(value));
}
struct Reader {
    const AnomalyUe5NamesServiceV1* names{};
    template<class T> bool Read(uintptr_t address, T& value) const {
        return address > 0x10000 && core && core->read_memory &&
            core->read_memory(core->user, address,
                {reinterpret_cast<uint8_t*>(&value), sizeof(value)}).code == ANOMALY_STATUS_V1_OK;
    }
    std::string Name(FName value) const {
        if (!value.id || !names || !names->resolve_utf8) return {};
        size_t size{};
        if (names->resolve_utf8(names->user, value.id, nullptr, &size).code != 0 || size < 2 || size > 512) return {};
        std::string text(size, '\0');
        if (names->resolve_utf8(names->user, value.id, text.data(), &size).code != 0 || size > text.size()) return {};
        text.resize(size);
        if (const auto nul = text.find('\0'); nul != std::string::npos) text.resize(nul);
        if (value.number) text += "_" + std::to_string(value.number - 1);
        return text;
    }
    std::string NameAt(uintptr_t address) const { FName n{}; return Read(address, n) ? Name(n) : std::string{}; }
    bool IsA(uintptr_t object, std::string_view type) const {
        uintptr_t cls{};
        if (!Read(object + 16, cls)) return false;
        for (int i = 0; cls && i < 64; ++i) {
            if (NameAt(cls + 24) == type) return true;
            uintptr_t parent{};
            if (!Read(cls + 64, parent) || parent == cls) break;
            cls = parent;
        }
        return false;
    }
    bool Function(uintptr_t fn, std::string_view name, uint8_t count, uint16_t size) const {
        uint8_t actual_count{}; uint16_t actual_size{};
        return IsA(fn, "Function") && NameAt(fn + 24) == name &&
            Read(fn + 180, actual_count) && actual_count == count &&
            Read(fn + 182, actual_size) && actual_size == size;
    }
    bool Property(uintptr_t cls, std::string_view name, int32_t offset, int32_t size) const {
        for (int depth = 0; cls && depth < 32; ++depth) {
            uintptr_t field{};
            if (!Read(cls + 112, field)) return false;
            for (int i = 0; field && i < 2048; ++i) {
                if (NameAt(field + 32) == name) {
                    int32_t actual_offset{}, actual_size{};
                    return Read(field + 68, actual_offset) && actual_offset == offset &&
                        Read(field + 52, actual_size) && actual_size == size;
                }
                uintptr_t next{};
                if (!Read(field + 72, next) || next == field) break;
                field = next;
            }
            uintptr_t parent{};
            if (!Read(cls + 64, parent) || parent == cls) break;
            cls = parent;
        }
        return false;
    }
};
uintptr_t Resolve(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, AnomalyGenerationHandleV1 h) {
    if (!registry || !h.id || !objects || !objects->snapshot_by_handle) return 0;
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (objects->snapshot_by_handle(objects->user, h, &snapshot).code != 0) return 0;
    const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(h);
    uintptr_t chunks{}, chunk{}, object{}; uint32_t count{}, serial{}; int32_t actual{};
    if (!r.Read(registry + 36, count) || count > 16*1024*1024 || index >= count ||
        !r.Read(registry + 16, chunks) || !r.Read(chunks + 8*(index/65536), chunk) ||
        !r.Read(chunk + 24*(index%65536), object) ||
        !r.Read(chunk + 24*(index%65536) + 16, serial) ||
        serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(h) ||
        !r.Read(object + 12, actual) || actual != static_cast<int32_t>(index)) return 0;
    return object;
}
bool Invoke(const Reader& r, uintptr_t object, uintptr_t fn, void* parameters) {
    uintptr_t vtable{}, entry{};
    if (!r.Read(object, vtable) || !r.Read(vtable + 0x4c*8, entry)) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(entry), &region, sizeof(region)) ||
        region.State != MEM_COMMIT ||
        !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(entry)(
            reinterpret_cast<void*>(object), reinterpret_cast<void*>(fn), parameters);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Initialize(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects) {
    if (registry) return true;
    if (Clock::now() < retry_init) return false;
    retry_init = Clock::now() + std::chrono::seconds(5);
    Host host(api);
    const auto sig = host.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
    uintptr_t instruction{}; int32_t displacement{};
    if (!sig || !sig->resolve || !objects || !objects->find_exact ||
        sig->resolve(sig->user, StringView("HTGame.exe"), StringView(".text"),
            StringView(accessory::profile::Objects),
            &instruction).code != 0 || !r.Read(instruction + 3, displacement)) return false;
    registry = static_cast<uintptr_t>(static_cast<intptr_t>(instruction) + 7 + displacement - 16);
    return true;
}

uint64_t Pack(FName name) { return (uint64_t{name.number} << 32) | name.id; }
#include "awakening_runtime.hpp"
}

namespace lite_awakening {
void Load(const AnomalyHostApiV1* host) noexcept {
    api = host;
    core = anomaly::sdk::Host(host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
}
void Start() noexcept {
    registry = 0; retry_init = {};
    try {
        const Reader r{Host(api).Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get()};
        const auto objects = Host(api).Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        Initialize(r,objects);
        awakening::Reset();
    } catch (...) { Log("Awakening: initialization failed"); }
}
AnomalyStatusV1 Stop() noexcept {
    try {
        const Reader r{Host(api).Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get()};
        const auto objects = Host(api).Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        if (!awakening::Restore(r,objects,false)) return {ANOMALY_STATUS_V1_FAILED,0,{}};
        awakening::requested = 0;
        return anomaly::sdk::Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED,0,{}}; }
}
void Update() noexcept {
    try {
        if (!awakening::interested && !awakening::Active() && !awakening::requested.load()) return;
        Host host(api);
        const auto framework = host.Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
        const auto objects = host.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        const Reader r{host.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get()};
        if (objects && framework && framework->is_game_thread && framework->is_game_thread(framework->user) &&
            Initialize(r,objects)) awakening::Tick(r,objects);
    } catch (...) { Log("Awakening: update failed"); }
}
void SetVisible(bool visible) noexcept { awakening::interested = visible; }
void Draw(const AnomalyUiServiceV1* ui) noexcept {
    try { awakening::Draw(ui); } catch (...) {}
}
}
