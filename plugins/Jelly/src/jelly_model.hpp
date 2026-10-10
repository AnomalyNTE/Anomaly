#pragma once
//
// UE 模型访问 (UE model access).
//
// Everything that touches a game object lives in this module: the object-registry
// walk, name and class lookups, FProperty and UFunction reflection, the identity
// validation that turns an actor-frame snapshot into a UE object, the reflected
// scale write, and the recovery records. It talks to the process through narrow
// interfaces (a read-only memory view, a name table and the reflected call), so
// the offline fixture can drive it against a synthetic UE image.
//
// Thread domain: Game only.

#include "jelly_effect.hpp"
#include "jelly_layout.hpp"
#include "jelly_selection.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jelly {

// --- the process -----------------------------------------------------------------

struct MemoryAccess {
    virtual ~MemoryAccess() = default;
    // Read-only by construction: the module has no raw-write path at all, every
    // scale change goes through the validated reflected call below.
    virtual bool Read(
        std::uintptr_t address, void* destination, std::size_t size) noexcept = 0;
};

struct NameTable {
    virtual ~NameTable() = default;
    // Resolves the id a piece of reflection data carries back to text. Names are
    // never turned into ids here: the host's pool lookup only reaches the newest
    // blocks, so an engine name registered long before the content is would never be
    // found by it, which is why every comparison in this module compares text.
    virtual bool Text(std::uint32_t id, std::string& name) = 0;
};

// Where the model module says which name this build does not carry. The window
// counts refusals; this is what turns "this class did not validate" into the key
// that was missing. Game domain only.
struct DiagnosticSink {
    virtual ~DiagnosticSink() = default;
    virtual void Note(std::string_view message) noexcept = 0;
};

struct ReflectionCall {
    virtual ~ReflectionCall() = default;
    // The engine's own UObject::ProcessEvent, called only from the Game domain.
    virtual bool ProcessEvent(
        std::uintptr_t object, std::uintptr_t function, void* parms,
        std::size_t parms_size) noexcept = 0;
};

// --- why a candidate produced no effect ------------------------------------------
//
// The numeric value is what crosses the thread boundary; the window turns it into
// localized text.
enum class Skip : std::uint32_t {
    none = 0,
    layout_unavailable,
    call_entry_missing,
    no_registry,
    no_identity,
    class_mismatch,
    identity_stale,
    no_root,
    no_visual,
    invisible,
    collision,
    physics,
    instanced,
    cross_actor,
    multi_part,
    static_mesh,
    // The actor itself is hidden in the game (AActor::bHidden), so there is nothing
    // in the world to squash: the three party members that stand next to the local
    // player are exactly this case.
    hidden,
    // An actor whose root component hangs under another vehicle's component: a spoiler,
    // a lamp or a kit of that vehicle. It follows the vehicle as it is scaled and must
    // not take a model slot of its own, or a car whose body is skipped would show only
    // its accessories moving.
    vehicle_follower,
    function_missing,
    signature_mismatch,
    parm_too_small,
    read_failed,
    write_failed,
    not_verified,
    conflict,
    capacity,
};

const char* SkipKey(Skip skip) noexcept;

// --- identity --------------------------------------------------------------------

// A validated UE object identity. `index` and `serial` come from the object
// registry, not from the actor-frame snapshot: the frame handle names a model for
// one frame, this names the object that must still be there before the plugin
// writes to it.
struct ActorIdentity {
    std::uintptr_t actor{};
    std::uint32_t index{};
    std::uint32_t serial{};
    bool valid{};

    [[nodiscard]] std::uint64_t handle() const noexcept {
        return (static_cast<std::uint64_t>(serial) << 32U) |
            (static_cast<std::uint64_t>(index) + 1U);
    }

    [[nodiscard]] bool SameObjectAs(const ActorIdentity& other) const noexcept {
        return index == other.index && serial == other.serial;
    }
};

