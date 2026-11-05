#pragma once
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
        if (!Read(object+accessory::profile::native_layout.ObjectClass, cls)) return false;
        for (int i = 0; cls && i < 64; ++i) {
            if (NameAt(cls+accessory::profile::native_layout.ObjectName) == type) return true;
            uintptr_t parent{};
            if (!Read(cls+accessory::profile::native_layout.Super, parent) || parent == cls) break;
            cls = parent;
        }
        return false;
    }
    bool Function(uintptr_t fn, std::string_view name, uint8_t count, uint16_t size) const {
        uint8_t actual_count{}; uint16_t actual_size{};
        return IsA(fn, "Function") && NameAt(fn+accessory::profile::native_layout.ObjectName) == name &&
            Read(fn+accessory::profile::native_layout.FunctionCount, actual_count) && actual_count == count &&
            Read(fn+accessory::profile::native_layout.FunctionSize, actual_size) && actual_size == size;
    }
    bool Property(uintptr_t cls, std::string_view name, int32_t offset, int32_t size) const {
        for (int depth = 0; cls && depth < 32; ++depth) {
            uintptr_t field{};
            if (!Read(cls+accessory::profile::native_layout.PropertyLink, field)) return false;
            for (int i = 0; field && i < 2048; ++i) {
                if (NameAt(field+accessory::profile::native_layout.FieldName) == name) {
                    int32_t actual_offset{}, actual_size{};
                    return Read(field+accessory::profile::native_layout.PropertyOffset, actual_offset) && actual_offset == offset &&
                        Read(field+accessory::profile::native_layout.PropertySize, actual_size) && actual_size == size;
                }
                uintptr_t next{};
                if (!Read(field+accessory::profile::native_layout.PropertyNext, next) || next == field) break;
                field = next;
            }
            uintptr_t parent{};
            if (!Read(cls+accessory::profile::native_layout.Super, parent) || parent == cls) break;
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
    if (!r.Read(registry+accessory::profile::native_layout.RegistryCount, count) || count > 16*1024*1024 || index >= count ||
        !r.Read(registry+accessory::profile::native_layout.RegistryItems, chunks) || !r.Read(chunks + sizeof(uintptr_t)*(index/accessory::profile::native_layout.ChunkSize), chunk) ||
        !r.Read(chunk + accessory::profile::native_layout.ItemStride*(index%accessory::profile::native_layout.ChunkSize), object) ||
        !r.Read(chunk + accessory::profile::native_layout.ItemStride*(index%accessory::profile::native_layout.ChunkSize)+accessory::profile::native_layout.Serial, serial) ||
        serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(h) ||
        !r.Read(object+accessory::profile::native_layout.ObjectIndex, actual) || actual != static_cast<int32_t>(index)) return 0;
    return object;
}
bool Invoke(const Reader& r, uintptr_t object, uintptr_t fn, void* parameters) {
    uintptr_t vtable{}, entry{};
    if (!r.Read(object, vtable) || !r.Read(vtable + accessory::profile::native_layout.ProcessEventSlot, entry)) return false;
    if (!accessory::profile::compatibility.MatchesEntry(core,"ProcessEvent.entry",entry,true)) return false;
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
    const auto instruction=accessory::profile::compatibility.Address("Objects");
    int32_t displacement{};
    if (!objects || !objects->find_exact || !instruction || !r.Read(instruction+accessory::profile::native_layout.RipDisplacement,displacement)) return false;
    registry=static_cast<uintptr_t>(static_cast<intptr_t>(instruction)+accessory::profile::native_layout.RipInstructionSize+displacement-accessory::profile::native_layout.RegistrySubtract);
    return true;
}

}
