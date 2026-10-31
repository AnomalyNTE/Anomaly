#pragma once
// 直接施加记录到的伤害效果（GameplayEffect）——与 b1anktool 自动攻击走同一条路径：
//   1. 用 Anomaly 的签名服务解析 GObjects（FUObjectArray）；
//   2. 用对象注册表把 generation handle 换成 UObject 地址（索引 + 序列号校验）；
//   3. 通过 UHTAbilitySystemComponent::K2_ApplyGameplayEffectToTargetWithClass
//      把伤害 GE 施加到记录中的目标身上。
//
// 所有地址与布局在使用前都用反射名字/长度校验（类名链、属性名字+偏移+大小、函数参数个数+大小），
// 任何一步不成立就拒绝调用并给出原因，绝不猜偏移、绝不写内存。

#include "anomaly/sdk/cpp.hpp"

// 头文件自己保证 Windows.h 的宏不污染后面的标准库头（仓库构建里通常已全局定义，
// 独立构建时也一样能编）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace damage_apply {

using anomaly::sdk::StringView;

// 与 Runtime Profile 中 ue5.GObjects 的签名一致。
// ProcessEvent 不能用签名扫描：Runtime 已经用它自己的 hook 改写了函数头，
// 所以这里改为从对象的 vtable 取 ProcessEvent 槽位（与 b1anktool 相同），并校验槽位里的字节。
inline constexpr const char* kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
inline constexpr const char* kApplyFunctionPath =
    "/Script/HTGame.HTAbilitySystemComponent.K2_ApplyGameplayEffectToTargetWithClass";