// The validated visual root and the scale it carried before the plugin touched it.
struct VisualTarget {
    std::uintptr_t component{};
    Vector3 base{1.0, 1.0, 1.0};
    // The rotation the component carried when the model was taken over, and whether it
    // could be read at all. A build that cannot report it is still a target: only the spin
    // needs it.
    Vector3 rotation{};
    bool rotation_valid{};
    bool valid{};
};

// --- the model module ------------------------------------------------------------

class ModelAccess final {
public:
    ModelAccess(
        const Layout& layout, MemoryAccess& memory, NameTable& names,
        ReflectionCall& call, DiagnosticSink& diagnostics) noexcept;

    ModelAccess(const ModelAccess&) = delete;
    ModelAccess& operator=(const ModelAccess&) = delete;

    // Resolves the resource's names and prepares the object-registry view and the
    // validated ProcessEvent entry. Returns false and sets `skip` when the
    // resource is incomplete or the process does not carry what it describes.
    bool Prepare(
        std::uintptr_t registry_address, std::uintptr_t process_event_address,
        Skip& skip);

    [[nodiscard]] bool ready() const noexcept { return ready_; }

    // Actor-frame candidate -> validated UE object identity.
    bool ResolveActor(
        std::uint64_t frame_handle_id, std::uint64_t class_id, ActorIdentity& out,
        Skip& skip);

    // Re-checks that a record still names the same live object in the same slot.
    bool IdentityHeld(const ActorIdentity& identity, Skip& skip);

    // Finds the actor's single safe visual root and reads the scale it had before
    // the plugin touched it. `allow_simulated_body` admits a vehicle whose body mesh is
    // the chassis rigid body - the car the player drives - which is refused otherwise;
    // it never relaxes anything for another family.
    bool ResolveVisual(
        std::uintptr_t actor, bool allow_simulated_body, VisualTarget& out, Skip& skip);

    // Whether the frame's class id names a character. The resource's class *name*
    // cannot answer this: this build calls its characters `player_076_shinku_C` and
    // `BP_NPC_Mass_fm_Sub_C`, so the question is answered from the live class chain
    // (an engine `Character` in the chain, or a class whose name says NPC/Character).
    // Answers are cached per class id; an unreadable class is not a character.
    [[nodiscard]] bool ClassIsCharacter(std::uint64_t frame_class_id) const;

    // Whether the actor's class names the vehicle family. Cached per class; the
    // family is matched as a case-insensitive substring of the class chain, which is
    // what makes one entry cover every `BP_Vehicle_*` variant this build spawns.
    [[nodiscard]] bool ClassIsVehicle(std::uintptr_t actor_class) const;

    // The same question asked the way the window's diagnostics ask it: from the class id
    // an actor-frame entry carries. Cached per id.
    [[nodiscard]] bool ClassIsVehicleById(std::uint64_t frame_class_id) const;

    bool ReadScale(std::uintptr_t component, Vector3& scale, Skip& skip);
    bool ApplyScale(std::uintptr_t component, const Vector3& scale, Skip& skip);

    // The rotation half of the same story: read the rotation the model carries, and write
    // one computed from it. Used while the window's spin switch is on, and to hand a
    // rotation back that the effect wrote earlier.
    bool ReadRotation(std::uintptr_t component, Vector3& rotator, Skip& skip);
    bool ApplyRotation(std::uintptr_t component, const Vector3& rotator, Skip& skip);

    // Says once per class that this model cannot be spun - its rotation cannot be read,
    // written or verified - which is a scope question about the spin alone: the jelly
    // scale keeps running.
    void NoteSpinUnavailable(std::uintptr_t component);

    [[nodiscard]] bool OwnedBy(std::uintptr_t object, std::uintptr_t owner);

private:
    struct FieldView {
        std::uintptr_t field{};
        std::int64_t offset{};
        std::int64_t element_size{};
        std::int64_t array_dim{};
        std::string type;       // FFieldClass name, e.g. "StructProperty"
        std::uintptr_t sub{};   // Struct / PropertyClass / Inner target
        bool valid{};
    };

