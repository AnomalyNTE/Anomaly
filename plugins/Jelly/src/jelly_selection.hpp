#pragma once
//
// 目标筛选 (target selection).
//
// Turns the actor page snapshot and the camera session into the bounded list of
// targets the effect pass may touch, and decides when each pass may run. Pure:
// it takes plain sample data and returns decisions, so the offline fixture can
// drive it without a game.

#include "jelly_effect.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jelly {

// Caps and cadence. The model count is the one cap the window can move; the caps
// that follow from it are derived here, so raising the count cannot leave the
// recovery ledger or the candidate shortlist behind.
inline constexpr std::size_t kMinimumModels = 1;
inline constexpr std::size_t kDefaultModels = 32;
inline constexpr std::size_t kMaximumModelsLimit = 128;
// Every record carries one visual root, so this is the room the ledger keeps per
// model. It is the ratio the fixed 32-model cap used to carry (64 roots).
inline constexpr std::size_t kRootsPerModel = 2;
inline constexpr double kEffectIntervalSeconds = 1.0 / 30.0;
inline constexpr double kSelectionIntervalSeconds = 1.0 / 5.0;
inline constexpr double kEffectSoftBudgetMilliseconds = 1.0;
inline constexpr std::size_t kSelectionCandidateBudget = 256;
// The floor of the shortlist capacity the resolution pass works from. The extra room
// over the model cap is what lets the nearest *scalable* models win, instead of
// whatever happens to sit closest to the camera (hidden party members, name plates,
// weather and camera actors). ShortlistCapacity() is what the caller uses, because it
// grows with a raised model cap.
inline constexpr std::size_t kCandidateShortlist = 128;
inline constexpr std::size_t kMaximumVisualDepth = 16;
inline constexpr std::size_t kMaximumVisualChildren = 64;

// The number of models the effect may control at once. The window's slider and the
// configuration file both carry a number, so this is the single place that rounds it
// and clamps it, and the plugin, the window and the fixture all agree on the range.
inline std::size_t ClampModels(const double value) noexcept {
    if (!IsFinite(value)) return kDefaultModels;
    if (value <= static_cast<double>(kMinimumModels)) return kMinimumModels;
    if (value >= static_cast<double>(kMaximumModelsLimit)) return kMaximumModelsLimit;
    return static_cast<std::size_t>(value + 0.5);
}

// The visual roots the ledger may hold for a model count.
inline std::size_t RootBudget(const std::size_t models) noexcept {
    return models * kRootsPerModel;
}

// The shortlist is a buffer, not a target: it holds the nearest in-view entries the
// resolution pass works from, and it is deliberately several times the model cap so a
// nearer entry that turns out not to be a model cannot push a model the effect already
// owns out of the list (which would restore it and take it over again).
inline std::size_t ShortlistCapacity(const std::size_t models) noexcept {
    const std::size_t wanted = models * 4U;
    return wanted > kCandidateShortlist ? wanted : kCandidateShortlist;
}

struct Settings {
    bool enabled{false};
    double amplitude{0.25};
    double frequency_hz{1.5};
    // The rest between two bounces, in seconds; zero is the continuous bounce.
    double pause_seconds{0.0};
    // Whether the models under the effect also turn about their own up axis, and how fast.
    bool spin{false};
    double spin_degrees_per_second{kDefaultSpinDegreesPerSecond};
    std::size_t max_models{kDefaultModels};
    bool local_player{true};
    bool characters{true};
    bool other_dynamic{true};
    // Whether a vehicle whose body mesh is the chassis rigid body - the car the player
    // drives - may be scaled as a whole. Off keeps the original rule (a simulating mesh
    // is never written); on scales the car with its physics body, which is the trade the
    // window's switch makes visible.
    bool simulated_vehicles{true};
};

struct Camera {
    Vector3 position{};
    // Degrees, in UE FRotator order: (pitch, yaw, roll).
    Vector3 rotation{};
    double horizontal_fov_degrees{90.0};
};

struct Bounds {
    Vector3 center{};
    Vector3 extent{};
};

struct CameraBasis {
    Vector3 position{};
    Vector3 forward{};
    Vector3 right{};
    Vector3 up{};
    double tan_half_horizontal{};
    double tan_half_vertical{};
    bool valid{};
};

inline constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
// Only the horizontal field of view is published. A 4:3 height is assumed so the
// vertical frustum planes stay wider than any current display and never cull
// something the player can actually see.
inline constexpr double kConservativeAspect = 4.0 / 3.0;
// Anything closer than this is behind or grazing the lens, not a model in view.
inline constexpr double kNearPlaneCentimetres = 1.0;

CameraBasis MakeCameraBasis(const Camera& camera) noexcept;

// Conservative box-versus-frustum test (the classic p-vertex test per plane).
// No far plane: a distant model is cheap to scale and harmless, and culling it
// would only make the effect pop.
bool IntersectsFrustum(const CameraBasis& basis, const Bounds& bounds) noexcept;