inline constexpr const char* kHpFunctionPath = "/Script/HTGame.HTAbilityCharacter.GetHP";
// UObject vtable 里 ProcessEvent 的槽位（UE5，本 build 与 b1anktool 一致）。
inline constexpr std::uint32_t kProcessEventVTableIndex = 0x4C;
// ProcessEvent 原始的序言；被 MinHook 改写后是 E9/FF 25 跳转。
inline constexpr std::array<std::uint8_t, 12> kProcessEventPrologue{
    0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

// 反射布局（由 Anomaly Runtime Profile 与 2026-10-23 游戏 build 的 dump 校对）。
inline constexpr std::int32_t kAbilitySystemOffset = 0x940;   // AActor::HTAbilitySystemComponent
inline constexpr std::int32_t kDurationPolicyOffset = 0x30;   // UHTGamePlayEffect::DurationPolicy
inline constexpr std::int32_t kClassOffset = 16;              // UObject::Class
inline constexpr std::int32_t kObjectIndexOffset = 12;        // UObject::InternalIndex
inline constexpr std::int32_t kObjectNameOffset = 24;         // UObject::Name (FName)
inline constexpr std::int32_t kStructSuperOffset = 64;        // UStruct::SuperStruct
inline constexpr std::int32_t kStructChildrenOffset = 112;    // UStruct::Children (FProperty*)
inline constexpr std::int32_t kPropertyNameOffset = 32;       // FProperty::Name (FName)
inline constexpr std::int32_t kPropertySizeOffset = 52;       // FProperty::ElementSize
inline constexpr std::int32_t kPropertyOffsetOffset = 68;     // FProperty::Offset_Internal
inline constexpr std::int32_t kFieldNextOffset = 72;          // FField::Next
inline constexpr std::int32_t kFunctionParmsOffset = 180;     // UFunction::NumParms
inline constexpr std::int32_t kFunctionSizeOffset = 182;      // UFunction::ParmsSize
inline constexpr std::uint32_t kRegistryCountOffset = 36;     // FUObjectArray::ObjObjects.NumElements
inline constexpr std::uint32_t kRegistryChunksOffset = 16;    // FUObjectArray::ObjObjects.Objects
inline constexpr std::uint32_t kItemStride = 24;              // FUObjectItem
inline constexpr std::uint32_t kChunkShift = 16;              // 每 chunk 65536 项
inline constexpr std::uint32_t kChunkSize = 1U << kChunkShift;
inline constexpr std::size_t kApplyParameterBytes = 120;

struct Bridge {
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalySignatureServiceV1* signature{};
    std::uintptr_t registry{};
    std::uintptr_t apply_function{};
    std::uintptr_t get_hp_function{};  // 可选：用于跳过已死亡的目标
    bool ready{};
    std::string status;
};

inline bool Read(const Bridge& bridge, const std::uintptr_t address, void* destination,
                 const std::size_t size) {
    if (address < 0x10000 || destination == nullptr || size == 0 ||
        bridge.core == nullptr || bridge.core->read_memory == nullptr) {
        return false;
    }
    return bridge.core->read_memory(
               bridge.core->user, address,
               {static_cast<std::uint8_t*>(destination), size}).code ==
        ANOMALY_STATUS_V1_OK;
}

template <typename Value>
inline bool ReadValue(const Bridge& bridge, const std::uintptr_t address, Value& value) {
    return Read(bridge, address, &value, sizeof(value));
}

// UObject/FProperty 的 FName（比较索引 + 序号）转成可读名字。
inline std::string NameAt(const Bridge& bridge, const std::uintptr_t address) {
    std::uint32_t id{};
    std::uint32_t number{};
    if (!ReadValue(bridge, address, id) || !ReadValue(bridge, address + 4, number)) return {};
    if (bridge.names == nullptr || bridge.names->resolve_utf8 == nullptr) return {};
    std::size_t size{};
    if (bridge.names->resolve_utf8(bridge.names->user, id, nullptr, &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size < 2 || size > 1024) {
        return {};
    }
    std::string name(size, '\0');
    if (bridge.names->resolve_utf8(bridge.names->user, id, name.data(), &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size > name.size()) {
        return {};
    }
    name.resize(size);
    if (const auto end = name.find('\0'); end != std::string::npos) name.resize(end);
    if (number != 0) {
        name += '_';
        name += std::to_string(number);
    }
    return name;
}

// 从类本身开始沿 SuperStruct 向上找名字。
inline bool ClassIsA(const Bridge& bridge, std::uintptr_t cls, const std::string_view base) {
    for (int depth = 0; cls != 0 && depth < 64; ++depth) {
        if (NameAt(bridge, cls + kObjectNameOffset) == base) return true;
        std::uintptr_t next{};
        if (!ReadValue(bridge, cls + kStructSuperOffset, next) || next == cls) return false;
        cls = next;
    }
    return false;
}

inline bool IsA(const Bridge& bridge, const std::uintptr_t object, const std::string_view base) {
    std::uintptr_t cls{};
    if (!ReadValue(bridge, object + kClassOffset, cls)) return false;
    return ClassIsA(bridge, cls, base);
}

// 属性存在且偏移、大小都与预期一致（只查这个类自己声明的属性）。
inline bool HasProperty(const Bridge& bridge, const std::uintptr_t owner,
                        const std::string_view name, const std::int32_t offset,
                        const std::int32_t size) {
    std::uintptr_t field{};
    if (!ReadValue(bridge, owner + kStructChildrenOffset, field)) return false;
    for (int depth = 0; field != 0 && depth < 2048; ++depth) {
        if (NameAt(bridge, field + kPropertyNameOffset) == name) {
            std::int32_t actual_offset{};
            std::int32_t actual_size{};
            return ReadValue(bridge, field + kPropertyOffsetOffset, actual_offset) &&
                actual_offset == offset &&
                ReadValue(bridge, field + kPropertySizeOffset, actual_size) &&
                actual_size == size;
        }
        std::uintptr_t next{};
        if (!ReadValue(bridge, field + kFieldNextOffset, next) || next == field) return false;
        field = next;
    }
    return false;
}

// 同上，但沿父类链查找：继承来的属性声明在父类上。
inline bool HasPropertyInChain(const Bridge& bridge, std::uintptr_t cls,
                               const std::string_view name, const std::int32_t offset,
                               const std::int32_t size) {
    for (int depth = 0; cls != 0 && depth < 64; ++depth) {
        if (HasProperty(bridge, cls, name, offset, size)) return true;
        std::uintptr_t next{};
        if (!ReadValue(bridge, cls + kStructSuperOffset, next) || next == cls) return false;
        cls = next;
    }
    return false;
}

inline bool HasFunction(const Bridge& bridge, const std::uintptr_t function,
                        const std::string_view name, const std::uint8_t parameter_count,
                        const std::uint16_t parameter_bytes) {
    std::uint8_t actual_count{};
    std::uint16_t actual_bytes{};
    return IsA(bridge, function, "Function") &&
        NameAt(bridge, function + kObjectNameOffset) == name &&
        ReadValue(bridge, function + kFunctionParmsOffset, actual_count) &&
        actual_count == parameter_count &&
        ReadValue(bridge, function + kFunctionSizeOffset, actual_bytes) &&
        actual_bytes == parameter_bytes;
}

// generation handle → UObject 地址：对象注册表索引 + 序列号 + InternalIndex 三重校验。
inline bool ResolveHandle(const Bridge& bridge, const AnomalyGenerationHandleV1 handle,
                          std::uintptr_t& object) {
    object = 0;
    if (handle.id == 0 || bridge.registry == 0 || bridge.objects == nullptr ||
        bridge.objects->snapshot_by_handle == nullptr) {
        return false;
    }
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (bridge.objects->snapshot_by_handle(bridge.objects->user, handle, &snapshot).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    const std::uint32_t index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    std::uint32_t count{};
    std::uintptr_t chunks{};
    std::uintptr_t chunk{};
    std::uintptr_t candidate{};
    std::uint32_t serial{};
    std::int32_t internal_index{};
    if (!ReadValue(bridge, bridge.registry + kRegistryCountOffset, count) ||
        count > 16U * 1024U * 1024U || index >= count ||
        !ReadValue(bridge, bridge.registry + kRegistryChunksOffset, chunks) || chunks == 0) {
        return false;
    }
    if (!ReadValue(bridge, chunks + 8U * (index / kChunkSize), chunk) || chunk == 0) {
        return false;
    }
    const std::uintptr_t item = chunk + kItemStride * (index % kChunkSize);
    if (!ReadValue(bridge, item, candidate) || candidate == 0 ||
        !ReadValue(bridge, item + 16, serial) ||
        serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ||
        !ReadValue(bridge, candidate + kObjectIndexOffset, internal_index) ||
        internal_index != static_cast<std::int32_t>(index)) {
        return false;
    }
    object = candidate;
    return true;
}

// 解析对象表与伤害结算函数；任何布局不符都返回 false 并给出原因。
inline bool Initialize(Bridge& bridge) {
    if (bridge.ready) return true;
    bridge.status.clear();
    if (bridge.core == nullptr || bridge.core->read_memory == nullptr) {
        bridge.status = "缺少内存读取服务";
        return false;
    }
    if (bridge.signature == nullptr || bridge.signature->resolve == nullptr) {
        bridge.status = "缺少签名服务";
        return false;
    }
    if (bridge.objects == nullptr || bridge.objects->find_exact == nullptr ||
        bridge.objects->snapshot_by_handle == nullptr) {
        bridge.status = "缺少对象注册表服务";
        return false;
    }
    if (bridge.names == nullptr || bridge.names->resolve_utf8 == nullptr) {
        bridge.status = "缺少名称服务";
        return false;
    }

    std::uintptr_t instruction{};
    if (bridge.signature->resolve(
            bridge.signature->user, StringView("HTGame.exe"), StringView(".text"),
            StringView(kGObjectsPattern), &instruction).code != ANOMALY_STATUS_V1_OK ||
        instruction == 0) {
        bridge.status = "未找到对象表签名";
        return false;
    }
    std::int32_t displacement{};
    if (!ReadValue(bridge, instruction + 3, displacement)) {
        bridge.status = "对象表位移不可读";
        return false;
    }
    bridge.registry = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(instruction) + 7 + displacement - 16);
    if (bridge.registry == 0) {
        bridge.status = "对象表地址无效";
        return false;
    }

    AnomalyGenerationHandleV1 function_handle{};
    if (bridge.objects->find_exact(bridge.objects->user, StringView(kApplyFunctionPath),
            &function_handle).code != ANOMALY_STATUS_V1_OK) {
        bridge.status = "未找到伤害结算函数";
        return false;
    }
    if (!ResolveHandle(bridge, function_handle, bridge.apply_function)) {
        bridge.status = "伤害结算函数地址不可用";
        return false;
    }
    if (!HasFunction(bridge, bridge.apply_function,
            "K2_ApplyGameplayEffectToTargetWithClass", 6, 116)) {
        bridge.status = "伤害结算函数签名不符";
        return false;
    }
    const bool parameters_ok =
        HasProperty(bridge, bridge.apply_function, "GameplayEffect", 0, 8) &&
        HasProperty(bridge, bridge.apply_function, "TargetAbilityCmp", 8, 8) &&
        HasProperty(bridge, bridge.apply_function, "ModifyData", 16, 88) &&
        HasProperty(bridge, bridge.apply_function, "BuffEventType", 104, 1) &&
        HasProperty(bridge, bridge.apply_function, "bUsePredictionKey", 105, 1) &&
        HasProperty(bridge, bridge.apply_function, "ReturnValue", 108, 8);
    if (!parameters_ok) {
        bridge.status = "伤害结算参数布局不符";
        return false;
    }
    // GetHP 只用于跳过尸体，拿不到不影响施加伤害。
    AnomalyGenerationHandleV1 hp_handle{};
    std::uintptr_t hp_function{};
    if (bridge.objects->find_exact(bridge.objects->user, StringView(kHpFunctionPath),
            &hp_handle).code == ANOMALY_STATUS_V1_OK &&
        ResolveHandle(bridge, hp_handle, hp_function) &&
        HasFunction(bridge, hp_function, "GetHP", 1, 4)) {
        bridge.get_hp_function = hp_function;
    }
    bridge.ready = true;
    bridge.status = "已就绪";
    return true;
}

// 把伤害来源（GameplayEffect CDO 的对象路径）换成可以施加的 GE 类。
inline bool ResolveEffectClass(const Bridge& bridge, const std::string& source_path,
                               std::uintptr_t& effect_class, std::string& reason) {
    effect_class = 0;
    if (!bridge.ready) {
        reason = "伤害施加桥未就绪";
        return false;
    }
    if (source_path.empty()) {
        reason = "该段没有伤害来源路径";
        return false;
    }
    std::uintptr_t object{};
    AnomalyGenerationHandleV1 handle{};
    if (bridge.objects->find_exact(bridge.objects->user, StringView(source_path), &handle).code ==
            ANOMALY_STATUS_V1_OK &&
        ResolveHandle(bridge, handle, object)) {
        // 正常情况：source_name 是 GE 的 CDO 路径。
        std::uintptr_t cls{};
        if (ReadValue(bridge, object + kClassOffset, cls) && cls != 0 &&
            IsA(bridge, object, "HTGamePlayEffect") && ClassIsA(bridge, cls, "HTGamePlayEffect")) {
            // 能读到 DurationPolicy 时确认是即时效果；读不到（属性继承/布局差异）就不拦。
            std::uint8_t policy{};
            if (HasPropertyInChain(bridge, cls, "DurationPolicy", kDurationPolicyOffset, 1) &&
                ReadValue(bridge, object + kDurationPolicyOffset, policy) && policy != 0) {
                reason = "伤害来源不是即时效果";
                return false;
            }
            effect_class = cls;
            return true;
        }
        if (IsA(bridge, object, "Class") && ClassIsA(bridge, object, "HTGamePlayEffect")) {
            effect_class = object;
            return true;
        }
    }
    // 退一步：路径可能指向 GE 类而不是 CDO，这时直接取 ".Default__" 之前的类路径。
    const auto cdo_marker = source_path.find(".Default__");
    if (cdo_marker != std::string::npos && cdo_marker != 0) {
        const std::string class_path = source_path.substr(0, cdo_marker);
        if (bridge.objects->find_exact(bridge.objects->user, StringView(class_path), &handle).code ==
                ANOMALY_STATUS_V1_OK &&
            ResolveHandle(bridge, handle, object) && IsA(bridge, object, "Class") &&
            ClassIsA(bridge, object, "HTGamePlayEffect")) {
            effect_class = object;
            return true;
        }
    }
    reason = "伤害来源对象未找到";
    return false;
}

// 在 Game 线程同步执行 ProcessEvent(object, function, parameters)。
// ProcessEvent 从对象自己的 vtable 取；入口字节必须是原始序言或 MinHook 的 E9/FF25 跳转，
// 否则说明槽位不对，直接拒绝。只保留 POD 局部变量，异常由 SEH 兜住。
inline bool InvokeOnObject(const Bridge& bridge, void* object, void* function,
                           void* parameters) {
    if (!bridge.ready || object == nullptr || function == nullptr || parameters == nullptr) {
        return false;
    }
    std::uintptr_t vtable{};
    std::uintptr_t entry{};
    if (!ReadValue(bridge, reinterpret_cast<std::uintptr_t>(object), vtable) || vtable == 0 ||
        !ReadValue(bridge, vtable + kProcessEventVTableIndex * 8, entry) || entry == 0) {
        return false;
    }
    std::array<std::uint8_t, 8> head{};
    if (!Read(bridge, entry, head.data(), head.size())) return false;
    const bool original = std::equal(
        kProcessEventPrologue.begin(), kProcessEventPrologue.end(), head.begin());
    const bool patched = head[0] == 0xE9 || (head[0] == 0xFF && head[1] == 0x25);
    if (!original && !patched) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<void*>(entry), &region, sizeof(region)) == 0 ||
        region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)) == 0) {
        return false;
    }
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(entry)(
            object, function, parameters);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 读目标当前 HP（只用于跳过已死亡的目标）。