    struct ParamView {
        std::string name;
        std::string type;
        std::uint32_t sub_id{};  // name id of the Struct the parameter points at
        std::int64_t offset{};
        std::int64_t element_size{};
    };

    struct FunctionView {
        std::uintptr_t function{};
        std::uint32_t num_parms{};
        std::uint32_t parms_size{};
        std::uint32_t return_value_offset{};
        std::vector<ParamView> params;
        bool valid{};
    };

    struct ComponentLayout {
        // The offset of the reflected value this layout writes: RelativeScale3D for the
        // jelly scale, RelativeRotation for the spin.
        std::int64_t value_offset{};
        std::uintptr_t function{};
        std::uint32_t parms_size{};
        std::int64_t param_offset{};
        Skip skip{Skip::none};  // why this class did not validate, when it did not
        bool valid{};
    };

    struct ClassTraits {
        bool mesh{};
        bool character{};
        bool instanced{};
        // A mesh-derived component that is not a model: a `WidgetComponent` (the
        // crowd NPCs carry a name-plate bubble) renders UI in the world, and treating
        // it as a second visual root left every one of those NPCs as multi-part.
        bool non_visual{};
    };

    bool ReadBytes(
        std::uintptr_t address, void* destination, std::size_t size) const noexcept;

    template <typename Value>
    bool ReadValue(const std::uintptr_t address, Value& value) const noexcept {
        return ReadBytes(address, &value, sizeof(value));
    }

    std::uintptr_t ReadPointer(std::uintptr_t address) const noexcept;
    std::uint32_t ReadNameId(std::uintptr_t object) const noexcept;
    std::string ReadNameText(std::uintptr_t object) const;
    std::string ReadFieldNameText(std::uintptr_t field) const;
    // The text a name id resolves to, cached for this generation; empty when the
    // process does not let that id be read back. The reference is only valid until
    // the next call, which may empty the cache when it reaches its cap.
    const std::string& NameText(std::uint32_t id) const;
    [[nodiscard]] bool NameMatches(
        std::uint32_t id, std::string_view expected) const;
    // The class name the resource records under a layout key, matched along a chain.
    [[nodiscard]] bool ClassChainHasName(
        std::uintptr_t class_object, std::string_view layout_key) const;
    [[nodiscard]] bool ClassChainContains(
        std::uintptr_t class_object, std::string_view expected) const;
    // Says once per class and key that this build carries no such property or
    // function, so a refusal that would otherwise only be a counter names itself.
    void NoteMissingName(
        std::uintptr_t class_object, std::string_view kind,
        std::string_view key) const;
    // Says once per class and key why a class is treated as unfiltered, with the
    // key naming the question that could not be asked.
    void NoteUnfiltered(
        std::uintptr_t class_object, std::string_view key,
        std::string_view reason) const;
    // Says once per class that this build does not reflect Mobility, so static scenery
    // cannot be told apart from a dynamic model for that class. The component is then
    // allowed rather than refused: a scope question must not silently stop the effect.
    void NoteUnfilteredClass(std::uintptr_t class_object) const;

    bool BuildFieldView(std::uintptr_t field, FieldView& out) const;
    bool FindProperty(
        std::uintptr_t root_struct, std::string_view layout_key, FieldView& out) const;
    bool ReadFunctionView(std::uintptr_t function, FunctionView& out) const;
    bool FindFunction(
        std::uintptr_t root_class, std::string_view expected_name,
        FunctionView& out) const;

    [[nodiscard]] bool PropertyMatches(
        const FieldView& field, const PropertySpec& spec) const;
    [[nodiscard]] bool SignatureMatches(
        const FunctionView& signature, const FunctionSpec& spec) const;

    bool ReadBoolProperty(
        std::uintptr_t owner, const FieldView& field, bool& out) const noexcept;
    bool ReadObjectProperty(
        std::uintptr_t owner, const FieldView& field, std::uintptr_t& out) const noexcept;
    bool ReadScaleProperty(
        std::uintptr_t owner, const FieldView& field, Vector3& out) const noexcept;
    bool ReadRotationProperty(
        std::uintptr_t owner, const FieldView& field, Vector3& out) const noexcept;

