#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
namespace scene {
enum class RouteKind { Transfer, Single, Clone, Exit };
// Status from live tests of every switchable scene (2026-10-30/31). Anything not listed has not
// been tried and stays unverified, including scenes the catalog cannot switch to at all.
enum class SceneStatus { Unverified, Verified, Unusable };
// A user-visible text: the localization key and its English fallback. The plugin translates it.
struct Text { const char* key; const char* fallback; };
// hold_exit: the scene sends the player back by itself; the loader holds that request while there.
struct KnownScene { std::string_view key; SceneStatus status; Text reason; bool hold_exit{}; };
inline constexpr Text kReasonInstance{"reason.instance", "Connection failed (instance; the server does not accept a direct teleport)"};
inline constexpr Text kReasonWeekly{"reason.weekly", "Connection failed (weekly instance; the server does not accept a direct teleport)"};
inline constexpr Text kReasonNotOpen{"reason.not_open", "Connection failed (not open on the server)"};
inline constexpr Text kReasonTestMap{"reason.test_map", "Connection failed (test map)"};
inline constexpr Text kReasonTravelError{"reason.travel_error", "The client fails to load the map (HandleTravelError) and stays on the loading screen"};
inline constexpr KnownScene kKnownScenes[] = {
    {"XL_map_bigworld_test", SceneStatus::Verified, {}},
    {"DLC_WuChao", SceneStatus::Verified, {}},
    {"DLC_SpiralDreams", SceneStatus::Verified, {}},
    {"DLC_HeiYu", SceneStatus::Verified, {}},
    {"DLC_Museum_Lamp", SceneStatus::Verified, {}},
    {"DLC_Museum_Lamp_Story2", SceneStatus::Verified, {}},
    {"DLC_DunnerHouse", SceneStatus::Verified, {}},
    {"DLC_School_boss01_WP", SceneStatus::Verified, {}},
    {"DLC_MoonDog", SceneStatus::Verified, {}},
    {"DLC_Hotel", SceneStatus::Verified, {}},
    {"FilmOrbit_map_WP_World", SceneStatus::Verified, {}},
    {"DLC_FilmOrbit_map_WP", SceneStatus::Verified, {}},
    {"Maps_4N", SceneStatus::Verified, {}},
    // Instanced content: the server only accepts its own entry flow ("目标场景失去联络信号").
    {"DLC_Bank_Safety", SceneStatus::Unusable, kReasonInstance},
    {"DLC_DunnerHouse_Week", SceneStatus::Unusable, kReasonWeekly},
    {"DLC_Hospital_FB", SceneStatus::Unusable, kReasonInstance},
    {"DLC_Castle_Demo", SceneStatus::Unusable, kReasonNotOpen},
    {"Testmap_liuyang", SceneStatus::Unusable, kReasonTestMap},
    {"Testmap_cc2", SceneStatus::Unusable, kReasonTestMap},
    {"MassAI_wtao", SceneStatus::Unusable, kReasonTestMap},
    // Traced 2026-10-31: the server accepts the entry and sends ClientTravel, then the client's map
    // load fails (GameInstance.HandleTravelError) and the game stays on the loading screen.
    {"DLC_DeadNumber", SceneStatus::Unusable, kReasonTravelError},
    // About 8 s after arrival the scene's main logic (MLPS_MainLogic_SLY_C) itself calls
    // ServerRequestExitSingleLevelSceneRPC(q110135). With that request held back the player stays
    // (live test 2026-10-31), so the loader holds it while the player is in this scene.
    {"DLC_Hockshop", SceneStatus::Verified, {}, true},
};
inline bool HoldsExit(std::string_view id) {
    for (const auto& known : kKnownScenes) if (known.key == id) return known.hold_exit;
    return false;
}
// UObject::ProcessEvent sends a native function to the server only while it carries FUNC_Net;
// without it the function runs locally. Holding the exit request clears that flag on the server RPC
// (and points its native entry at a no-op), so the request is swallowed on the client.
constexpr uint32_t kFuncNet = 0x00000040;
constexpr uint32_t kFuncNetServer = 0x00200000;
inline bool CanHoldExit(uint32_t flags) {return (flags & kFuncNet) && (flags & kFuncNetServer);}
inline uint32_t HeldExitFlags(uint32_t flags) {return flags & ~kFuncNet;}
inline const KnownScene* FindKnownScene(std::string_view id) {
    for (const auto& known : kKnownScenes) if (known.key == id) return &known;
    return nullptr;
}
inline SceneStatus StatusOf(std::string_view id) {
    const auto* known = FindKnownScene(id);
    return known ? known->status : SceneStatus::Unverified;
}
inline bool Verified(std::string_view id) {return StatusOf(id) == SceneStatus::Verified;}
inline bool Unusable(std::string_view id) {return StatusOf(id) == SceneStatus::Unusable;}
inline Text UnusableReason(std::string_view id) {
    const auto* known = FindKnownScene(id);
    return known && known->status == SceneStatus::Unusable ? known->reason : Text{};
}
inline Text StatusLabel(std::string_view id) {
    switch (StatusOf(id)) {
    case SceneStatus::Verified: return {"scene.verified", "Verified"};
    case SceneStatus::Unusable: return {"scene.unusable", "Unusable"};
    default: return {"scene.unverified", "Unverified"};
    }
}
inline bool SceneConnectionError(std::string_view text) {
    return text.find("目标场景失去联络信号")!=std::string_view::npos;
}
inline size_t Nearest(const std::vector<std::array<double,3>>& points,const std::array<double,3>& anchor) {
    size_t found=points.size();double best=std::numeric_limits<double>::infinity();
    for(size_t i=0;i<points.size();++i) {
        double distance{};bool valid=true;
        for(size_t k=0;k<3;++k) {if(!std::isfinite(points[i][k]) || !std::isfinite(anchor[k])) valid=false;const auto delta=points[i][k]-anchor[k];distance+=delta*delta;}
        if(valid && distance<best) {best=distance;found=i;}
    }
    return found;
}
inline bool CanEnter(uint8_t type,bool has_route) {return has_route && type!=1 && type!=4 && type<=8;}
// Choosing another entrance of the scene the player is already in: the entrance is an in-scene
// teleport, so no world change (and no transition wait) is involved.
inline bool SameSceneSwitch(std::string_view current,std::string_view target,bool has_route) {
    return has_route && !target.empty() && current==target;
}
// ClientDataLayerRPC message type that activates the named layer instance (observed in live
// server traffic; replaying it locally sets the instance to Activated).
constexpr int32_t kActivateLayerMessage = 39;
// Its counterpart: in live big-world traffic a layer opened with 39 is closed with 40 when the
// player leaves the area (same instance-name payload).
constexpr int32_t kDeactivateLayerMessage = 40;
// Parameter type tag of the wrapper carrying an FString (seen in the captured traffic).
constexpr uint64_t kStringParameterTag = 0x0F;
// Serialized FString carried by that message: int32 length including the terminator, the
// characters, then a 0 byte (a 43-character instance name is a 47-byte buffer).
inline std::vector<uint8_t> LayerNamePayload(std::string_view name) {
    const auto length = static_cast<int32_t>(name.size() + 1);
    std::vector<uint8_t> out(4 + name.size() + 1, 0);
    for (size_t i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>((static_cast<uint32_t>(length) >> (8 * i)) & 0xFF);
    for (size_t i = 0; i < name.size(); ++i) out[4 + i] = static_cast<uint8_t>(name[i]);
    return out;
}
// EDataLayerRuntimeState: Unloaded=0, Loaded=1, Activated=2.
inline Text LayerStateName(uint8_t state) {
    switch (state) {
    case 0: return {"layer.unloaded", "Unloaded"};
    case 1: return {"layer.loaded", "Loaded"};
    case 2: return {"layer.activated", "Activated"};
    default: return {"layer.unknown", "Unknown"};
    }
}

inline Text TypeName(uint8_t type) {
    switch(type) {
    case 0: return {"type.open_world", "Open world"};
    case 1: return {"type.character_creation", "Character creation"};
    case 2: return {"type.single_level", "Single-player scene"};
    case 3: return {"type.instance", "Instance"};
    case 4: return {"type.updater", "Updater"};
    case 5: return {"type.abyss", "Abyss"};
    case 6: return {"type.racing", "Racing"};
    case 7: return {"type.tower", "Tower"};
    case 8: return {"type.multiplayer", "Multiplayer"};
    default: return {"type.other", "Other"};
    }
}
struct Transition {
    bool busy{},same_world_recovery{};uint64_t started{};std::string from,target;
    void Recover(std::string current,std::string next,uint64_t now) {
        from=std::move(current);target=std::move(next);started=now;busy=true;same_world_recovery=from==target;
    }
    bool Begin(std::string current,std::string next,uint64_t now) {
        if(busy || next.empty() || current==next) return false;
        from=std::move(current);target=std::move(next);started=now;busy=true;same_world_recovery=false;return true;
    }
    enum class Result { Waiting, Arrived, Different, Timeout };
    Result Check(std::string_view actual,uint64_t now,bool confirmed=false) {
        if(actual==target && (!same_world_recovery || confirmed)) {busy=false;return Result::Arrived;}
        if(!actual.empty() && actual!=from) {busy=false;return Result::Different;}
        if(now-started>=30000) {busy=false;return Result::Timeout;}
        return Result::Waiting;
    }
};
}
