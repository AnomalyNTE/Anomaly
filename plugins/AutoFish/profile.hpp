#pragma once
namespace fishing::profile {
// Verified for the current NTE/UE5 SDK. Each native symbol must resolve uniquely;
// reflected object classes and parameter layouts are checked at its call site.
inline constexpr auto Objects="48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
inline constexpr auto World="48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
inline constexpr auto RodUpdate="48 89 5C 24 10 48 89 6C 24 18 56 48 81 EC 80 00 00 00 80 B9 58 10 00 00 00 41 0F B6 E8 0F 29 7C 24 60 8B F2 0F 28 FB 48 8B D9 74 1C 80";
inline constexpr auto CastBegin="40 55 56 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC D0 00 00 00 48 8B F1";
inline constexpr auto CatchContinue="48 89 5C 24 ?? 57 48 83 EC 60 80 B9 ?? ?? ?? ?? ?? 0F B6 FA 48 8B D9 0F 84 ?? ?? ?? ?? 48 8B 91";
inline constexpr auto TextToString="40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 0B 48 8B 01 48 83 C4 20";
inline constexpr int RodSpeedOffset=0x1040;
}
