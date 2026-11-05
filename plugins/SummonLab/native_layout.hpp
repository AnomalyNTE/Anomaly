#pragma once

#include <anomaly/sdk/profile.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace summon_lab {

// Fixed native call ABI supported by the profile's semantic contract and
// checked against both SDKs. These types are not fallback memory offsets.
using NativeIdentity = std::array<std::uint8_t, 40>;
using SoftClassPath = std::array<std::uint8_t, 32>;
using SoftClassRef = std::array<std::uint8_t, 40>;
using MirrorEntry = std::array<std::uint8_t, 32>;
using MirrorBuffer = std::array<std::uint8_t, 96>;
using Vector = std::array<double, 3>;
struct alignas(16) Transform {
    std::array<double, 4> rotation{};
    Vector translation{};
    double translation_padding{};
    Vector scale{};
    double scale_padding{};
};
struct TeleportParameters { Vector location{}, rotation{}; std::uint8_t result{}; };
struct NativeString { const wchar_t* data{}; std::int32_t count{}, capacity{}; };
struct MakeSoftClassPath { NativeString input; SoftClassPath result{}; };
struct ConvertSoftClass { SoftClassPath input{}; SoftClassRef result{}; };
struct LoadClass { SoftClassRef input{}; std::uintptr_t result{}; };
struct HealthMaximum { std::uint8_t fixed{}; float result{}; };
struct SetHealth {
    float hp{};
    std::uint8_t damage_reason{}, real_dead_damage{};
    float real_damage{};
};
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324) // The native FTransform parameter requires 16-byte alignment.
#endif
struct SummonParameters {
    SoftClassRef uclass{};
    std::uint8_t pet{};
    Vector offset{};
    std::uint8_t attack_mode{};
    std::int32_t level{}, skill_level{};
    Transform transform;
    std::uint8_t same_camp{};
    std::uintptr_t result{};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
struct ConfigureSummon {
    NativeIdentity leader{};
    std::int32_t index{};
    std::uint8_t camp{};
    float hp_percent{};
};
struct ObjectBool { std::uintptr_t object{}; std::uint8_t value{}; };
struct TextProperty { std::uintptr_t object{}; std::uint64_t name{}; std::array<std::uint8_t, 16> value{}; };
struct BoolProperty { std::uintptr_t object{}; std::uint64_t name{}; std::uint8_t value{}; };
struct NameProperty { std::uintptr_t object{}; std::uint64_t name{}, value{}; };
static_assert(sizeof(SummonParameters) == 208 && sizeof(ConfigureSummon) == 52);
static_assert(sizeof(MakeSoftClassPath) == 48 && sizeof(ConvertSoftClass) == 72 && sizeof(LoadClass) == 48);
static_assert(sizeof(HealthMaximum) == 8 && sizeof(Transform) == 96 && sizeof(TextProperty) == 32);
static_assert(sizeof(ObjectBool) == 16 && sizeof(BoolProperty) == 24);
static_assert(sizeof(NameProperty) == 24);
static_assert(sizeof(SetHealth) == 12 && offsetof(SetHealth, real_damage) == 8);
static_assert(offsetof(Transform, translation) == 32 && offsetof(Transform, scale) == 64);
static_assert(offsetof(SummonParameters, offset) == 48 && offsetof(SummonParameters, transform) == 96);
static_assert(sizeof(TeleportParameters) == 56 && offsetof(TeleportParameters, result) == 48);

// Numeric offsets come only from the host-loaded profile. No compiled fallback.
#define SUMMON_MEMORY_FIELDS(X) \
    X(objectFlags, std::uint32_t) \
    X(objectInternalIndex, std::int32_t) \
    X(objectClass, std::uintptr_t) \
    X(objectName, std::uint64_t) \
    X(objectOuter, std::uintptr_t) \
    X(functionFlags, std::uint32_t) \
    X(functionParameterSize, std::uint16_t) \
    X(functionExec, std::uintptr_t) \
    X(scriptFrameObject, std::uintptr_t) \
    X(classDefaultObject, std::uintptr_t) \
    X(characterAbilitySystem, std::uintptr_t) \
    X(characterAttributeSet, std::uintptr_t) \
    X(attributeBaseValue, float) \
    X(attributeCurrentValue, float) \
    X(structSuper, std::uintptr_t) \
    X(structProperties, std::uintptr_t) \
    X(structSize, std::int32_t) \
    X(fieldNext, std::uintptr_t) \
    X(fieldClass, std::uintptr_t) \
    X(fieldName, std::uint64_t) \
    X(fieldClassName, std::uint32_t) \
    X(propertyArrayDim, std::int32_t) \
    X(propertyElementSize, std::int32_t) \
    X(propertyOffset, std::int32_t) \
    X(propertyFlags, std::uint64_t) \
    X(boolByteOffset, std::uint8_t) \
    X(boolByteMask, std::uint8_t) \
    X(boolFieldMask, std::uint8_t) \
    X(propertyStruct, std::uintptr_t) \
    X(arrayInner, std::uintptr_t) \
    X(attributeProperty, std::uintptr_t) \
    X(containerOwner, std::uintptr_t) \
    X(spawnedAttributes, std::uintptr_t) \
    X(mirrorGetterSlot, std::uintptr_t)

#define SUMMON_BUFFER_FIELDS(X) \
    X(characterForNetSize) X(diagnosticSummonIndex) X(attributeDataSize) \
    X(mirrorVtable) X(mirrorObject) X(mirrorValues) X(mirrorValueCount) X(mirrorValueCapacity) \
    X(mirrorValueStride) X(mirrorValueName) X(mirrorValueData) X(mirrorValueBytes) \
    X(mirrorValueDataCapacity) X(mirrorValueCompressed)

struct NativeLayout {
#define DECLARE_MEMORY(name, type) std::uint32_t name{};
    SUMMON_MEMORY_FIELDS(DECLARE_MEMORY)
#undef DECLARE_MEMORY
#define DECLARE_BUFFER(name) std::uint32_t name{};
    SUMMON_BUFFER_FIELDS(DECLARE_BUFFER)
#undef DECLARE_BUFFER
};

inline bool Fits(std::uint32_t offset, std::size_t width, std::size_t size) {
    return offset <= size && width <= size - offset;
}

struct BufferField { std::uint32_t offset; std::size_t width; };

// Filtering rewrites borrowed buffers. Reject overlapping fields before a
// profile can turn an array-size write into an owner/vtable write.
inline bool ValidBuffer(std::size_t size, std::initializer_list<BufferField> fields) {
    for (auto current = fields.begin(); current != fields.end(); ++current) {
        if (!Fits(current->offset, current->width, size)) return false;
        for (auto other = fields.begin(); other != current; ++other)
            if (current->offset < other->offset + other->width &&
                other->offset < current->offset + current->width) return false;
    }
    return true;
}

inline bool ValidMirrorRecord(const NativeLayout& layout, std::size_t size) {
    return size <= sizeof(MirrorBuffer) && size % alignof(std::uintptr_t) == 0 &&
        ValidBuffer(size, {{layout.mirrorVtable, sizeof(std::uintptr_t)},
            {layout.mirrorObject, sizeof(std::uintptr_t)},
            {layout.mirrorValues, sizeof(std::uintptr_t)},
            {layout.mirrorValueCount, sizeof(std::int32_t)},
            {layout.mirrorValueCapacity, sizeof(std::int32_t)}});
}

inline bool LoadNativeLayout(const anomaly::sdk::Profile& profile, NativeLayout* output) {
    try {
        NativeLayout parsed;
        const auto& values = profile.layout;
        // Bounded/aligned reads plus each use site's reflected name/type/size
        // checks replace version-specific numeric equality guards.
#define LOAD_MEMORY(name, type) \
        parsed.name = values.at(#name); \
        if (parsed.name > 16384 || parsed.name % alignof(type) != 0) return false;
        SUMMON_MEMORY_FIELDS(LOAD_MEMORY)
#undef LOAD_MEMORY
#define LOAD_BUFFER(name) parsed.name = values.at(#name);
        SUMMON_BUFFER_FIELDS(LOAD_BUFFER)
#undef LOAD_BUFFER
        if (!ValidBuffer(parsed.propertyStruct, {{parsed.propertyArrayDim, sizeof(std::int32_t)},
                {parsed.propertyElementSize, sizeof(std::int32_t)}, {parsed.propertyFlags, sizeof(std::uint64_t)},
                {parsed.propertyOffset, sizeof(std::int32_t)}}) ||
            parsed.boolByteOffset < parsed.propertyStruct ||
            parsed.boolByteMask != parsed.boolByteOffset + sizeof(std::uint8_t) ||
            parsed.boolFieldMask != parsed.boolByteMask + sizeof(std::uint8_t)) return false;
        if (parsed.characterForNetSize != sizeof(NativeIdentity) ||
            parsed.diagnosticSummonIndex != sizeof(NativeIdentity) - sizeof(std::int32_t) ||
            parsed.attributeDataSize > 64 ||
            !ValidBuffer(parsed.attributeDataSize, {{parsed.attributeBaseValue, sizeof(float)},
                {parsed.attributeCurrentValue, sizeof(float)}}) ||
            parsed.mirrorValueStride != sizeof(MirrorEntry) ||
            !ValidMirrorRecord(parsed, sizeof(MirrorBuffer)) ||
            !ValidBuffer(sizeof(MirrorEntry), {{parsed.mirrorValueName, sizeof(std::uint64_t)},
                {parsed.mirrorValueData, sizeof(std::uintptr_t)},
                {parsed.mirrorValueBytes, sizeof(std::int32_t)},
                {parsed.mirrorValueDataCapacity, sizeof(std::int32_t)},
                {parsed.mirrorValueCompressed, sizeof(std::uint8_t)}})) return false;
        *output = parsed;
        return true;
    } catch (const std::out_of_range&) { return false; }
}

#undef SUMMON_MEMORY_FIELDS
#undef SUMMON_BUFFER_FIELDS
} // namespace summon_lab
