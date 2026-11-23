#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace equipment_calculator::consumption_abi {

// Fixed ProcessEvent parameter ABI, shared with the existing bridge's UID and
// TArray header types. Game object fields, record strides and native entry
// compatibility remain in Profile or validated runtime reflection.
// FName has four-byte alignment in this ABI. Using uint64_t for it would move
// the awaken-choice name from offset 12 to 16 and change the actual call.
struct Name final { std::uint32_t comparison{}, number{}; };
static_assert(sizeof(Name) == 8 && alignof(Name) == 4);

template<class Uid> struct Cost32 final { Uid uid; std::int32_t amount{}; };
template<class Uid> struct Cost64 final { Uid uid; std::int64_t amount{}; };
template<class Uid> struct Target final { Uid uid; };
template<class Uid, class Array> struct WithCosts final { Uid uid; Array costs; };
template<class Uid> struct WithName final { Uid uid; Name name; };
template<class Uid> struct SkillToLevel final { Uid uid; Name ability; std::int32_t level{}; };
template<class Uid> struct AwakenChoice final { Uid uid; std::int32_t level{}; Name effect; };
template<class Uid> struct AwakenRemove final { Uid uid; std::int32_t level{}; };
template<class Uid> struct Locked final { Uid uid; std::uint8_t locked{}; std::array<std::uint8_t, 3> padding{}; };
struct LotteryDraw final { std::int32_t count{}; std::uint8_t quality{}; std::array<std::uint8_t, 3> padding{}; };
template<class Array> struct LotteryGroup final {
    std::uint8_t quality{};
    std::array<std::uint8_t, 7> padding{};
    Array geometries;
};
template<class Uid> struct EquipmentLevel final { Uid uid; std::int32_t new_level{}, old_level{}; };
template<class Array> struct EquipmentOneKey final { Array equipment; };

// Client receipts are borrowed only during their observer callback. An Array
// here is a header, never an owning STL container; refund/award elements must
// be copied with their separately validated reflected schema before return.
template<class Uid, class Array> struct LevelReceipt final {
    Uid uid;
    std::int32_t old_level{}, new_level{}, old_experience{}, new_experience{};
    Array refunds;
};
template<class Uid> struct StageReceipt final { Uid uid; std::int32_t old_stage{}, new_stage{}; };
template<class Uid, class Array> struct AwakenReceipt final { Uid uid; std::int32_t level{}; Name effect; Array awards; };
template<class Array> struct LotteryReceipt final { std::int32_t count{}; std::array<std::uint8_t, 4> padding{}; Array awards; };
template<class Array> struct OneKeyReceipt final { Array equipment, refunds; };

template<class Uid, class Array> struct Frames final {
    using Amount32 = Cost32<Uid>;
    using Amount64 = Cost64<Uid>;
    using Costs = WithCosts<Uid, Array>;
    using Named = WithName<Uid>;
    using Skill = SkillToLevel<Uid>;
    using Choice = AwakenChoice<Uid>;
    using Remove = AwakenRemove<Uid>;
    using Lock = Locked<Uid>;
    using Group = LotteryGroup<Array>;
    using Equipment = EquipmentLevel<Uid>;
    using Levels = LevelReceipt<Uid, Array>;
    using Awaken = AwakenReceipt<Uid, Array>;
    using Lottery = LotteryReceipt<Array>;
    using OneKey = OneKeyReceipt<Array>;

    static_assert(std::is_standard_layout_v<Uid> && std::is_standard_layout_v<Array>);
    static_assert(sizeof(Uid) == 8 && alignof(Uid) == 4);
    static_assert(sizeof(Array) == 16 && alignof(Array) == 8);
    static_assert(sizeof(Amount32) == 12 && offsetof(Amount32, amount) == 8);
    static_assert(sizeof(Amount64) == 16 && offsetof(Amount64, amount) == 8);
    static_assert(sizeof(Target<Uid>) == 8);
    static_assert(sizeof(Costs) == 24 && offsetof(Costs, costs) == 8);
    static_assert(sizeof(Named) == 16 && offsetof(Named, name) == 8);
    static_assert(sizeof(Skill) == 20 && offsetof(Skill, level) == 16);
    static_assert(sizeof(Choice) == 20 && offsetof(Choice, effect) == 12);
    static_assert(sizeof(Remove) == 12 && offsetof(Remove, level) == 8);
    static_assert(sizeof(Lock) == 12 && offsetof(Lock, locked) == 8);
    static_assert(sizeof(Group) == 24 && offsetof(Group, geometries) == 8);
    static_assert(sizeof(Equipment) == 16 && offsetof(Equipment, new_level) == 8 && offsetof(Equipment, old_level) == 12);
    static_assert(sizeof(EquipmentOneKey<Array>) == 16);
    static_assert(sizeof(Levels) == 40 && offsetof(Levels, refunds) == 24);
    static_assert(sizeof(StageReceipt<Uid>) == 16);
    static_assert(sizeof(Awaken) == 40 && offsetof(Awaken, effect) == 12 && offsetof(Awaken, awards) == 24);
    static_assert(sizeof(Lottery) == 24 && offsetof(Lottery, awards) == 8);
    static_assert(sizeof(OneKey) == 32);

    // Reflected ParmsSize excludes trailing C++ padding for these functions.
    // The host accepts a full native buffer >= ParmsSize; validate reflection
    // against the actual endpoint rather than sizeof(the padded buffer).
    static constexpr std::size_t lottery_size = offsetof(LotteryDraw, quality) + sizeof(LotteryDraw::quality);
    static constexpr std::size_t lock_size = offsetof(Lock, locked) + sizeof(Lock::locked);
    static_assert(sizeof(LotteryDraw) == 8 && lottery_size == 5 && lock_size == 9);
};

}  // namespace equipment_calculator::consumption_abi