inline bool Health(const Bridge& bridge, const std::uintptr_t character, float& hp) {
    hp = -1.0F;
    if (!bridge.ready || bridge.get_hp_function == 0 || character == 0) return false;
    alignas(8) std::array<std::uint8_t, 16> parameters{};
    if (!InvokeOnObject(bridge, reinterpret_cast<void*>(character),
            reinterpret_cast<void*>(bridge.get_hp_function), parameters.data())) {
        return false;
    }
    std::memcpy(&hp, parameters.data(), sizeof(hp));
    return hp >= 0.0F && hp < 1.0e9F;
}

// 对目标施加一次伤害 GE（强度倍率 1、附加 0，模拟一次普通命中）。
inline bool Apply(const Bridge& bridge, const std::uintptr_t source_character,
                  const std::uintptr_t target_character, const std::uintptr_t effect_class,
                  std::int32_t& result, std::string& reason) {
    result = 0;
    if (!bridge.ready) {
        reason = "伤害施加桥未就绪";
        return false;
    }
    std::uintptr_t source_class{};
    std::uintptr_t target_class{};
    std::uintptr_t source_asc{};
    std::uintptr_t target_asc{};
    if (!ReadValue(bridge, source_character + kClassOffset, source_class) ||
        !ReadValue(bridge, target_character + kClassOffset, target_class) ||
        source_class == 0 || target_class == 0) {
        reason = "角色类型不可读";
        return false;
    }
    if (!HasPropertyInChain(bridge, source_class, "HTAbilitySystemComponent",
            kAbilitySystemOffset, 8) ||
        !HasPropertyInChain(bridge, target_class, "HTAbilitySystemComponent",
            kAbilitySystemOffset, 8)) {
        reason = "能力系统偏移与预期不符";
        return false;
    }
    if (!ReadValue(bridge, source_character + kAbilitySystemOffset, source_asc) ||
        !ReadValue(bridge, target_character + kAbilitySystemOffset, target_asc) ||
        source_asc == 0 || target_asc == 0 ||
        !IsA(bridge, source_asc, "HTAbilitySystemComponent") ||
        !IsA(bridge, target_asc, "HTAbilitySystemComponent")) {
        reason = "能力系统不可用";
        return false;
    }

    alignas(8) std::array<std::uint8_t, kApplyParameterBytes> parameters{};
    const auto put = [&parameters](const std::size_t at, auto value) {
        std::memcpy(parameters.data() + at, &value, sizeof(value));
    };
    put(0, effect_class);            // GameplayEffect（TSubclassOf）
    put(8, target_asc);              // TargetAbilityCmp
    put(16 + 0x28, std::int32_t{1}); // 效果等级 1
    put(16 + 0x38, std::int32_t{1}); // 施加一次
    put(16 + 0x3c, 1.0F);            // fStrengthMult
    put(16 + 0x40, 0.0F);            // fStrengthAdd
    // BuffEventType(104) / bUsePredictionKey(105) 保持 0。
    if (!InvokeOnObject(bridge, reinterpret_cast<void*>(source_asc),
            reinterpret_cast<void*>(bridge.apply_function), parameters.data())) {
        reason = "伤害结算调用异常";
        return false;
    }
    std::memcpy(&result, parameters.data() + 108, sizeof(result));
    return true;
}

}  // namespace damage_apply