    const ClassTraits* TraitsFor(std::uintptr_t class_object);
    const ComponentLayout* LayoutFor(std::uintptr_t class_object, Skip& skip);
    const ComponentLayout* RotationLayoutFor(std::uintptr_t class_object, Skip& skip);
    bool ResolveComponentLayout(
        std::uintptr_t class_object, std::string_view property_key,
        std::string_view function_key, ComponentLayout& out, Skip& skip);

    // Whether the actor itself is hidden. Read from the actor's own reflected
    // `bHidden`; a build that does not carry it reports the class once and treats the
    // actor as visible, because a scope question must not stop the whole effect.
    [[nodiscard]] bool ActorHidden(
        std::uintptr_t actor, std::uintptr_t actor_class) const;
    // Whether the actor's root component hangs under a component of another vehicle
    // actor, which makes this actor a part of that vehicle rather than a model.
    [[nodiscard]] bool AttachedToAnotherVehicle(std::uintptr_t root) const;
    [[nodiscard]] bool ComponentIsUsable(
        std::uintptr_t actor, std::uintptr_t actor_class, std::uintptr_t component,
        bool vehicle, bool allow_simulated_body, bool& simulating_part, Skip& skip);
    bool CollectAttachTree(
        std::uintptr_t root, std::vector<std::uintptr_t>& components,
        std::unordered_map<std::uintptr_t, std::uintptr_t>& parents) const;
    bool ReadObjectSlot(
        std::uint32_t index, std::uintptr_t& object, std::uint32_t& serial) const noexcept;

    const Layout& layout_;
    MemoryAccess& memory_;
    NameTable& names_;
    ReflectionCall& call_;
    DiagnosticSink& diagnostics_;

    bool registry_ready_{};
    std::uintptr_t items_{};
    std::uintptr_t count_address_{};
    std::uint32_t max_count_{};
    // Refreshed from the registry header by every lookup, so it is mutable in a
    // const walk.
    mutable std::uint32_t object_count_{};
    std::uint32_t num_chunks_{};
    std::uint32_t chunk_size_{};

    bool ready_{};
    // Name ids this generation has read back, so a class whose chain is walked once
    // does not ask the host for the same text again. The ids cannot be recorded:
    // they are assigned as names are registered and differ from run to run.
    mutable std::unordered_map<std::uint32_t, std::string> name_text_;
    // Whether a class id names a character, so a sweep does not walk the same chain
    // once per actor.
    mutable std::unordered_map<std::uint32_t, bool> character_classes_;
    // Whether an actor class belongs to the vehicle family, so the per-component walk
    // of a model does not re-walk the chain for every component.
    mutable std::unordered_map<std::uintptr_t, bool> vehicle_classes_;
    // The same answer keyed by the class id a frame entry carries, for the diagnostics.
    mutable std::unordered_map<std::uint32_t, bool> vehicle_family_ids_;
    // Classes already reported through the sink, as class object and key.
    mutable std::vector<std::pair<std::uintptr_t, std::string>> naming_notes_;
    std::unordered_map<std::uintptr_t, ClassTraits> traits_;
    std::unordered_map<std::uintptr_t, ComponentLayout> component_layouts_;
    // The rotation write path is resolved and cached apart from the scale one: a build
    // that carries the scale function but not the rotation one must still scale.
    std::unordered_map<std::uintptr_t, ComponentLayout> rotation_layouts_;
};

// --- recovery records ------------------------------------------------------------

enum class RecordState : std::uint32_t { controlling, restore_pending };

