#include "synthetic_ue.hpp"

#include "jelly_effect.hpp"
#include "jelly_json.hpp"
#include "jelly_selection.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace jelly_tests {
namespace {

void TestEffect() {
    Section("effect: identity, squash/stretch, volume, no drift, frame-rate independence");
    const jelly::Vector3 base{1.7, 0.6, -2.3};

    const jelly::Vector3 idle = jelly::Output(base, 0.0, 1.5, 12.5, 0.7);
    CHECK(jelly::Equal(idle, base));

    // sin(2*pi*f*t + phase) == 1 at t == (pi/2 - phase) / (2*pi*f)
    const double frequency = 1.5;
    const double stretched_at = (3.14159265358979323846 / 2.0) / (jelly::kTau * frequency);
    const jelly::Vector3 peak = jelly::Output(base, 0.5, frequency, stretched_at, 0.0);
    // The third axis of this base is mirrored, so stretching it means a larger
    // magnitude, not a larger number.
    CHECK(std::abs(peak[2]) > std::abs(base[2]));
    CHECK(std::abs(peak[0]) < std::abs(base[0]));
    CHECK(std::abs(peak[1]) < std::abs(base[1]));
    CHECK(std::abs(peak[0] / base[0] - peak[1] / base[1]) < 1e-12);

    // The three factors multiply to one for any displacement, the original ratio is
    // preserved, and a mirrored axis stays mirrored.
    for (const double amplitude : {0.0, 0.1, 0.25, 0.6}) {
        for (double elapsed = 0.0; elapsed < 3.0; elapsed += 0.017) {
            const jelly::Vector3 factors =
                jelly::Factors(jelly::Wave(amplitude, frequency, elapsed, 0.31));
            CHECK(std::abs(factors[0] * factors[1] * factors[2] - 1.0) < 1e-12);
            const jelly::Vector3 output =
                jelly::Output(base, amplitude, frequency, elapsed, 0.31);
            CHECK(std::abs(output[2] - base[2] * factors[2]) < 1e-9);
            CHECK(std::abs(output[0] / base[0] - factors[0]) < 1e-9);
            if (base[2] < 0.0) CHECK(output[2] < 0.0);
        }
    }

    // A long run always recomputes from the base, so nothing accumulates.
    double worst = 0.0;
    for (int step = 0; step < 200000; ++step) {
        const double elapsed = static_cast<double>(step) * 0.01;
        const jelly::Vector3 output = jelly::Output(base, 0.25, 1.5, elapsed, 1.1);
        const jelly::Vector3 expected =
            jelly::Apply(base, jelly::Factors(jelly::Wave(0.25, 1.5, elapsed, 1.1)));
        for (int axis = 0; axis < 3; ++axis) {
            worst = (std::max)(worst, std::abs(output[axis] - expected[axis]));
        }
    }
    CHECK(worst == 0.0);

    // The same elapsed time gives the same result whatever the step size was, which
    // is what makes the effect independent of the frame rate.
    double coarse = 0.0;
    double fine = 0.0;
    for (int step = 0; step < 100; ++step) coarse += 0.1;
    for (int step = 0; step < 1000; ++step) fine += 0.01;
    CHECK(std::abs(coarse - fine) < 1e-9);
    CHECK(jelly::Equal(
        jelly::Output(base, 0.25, 1.5, coarse, 0.0),
        jelly::Output(base, 0.25, 1.5, fine, 0.0), 1e-9));

    // A stable phase per identity, inside one turn and different per identity.
    const double first = jelly::PhaseForIdentity(17);
    CHECK(jelly::PhaseForIdentity(17) == first);
    CHECK(first >= 0.0 && first < jelly::kTau);
    CHECK(jelly::PhaseForIdentity(18) != first);

    CHECK(jelly::ClampAmplitude(-1.0) == jelly::kMinimumAmplitude);
    CHECK(jelly::ClampAmplitude(9.0) == jelly::kMaximumAmplitude);
    CHECK(jelly::ClampFrequency(0.0) == jelly::kMinimumFrequencyHz);
    CHECK(jelly::ClampFrequency(99.0) == jelly::kMaximumFrequencyHz);
    CHECK(jelly::ClampAmplitude(std::nan("")) == jelly::kMinimumAmplitude);
    CHECK(!jelly::IsUsableScale({1.0, 0.0, 1.0}));
    CHECK(!jelly::IsUsableScale({1.0, 1.0, std::nan("")}));
}

void TestSelection() {
    Section("selection: categories, frustum, order, cadence");
    // Only the local-player bit is a gate, and the host decides it by comparing the actor
    // with the player pawn. The mobility bits are not a gate anywhere: the host reads
    // them from a byte it never validates, so a class the game calls static is still
    // classified by what it is (scope is settled per component, by the component's own
    // reflected Mobility).
    CHECK(
        jelly::Categorize(jelly::kEntityFlagMovable | jelly::kEntityFlagLocalPlayer, false) ==
        jelly::Category::local_player);
    CHECK(
        jelly::Categorize(jelly::kEntityFlagMovable, true) == jelly::Category::character);
    CHECK(
        jelly::Categorize(jelly::kEntityFlagMovable, false) ==
        jelly::Category::other_dynamic);
    CHECK(jelly::Categorize(jelly::kEntityFlagStatic, true) == jelly::Category::character);
    CHECK(
        jelly::Categorize(jelly::kEntityFlagStatic, false) ==
        jelly::Category::other_dynamic);
    CHECK(jelly::Categorize(0, false) == jelly::Category::other_dynamic);
    // The local-player bit wins over everything else the frame may carry.
    CHECK(
        jelly::Categorize(
            jelly::kEntityFlagStatic | jelly::kEntityFlagLocalPlayer, true) ==
        jelly::Category::local_player);

    jelly::Settings settings;
    CHECK(jelly::CategoryEnabled(jelly::Category::local_player, settings));
    CHECK(jelly::CategoryEnabled(jelly::Category::character, settings));
    CHECK(jelly::CategoryEnabled(jelly::Category::other_dynamic, settings));
    settings.local_player = false;
    CHECK(!jelly::CategoryEnabled(jelly::Category::local_player, settings));
    settings.characters = false;
    CHECK(!jelly::CategoryEnabled(jelly::Category::character, settings));
    CHECK(!jelly::CategoryEnabled(jelly::Category::unsupported, settings));

    jelly::Camera camera;
    camera.position = jelly::Vector3{0.0, 0.0, 0.0};
    camera.rotation = jelly::Vector3{0.0, 0.0, 0.0};
    camera.horizontal_fov_degrees = 90.0;
    const jelly::CameraBasis basis = jelly::MakeCameraBasis(camera);
    CHECK(basis.valid);
    CHECK(std::abs(basis.forward[0] - 1.0) < 1e-12);
    CHECK(std::abs(basis.tan_half_horizontal - 1.0) < 1e-9);
    CHECK(basis.tan_half_vertical < basis.tan_half_horizontal);

    const auto box = [](const double x, const double y, const double z) {
        return jelly::Bounds{jelly::Vector3{x, y, z}, jelly::Vector3{50.0, 50.0, 50.0}};
    };
    CHECK(jelly::IntersectsFrustum(basis, box(500.0, 0.0, 0.0)));
    CHECK(!jelly::IntersectsFrustum(basis, box(-500.0, 0.0, 0.0)));
    CHECK(!jelly::IntersectsFrustum(basis, box(500.0, 700.0, 0.0)));
    CHECK(jelly::IntersectsFrustum(basis, box(500.0, 520.0, 0.0)));
    CHECK(!jelly::IntersectsFrustum(basis, box(500.0, 0.0, 900.0)));
    CHECK(!jelly::IntersectsFrustum(
        basis,
        jelly::Bounds{jelly::Vector3{500.0, 0.0, 0.0}, jelly::Vector3{0.0, 1.0, 1.0}}));
    // An unreadable camera must not throw every model out of view.
    jelly::Camera broken = camera;
    broken.horizontal_fov_degrees = 0.0;
    CHECK(!jelly::MakeCameraBasis(broken).valid);
    CHECK(jelly::IntersectsFrustum(jelly::MakeCameraBasis(broken), box(500.0, 0.0, 0.0)));

    std::vector<jelly::Candidate> candidates(3);
    candidates[0].frame_handle_id = 1;
    candidates[0].category = jelly::Category::other_dynamic;
    candidates[0].bounds.center = jelly::Vector3{900.0, 0.0, 0.0};
    candidates[1].frame_handle_id = 2;
    candidates[1].category = jelly::Category::local_player;
    candidates[1].bounds.center = jelly::Vector3{800.0, 0.0, 0.0};
    candidates[2].frame_handle_id = 3;
    candidates[2].category = jelly::Category::other_dynamic;
    candidates[2].bounds.center = jelly::Vector3{300.0, 0.0, 0.0};
    jelly::SortForUpdate(candidates, jelly::Vector3{0.0, 0.0, 0.0});
    CHECK(candidates[0].frame_handle_id == 2);  // the local player is first
    CHECK(candidates[1].frame_handle_id == 3);  // then nearest first
    CHECK(candidates[2].frame_handle_id == 1);

    CHECK(jelly::IsDue(10.0, -1.0, 1.0));
    CHECK(jelly::IsDue(10.0, 9.5, 0.5));
    CHECK(!jelly::IsDue(10.0, 9.7, 0.5));
}

// The shortlist is what the model budget is spent from: the nearest in-view models are
// kept, the local player is never the entry that gives way, and a list that is full
// replaces its farthest non-player rather than refusing everything after it.
void TestShortlist() {
    Section("selection: the candidate shortlist keeps the nearest models and the player");
    const jelly::Vector3 camera{0.0, 0.0, 0.0};
    const auto make = [](const std::uint64_t key, const double distance,
                         const jelly::Category category = jelly::Category::other_dynamic) {
        jelly::Candidate candidate;
        candidate.frame_handle_id = key;
        candidate.category = category;
        candidate.bounds.center = jelly::Vector3{distance, 0.0, 0.0};
        candidate.bounds.extent = jelly::Vector3{50.0, 50.0, 50.0};
        return candidate;
    };

    std::vector<jelly::Candidate> shortlist;
    CHECK(jelly::KeepCandidate(shortlist, make(1, 500.0), camera, 3));
    CHECK(jelly::KeepCandidate(shortlist, make(2, 100.0), camera, 3));
    CHECK(jelly::KeepCandidate(shortlist, make(3, 900.0), camera, 3));
    CHECK(shortlist.size() == 3);

    // A nearer model takes the slot of the farthest one.
    CHECK(jelly::KeepCandidate(shortlist, make(4, 10.0), camera, 3));
    CHECK(shortlist.size() == 3);
    jelly::SortForUpdate(shortlist, camera);
    CHECK(shortlist[0].frame_handle_id == 4);
    CHECK(shortlist[1].frame_handle_id == 2);
    CHECK(shortlist[2].frame_handle_id == 1);

    // A farther one than everything in the list is dropped instead of being appended.
    CHECK(!jelly::KeepCandidate(shortlist, make(5, 5000.0), camera, 3));
    CHECK(shortlist.size() == 3);

    // The local player keeps its slot even when it is the farthest entry: it is the model
    // the window asks for by name.
    std::vector<jelly::Candidate> with_player;
    CHECK(jelly::KeepCandidate(
        with_player, make(11, 0.0, jelly::Category::local_player), camera, 2));
    CHECK(jelly::KeepCandidate(with_player, make(12, 100.0), camera, 2));
    // The only entry that may give way is the non-player one, and it is nearer than this
    // candidate, so the list stays as it is.
    CHECK(!jelly::KeepCandidate(with_player, make(13, 200.0), camera, 2));
    jelly::SortForUpdate(with_player, camera);
    CHECK(with_player.size() == 2);
    CHECK(with_player[0].frame_handle_id == 11);  // the player is still first
    CHECK(with_player[1].frame_handle_id == 12);  // and the nearest rival followed

    // A zero-capacity list keeps nothing, and the shipped capacity is room for the model
    // cap with slack.
    std::vector<jelly::Candidate> none;
    CHECK(!jelly::KeepCandidate(none, make(21, 1.0), camera, 0));
    CHECK(none.empty());
    static_assert(jelly::kCandidateShortlist >= jelly::kDefaultModels);
    // The window moves the model cap, and everything that has to be at least as large as
    // it follows from the same function, so a raised cap cannot leave the shortlist or the
    // ledger behind.
    CHECK(jelly::ShortlistCapacity(jelly::kDefaultModels) == jelly::kCandidateShortlist);
    CHECK(jelly::ShortlistCapacity(jelly::kMaximumModelsLimit) >=
          jelly::kMaximumModelsLimit * 4U);
    CHECK(jelly::RootBudget(jelly::kDefaultModels) == 64U);
    CHECK(jelly::ClampModels(0.0) == jelly::kMinimumModels);
    CHECK(jelly::ClampModels(3.4) == 3U);
    CHECK(jelly::ClampModels(999.0) == jelly::kMaximumModelsLimit);
    CHECK(jelly::ClampModels(std::nan("")) == jelly::kDefaultModels);
    // The strength the user can dial: the ceiling was raised so a car can be pushed far
    // enough to read as jelly, and the clamp is what keeps the slider honest.
    CHECK(jelly::kMaximumAmplitude == 1.0);
    CHECK(jelly::ClampAmplitude(0.8) == 0.8);
    CHECK(jelly::ClampAmplitude(9.0) == jelly::kMaximumAmplitude);
    CHECK(jelly::ClampAmplitude(-1.0) == jelly::kMinimumAmplitude);
    // The rest between bounces and the spin speed, both of them the window's.
    CHECK(jelly::ClampPause(-1.0) == jelly::kMinimumPauseSeconds);
    CHECK(jelly::ClampPause(2.5) == 2.5);
    CHECK(jelly::ClampPause(99.0) == jelly::kMaximumPauseSeconds);
    CHECK(jelly::ClampSpin(-5.0) == jelly::kMinimumSpinDegreesPerSecond);
    CHECK(jelly::ClampSpin(300.0) == 300.0);
    CHECK(jelly::ClampSpin(5000.0) == jelly::kMaximumSpinDegreesPerSecond);
}

void TestLayout() {
    Section("layout resource: ships complete, records sources, rejects bad documents");
    const jelly::Layout& layout = ShippedLayout();
    CHECK(jelly::LayoutProblems(layout).empty());
    CHECK(layout.version == 1);
    CHECK(layout.game == "nte");
    CHECK(!layout.process_event_prologue.empty());
    CHECK(layout.offsets.size() > 30);
    for (const auto& entry : layout.offsets) {
        CHECK(!entry.second.type.empty());
        CHECK(!entry.second.source.empty());
    }
    for (const auto& entry : layout.properties) {
        CHECK(!entry.second.source.empty());
    }
    // The vehicle family is matched as a substring of the actor's class chain, so the
    // entry has to be a fragment rather than a whole class name.
    const auto vehicle = layout.class_families.find("vehicle");
    CHECK(vehicle != layout.class_families.end());
    if (vehicle != layout.class_families.end()) {
        CHECK(!vehicle->second.empty());
        for (const std::string& fragment : vehicle->second) CHECK(!fragment.empty());
    }
    CHECK(jelly::LayoutOffset(layout, "object.class", -1) == 16);
    CHECK(jelly::LayoutOffset(layout, "missing.key", -7) == -7);
    CHECK(jelly::LayoutName(layout, "scene_component") == "SceneComponent");

    jelly::Layout incomplete = layout;
    incomplete.offsets.erase("objects.items");
    const std::vector<std::string> missing = jelly::LayoutProblems(incomplete);
    CHECK(
        std::find(missing.begin(), missing.end(), "missing offset objects.items") !=
        missing.end());

    jelly::Layout unattributed = layout;
    unattributed.offsets["object.class"].source.clear();
    CHECK(!jelly::LayoutProblems(unattributed).empty());

    // A resource that does not name the vehicle family would silently treat every car as
    // an ordinary multi-part model again, so it is refused as incomplete instead.
    jelly::Layout no_vehicle = layout;
    no_vehicle.class_families.erase("vehicle");
    const std::vector<std::string> without_vehicle =
        jelly::LayoutProblems(no_vehicle);
    CHECK(
        std::find(
            without_vehicle.begin(), without_vehicle.end(),
            "missing the vehicle class family") != without_vehicle.end());

    // AttachParent is how the module tells a part of a vehicle from a model of its own, so
    // a resource without it has to be refused rather than silently skip that question.
    jelly::Layout no_parent = layout;
    no_parent.properties.erase("attach_parent");
    const std::vector<std::string> without_parent = jelly::LayoutProblems(no_parent);
    CHECK(
        std::find(
            without_parent.begin(), without_parent.end(),
            "missing property attach_parent") != without_parent.end());

    // The spin needs its own property and its own reflected setter, and a resource without
    // either is refused rather than quietly spinning nothing.
    jelly::Layout no_rotation = layout;
    no_rotation.properties.erase("relative_rotation");
    CHECK(!jelly::LayoutProblems(no_rotation).empty());
    jelly::Layout no_rotation_function = layout;
    no_rotation_function.functions.erase("set_relative_rotation");
    CHECK(!jelly::LayoutProblems(no_rotation_function).empty());
    const auto rotation_family = layout.functions.find("set_relative_rotation");
    CHECK(rotation_family != layout.functions.end() && !rotation_family->second.empty());
    if (rotation_family != layout.functions.end() && !rotation_family->second.empty()) {
        const jelly::FunctionSpec& spec = rotation_family->second.front();
        CHECK(spec.name == "K2_SetRelativeRotation");
        CHECK(spec.params.size() == 4 && spec.params.front().name == "NewRotation");
        CHECK(spec.parms_size == 297);
    }

    jelly::Layout parsed;
    std::string error;
    CHECK(!jelly::ParseLayout("{\"schema\":", parsed, error));
    CHECK(!jelly::ParseLayout(
        "{\"schema\":\"anomaly.plugin.jelly.layout\"} trailing", parsed, error));
    CHECK(!jelly::ParseLayout("[1,2,3]", parsed, error));
    CHECK(!jelly::ParseLayout("{}", parsed, error));
    CHECK(jelly::ParseLayout("{\"schema\":\"anomaly.plugin.jelly.layout\"}", parsed, error));
    CHECK(!jelly::LayoutProblems(parsed).empty());
}

void TestFramePolicy() {
    Section("selection: which snapshots are usable, and what the funnel drops");
    // STALE is the only thing that makes a snapshot unusable here. PARTIAL is set both
    // when the host could not read some actor and when its player-ESP feature is
    // unavailable; neither says anything about the actor list, so refusing partial
    // snapshots only ever hid the whole effect.
    CHECK(jelly::IsUsableSnapshot(jelly::kSnapshotValid));
    CHECK(jelly::IsUsableSnapshot(jelly::kSnapshotValid | jelly::kSnapshotPartial));
    CHECK(!jelly::IsUsableSnapshot(jelly::kSnapshotValid | jelly::kSnapshotStale));
    CHECK(!jelly::IsUsableSnapshot(jelly::kSnapshotStale));
    CHECK(!jelly::IsUsableSnapshot(0));
    CHECK(!jelly::IsUsableSnapshot(jelly::kSnapshotPartial));

    // An entry carries the entity's own flags *plus* the frame's, so the PARTIAL bit
    // has to be tolerated all the way down to the per-entry checks.
    CHECK(jelly::IsUsableSnapshot(
        jelly::kEntityFlagMovable | jelly::kSnapshotValid | jelly::kSnapshotPartial));

    jelly::Camera camera;
    camera.position = jelly::Vector3{0.0, 0.0, 0.0};
    camera.rotation = jelly::Vector3{0.0, 0.0, 0.0};
    camera.horizontal_fov_degrees = 90.0;
    const jelly::CameraBasis basis = jelly::MakeCameraBasis(camera);
    const auto box = [](const double x) {
        return jelly::Bounds{jelly::Vector3{x, 0.0, 0.0}, jelly::Vector3{50.0, 50.0, 50.0}};
    };

    // The funnel's only step here is the frustum: scope is settled later, per component,
    // from the component's own reflected Mobility.
    CHECK(jelly::Screen(basis, box(500.0)) == jelly::ScreenResult::candidate);
    CHECK(jelly::Screen(basis, box(-500.0)) == jelly::ScreenResult::culled);

    // The page contract a sweep that continues across frame generations depends on.
    CHECK(jelly::SanePage(jelly::PageResult{100, 64, 64}, 0, 64));
    CHECK(jelly::SanePage(jelly::PageResult{100, 36, 100}, 64, 64));
    // offset == total is the terminal empty page.
    CHECK(jelly::SanePage(jelly::PageResult{100, 0, 100}, 100, 64));
    // A frame that shrank below the offset ends the sweep there.
    CHECK(jelly::SanePage(jelly::PageResult{40, 0, 40}, 100, 64));
    // The host promises next_offset == offset + returned; a page that breaks it is not
    // allowed to move the sweep.
    CHECK(!jelly::SanePage(jelly::PageResult{100, 64, 100}, 0, 64));
    CHECK(!jelly::SanePage(jelly::PageResult{100, 64, 65}, 0, 64));
    CHECK(!jelly::SanePage(jelly::PageResult{100, 65, 65}, 0, 64));
    CHECK(!jelly::SanePage(jelly::PageResult{100, 0, 0}, 10, 64));
    CHECK(!jelly::SanePage(jelly::PageResult{100, 0, 100}, 0, 64));
}

void TestProcessEventAnchor() {
    Section("layout resource: the anchored ProcessEvent pattern drops the observer's bytes");
    const jelly::SymbolSpec& symbol = ShippedLayout().symbols.at("process_event");
    CHECK(symbol.detour_bytes == jelly::kProcessEventDetourBytes);

    // The bytes the observer overwrites are the first instruction of the function, so
    // the anchor starts on the push sequence that survives it.
    std::string anchored;
    CHECK(jelly::DropLeadingBytes(symbol.pattern, symbol.detour_bytes, anchored));
    CHECK(anchored.starts_with("54 41 55 41 56 41 57 "));
    // Exactly the five leading tokens are gone: "40 55 56 57 41 " is fifteen bytes of
    // pattern text.
    CHECK(anchored.size() + 15 == symbol.pattern.size());

    CHECK(!jelly::DropLeadingBytes(symbol.pattern, 0, anchored));
    CHECK(!jelly::DropLeadingBytes("48 8B", 2, anchored));
    CHECK(!jelly::DropLeadingBytes("?? ?? 48 8B", 1, anchored));

    // A resource that does not say how much of the entry the observer may have
    // written is refused, because nothing would then describe the bytes the entry
    // check would accept.
    jelly::Layout undeclared = ShippedLayout();
    undeclared.symbols["process_event"].detour_bytes = 0;
    const std::vector<std::string> problems = jelly::LayoutProblems(undeclared);
    CHECK(std::find(problems.begin(), problems.end(),
              "symbol process_event does not declare the supported detour size") !=
          problems.end());
}

}  // namespace