// AnomalyNteEntityFlagsV1 values, mirrored so the policy is testable without the SDK.
//
// Only `kEntityFlagLocalPlayer` is a gate: the host sets it from
// `actor == player_pawn`, which is a comparison it makes itself. The three mobility
// bits are *not* used anywhere: the host derives them from a byte it reads at
// `sceneComponent.mobility` and never validates (its actor validator reads the byte and
// discards it), and the ecosystem treats that byte as four-state including Unknown.
// Measured on the live build, no actor in a 5400-actor frame carried Movable at all, so
// gating on it either reads everything and refuses everything or matches nothing.
inline constexpr std::uint32_t kEntityFlagStatic = 1U << 0U;
inline constexpr std::uint32_t kEntityFlagStationary = 1U << 1U;
inline constexpr std::uint32_t kEntityFlagMovable = 1U << 2U;
inline constexpr std::uint32_t kEntityFlagLocalPlayer = 1U << 3U;

// AnomalyNteSnapshotFlagsV1 values, mirrored so the policy is testable without the
// SDK; the plugin static_asserts the mirror against the real constants.
inline constexpr std::uint32_t kSnapshotValid = 1U << 29U;
inline constexpr std::uint32_t kSnapshotStale = 1U << 30U;
inline constexpr std::uint32_t kSnapshotPartial = 1U << 31U;

// Whether a frame, a page result or one entry may be read at all. STALE means the
// data belongs to an earlier frame, which is the only thing that makes a snapshot
// unusable here. PARTIAL is deliberately *not* a rejection: the host sets it whenever
// some actor could not be read or the player-ESP feature is unavailable, which says
// nothing about the actor list, and every entry is validated on its own before it is
// used. Refusing partial snapshots therefore only ever hid the whole effect.
inline bool IsUsableSnapshot(const std::uint32_t flags) noexcept {
    return (flags & kSnapshotValid) != 0 && (flags & kSnapshotStale) == 0;
}

enum class Category { local_player, character, other_dynamic, unsupported };

Category Categorize(std::uint32_t entity_flags, bool class_is_character) noexcept;

// The per-entry decision of a selection pass, so the plugin and the fixture agree on
// what a candidate is instead of each spelling the order out. Scope (a static mesh is
// the level's scenery, not a jelly target) is decided per component by the model
// module, where the component's own reflected Mobility can be read.
enum class ScreenResult : std::uint32_t { candidate, culled };

ScreenResult Screen(const CameraBasis& basis, const Bounds& bounds) noexcept;

// The page contract the host documents, in the plugin's own numbers: what a page says
// it returned against what was asked for. A sweep that continues across frame
// generations depends on this staying true, so it is checked here rather than trusted.
struct PageResult {
    std::uint32_t total_matches{};
    std::uint32_t returned{};
    std::uint32_t next_offset{};
};

bool SanePage(const PageResult& result, std::uint32_t offset, std::size_t capacity) noexcept;

inline bool CategoryEnabled(const Category category, const Settings& settings) noexcept {
    switch (category) {
        case Category::local_player: return settings.local_player;
        case Category::character: return settings.characters;
        case Category::other_dynamic: return settings.other_dynamic;
        case Category::unsupported: break;
    }
    return false;
}

struct Candidate {
    // Actor-frame identity from the snapshot service. Never a UE object handle:
    // the model module derives that separately and keeps both.
    std::uint64_t frame_handle_id{};
    std::uint64_t class_id{};
    std::uint32_t entity_flags{};
    Bounds bounds{};
    Category category{Category::unsupported};
    // Set by the effect stage once this entry has been resolved or refused, so the
    // resolution work of one sweep happens once rather than on every update.
    bool attempted{};
};

inline double SquaredDistanceTo(const Candidate& candidate, const Vector3& point) noexcept {
    const double x = candidate.bounds.center[0] - point[0];
    const double y = candidate.bounds.center[1] - point[1];
    const double z = candidate.bounds.center[2] - point[2];
    return x * x + y * y + z * z;
}

// Local player first, then nearest. Stable enough for the effect pass to keep its
// own records: the sort only decides who gets the remaining budget.
bool Precedes(const Candidate& left, const Candidate& right, const Vector3& camera_position) noexcept;

void SortForUpdate(std::vector<Candidate>& candidates, const Vector3& camera_position) noexcept;

// Keeps a candidate for the next resolution pass. The shortlist holds the nearest
// `capacity` in-view models and always keeps the local player, which is the one target
// the window asks for by name and therefore never the entry that gets evicted. Returns
// true when the candidate ended up in the list.
bool KeepCandidate(
    std::vector<Candidate>& shortlist, const Candidate& candidate,
    const Vector3& camera_position, std::size_t capacity) noexcept;

// The pass gate: a pass runs when it has been at least `interval` since the last
// one, or when it has never run.
inline bool IsDue(const double now, const double last, const double interval) noexcept {
    return last < 0.0 || now - last >= interval;
}

}  // namespace jelly