struct RecoveryRecord {
    std::uint64_t model_key{};         // actor-frame handle id, the model's frame name
    // The frame class the model was taken over as, so the sweep line can name which
    // classes the model budget actually went to. Not an identity: it never decides
    // whether a write may happen, the object registry does.
    std::uint64_t class_id{};
    ActorIdentity identity{};          // the validated UE object, never the frame handle
    std::uintptr_t component{};
    Vector3 base{1.0, 1.0, 1.0};
    Vector3 applied{1.0, 1.0, 1.0};
    // The rotation the model carried when it was taken over, the rotation the effect last
    // wrote, and whether one of ours is on the component right now. A rotation that the
    // game drives itself is never fought: the record stops spinning instead.
    Vector3 base_rotation{};
    Vector3 applied_rotation{};
    // Whether the model's own rotation could be read when it was taken over. Without it
    // there is nothing to spin from, so the record never spins.
    bool rotation_valid{};
    bool rotation_written{};
    // Set once the spin could not be verified for this model: the scale keeps running and
    // the class is named once in the log.
    bool spin_blocked{};
    double phase{};
    double start_time{};  // the Game-domain clock when the effect took the model over
    RecordState state{RecordState::controlling};
    bool written{};
};

// Holds what the plugin touched so it can put it back. Records live for one plugin
// generation only and are never persisted.
class RecoveryLedger final {
public:
    [[nodiscard]] std::size_t model_count() const noexcept { return records_.size(); }
    [[nodiscard]] std::size_t root_count() const noexcept;
    [[nodiscard]] std::size_t pending_restore() const noexcept;
    [[nodiscard]] std::size_t written_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }

    [[nodiscard]] RecoveryRecord* Find(std::uint64_t model_key) noexcept;
    [[nodiscard]] const RecoveryRecord* Find(std::uint64_t model_key) const noexcept;

    // Fails when the model or root cap is already reached. `model_cap` is the count
    // the window asked for; the root budget follows from it (RootBudget()).
    bool Add(const RecoveryRecord& record, std::size_t model_cap) noexcept;
    void Erase(std::uint64_t model_key) noexcept;
    void Clear() noexcept;

    void MarkRestore(std::uint64_t model_key) noexcept;
    // Marks every record for restore and returns how many are pending.
    std::size_t MarkAllRestore() noexcept;

    // Model keys that still have to be put back, oldest first.
    [[nodiscard]] std::vector<std::uint64_t> restore_order() const;

    // Model keys the ledger still owns, oldest first.
    [[nodiscard]] std::vector<std::uint64_t> controlled_keys() const;

    // The frame class of every model the ledger still holds, for the sweep line that
    // says which classes the model budget actually went to.
    [[nodiscard]] std::vector<std::uint64_t> held_class_ids() const;

    // Queues every record whose model is not in `still_wanted` — a model that left
    // the frustum, was deselected, or whose category was switched off — and returns
    // how many it queued.
    std::size_t MarkLeavers(const std::vector<std::uint64_t>& still_wanted) noexcept;

private:
    std::vector<RecoveryRecord> records_;
};

// --- the two recovery decisions ---------------------------------------------------
//
// They live beside the records because they are what the records are for, and they
// take the model module rather than a game so the fixture can drive them.

// One restore step for a record the effect has left. The caller erases the record
// for every outcome except `failed`, which keeps it pending so the pending count
// stays honest and Stop can report it.
enum class RestoreStep : std::uint32_t {
    restored,       // the original scale was written back and verified
    already_back,   // the component already carried its original scale
    identity_lost,  // the object is gone or its slot now holds another object
    foreign_value,  // another writer owns the component; nothing is written
    failed,         // the write or the read failed
};

RestoreStep RestoreRecord(ModelAccess& model, RecoveryRecord& record, Skip& skip);

// One effect step for a record the effect still owns: the conflict check, the
// per-target phase and the write, always computed from the original scale.
enum class EffectStep : std::uint32_t {
    updated,
    unchanged,      // the component already carries this step's value
    identity_lost,
    foreign_value,
    failed,
};

EffectStep StepRecord(
    ModelAccess& model, RecoveryRecord& record, const Settings& settings, double clock,
    Skip& skip);

}  // namespace jelly