// The bounce with a rest after it, and the spin that keeps turning through that rest.
void TestPauseAndSpin() {
    Section("effect: the pause between bounces, and the spin it does not touch");
    const jelly::Vector3 base{1.4, 0.75, -1.1};
    const double frequency = 1.5;
    const double amplitude = 0.4;
    const double period = 1.0 / frequency;
    const double phase = 1.1;

    // A zero pause is the continuous wave: the same numbers, term for term.
    for (double elapsed = 0.0; elapsed < 3.0; elapsed += 0.013) {
        const jelly::Vector3 continuous =
            jelly::Output(base, amplitude, frequency, elapsed, phase);
        const jelly::Vector3 cyclic =
            jelly::OutputCyclic(base, amplitude, frequency, elapsed, phase, 0.0);
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(std::abs(continuous[axis] - cyclic[axis]) < 1e-9);
        }
    }

    // With a rest, the model is exactly at its base for the whole rest window, and the
    // wave resumes from zero, so nothing jumps.
    const double pause = 0.7;
    const double cycle = period + pause;
    const double offset = phase / jelly::kTau * period;
    bool saw_rest = false;
    for (double elapsed = 0.0; elapsed < 4.0 * cycle; elapsed += 0.005) {
        double into = std::fmod(elapsed + offset, cycle);
        if (into < 0.0) into += cycle;
        const jelly::Vector3 output =
            jelly::OutputCyclic(base, amplitude, frequency, elapsed, phase, pause);
        if (into >= period) {
            saw_rest = true;
            CHECK(jelly::Equal(output, base));
            continue;
        }
        // The wave is continuous: at either end of a bounce it is at the base as well.
        if (into < 0.02 || into > period - 0.02) CHECK(jelly::Equal(output, base, 0.1));
    }
    CHECK(saw_rest);

    // A longer rest cannot change what the wave does inside its own window.
    for (double elapsed = 0.0; elapsed < period; elapsed += 0.01) {
        const jelly::Vector3 short_rest =
            jelly::OutputCyclic(base, amplitude, frequency, elapsed, phase, 0.2);
        const jelly::Vector3 long_rest =
            jelly::OutputCyclic(base, amplitude, frequency, elapsed, phase, 3.0);
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(std::abs(short_rest[axis] - long_rest[axis]) < 1e-12);
        }
    }

    // The spin: bounded, continuous, and independent of the rest.
    const double speed = 90.0;
    double previous = jelly::SpinDegrees(speed, 0.0, phase);
    CHECK(previous >= 0.0 && previous < 360.0);
    for (double elapsed = 0.0; elapsed < 12.0; elapsed += 0.011) {
        const double angle = jelly::SpinDegrees(speed, elapsed, phase);
        CHECK(angle >= 0.0 && angle < 360.0);
        double step = angle - previous;
        if (step < -180.0) step += 360.0;  // the only discontinuity is the wrap
        CHECK(std::abs(step) < 5.0);
        previous = angle;
    }
    // While the model rests, the spin keeps turning.
    const double resting_at = period + 0.3;
    CHECK(jelly::Equal(
        jelly::OutputCyclic(base, amplitude, frequency, resting_at, phase, pause), base));
    CHECK(
        std::abs(
            jelly::SpinDegrees(speed, resting_at + 0.5, phase) -
            jelly::SpinDegrees(speed, resting_at, phase)) > 1.0);

    // The spin only takes the yaw: pitch and roll stay exactly as the model carried them.
    const jelly::Vector3 carried{12.0, 10.0, -3.0};
    const jelly::Vector3 spun = jelly::SpinRotator(carried, 90.0);
    CHECK(spun[0] == carried[0] && spun[2] == carried[2]);
    CHECK(spun[1] == 100.0);
    // A pose comparison tolerates the engine wrapping the yaw by whole turns.
    CHECK(jelly::SameRotation(jelly::Vector3{12.0, 370.0, -3.0}, carried));
    CHECK(jelly::SameRotation(jelly::Vector3{12.0, -10.0, -3.0}, jelly::Vector3{12.0, 350.0, -3.0}));
    CHECK(!jelly::SameRotation(jelly::Vector3{12.0, 90.0, -3.0}, carried));
    CHECK(!jelly::SameRotation(jelly::Vector3{12.5, 10.0, -3.0}, carried));
}

int RunLogicTests() {
    TestEffect();
    TestPauseAndSpin();
    TestSelection();
    TestShortlist();
    TestFramePolicy();
    TestLayout();
    TestProcessEventAnchor();
    return 0;
}

}  // namespace jelly_tests
