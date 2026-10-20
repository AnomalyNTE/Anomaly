#include "synthetic_ue.hpp"

#include "jelly_model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace jelly_tests {
namespace {

std::uint64_t ClassIdOf(SyntheticUe& image, const std::uintptr_t actor) {
    const std::uintptr_t klass = image.ClassOf(actor);
    return static_cast<std::uint64_t>(image.index_of(klass)) + 1U;
}

// How many notes carry a phrase. A note about a missing name may come first (the model
// module reports the key it could not find), so the order is never assumed.
std::size_t NotesLike(
    const std::vector<std::string>& notes, const std::string_view needle) {
    std::size_t count = 0;
    for (const std::string& note : notes) {
        if (note.find(needle) != std::string::npos) ++count;
    }
    return count;
}

void TestIdentity() {
    Section("model: registry identity, class agreement, serial reuse");
    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    CHECK(harness.model.ready());

    const ActorFixture actor =
        BuildSimpleActor(harness.image, "Actor", jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.Finalize();

    const auto frame_handle = static_cast<std::uint64_t>(actor.index) + 1U;
    const std::uint64_t class_id = ClassIdOf(harness.image, actor.actor);
    jelly::ActorIdentity identity;
    jelly::Skip skip = jelly::Skip::none;
    CHECK(harness.model.ResolveActor(frame_handle, class_id, identity, skip));
    CHECK(identity.valid);
    CHECK(identity.actor == actor.actor);
    CHECK(identity.handle() ==
          ((static_cast<std::uint64_t>(identity.serial) << 32U) | frame_handle));
    CHECK(harness.model.IdentityHeld(identity, skip));

    // A slot that now carries a different serial is a different object: the address
    // was reused and must never be written to again.
    harness.image.SetSerial(actor.index, 9999);
    CHECK(!harness.model.IdentityHeld(identity, skip));
    CHECK_SKIP(skip, jelly::Skip::identity_stale);
    CHECK(harness.call.calls == 0);

    jelly::ActorIdentity changed;
    CHECK(harness.model.ResolveActor(frame_handle, class_id, changed, skip));
    CHECK(!changed.SameObjectAs(identity));

    // The class the frame names has to be the class the slot holds.
    CHECK(!harness.model.ResolveActor(frame_handle, class_id + 4242U, changed, skip));
    CHECK_SKIP(skip, jelly::Skip::class_mismatch);

    // An empty handle and an out-of-range slot are refused without touching memory.
    CHECK(!harness.model.ResolveActor(0, 0, changed, skip));
    CHECK_SKIP(skip, jelly::Skip::no_identity);
    CHECK(!harness.model.ResolveActor(9999999, 0, changed, skip));
    CHECK_SKIP(skip, jelly::Skip::no_registry);
}

void TestVisualSelection() {
    Section("model: visual root, base scale, nested and unsupported parts");
    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());

    const ActorFixture actor =
        BuildSimpleActor(harness.image, "Hero", jelly::Vector3{1.2, 0.9, 1.05});
    // A weapon attached under the mesh follows its parent and is not a second root.
    const std::uintptr_t weapon =
        harness.image.MakeComponent("Hero_Weapon", actor.actor, jelly::Vector3{2.0, 2.0, 2.0});
    harness.image.Attach(actor.mesh, {weapon});
    harness.image.Finalize();

    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;
    CHECK(harness.model.ResolveVisual(actor.actor, false, target, skip));
    std::fflush(stdout);
    CHECK(target.component == actor.mesh);
    CHECK(jelly::Equal(target.base, jelly::Vector3{1.2, 0.9, 1.05}));

    // Two topmost meshes cannot be scaled apart, so the whole model is skipped.
    Harness multipart(ShippedLayout());
    CHECK(multipart.Prepare());
    const ActorFixture car =
        BuildSimpleActor(multipart.image, "Car", jelly::Vector3{1.0, 1.0, 1.0});
    // The actor's root is a plain scene node with two independent meshes under it,
    // which is the shape the single-root rule cannot handle.
    const std::uintptr_t car_root = multipart.image.MakePlainComponent("Car_Root", car.actor);
    multipart.image.SetRoot(car.actor, car_root);
    const std::uintptr_t body = multipart.image.MakeComponent(
        "Car_Body", car.actor, jelly::Vector3{1.0, 1.0, 1.0});
    const std::uintptr_t wheel = multipart.image.MakeComponent(
        "Car_Wheel", car.actor, jelly::Vector3{1.0, 1.0, 1.0});
    multipart.image.Attach(car_root, {body, wheel});
    multipart.image.Finalize();
    CHECK(!multipart.model.ResolveVisual(car.actor, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::multi_part);

    // A mesh that belongs to another actor is never this actor's model: it is left
    // where it is, and when it is the only candidate the model is reported as such.
    Harness attached(ShippedLayout());
    CHECK(attached.Prepare());
    const ActorFixture guest =
        BuildSimpleActor(attached.image, "Guest", jelly::Vector3{1.0, 1.0, 1.0});
    const std::uintptr_t host = attached.image.MakeActor("Host");
    const std::uintptr_t host_root = attached.image.MakePlainComponent("Host_Root", host);
    attached.image.SetRoot(host, host_root);
    attached.image.Attach(host_root, {guest.mesh});
    attached.image.Finalize();
    CHECK(!attached.model.ResolveVisual(host, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::cross_actor);
    // The guest still resolves through its own root.
    CHECK(attached.model.ResolveVisual(guest.actor, false, target, skip));
    CHECK(target.component == guest.mesh);

    // An actor without a root component has nothing to scale.
    Harness rootless(ShippedLayout());
    CHECK(rootless.Prepare());
    const std::uintptr_t lonely = rootless.image.MakeActor("Lonely");
    rootless.image.Finalize();
    CHECK(!rootless.model.ResolveVisual(lonely, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::no_root);

    // A root that is not a mesh at all is not a visual model.
    Harness plain(ShippedLayout());
    CHECK(plain.Prepare());
    const std::uintptr_t plain_actor = plain.image.MakeActor("Plain");
    const std::uintptr_t scene = plain.image.MakePlainComponent("Plain_Scene", plain_actor);
    plain.image.SetRoot(plain_actor, scene);
    plain.image.Finalize();
    CHECK(!plain.model.ResolveVisual(plain_actor, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::no_visual);
}

void TestStateChecks() {
    Section("model: invisible, colliding, physics-simulated and instanced meshes");
    const auto expect_skip = [](const bool visible, const std::uint8_t collision,
                                const bool simulating, const bool instanced,
                                const jelly::Skip expected) {
        Harness harness(ShippedLayout());
        if (!harness.Prepare()) return;
        const std::uintptr_t actor = harness.image.MakeActor("Target");
        const std::uintptr_t mesh = harness.image.MakeComponent(
            "Target_Mesh", actor, jelly::Vector3{1.0, 1.0, 1.0}, visible, collision,
            simulating, instanced ? harness.image.MakeInstancedClass() : 0);
        harness.image.SetRoot(actor, mesh);
        harness.image.Finalize();
        jelly::VisualTarget target;
        jelly::Skip skip = jelly::Skip::none;
        CHECK(!harness.model.ResolveVisual(actor, false, target, skip));
        CHECK_SKIP(skip, expected);
    };
    expect_skip(false, 0, false, false, jelly::Skip::invisible);
    expect_skip(true, 1, false, false, jelly::Skip::collision);
    expect_skip(true, 0, true, false, jelly::Skip::physics);
    expect_skip(true, 0, false, true, jelly::Skip::instanced);
}

void TestWrite() {
    Section("model: reflected write, read-back verification, signature validation");
    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const ActorFixture actor =
        BuildSimpleActor(harness.image, "Body", jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.Finalize();

    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;
    CHECK(harness.model.ResolveVisual(actor.actor, false, target, skip));

    const jelly::Vector3 wanted{1.1, 1.1, 0.8};
    CHECK(harness.model.ApplyScale(target.component, wanted, skip));
    CHECK(harness.call.calls == 1);
    CHECK(harness.call.last_parms_size == 24);
    CHECK(jelly::Equal(harness.image.ScaleOf(target.component), wanted));
    jelly::Vector3 live{};
    CHECK(harness.model.ReadScale(target.component, live, skip));
    CHECK(jelly::Equal(live, wanted));

    // A write the engine does not commit is not reported as a success.
    harness.call.engine_writes = false;
    CHECK(!harness.model.ApplyScale(target.component, jelly::Vector3{2.0, 2.0, 0.5}, skip));
    CHECK_SKIP(skip, jelly::Skip::not_verified);
    CHECK(jelly::Equal(harness.image.ScaleOf(target.component), wanted));

    // A build whose reflected write takes a float FVector is refused before a call
    // is made, and the component is left exactly as it was.
    SyntheticUe::Options options;
    options.scalar_float_scale = true;
    Harness scalar(ShippedLayout(), options);
    CHECK(scalar.Prepare());
    const ActorFixture scalar_actor =
        BuildSimpleActor(scalar.image, "Scalar", jelly::Vector3{1.0, 1.0, 1.0});
    scalar.image.Finalize();
    CHECK(!scalar.model.ResolveVisual(scalar_actor.actor, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::signature_mismatch);
    CHECK(scalar.call.calls == 0);
    CHECK(jelly::Equal(
        scalar.image.ScaleOf(scalar_actor.mesh), jelly::Vector3{1.0, 1.0, 1.0}));

    // A build in which the reflected function is not registered at all is refused
    // as a missing function rather than as a bad signature.
    SyntheticUe::Options absent;
    absent.omit_write_function = true;
    Harness missing(ShippedLayout(), absent);
    CHECK(missing.Prepare());
    const ActorFixture plain =
        BuildSimpleActor(missing.image, "NoFunction", jelly::Vector3{1.0, 1.0, 1.0});
    missing.image.Finalize();
    CHECK(!missing.model.ResolveVisual(plain.actor, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::function_missing);
    CHECK(missing.call.calls == 0);
}

void TestCallEntry() {
    Section("model: the ProcessEvent entry has to carry the recorded prologue");
    Harness harness(ShippedLayout());
    static_cast<void>(
        BuildSimpleActor(harness.image, "Entry", jelly::Vector3{1.0, 1.0, 1.0}));
    harness.image.Finalize();
    CHECK(harness.Prepare());
    harness.image.CorruptProcessEventEntry();
    jelly::Skip skip = jelly::Skip::none;
    CHECK(!harness.model.Prepare(
        harness.image.registry_address(), harness.image.process_event_address(), skip));
    CHECK_SKIP(skip, jelly::Skip::call_entry_missing);
    CHECK(!harness.model.ready());
}

void TestCallEntryBehindTheObserverDetour() {
    Section("model: the entry is accepted behind the observer's five byte detour");
    // Anomaly's own process-wide observer hot-loads before a plugin does, so the
    // entry a plugin reads there is that jump with the same prologue behind it.
    Harness harness(ShippedLayout());
    harness.image.InstallProcessEventDetour();
    CHECK(harness.Prepare());

    // A jump that lands outside any mapped page is refused rather than called.
    Harness unreadable(ShippedLayout());
    unreadable.image.InstallProcessEventDetour(false);
    jelly::Skip skip = jelly::Skip::none;
    CHECK(!unreadable.model.Prepare(
        unreadable.image.registry_address(), unreadable.image.process_event_address(), skip));
    CHECK_SKIP(skip, jelly::Skip::call_entry_missing);
    CHECK(!unreadable.model.ready());

    // A jump over a body that is no longer the recorded prologue is refused too.
    Harness corrupted(ShippedLayout());
    corrupted.image.InstallProcessEventDetour();
    corrupted.image.CorruptProcessEventDetourTail();
    CHECK(!corrupted.model.Prepare(
        corrupted.image.registry_address(), corrupted.image.process_event_address(), skip));
    CHECK_SKIP(skip, jelly::Skip::call_entry_missing);
}

void TestConflict() {
    Section("model: another writer on the component stops the effect for that target");
    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const ActorFixture actor =
        BuildSimpleActor(harness.image, "Conflict", jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.Finalize();
    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;
    CHECK(harness.model.ResolveVisual(actor.actor, false, target, skip));

    const jelly::Vector3 applied{1.1, 1.1, 0.83};
    CHECK(harness.model.ApplyScale(target.component, applied, skip));
    // Something else rewrites the component. It no longer carries what the plugin
    // wrote, so the plugin stops owning it instead of overwriting or restoring.
    const jelly::Vector3 foreign{3.0, 3.0, 3.0};
    harness.image.SetScale(target.component, foreign);
    jelly::Vector3 live{};
    CHECK(harness.model.ReadScale(target.component, live, skip));
    CHECK(!jelly::Equal(live, applied));
    CHECK(jelly::Equal(harness.image.ScaleOf(target.component), foreign));

    // The identity check still holds: the object is alive, it is the value that
    // belongs to someone else.
    jelly::ActorIdentity identity;
    const std::uint64_t class_id = ClassIdOf(harness.image, actor.actor);
    CHECK(harness.model.ResolveActor(
        static_cast<std::uint64_t>(actor.index) + 1U, class_id, identity, skip));
}

void TestRequirements() {
    Section("model: an incomplete resource or an unusable registry keeps the module cold");
    jelly::Layout broken = ShippedLayout();
    broken.properties.erase("relative_scale");
    Harness harness(broken);
    jelly::Skip skip = jelly::Skip::none;
    CHECK(!harness.model.Prepare(
        harness.image.registry_address(), harness.image.process_event_address(), skip));
    CHECK_SKIP(skip, jelly::Skip::layout_unavailable);
    CHECK(!harness.model.ready());

    // A registry header that does not add up is refused rather than walked.
    Harness registry(ShippedLayout());
    CHECK(!registry.model.Prepare(0x10, 0x20, skip));
    CHECK_SKIP(skip, jelly::Skip::no_registry);
}

void TestMovementScope() {
    Section("model: static scenery is skipped, and a build without Mobility still runs");
    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;

    // EComponentMobility::Static is the level's scenery: the component is refused, and
    // the window says why rather than reporting a healthy run that scaled a building.
    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const ActorFixture scenery =
        BuildSimpleActor(harness.image, "Scenery", jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.SetMobility(scenery.mesh, 0);
    harness.image.Finalize();
    CHECK(!harness.model.ResolveVisual(scenery.actor, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::static_mesh);
    CHECK(harness.call.calls == 0);
    CHECK(harness.model.ready());

    // Stationary and Movable are both things the game moves, so they stay in scope.
    Harness stationary(ShippedLayout());
    CHECK(stationary.Prepare());
    const ActorFixture door =
        BuildSimpleActor(stationary.image, "Door", jelly::Vector3{1.0, 1.0, 1.0});
    stationary.image.SetMobility(door.mesh, 1);
    stationary.image.Finalize();
    CHECK(stationary.model.ResolveVisual(door.actor, false, target, skip));
    CHECK(stationary.model.ApplyScale(target.component, jelly::Vector3{1.1, 1.1, 0.83}, skip));
    CHECK(stationary.call.calls == 1);

    // A build that does not reflect Mobility cannot tell scenery from a dynamic model.
    // That is a scope question, not a safety one, so the target is allowed and the class
    // is named once in the log instead of silently leaving the whole effect off.
    SyntheticUe::Options unfiltered;
    unfiltered.omit_mobility_property = true;
    Harness fallback(ShippedLayout(), unfiltered);
    CHECK(fallback.Prepare());
    const ActorFixture anything =
        BuildSimpleActor(fallback.image, "Anything", jelly::Vector3{1.0, 1.0, 1.0});
    fallback.image.Finalize();
    CHECK(fallback.model.ResolveVisual(anything.actor, false, target, skip));
    CHECK(fallback.model.ApplyScale(target.component, jelly::Vector3{1.2, 1.2, 0.69}, skip));
    CHECK(fallback.call.calls == 1);
    CHECK(!fallback.log.notes.empty());
    CHECK(fallback.log.notes.front().find("Mobility") != std::string::npos);
    // Said once per class, not once per candidate.
    CHECK(fallback.log.notes.size() == 1);
}

void TestNames() {
    Section("model: names are matched as text, not looked up in the name pool");
    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;

    // A build whose mesh class carries another name is not a resource problem: the
    // module stays ready and the models of that class are refused, untouched.
    SyntheticUe::Options renamed_class;
    renamed_class.rename_mesh_component_class = true;
    Harness renamed(ShippedLayout(), renamed_class);
    CHECK(renamed.Prepare());
    CHECK(renamed.model.ready());
    const ActorFixture renamed_actor =
        BuildSimpleActor(renamed.image, "RenamedMesh", jelly::Vector3{1.0, 1.0, 1.0});
    renamed.image.Finalize();
    CHECK(!renamed.model.ResolveVisual(renamed_actor.actor, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::no_visual);
    CHECK(renamed.call.calls == 0);

    // A property this build names differently is refused for the model that needed
    // it, and the note says which name was missing.
    SyntheticUe::Options renamed_property;
    renamed_property.rename_body_instance = true;
    Harness property(ShippedLayout(), renamed_property);
    CHECK(property.Prepare());
    const ActorFixture body =
        BuildSimpleActor(property.image, "RenamedBody", jelly::Vector3{1.0, 1.0, 1.0});
    property.image.Finalize();
    CHECK(!property.model.ResolveVisual(body.actor, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::collision);
    CHECK(property.call.calls == 0);
    CHECK(!property.log.notes.empty());
    CHECK(property.log.notes.front().find("body_instance") != std::string::npos);

    // The resource records the spelling the build's reflection carries, and UE's own
    // FName comparison ignores case, so a case difference still matches.
    jelly::Layout folded_layout = ShippedLayout();
    folded_layout.properties["relative_scale"].name = "relativescale3d";
    Harness folded(folded_layout);
    CHECK(folded.Prepare());
    const ActorFixture folded_actor =
        BuildSimpleActor(folded.image, "CaseFolded", jelly::Vector3{1.0, 1.0, 1.0});
    folded.image.Finalize();
    CHECK(folded.model.ResolveVisual(folded_actor.actor, false, target, skip));
    CHECK(folded.model.ApplyScale(target.component, jelly::Vector3{1.2, 1.2, 0.69}, skip));
    CHECK(folded.call.calls == 1);
}

// The three party members that stand next to the local player carry AActor::bHidden: the
// game is not rendering them, and they used to take three of the model slots.
void TestHiddenActors() {
    Section("model: a hidden actor is not a target, and a build without bHidden still runs");
    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;

    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const ActorFixture party =
        BuildSimpleActor(harness.image, "Party", jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.SetHidden(party.actor, true);
    harness.image.Finalize();
    CHECK(!harness.model.ResolveVisual(party.actor, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::hidden);
    CHECK(harness.call.calls == 0);

    // The same actor is a target again the moment the game shows it.
    harness.image.SetHidden(party.actor, false);
    CHECK(harness.model.ResolveVisual(party.actor, false, target, skip));
    CHECK(target.component == party.mesh);

    // A build that does not reflect bHidden cannot tell a hidden actor from a visible one.
    // That is a scope question, not a safety one: the model is allowed and the class is
    // named once in the log instead of the effect quietly switching itself off.
    SyntheticUe::Options unfiltered;
    unfiltered.omit_hidden_property = true;
    Harness fallback(ShippedLayout(), unfiltered);
    CHECK(fallback.Prepare());
    const ActorFixture anything =
        BuildSimpleActor(fallback.image, "Anything", jelly::Vector3{1.0, 1.0, 1.0});
    fallback.image.Finalize();
    CHECK(fallback.model.ResolveVisual(anything.actor, false, target, skip));
    CHECK(!fallback.log.notes.empty());
    CHECK(fallback.log.notes.front().find("bHidden") != std::string::npos);
    CHECK(fallback.log.notes.size() == 1);  // said once per class, not per candidate
}

// The shape the crowd NPCs have: the capsule carries the body mesh and, next to it, a
// name-plate bubble. The bubble is a UMeshComponent, so it used to count as a second
// visual root and refused every one of those NPCs as multi-part.
void TestNonVisualMesh() {
    Section("model: a UI mesh in the world is not a second visual root");
    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;

    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const std::uintptr_t actor =
        harness.image.MakeActor("BP_NPC_Mass_fm_Sub_C", harness.image.ActorClass());
    const std::uintptr_t root = harness.image.MakePlainComponent("Npc_Capsule", actor);
    harness.image.SetRoot(actor, root);
    const std::uintptr_t body =
        harness.image.MakeComponent("Npc_Body", actor, jelly::Vector3{1.0, 1.0, 1.0});
    const std::uintptr_t plate = harness.image.MakeComponent(
        "Npc_Plate", actor, jelly::Vector3{1.0, 1.0, 1.0}, true, 0, false,
        harness.image.MakeWidgetClass());
    harness.image.Attach(root, {body, plate});
    harness.image.Finalize();

    CHECK(harness.model.ResolveVisual(actor, false, target, skip));
    CHECK(target.component == body);
    CHECK(harness.model.ApplyScale(target.component, jelly::Vector3{1.1, 1.1, 0.83}, skip));
    CHECK(harness.call.calls == 1);

    // An actor whose only mesh is the UI one has no model at all, and says so.
    Harness bare(ShippedLayout());
    CHECK(bare.Prepare());
    const std::uintptr_t lonely =
        bare.image.MakeActor("PlateOnly", bare.image.ActorClass());
    const std::uintptr_t lonely_root =
        bare.image.MakePlainComponent("PlateOnly_Root", lonely);
    bare.image.SetRoot(lonely, lonely_root);
    const std::uintptr_t bubble = bare.image.MakeComponent(
        "PlateOnly_Bubble", lonely, jelly::Vector3{1.0, 1.0, 1.0}, true, 0, false,
        bare.image.MakeWidgetClass());
    bare.image.Attach(lonely_root, {bubble});
    bare.image.Finalize();
    CHECK(!bare.model.ResolveVisual(lonely, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::no_visual);
}

// The traffic cars of this build, measured read-only in the live process rather than
// guessed: the actor's root is a plain scene node (on some classes the body mesh itself),
// the body, the four wheels and a name-plate bubble are siblings under it, the body
// carries CollisionEnabled = 3 and the actor carries AActor::bHidden although the game
// renders the car. Each of those facts used to refuse the whole model on its own -
// hidden, collision, multi_part - which is why no car ever moved.
void TestVehicleFamily() {
    Section("model: a vehicle is one model, scaled from its root with its collision");
    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;

    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const std::uintptr_t actor = harness.image.MakeActor(
        "BP_Vehicle_002_ModuleTraffic_C", harness.image.ActorClass());
    harness.image.SetHidden(actor, true);
    const std::uintptr_t root =
        harness.image.MakePlainComponent("VehicleRootComponent", actor);
    harness.image.SetScale(root, jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.SetRoot(actor, root);
    const std::uintptr_t body = harness.image.MakeComponent(
        "VehicleMesh", actor, jelly::Vector3{1.0, 1.0, 1.0}, true, 3);
    const std::uintptr_t wheel = harness.image.MakeComponent(
        "Wheel_FL", actor, jelly::Vector3{1.0, 1.0, 1.0}, true, 1);
    const std::uintptr_t plate = harness.image.MakeComponent(
        "BPC_VehicleNamePlate", actor, jelly::Vector3{1.0, 1.0, 1.0}, true, 0, false,
        harness.image.MakeWidgetClass());
    harness.image.Attach(root, {body, wheel, plate});
    harness.image.Finalize();

    // One target for the whole car: the root, not the body and certainly not four
    // sibling parts that would move apart if each were scaled on its own.
    CHECK(harness.model.ResolveVisual(actor, false, target, skip));
    CHECK(target.component == root);
    CHECK(jelly::Equal(target.base, jelly::Vector3{1.0, 1.0, 1.0}));
    CHECK(harness.model.ApplyScale(target.component, jelly::Vector3{1.1, 1.1, 0.83}, skip));
    CHECK(harness.call.calls == 1);
    // The parts follow the root through the component hierarchy; nothing writes to them.
    CHECK(jelly::Equal(harness.image.ScaleOf(body), jelly::Vector3{1.0, 1.0, 1.0}));
    CHECK(jelly::Equal(harness.image.ScaleOf(wheel), jelly::Vector3{1.0, 1.0, 1.0}));

    // A part that simulates physics is the one thing the family still refuses: scaling
    // the root would scale that body too, so nothing is written.
    Harness physics(ShippedLayout());
    CHECK(physics.Prepare());
    const std::uintptr_t driving = physics.image.MakeActor(
        "BP_Vehicle_007a_Hight_C", physics.image.ActorClass());
    const std::uintptr_t driving_root =
        physics.image.MakePlainComponent("HightRoot", driving);
    physics.image.SetScale(driving_root, jelly::Vector3{1.0, 1.0, 1.0});
    physics.image.SetRoot(driving, driving_root);
    const std::uintptr_t shell = physics.image.MakeComponent(
        "HightBody", driving, jelly::Vector3{1.0, 1.0, 1.0}, true, 0, true);
    physics.image.Attach(driving_root, {shell});
    physics.image.Finalize();
    CHECK(!physics.model.ResolveVisual(driving, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::physics);
    CHECK(physics.call.calls == 0);

    // A vehicle rendered only by an instanced mesh has no part of its own to scale, and
    // says so; an instanced part next to a real mesh is the vehicle's own and follows the
    // root, which is why the tracked vehicle still moves.
    Harness instanced(ShippedLayout());
    CHECK(instanced.Prepare());
    const std::uintptr_t batched = instanced.image.MakeActor(
        "BP_Vehicle_009b_Low_C", instanced.image.ActorClass());
    const std::uintptr_t batched_root =
        instanced.image.MakePlainComponent("LowRoot", batched);
    instanced.image.SetRoot(batched, batched_root);
    const std::uintptr_t batch = instanced.image.MakeComponent(
        "WheelBatch", batched, jelly::Vector3{1.0, 1.0, 1.0}, true, 0, false,
        instanced.image.MakeInstancedClass());
    instanced.image.Attach(batched_root, {batch});
    instanced.image.Finalize();
    CHECK(!instanced.model.ResolveVisual(batched, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::instanced);

    // Level scenery is not a vehicle: the identical shape without the family name keeps
    // the old answer, so this relaxation is not a global one.
    Harness scenery(ShippedLayout());
    CHECK(scenery.Prepare());
    const std::uintptr_t prop = scenery.image.MakeActor(
        "BP_SeatActor_SM_Parasol_Chair_02_C", scenery.image.ActorClass());
    const std::uintptr_t prop_root =
        scenery.image.MakePlainComponent("ChairRoot", prop);
    scenery.image.SetRoot(prop, prop_root);
    const std::uintptr_t prop_body = scenery.image.MakeComponent(
        "Chair_Body", prop, jelly::Vector3{1.0, 1.0, 1.0}, true, 3);
    const std::uintptr_t prop_leg = scenery.image.MakeComponent(
        "Chair_Leg", prop, jelly::Vector3{1.0, 1.0, 1.0}, true, 1);
    const std::uintptr_t prop_plate = scenery.image.MakeComponent(
        "Chair_Plate", prop, jelly::Vector3{1.0, 1.0, 1.0}, true, 0, false,
        scenery.image.MakeWidgetClass());
    scenery.image.Attach(prop_root, {prop_body, prop_leg, prop_plate});
    scenery.image.Finalize();
    CHECK(!scenery.model.ResolveVisual(prop, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::collision);
    CHECK(scenery.call.calls == 0);

    // The family question is answered from the class chain, not from one spelling.
    // Each check asks the module that owns the image: two fixtures allocate at the same
    // addresses, and the module caches its answer per class pointer.
    CHECK(harness.model.ClassIsVehicle(harness.image.ClassOf(actor)));
    CHECK(!scenery.model.ClassIsVehicle(scenery.image.ClassOf(prop)));
    CHECK(!harness.model.ClassIsVehicle(0));
}

// The car the player drives carries its chassis rigid body on the body mesh: measured on
// the live player, BP_Vehicle_026_Module_C / VehicleMesh has bSimulatePhysics = true. The
// window's switch decides whether such a vehicle is scaled as a whole (its physics body
// scales with it) or refused exactly as before; another family is never relaxed.
void TestSimulatedVehicleBody() {
    Section("model: a physics-driven vehicle is scaled only while the switch says so");
    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;

    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const std::uintptr_t actor = harness.image.MakeActor(
        "BP_Vehicle_026_Module_C", harness.image.ActorClass());
    const std::uintptr_t root =
        harness.image.MakePlainComponent("VehicleRootComponent", actor);
    harness.image.SetScale(root, jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.SetRoot(actor, root);
    const std::uintptr_t body = harness.image.MakeComponent(
        "VehicleMesh", actor, jelly::Vector3{1.0, 1.0, 1.0}, true, 3, true);
    harness.image.Attach(root, {body});
    harness.image.Finalize();

    // The switch on: the whole car is the target, and the class is named once in the log
    // so the trade that was made is visible.
    CHECK(harness.model.ResolveVisual(actor, true, target, skip));
    CHECK(target.component == root);
    CHECK(jelly::Equal(target.base, jelly::Vector3{1.0, 1.0, 1.0}));
    CHECK(harness.model.ApplyScale(target.component, jelly::Vector3{1.1, 1.1, 0.83}, skip));
    CHECK(harness.call.calls == 1);
    CHECK(!harness.log.notes.empty());
    CHECK(harness.log.notes.front().find("simulating body") != std::string::npos);
    CHECK(harness.log.notes.size() == 1);  // said once per class, not per update

    // The same car with the switch off is refused exactly as it was before, and nothing
    // is written to a body that is simulating.
    Harness refused(ShippedLayout());
    CHECK(refused.Prepare());
    const std::uintptr_t car = refused.image.MakeActor(
        "BP_Vehicle_026_Module_C", refused.image.ActorClass());
    const std::uintptr_t car_root = refused.image.MakePlainComponent("CarRoot", car);
    refused.image.SetRoot(car, car_root);
    const std::uintptr_t chassis = refused.image.MakeComponent(
        "VehicleMesh", car, jelly::Vector3{1.0, 1.0, 1.0}, true, 3, true);
    refused.image.Attach(car_root, {chassis});
    refused.image.Finalize();
    CHECK(!refused.model.ResolveVisual(car, false, target, skip));
    CHECK_SKIP(skip, jelly::Skip::physics);
    CHECK(refused.call.calls == 0);

    // The switch is about vehicles: a prop whose mesh simulates physics stays refused.
    Harness prop_harness(ShippedLayout());
    CHECK(prop_harness.Prepare());
    const std::uintptr_t prop = prop_harness.image.MakeActor(
        "BP_CabinetKey_CityC_C", prop_harness.image.ActorClass());
    const std::uintptr_t prop_root = prop_harness.image.MakePlainComponent("KeyRoot", prop);
    prop_harness.image.SetRoot(prop, prop_root);
    const std::uintptr_t key = prop_harness.image.MakeComponent(
        "KeyMesh", prop, jelly::Vector3{1.0, 1.0, 1.0}, true, 0, true);
    prop_harness.image.Attach(prop_root, {key});
    prop_harness.image.Finalize();
    CHECK(!prop_harness.model.ResolveVisual(prop, true, target, skip));
    CHECK_SKIP(skip, jelly::Skip::physics);
    CHECK(prop_harness.call.calls == 0);
}

// The spoiler and lamp actors of this build are separate actors whose root component hangs
// under the vehicle that owns them. They follow the vehicle as it is scaled, so scaling
// them on their own is what made a car whose body was skipped look as if only its
// accessories moved. They are refused; an actor attached to something that is not a
// vehicle keeps behaving like any other model.
void TestVehicleFollowers() {
    Section("model: a part of a vehicle follows it instead of taking a model slot");
    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;

    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const std::uintptr_t car = harness.image.MakeActor(
        "BP_Vehicle_002_ModuleTraffic_C", harness.image.ActorClass());
    const std::uintptr_t car_root =
        harness.image.MakePlainComponent("VehicleRootComponent", car);
    harness.image.SetScale(car_root, jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.SetRoot(car, car_root);
    const std::uintptr_t car_body = harness.image.MakeComponent(
        "VehicleMesh", car, jelly::Vector3{1.0, 1.0, 1.0}, true, 3);
    harness.image.Attach(car_root, {car_body});
    // The spoiler actor: its own actor, its own single mesh, attached under the car.
    const std::uintptr_t spoiler = harness.image.MakeActor(
        "Vehicle002_Spoiler_05_C", harness.image.ActorClass());
    const std::uintptr_t spoiler_mesh = harness.image.MakeComponent(
        "SkeletalSpoilerMesh", spoiler, jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.SetRoot(spoiler, spoiler_mesh);
    harness.image.SetAttachParent(spoiler_mesh, car_body);
    harness.image.Finalize();

    // The car still resolves as one model, and the spoiler is refused rather than scaled
    // on its own: it moves because the car moves.
    CHECK(harness.model.ResolveVisual(car, true, target, skip));
    CHECK(target.component == car_root);
    CHECK(!harness.model.ResolveVisual(spoiler, true, target, skip));
    CHECK_SKIP(skip, jelly::Skip::vehicle_follower);
    CHECK(harness.call.calls == 0);

    // The same shape under an actor that is not a vehicle is an ordinary model, so the rule
    // is scoped to the vehicle family.
    Harness prop_harness(ShippedLayout());
    CHECK(prop_harness.Prepare());
    const std::uintptr_t bench = prop_harness.image.MakeActor(
        "BP_SeatActor_SM_Parasol_Chair_02_C", prop_harness.image.ActorClass());
    const std::uintptr_t bench_root =
        prop_harness.image.MakePlainComponent("ChairRoot", bench);
    prop_harness.image.SetScale(bench_root, jelly::Vector3{1.0, 1.0, 1.0});
    prop_harness.image.SetRoot(bench, bench_root);
    const std::uintptr_t bench_body = prop_harness.image.MakeComponent(
        "Chair_Body", bench, jelly::Vector3{1.0, 1.0, 1.0});
    prop_harness.image.Attach(bench_root, {bench_body});
    const std::uintptr_t guest = prop_harness.image.MakeActor(
        "GuestActor", prop_harness.image.ActorClass());
    const std::uintptr_t guest_mesh = prop_harness.image.MakeComponent(
        "Guest_Mesh", guest, jelly::Vector3{1.0, 1.0, 1.0});
    prop_harness.image.SetRoot(guest, guest_mesh);
    prop_harness.image.SetAttachParent(guest_mesh, bench_root);
    prop_harness.image.Finalize();
    CHECK(prop_harness.model.ResolveVisual(guest, true, target, skip));
    CHECK(target.component == guest_mesh);
}

// The spin: the rotation is read at takeover, written from that base on every update, and
// put back when the model leaves. A component whose orientation the game drives itself - or
// a build that does not carry the rotation function - stops spinning and keeps the jelly.
void TestSpin() {
    Section("model: the rotation is read, written from the base, and handed back");
    jelly::VisualTarget target;
    jelly::Skip skip = jelly::Skip::none;

    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const ActorFixture actor =
        BuildSimpleActor(harness.image, "Spin", jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.SetRotation(actor.mesh, jelly::Vector3{5.0, 40.0, -2.0});
    harness.image.Finalize();
    CHECK(harness.model.ResolveVisual(actor.actor, false, target, skip));
    CHECK(target.component == actor.mesh);
    CHECK(target.rotation_valid);
    CHECK(jelly::Equal(target.rotation, jelly::Vector3{5.0, 40.0, -2.0}));

    // The record is built the way the effect pass builds it: the module's identity first,
    // then the target it resolved.
    const std::uint64_t class_id = ClassIdOf(harness.image, actor.actor);
    const std::uint64_t model_key = static_cast<std::uint64_t>(actor.index) + 1U;
    jelly::ActorIdentity identity;
    CHECK(harness.model.ResolveActor(model_key, class_id, identity, skip));

    jelly::Settings settings;
    settings.amplitude = 0.0;  // hold the scale at its base to watch only the rotation
    settings.spin = true;
    settings.spin_degrees_per_second = 90.0;
    jelly::RecoveryLedger ledger;
    jelly::RecoveryRecord record;
    record.model_key = model_key;
    record.identity = identity;
    record.component = target.component;
    record.base = target.base;
    record.applied = target.base;
    record.base_rotation = target.rotation;
    record.applied_rotation = target.rotation;
    record.rotation_valid = true;
    record.phase = 0.0;
    record.start_time = 0.0;
    CHECK(ledger.Add(record, jelly::kDefaultModels));
    jelly::RecoveryRecord* held = ledger.Find(record.model_key);
    CHECK(held != nullptr);

    // One update writes the rotation once and leaves the scale alone.
    const int before = harness.call.calls;
    CHECK(
        jelly::StepRecord(harness.model, *held, settings, 1.0, skip) ==
        jelly::EffectStep::updated);
    CHECK(harness.call.calls == before + 1);
    CHECK(held->rotation_written);
    const jelly::Vector3 spun = harness.image.RotationOf(actor.mesh);
    CHECK(std::abs(spun[1] - 130.0) < 1e-6);  // 40 degrees carried + 90 per second
    CHECK(spun[0] == 5.0 && spun[2] == -2.0);
    // The same instant again is already there, so nothing is written a second time.
    CHECK(
        jelly::StepRecord(harness.model, *held, settings, 1.0, skip) ==
        jelly::EffectStep::unchanged);

    // A model whose rotation the game takes back stops spinning and says why, once.
    harness.call.rotation_sticks = false;
    CHECK(
        jelly::StepRecord(harness.model, *held, settings, 1.5, skip) ==
        jelly::EffectStep::unchanged);
    CHECK(held->spin_blocked);
    CHECK(NotesLike(harness.log.notes, "cannot be spun") == 1);
    harness.call.rotation_sticks = true;

    // Restoring puts the carried rotation back along with the scale.
    held->rotation_written = true;
    harness.image.SetRotation(
        actor.mesh, jelly::Vector3{5.0, 130.0, -2.0});  // what the effect last wrote
    CHECK(
        jelly::RestoreRecord(harness.model, *held, skip) ==
        jelly::RestoreStep::already_back);
    CHECK(jelly::Equal(harness.image.RotationOf(actor.mesh), jelly::Vector3{5.0, 40.0, -2.0}));
    CHECK(!held->rotation_written);

    // A build without the rotation function still scales: the spin is the only casualty.
    SyntheticUe::Options no_rotation;
    no_rotation.omit_rotation_function = true;
    Harness plain(ShippedLayout(), no_rotation);
    CHECK(plain.Prepare());
    const ActorFixture stopped =
        BuildSimpleActor(plain.image, "NoSpin", jelly::Vector3{1.0, 1.0, 1.0});
    plain.image.Finalize();
    jelly::VisualTarget plain_target;
    CHECK(plain.model.ResolveVisual(stopped.actor, false, plain_target, skip));
    CHECK(!plain_target.rotation_valid);
    const std::uint64_t plain_class_id = ClassIdOf(plain.image, stopped.actor);
    const std::uint64_t plain_key = static_cast<std::uint64_t>(stopped.index) + 1U;
    jelly::ActorIdentity plain_identity;
    CHECK(plain.model.ResolveActor(plain_key, plain_class_id, plain_identity, skip));
    jelly::RecoveryRecord plain_record;
    plain_record.model_key = plain_key;
    plain_record.identity = plain_identity;
    plain_record.component = plain_target.component;
    plain_record.base = plain_target.base;
    plain_record.applied = plain_target.base;
    plain_record.rotation_valid = false;
    plain_record.phase = 0.0;
    plain_record.start_time = 0.0;
    jelly::Settings plain_settings = settings;
    plain_settings.amplitude = 0.4;
    const int plain_before = plain.call.calls;
    CHECK(
        jelly::StepRecord(plain.model, plain_record, plain_settings, 0.25, skip) ==
        jelly::EffectStep::updated);
    CHECK(plain.call.calls == plain_before + 1);  // the scale write, and nothing else
    CHECK(plain_record.written);
    CHECK(plain_record.spin_blocked);
    // The spin is the only casualty: the note names it once, and the scale write landed.
    CHECK(NotesLike(plain.log.notes, "cannot be spun") == 1);
}

// This build names its characters `player_076_shinku_C` and its crowd `BP_NPC_*`, so the
// word the window's "characters" switch means is never in the class name.
void TestCharacterClasses() {
    Section("model: a character is recognised by its class chain, not by its name");
    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const std::uintptr_t hero = harness.image.MakeActor("player_076_shinku_C");
    const std::uintptr_t crowd =
        harness.image.MakeActor("BP_NPC_Mass_fm_Sub_C", harness.image.ActorClass());
    const std::uintptr_t prop =
        harness.image.MakeActor("StaticMeshActor", harness.image.ActorClass());
    harness.image.Finalize();

    // A Character-derived class whose own name says nothing about it.
    CHECK(harness.model.ClassIsCharacter(ClassIdOf(harness.image, hero)));
    // A crowd actor that is not Character-derived, recognised by what its name says.
    CHECK(harness.model.ClassIsCharacter(ClassIdOf(harness.image, crowd)));
    // Level scenery is neither.
    CHECK(!harness.model.ClassIsCharacter(ClassIdOf(harness.image, prop)));
    CHECK(!harness.model.ClassIsCharacter(0));
    CHECK(!harness.model.ClassIsCharacter(9999999));
}

void TestLedger() {
    Section("ledger: capacity, restore queue, conflicts");
    const std::size_t cap = jelly::kDefaultModels;
    jelly::RecoveryLedger ledger;
    CHECK(ledger.empty());
    for (std::size_t index = 0; index < cap; ++index) {
        jelly::RecoveryRecord record;
        record.model_key = index + 1U;
        record.component = 0x1000U + index;
        record.phase = jelly::PhaseForIdentity(record.model_key);
        CHECK(ledger.Add(record, cap));
    }
    CHECK(ledger.model_count() == cap);
    CHECK(ledger.root_count() == cap);
    jelly::RecoveryRecord overflow;
    overflow.model_key = 100000;
    overflow.component = 0x9000;
    CHECK(!ledger.Add(overflow, cap));

    // The same model key is never added twice.
    jelly::RecoveryRecord duplicate;
    duplicate.model_key = 1;
    duplicate.component = 0x2000;
    CHECK(!ledger.Add(duplicate, cap));

    CHECK(ledger.pending_restore() == 0);
    ledger.MarkRestore(3);
    CHECK(ledger.pending_restore() == 1);
    const std::vector<std::uint64_t> order = ledger.restore_order();
    CHECK(order.size() == 1 && order.front() == 3);
    CHECK(ledger.MarkAllRestore() == cap);
    const std::vector<std::uint64_t> all = ledger.restore_order();
    CHECK(all.size() == cap);
    CHECK(all.front() == 1 && all.back() == cap);  // oldest first

    // A restored record leaves the ledger, which is what lets the pending count
    // reach zero before the plugin may stop.
    ledger.Erase(1);
    CHECK(ledger.Find(1) == nullptr);
    CHECK(ledger.model_count() == cap - 1);
    CHECK(ledger.root_count() == cap - 1);
    ledger.Clear();
    CHECK(ledger.empty());
    CHECK(ledger.pending_restore() == 0);

    // The root count only counts records that name a component, and the root cap is
    // the documented backstop behind the model cap: with one visual root per model
    // the model cap is what binds first.
    jelly::RecoveryLedger roots;
    jelly::RecoveryRecord record;
    record.model_key = 1;
    CHECK(roots.Add(record, cap));
    CHECK(roots.root_count() == 0);
    CHECK(roots.model_count() == 1);
    static_assert(jelly::kRootsPerModel >= 1);
    CHECK(jelly::RootBudget(cap) == cap * jelly::kRootsPerModel);

    // The cap is the window's, so a smaller one binds instead: three models fit in a
    // cap of three and the fourth is refused with the capacity reason, which is what
    // the window's slider has to be able to do without a rebuild.
    jelly::RecoveryLedger small;
    for (std::uint64_t key = 1; key <= 3; ++key) {
        jelly::RecoveryRecord limited;
        limited.model_key = key;
        limited.component = 0x3000U + key;
        CHECK(small.Add(limited, 3));
    }
    jelly::RecoveryRecord fourth;
    fourth.model_key = 4;
    fourth.component = 0x4000;
    CHECK(!small.Add(fourth, 3));
    CHECK(small.model_count() == 3);

    // The class each model was taken over as travels with the record, which is what
    // the sweep line names the model budget with.
    jelly::RecoveryLedger classes;
    for (std::uint64_t key = 1; key <= 2; ++key) {
        jelly::RecoveryRecord tagged;
        tagged.model_key = key;
        tagged.class_id = 700U + key;
        tagged.component = 0x5000U + key;
        CHECK(classes.Add(tagged, cap));
    }
    const std::vector<std::uint64_t> held = classes.held_class_ids();
    CHECK(held.size() == 2 && held[0] == 701 && held[1] == 702);

    // A model that leaves the selection is queued, and one that is still wanted is
    // not, which is how leaving the frustum or switching a category off is handled.
    jelly::RecoveryLedger leavers;
    for (std::uint64_t key = 1; key <= 3; ++key) {
        jelly::RecoveryRecord queued_record;
        queued_record.model_key = key;
        queued_record.component = 0x2000U + key;
        CHECK(leavers.Add(queued_record, cap));
    }
    CHECK(leavers.MarkLeavers({1, 3}) == 1);
    CHECK(leavers.pending_restore() == 1);
    const std::vector<std::uint64_t> queued = leavers.restore_order();
    CHECK(queued.size() == 1 && queued.front() == 2);
    CHECK(leavers.MarkLeavers({1, 3}) == 0);  // queueing twice is not a second queue
}

// Resolves a model the way the effect pass does and records it, so a test can walk
// the same acquisition, effect and restore steps the plugin walks.
std::uint64_t AcquireRecord(
    Harness& harness, jelly::RecoveryLedger& ledger, const ActorFixture& fixture) {
    const std::uint64_t class_id = ClassIdOf(harness.image, fixture.actor);
    const std::uint64_t key = static_cast<std::uint64_t>(fixture.index) + 1U;
    jelly::ActorIdentity identity;
    jelly::Skip skip = jelly::Skip::none;
    if (!harness.model.ResolveActor(key, class_id, identity, skip)) return 0;
    jelly::VisualTarget target;
    if (!harness.model.ResolveVisual(identity.actor, false, target, skip)) return 0;
    jelly::RecoveryRecord record;
    record.model_key = key;
    record.identity = identity;
    record.component = target.component;
    record.base = target.base;
    record.applied = target.base;
    record.phase = jelly::PhaseForIdentity(key);
    return ledger.Add(record, jelly::kDefaultModels) ? key : 0;
}

void TestStepDecisions() {
    Section("step: the value comes from the base, and a foreign value stops the effect");
    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const ActorFixture actor =
        BuildSimpleActor(harness.image, "Step", jelly::Vector3{1.0, 1.25, 0.8});
    harness.image.Finalize();

    jelly::RecoveryLedger ledger;
    const std::uint64_t key = AcquireRecord(harness, ledger, actor);
    CHECK(key != 0);
    jelly::RecoveryRecord* record = ledger.Find(key);
    CHECK(record != nullptr);
    jelly::Skip skip = jelly::Skip::none;
    jelly::Settings settings;
    settings.amplitude = 0.4;
    settings.frequency_hz = 1.5;

    const double clock = 0.37;
    CHECK(
        jelly::StepRecord(harness.model, *record, settings, clock, skip) ==
        jelly::EffectStep::updated);
    const jelly::Vector3 expected = jelly::Output(
        record->base, settings.amplitude, settings.frequency_hz,
        clock - record->start_time, record->phase);
    CHECK(jelly::Equal(harness.image.ScaleOf(record->component), expected));
    CHECK(!jelly::Equal(expected, record->base));
    CHECK(ledger.written_count() == 1);

    // The same instant does not produce a second write.
    const int calls = harness.call.calls;
    CHECK(
        jelly::StepRecord(harness.model, *record, settings, clock, skip) ==
        jelly::EffectStep::unchanged);
    CHECK(harness.call.calls == calls);

    // Another writer takes the component over: the step reports it and writes
    // nothing over the value it does not own.
    const jelly::Vector3 foreign{5.0, 5.0, 5.0};
    harness.image.SetScale(record->component, foreign);
    CHECK(
        jelly::StepRecord(harness.model, *record, settings, clock + 0.1, skip) ==
        jelly::EffectStep::foreign_value);
    CHECK_SKIP(skip, jelly::Skip::conflict);
    CHECK(jelly::Equal(harness.image.ScaleOf(record->component), foreign));

    // A reused slot is not written to at all.
    harness.image.SetSerial(actor.index, 7777);
    CHECK(
        jelly::StepRecord(harness.model, *record, settings, clock + 0.2, skip) ==
        jelly::EffectStep::identity_lost);
    CHECK(jelly::Equal(harness.image.ScaleOf(record->component), foreign));
}

void TestRestoreDecisions() {
    Section("restore: back to base, conflicts, reuse and a refused write");
    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const ActorFixture actor =
        BuildSimpleActor(harness.image, "Restore", jelly::Vector3{1.0, 1.0, 1.0});
    harness.image.Finalize();
    jelly::RecoveryLedger ledger;
    const std::uint64_t key = AcquireRecord(harness, ledger, actor);
    CHECK(key != 0);
    jelly::RecoveryRecord* record = ledger.Find(key);
    CHECK(record != nullptr);
    jelly::Skip skip = jelly::Skip::none;
    jelly::Settings settings;

    // A record that never carried the effect has nothing to put back.
    CHECK(
        jelly::RestoreRecord(harness.model, *record, skip) ==
        jelly::RestoreStep::already_back);

    CHECK(
        jelly::StepRecord(harness.model, *record, settings, 0.25, skip) ==
        jelly::EffectStep::updated);
    CHECK(
        jelly::RestoreRecord(harness.model, *record, skip) == jelly::RestoreStep::restored);
    CHECK(jelly::Equal(harness.image.ScaleOf(record->component), record->base));

    // A restore the engine does not commit is reported as a failure so the caller
    // keeps the record, and the component keeps the value the plugin wrote.
    CHECK(
        jelly::StepRecord(harness.model, *record, settings, 0.5, skip) ==
        jelly::EffectStep::updated);
    const jelly::Vector3 applied = harness.image.ScaleOf(record->component);
    CHECK(!jelly::Equal(applied, record->base));
    harness.call.engine_writes = false;
    CHECK(
        jelly::RestoreRecord(harness.model, *record, skip) == jelly::RestoreStep::failed);
    CHECK(jelly::Equal(harness.image.ScaleOf(record->component), applied));
    harness.call.engine_writes = true;

    // A component someone else rewrote is never overwritten with the old value.
    const jelly::Vector3 foreign{4.0, 4.0, 4.0};
    harness.image.SetScale(record->component, foreign);
    CHECK(
        jelly::RestoreRecord(harness.model, *record, skip) ==
        jelly::RestoreStep::foreign_value);
    CHECK_SKIP(skip, jelly::Skip::conflict);
    CHECK(jelly::Equal(harness.image.ScaleOf(record->component), foreign));

    // And a slot that now carries another object is dropped without a write.
    harness.image.SetSerial(actor.index, 31337);
    CHECK(
        jelly::RestoreRecord(harness.model, *record, skip) ==
        jelly::RestoreStep::identity_lost);
    CHECK_SKIP(skip, jelly::Skip::identity_stale);
    CHECK(jelly::Equal(harness.image.ScaleOf(record->component), foreign));
}

void TestDisableGivesEverythingBack() {
    Section("recovery: switching the effect off returns every component to its base");
    Harness harness(ShippedLayout());
    CHECK(harness.Prepare());
    const ActorFixture first =
        BuildSimpleActor(harness.image, "First", jelly::Vector3{1.0, 1.0, 1.0});
    const ActorFixture second =
        BuildSimpleActor(harness.image, "Second", jelly::Vector3{2.0, 1.0, 0.5});
    harness.image.Finalize();

    jelly::RecoveryLedger ledger;
    const std::uint64_t first_key = AcquireRecord(harness, ledger, first);
    const std::uint64_t second_key = AcquireRecord(harness, ledger, second);
    CHECK(first_key != 0 && second_key != 0);
    CHECK(ledger.model_count() == 2);

    jelly::Settings settings;
    jelly::Skip skip = jelly::Skip::none;
    double clock = 0.0;
    for (int frame = 0; frame < 5; ++frame) {
        clock += 1.0 / 30.0;
        for (const std::uint64_t key : ledger.controlled_keys()) {
            jelly::RecoveryRecord* record = ledger.Find(key);
            CHECK(
                jelly::StepRecord(harness.model, *record, settings, clock, skip) ==
                jelly::EffectStep::updated);
        }
    }
    // Both components are away from their original scale while the effect runs.
    CHECK(!jelly::Equal(
        harness.image.ScaleOf(first.mesh), jelly::Vector3{1.0, 1.0, 1.0}));
    CHECK(!jelly::Equal(
        harness.image.ScaleOf(second.mesh), jelly::Vector3{2.0, 1.0, 0.5}));

    // Switching the effect off queues everything, and the restore pass empties the
    // ledger; only then is the plugin allowed to stop.
    CHECK(ledger.MarkAllRestore() == 2);
    CHECK(ledger.pending_restore() == 2);
    for (const std::uint64_t key : ledger.restore_order()) {
        jelly::RecoveryRecord* record = ledger.Find(key);
        CHECK(
            jelly::RestoreRecord(harness.model, *record, skip) ==
            jelly::RestoreStep::restored);
        ledger.Erase(key);
    }
    CHECK(ledger.empty());
    CHECK(ledger.pending_restore() == 0);
    CHECK(jelly::Equal(harness.image.ScaleOf(first.mesh), jelly::Vector3{1.0, 1.0, 1.0}));
    CHECK(jelly::Equal(harness.image.ScaleOf(second.mesh), jelly::Vector3{2.0, 1.0, 0.5}));
}

}  // namespace

int RunModelTests() {
    TestIdentity();
    TestVisualSelection();
    TestStateChecks();
    TestWrite();
    TestCallEntry();
    TestCallEntryBehindTheObserverDetour();
    TestConflict();
    TestRequirements();
    TestNames();
    TestMovementScope();
    TestHiddenActors();
    TestNonVisualMesh();
    TestVehicleFamily();
    TestSimulatedVehicleBody();
    TestVehicleFollowers();
    TestSpin();
    TestCharacterClasses();
    TestLedger();
    TestStepDecisions();
    TestRestoreDecisions();
    TestDisableGivesEverythingBack();
    return 0;
}

}  // namespace jelly_tests
