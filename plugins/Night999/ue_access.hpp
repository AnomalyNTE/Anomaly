#pragma once
// UE 反射访问层：对象注册表定位、句柄→地址、按字段名查偏移、ProcessEvent 调用。
// 只在 Game 回调域使用。

namespace {

struct UeFName {
    std::uint32_t id{}, number{};
};
static_assert(sizeof(UeFName) == 8, "UE FName 必须是 8 字节");

// 对象注册表特征码：mov rax,[rip+disp] 之后按 index 查块。
constexpr std::string_view kUeObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";

// UObject 布局：vtable@0x00，ObjectFlags@0x08，InternalIndex@0x0C，
// ClassPrivate@0x10，NamePrivate@0x18，OuterPrivate@0x20。
constexpr std::uint32_t kUeObjectClassOffset = 0x10;
constexpr std::uint32_t kUeObjectNameOffset = 0x18;
constexpr std::uint32_t kUeObjectOuterOffset = 0x20;
// UStruct：SuperStruct@0x40，PropertyLink 链头@0x70。
constexpr std::uint32_t kUeStructSuperOffset = 0x40;
constexpr std::uint32_t kUeStructPropertyLinkOffset = 0x70;
// FField：NamePrivate@0x20，Offset@0x44，Next@0x48。
constexpr std::uint32_t kUeFieldNextOffset = 0x48;
constexpr std::uint32_t kUeFieldNameOffset = 0x20;
constexpr std::uint32_t kUeFieldOffsetOffset = 0x44;
// UFunction：ParmsSize@0xB6。
constexpr std::uint32_t kUeFunctionParmsSizeOffset = 0xB6;
// ProcessEvent 在 UObject vtable 里的槽位（76 * 8）。
constexpr std::uint32_t kUeProcessEventVtableSlot = 0x4C;

struct UeReader {
    const AnomalyUe5NamesServiceV1* names{};

    template <class T>
    bool Read(const std::uintptr_t address, T& value) const {
        const auto* core = g_context.core;
        return address > 0x10000 && core != nullptr && core->read_memory != nullptr &&
            core->read_memory(
                core->user, address,
                {reinterpret_cast<std::uint8_t*>(&value), sizeof(value)}).code ==
                ANOMALY_STATUS_V1_OK;
    }

    std::string Name(const UeFName value) const {
        if (value.id == 0 || names == nullptr || names->resolve_utf8 == nullptr) return {};
        std::size_t size{};
        if (names->resolve_utf8(names->user, value.id, nullptr, &size).code != 0 || size < 2 ||
            size > 512) {
            return {};
        }
        std::string text(size, '\0');
        if (names->resolve_utf8(names->user, value.id, text.data(), &size).code != 0 ||
            size > text.size()) {
            return {};
        }
        text.resize(size);
        if (const auto nul = text.find('\0'); nul != std::string::npos) text.resize(nul);
        if (value.number != 0) text += "_" + std::to_string(value.number - 1);
        return text;
    }

    std::string NameAt(const std::uintptr_t address) const {
        UeFName name{};
        return Read(address, name) ? Name(name) : std::string{};
    }
};

std::uintptr_t g_ue_registry{};
std::chrono::steady_clock::time_point g_ue_registry_retry{};

// 句柄 → 当前地址。注册表换代或槽位复用都会让句柄失效，这里逐级校验。
std::uintptr_t UeResolve(
    const UeReader& reader, const AnomalyUe5ObjectsServiceV1* objects,
    const AnomalyGenerationHandleV1 handle) {
    if (g_ue_registry == 0 || handle.id == 0 || objects == nullptr ||
        objects->snapshot_by_handle == nullptr) {
        return 0;
    }
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (objects->snapshot_by_handle(objects->user, handle, &snapshot).code != 0) return 0;
    const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    std::uintptr_t chunks{}, chunk{}, object{};
    std::uint32_t count{}, serial{};
    std::int32_t actual{};
    if (!reader.Read(g_ue_registry + 36, count) || count > 16u * 1024u * 1024u || index >= count ||
        !reader.Read(g_ue_registry + 16, chunks) || !reader.Read(chunks + 8 * (index / 65536), chunk) ||
        !reader.Read(chunk + 24 * (index % 65536), object) ||
        !reader.Read(chunk + 24 * (index % 65536) + 16, serial) ||
        serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ||
        !reader.Read(object + 12, actual) || actual != static_cast<std::int32_t>(index)) {
        return 0;
    }
    return object;
}

// 对象的类指针。
std::uintptr_t UeClassOf(const UeReader& reader, const std::uintptr_t object) {
    std::uintptr_t cls{};
    return reader.Read(object + kUeObjectClassOffset, cls) ? cls : 0;
}

// 类是否就是 base 或派生自 base：活实例几乎都是蓝图子类，必须沿 SuperStruct 链走。
bool UeClassDerivesFrom(
    const UeReader& reader, std::uintptr_t cls, const std::uintptr_t base) {
    for (int depth = 0; cls != 0 && depth < 32; ++depth) {
        if (cls == base) return true;
        std::uintptr_t parent{};
        if (!reader.Read(cls + kUeStructSuperOffset, parent) || parent == cls) break;
        cls = parent;
    }
    return false;
}

bool UeIsExecutable(const std::uintptr_t entry) {
    MEMORY_BASIC_INFORMATION region{};
    if (entry == 0 || !VirtualQuery(reinterpret_cast<void*>(entry), &region, sizeof(region))) {
        return false;
    }
    return region.State == MEM_COMMIT &&
        (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)) != 0;
}

// ProcessEvent(object, function, parms)。必须只在 Game 回调域调用，
// 且绝不能在 ProcessEvent 回调内部调用（宿主在回调里持有锁，会死锁）。
bool UeInvoke(const UeReader& reader, const std::uintptr_t object, const std::uintptr_t function,
              void* parameters) {
    std::uintptr_t vtable{}, entry{};
    if (!reader.Read(object, vtable) ||
        !reader.Read(vtable + kUeProcessEventVtableSlot * 8, entry) || !UeIsExecutable(entry)) {
        return false;
    }
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(entry)(
            reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), parameters);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 解析对象注册表基址（一次，失败后退避 5 秒）。
bool UeEnsureRegistry() {
    if (g_ue_registry != 0) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_ue_registry_retry) return false;
    g_ue_registry_retry = now + std::chrono::seconds(5);

    const auto* host = g_context.host;
    if (host == nullptr) return false;
    const auto signature = anomaly::sdk::Host(host)
        .Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID,
                                          ANOMALY_SIGNATURE_SERVICE_V1_VERSION)
        .get();
    if (signature == nullptr || signature->resolve == nullptr) return false;

    UeReader reader{nullptr};
    std::uintptr_t instruction{};
    std::int32_t displacement{};
    if (signature->resolve(signature->user, StringView("HTGame.exe"), StringView(".text"),
                           StringView(kUeObjectsPattern), &instruction).code != 0 ||
        !reader.Read(instruction + 3, displacement)) {
        return false;
    }
    const auto resolved = static_cast<std::intptr_t>(instruction) + 7 +
        static_cast<std::intptr_t>(displacement) - 16;
    if (resolved <= 0) return false;
    g_ue_registry = static_cast<std::uintptr_t>(resolved);
    return true;
}

}  // namespace
