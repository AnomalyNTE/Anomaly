#include "jelly_model.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace jelly {
namespace {

// Every walk is bounded; a malformed or hostile object graph must not be able to
// turn a Game update into an unbounded traversal.
constexpr std::size_t kMaxFieldChain = 8192;
constexpr std::size_t kMaxClassDepth = 32;
constexpr std::size_t kMaxParams = 16;
constexpr std::size_t kMaxParmsBytes = 4096;
constexpr std::size_t kMaxTreeComponents = 512;
constexpr std::uint32_t kMaxObjectCount = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaxChunks = 4096;
constexpr std::int64_t kMaxLayoutOffset = 64LL * 1024LL * 1024LL;
// Name ids read back this generation. The cache is an optimisation, so it stops
// growing rather than evicting: a class whose chain was walked keeps its answers.
constexpr std::size_t kMaxNameTextEntries = 8192;
constexpr std::size_t kMaxNamingNotes = 256;
// Class ids whose "is this a character" answer is remembered. The cache exists so a
// sweep does not walk the same class chain once per actor, and it stops growing
// rather than evicting.
constexpr std::size_t kMaxCharacterClasses = 4096;
// Actor classes whose "is this a vehicle" answer is remembered, for the same reason:
// the family question is asked once per model, not once per component.
constexpr std::size_t kMaxVehicleClasses = 1024;

// The opcode Anomaly's public ProcessEvent observer writes over the first byte of
// UObject::ProcessEvent; the jump it heads is kProcessEventDetourBytes long, which
// is what the resource has to declare for an entry to be accepted in that form.
constexpr std::uint8_t kDetourJump = 0xE9;

// EComponentMobility::Static, read from the component's own reflected Mobility (the
// enum is Static 0, Stationary 1, Movable 2). The host publishes the same enum as
// entity flags, but from a byte it reads and never validates, so the component itself
// is asked instead: only the level's scenery is out of scope, and both Stationary and
// Movable stay in it.
constexpr std::uint8_t kComponentMobilityStatic = 0;

static_assert(sizeof(Vector3) == 24, "an FVector of three doubles is 24 bytes");

bool AddOffset(
    const std::uintptr_t base, const std::int64_t offset, std::uintptr_t& out) noexcept {
    if (base == 0 || offset < 0 || offset > kMaxLayoutOffset) return false;
    const auto value = static_cast<std::uint64_t>(offset);
    if (value > (std::numeric_limits<std::uintptr_t>::max)() - base) return false;
    out = base + static_cast<std::uintptr_t>(value);
    return true;
}

}  // namespace

const char* SkipKey(const Skip skip) noexcept {
    switch (skip) {
        case Skip::none: return "skip.none";
        case Skip::layout_unavailable: return "skip.not_verified_layout";
        case Skip::call_entry_missing: return "skip.call_entry_missing";
        case Skip::no_registry: return "skip.no_registry";
        case Skip::no_identity: return "skip.no_frame_handle";
        case Skip::class_mismatch: return "skip.class_mismatch";
        case Skip::identity_stale: return "skip.identity_stale";
        case Skip::no_root: return "skip.no_root";
        case Skip::no_visual: return "skip.no_visual";
        case Skip::invisible: return "skip.invisible";
        case Skip::collision: return "skip.collision";
        case Skip::physics: return "skip.physics";
        case Skip::instanced: return "skip.instanced";
        case Skip::cross_actor: return "skip.cross_actor";
        case Skip::multi_part: return "skip.multi_part";
        case Skip::static_mesh: return "skip.static_mesh";
        case Skip::hidden: return "skip.hidden";
        case Skip::vehicle_follower: return "skip.vehicle_follower";
        case Skip::function_missing: return "skip.function_missing";
        case Skip::signature_mismatch: return "skip.signature_mismatch";
        case Skip::parm_too_small: return "skip.parm_too_small";
        case Skip::read_failed: return "skip.read_failed";
        case Skip::write_failed: return "skip.write_failed";
        case Skip::not_verified: return "skip.not_verified";
        case Skip::conflict: return "skip.conflict";
        case Skip::capacity: return "skip.capacity";
    }
    return "skip.none";
}

ModelAccess::ModelAccess(
    const Layout& layout, MemoryAccess& memory, NameTable& names,
    ReflectionCall& call, DiagnosticSink& diagnostics) noexcept
    : layout_(layout), memory_(memory), names_(names), call_(call),
      diagnostics_(diagnostics) {}

bool ModelAccess::ReadBytes(
    const std::uintptr_t address, void* const destination,
    const std::size_t size) const noexcept {
    return address != 0 && destination != nullptr && size != 0 &&
        memory_.Read(address, destination, size);
}

std::uintptr_t ModelAccess::ReadPointer(const std::uintptr_t address) const noexcept {
    std::uintptr_t value{};
    if (!ReadValue(address, value)) return 0;
    return value;
}

std::uint32_t ModelAccess::ReadNameId(const std::uintptr_t object) const noexcept {
    const std::int64_t offset = LayoutOffset(layout_, "object.name_offset", -1);
    std::uintptr_t address{};
    std::uint32_t id{};
    if (object == 0 || offset < 0 || !AddOffset(object, offset, address) ||
        !ReadValue(address, id)) {
        return 0;
    }
    return id;
}

std::string ModelAccess::ReadNameText(const std::uintptr_t object) const {
    std::string text;
    const std::uint32_t id = ReadNameId(object);
    if (id == 0 || !names_.Text(id, text)) return {};
    return text;
}

// An FProperty is an FField, not a UObject: its name lives at the FField offset
// even when it describes a member of a UObject-derived class.
std::string ModelAccess::ReadFieldNameText(const std::uintptr_t field) const {
    const std::int64_t offset = LayoutOffset(layout_, "ffield.name", -1);
    std::uintptr_t address{};
    std::uint32_t id{};
    std::string text;
    if (field == 0 || offset < 0 || !AddOffset(field, offset, address) ||
        !ReadValue(address, id) || id == 0 || !names_.Text(id, text)) {
        return {};
    }
    return text;
}

const std::string& ModelAccess::NameText(const std::uint32_t id) const {
    static const std::string empty;
    if (id == 0) return empty;
    if (const auto found = name_text_.find(id); found != name_text_.end()) {
        return found->second;
    }
    std::string text;
    static_cast<void>(names_.Text(id, text));
    // A cache that simply stopped growing would start refusing models once a busy
    // scene had walked more names than its cap, so it is emptied instead: the answers
    // the class caches hold stay valid, and the worst case is reading a name twice.
    if (name_text_.size() >= kMaxNameTextEntries) name_text_.clear();
    const auto inserted = name_text_.emplace(id, std::move(text));
    return inserted.first->second;
}

bool ModelAccess::NameMatches(
    const std::uint32_t id, const std::string_view expected) const {
    if (id == 0 || expected.empty()) return false;
    const std::string& text = NameText(id);
    if (text.size() != expected.size()) return false;
    // UE compares FNames case-insensitively, and the resource records the spelling
    // the build's own reflection carries, so only case is allowed to differ.
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto live = static_cast<unsigned char>(text[index]);
        const auto want = static_cast<unsigned char>(expected[index]);
        if (live == want) continue;
        const auto lower = [](const unsigned char character) {
            return character >= 'A' && character <= 'Z'
                ? static_cast<unsigned char>(character - 'A' + 'a')
                : character;
        };
        if (lower(live) != lower(want)) return false;
    }
    return true;
}

bool ModelAccess::ClassChainContains(
    std::uintptr_t class_object, const std::string_view expected) const {
    if (expected.empty() || class_object == 0) return false;
    const std::int64_t super = LayoutOffset(layout_, "ustruct.super_struct", -1);
    if (super < 0) return false;
    for (std::size_t depth = 0; depth < kMaxClassDepth && class_object != 0; ++depth) {
        if (NameMatches(ReadNameId(class_object), expected)) return true;
        std::uintptr_t next{};
        if (!AddOffset(class_object, super, next)) return false;
        class_object = ReadPointer(next);
    }
    return false;
}

bool ModelAccess::ClassChainHasName(
    const std::uintptr_t class_object, const std::string_view layout_key) const {
    return ClassChainContains(class_object, LayoutName(layout_, layout_key));
}

void ModelAccess::NoteMissingName(
    const std::uintptr_t class_object, const std::string_view kind,
    const std::string_view key) const {
    if (class_object == 0) return;
    for (const auto& note : naming_notes_) {
        if (note.first == class_object && note.second == key) return;
    }
    std::string owner = ReadNameText(class_object);
    if (owner.empty()) owner = "?";
    if (naming_notes_.size() < kMaxNamingNotes) {
        naming_notes_.emplace_back(class_object, std::string(key));
    }
    std::string message = "jelly: ";
    message.append(owner);
    message.append(" has no ");
    message.append(kind);
    message.append(" '");
    message.append(key);
    message.append("'; targets of this class are skipped");
    diagnostics_.Note(message);
}

void ModelAccess::NoteUnfilteredClass(const std::uintptr_t class_object) const {
    NoteUnfiltered(
        class_object, "mobility",
        " has no reflected Mobility; static scenery cannot be filtered for this class");
}

void ModelAccess::NoteUnfiltered(
    const std::uintptr_t class_object, const std::string_view key,
    const std::string_view reason) const {
    if (class_object == 0) return;
    for (const auto& note : naming_notes_) {
        if (note.first == class_object && note.second == key) return;
    }
    std::string owner = ReadNameText(class_object);
    if (owner.empty()) owner = "?";
    if (naming_notes_.size() < kMaxNamingNotes) {
        naming_notes_.emplace_back(class_object, std::string(key));
    }
    std::string message = "jelly: ";
    message.append(owner);
    message.append(reason);
    diagnostics_.Note(message);
}

bool ModelAccess::Prepare(
    const std::uintptr_t registry_address, const std::uintptr_t process_event_address,
    Skip& skip) {
    ready_ = false;
    registry_ready_ = false;
    skip = Skip::layout_unavailable;
    if (!LayoutProblems(layout_).empty()) return false;

    // Names are not resolved here at all: every lookup below compares the text the
    // reflection data carries with the text the resource records, so a name this
    // build registers in an old pool block is not a resource problem, and a name it
    // does not carry at all is reported for the model that needed it.
    name_text_.clear();
    character_classes_.clear();
    naming_notes_.clear();
    traits_.clear();
    component_layouts_.clear();

    const std::int64_t items_offset = LayoutOffset(layout_, "objects.items", -1);
    const std::int64_t count_offset = LayoutOffset(layout_, "objects.count", -1);
    const std::int64_t max_count_offset = LayoutOffset(layout_, "objects.max_count", -1);
    const std::int64_t num_chunks_offset = LayoutOffset(layout_, "objects.num_chunks", -1);
    const std::int64_t max_chunks_offset = LayoutOffset(layout_, "objects.max_chunks", -1);
    const std::int64_t chunk_size = LayoutOffset(layout_, "objects.chunk_size", 0);
    const std::int64_t item_stride = LayoutOffset(layout_, "objects.item_stride", 0);
    const std::int64_t object_offset = LayoutOffset(layout_, "objects.object_offset", -1);
    const std::int64_t serial_offset = LayoutOffset(layout_, "objects.serial_offset", -1);
    const std::int64_t chunk_count_size =
        LayoutOffset(layout_, "objects.chunk_count_size", 4);
    if (items_offset < 0 || count_offset < 0 || max_count_offset < 0 ||
        num_chunks_offset < 0 || max_chunks_offset < 0 || chunk_size <= 0 ||
        item_stride < 8 || object_offset < 0 || serial_offset < 0 ||
        object_offset + 8 > item_stride || serial_offset + 4 > item_stride ||
        (chunk_count_size != 2 && chunk_count_size != 4)) {
        return false;
    }

    skip = Skip::no_registry;
    std::uintptr_t items_address{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t num_chunks{};
    std::uint32_t max_chunks{};
    const bool narrow_chunk_count = chunk_count_size == 2;
    const auto read_chunk_count = [this, narrow_chunk_count](
                                      const std::uintptr_t address,
                                      std::uint32_t& value) {
        if (!narrow_chunk_count) return ReadValue(address, value);
        std::uint16_t small{};
        if (!ReadValue(address, small)) return false;
        value = small;
        return true;
    };
    if (registry_address == 0 ||
        !AddOffset(registry_address, items_offset, items_address) ||
        !ReadValue(items_address, items_) || items_ == 0 ||
        !AddOffset(registry_address, count_offset, count_address_) ||
        !ReadValue(count_address_, count) ||
        !AddOffset(registry_address, max_count_offset, items_address) ||
        !ReadValue(items_address, max_count) ||
        !AddOffset(registry_address, num_chunks_offset, items_address) ||
        !read_chunk_count(items_address, num_chunks) ||
        !AddOffset(registry_address, max_chunks_offset, items_address) ||
        !read_chunk_count(items_address, max_chunks)) {
        return false;
    }
    const auto chunk_objects = static_cast<std::uint64_t>(chunk_size);
    if (max_count == 0 || max_count > kMaxObjectCount || count > max_count ||
        max_chunks == 0 || max_chunks > kMaxChunks || num_chunks > max_chunks ||
        static_cast<std::uint64_t>(max_count) >
            static_cast<std::uint64_t>(max_chunks) * chunk_objects) {
        return false;
    }
    chunk_size_ = static_cast<std::uint32_t>(chunk_size);
    max_count_ = max_count;
    object_count_ = count;
    num_chunks_ = num_chunks;
    registry_ready_ = true;

    // The call entry: the signature service already confined the address to the
    // module's code section, and the entry has to read back as something the plugin
    // recognises before it is willing to call through it. Anomaly's observer
    // installs its five byte jump on this function before a plugin hot-loads, so the
    // entry is either the recorded prologue itself or that jump with the same
    // prologue behind it.
    skip = Skip::call_entry_missing;
    if (process_event_address == 0 || layout_.process_event_prologue.empty()) return false;
    const auto detour = static_cast<std::ptrdiff_t>(layout_.process_event_detour_bytes);
    std::vector<std::uint8_t> observed(layout_.process_event_prologue.size(), 0);
    if (!ReadBytes(process_event_address, observed.data(), observed.size())) return false;
    if (observed != layout_.process_event_prologue) {
        // LayoutProblems() has established that the detour is the supported five
        // bytes and that the prologue outlasts it, so the jump and its displacement
        // fit inside the bytes just read.
        if (observed[0] != kDetourJump) return false;
        if (!std::equal(
                observed.begin() + detour, observed.end(),
                layout_.process_event_prologue.begin() + detour)) {
            return false;
        }
        std::int32_t displacement{};
        std::memcpy(&displacement, observed.data() + 1, sizeof(displacement));
        const auto target = static_cast<std::intptr_t>(process_event_address) + detour +
            displacement;
        std::uint8_t probe{};
        if (target <= 0 || !ReadBytes(static_cast<std::uintptr_t>(target), &probe, 1)) {
            return false;
        }
    }

    ready_ = true;
    skip = Skip::none;
    return true;
}

bool ModelAccess::ReadObjectSlot(
    const std::uint32_t index, std::uintptr_t& object,
    std::uint32_t& serial) const noexcept {
    object = 0;
    serial = 0;
    if (!registry_ready_ || chunk_size_ == 0) return false;
    // The registry grows while the process runs, so the count is read on every
    // lookup instead of being remembered from the last prepare.
    std::uint32_t count{};
    if (count_address_ == 0 || !ReadValue(count_address_, count) || count == 0 ||
        count > max_count_ || index >= count) {
        return false;
    }
    object_count_ = count;
    const std::int64_t item_stride = LayoutOffset(layout_, "objects.item_stride", 0);
    const std::int64_t object_offset = LayoutOffset(layout_, "objects.object_offset", -1);
    const std::int64_t serial_offset = LayoutOffset(layout_, "objects.serial_offset", -1);
    if (item_stride < 8 || object_offset < 0 || serial_offset < 0) return false;

    const std::uint32_t page = index / chunk_size_;
    const std::uint32_t slot = index % chunk_size_;
    if (page >= (num_chunks_ == 0 ? kMaxChunks : num_chunks_)) return false;
    std::uintptr_t entry{};
    if (!AddOffset(items_, static_cast<std::int64_t>(page) * 8, entry)) return false;
    const std::uintptr_t chunk = ReadPointer(entry);
    if (chunk == 0 || (chunk & (alignof(std::uintptr_t) - 1U)) != 0) return false;
    std::uintptr_t item{};
    if (!AddOffset(chunk, static_cast<std::int64_t>(slot) * item_stride, item)) return false;
    std::uintptr_t address{};
    return AddOffset(item, object_offset, address) && ReadValue(address, object) &&
        AddOffset(item, serial_offset, address) && ReadValue(address, serial);
}

bool ModelAccess::ResolveActor(
    const std::uint64_t frame_handle_id, const std::uint64_t class_id,
    ActorIdentity& out, Skip& skip) {
    out = ActorIdentity{};
    if (!ready_) {
        skip = Skip::layout_unavailable;
        return false;
    }
    skip = Skip::no_identity;
    const auto raw_index = static_cast<std::uint32_t>(frame_handle_id);
    if (frame_handle_id == 0 || raw_index == 0) return false;
    const std::uint32_t index = raw_index - 1U;
    std::uintptr_t object{};
    std::uint32_t serial{};
    if (!ReadObjectSlot(index, object, serial)) {
        skip = Skip::no_registry;
        return false;
    }
    if (object == 0 || serial == 0) return false;

    const std::int64_t internal_offset = LayoutOffset(layout_, "object.internal_index", -1);
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    if (internal_offset < 0 || class_offset < 0) {
        skip = Skip::layout_unavailable;
        return false;
    }
    std::uintptr_t address{};
    std::int32_t internal{};
    if (!AddOffset(object, internal_offset, address) || !ReadValue(address, internal)) {
        skip = Skip::read_failed;
        return false;
    }
    // The slot the snapshot named has to be the slot the object itself believes it
    // lives in, or the handle is not an identity the plugin may act on.
    if (internal < 0 || static_cast<std::uint32_t>(internal) != index) {
        skip = Skip::no_identity;
        return false;
    }
    if (class_id != 0) {
        if (!AddOffset(object, class_offset, address)) return false;
        const std::uintptr_t klass = ReadPointer(address);
        if (klass == 0) {
            skip = Skip::read_failed;
            return false;
        }
        if (!AddOffset(klass, internal_offset, address)) return false;
        std::int32_t class_index{};
        if (!ReadValue(address, class_index)) {
            skip = Skip::read_failed;
            return false;
        }
        if (class_index < 0 ||
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(class_index) + 1U) !=
                class_id) {
            skip = Skip::class_mismatch;
            return false;
        }
    }
    out = ActorIdentity{object, index, serial, true};
    skip = Skip::none;
    return true;
}

bool ModelAccess::IdentityHeld(const ActorIdentity& identity, Skip& skip) {
    skip = Skip::identity_stale;
    if (!ready_ || !identity.valid) return false;
    std::uintptr_t object{};
    std::uint32_t serial{};
    if (!ReadObjectSlot(identity.index, object, serial)) return false;
    // The object address alone is not enough: a freed object's address can be
    // reused, the registry serial is what distinguishes the two.
    if (object != identity.actor || serial != identity.serial) return false;
    const std::int64_t internal_offset = LayoutOffset(layout_, "object.internal_index", -1);
    std::uintptr_t address{};
    std::int32_t internal{};
    if (internal_offset < 0 || !AddOffset(identity.actor, internal_offset, address) ||
        !ReadValue(address, internal) || internal < 0 ||
        static_cast<std::uint32_t>(internal) != identity.index) {
        return false;
    }
    skip = Skip::none;
    return true;
}

bool ModelAccess::OwnedBy(
    const std::uintptr_t object, const std::uintptr_t owner) {
    const std::int64_t outer_offset = LayoutOffset(layout_, "object.outer", -1);
    std::uintptr_t address{};
    return object != 0 && owner != 0 && outer_offset >= 0 &&
        AddOffset(object, outer_offset, address) && ReadPointer(address) == owner;
}

bool ModelAccess::ActorHidden(
    const std::uintptr_t actor, const std::uintptr_t actor_class) const {
    if (actor == 0 || actor_class == 0) return false;
    const auto spec = layout_.properties.find("hidden");
    if (spec == layout_.properties.end()) return false;
    FieldView field;
    bool hidden{};
    if (FindProperty(actor_class, "hidden", field) &&
        PropertyMatches(field, spec->second) && ReadBoolProperty(actor, field, hidden)) {
        return hidden;
    }
    NoteUnfiltered(
        actor_class, "hidden",
        " has no reflected bHidden; hidden models cannot be filtered for this class");
    return false;
}

namespace {

// ASCII case-insensitive substring search, which is the comparison UE's own FName
// lookup uses for the names involved here.
bool ContainsFold(const std::string_view haystack, const std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    const auto fold = [](const char value) {
        return static_cast<char>(
            value >= 'A' && value <= 'Z' ? value - 'A' + 'a' : value);
    };
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        std::size_t index = 0;
        while (index < needle.size() && fold(haystack[start + index]) == fold(needle[index])) {
            ++index;
        }
        if (index == needle.size()) return true;
    }
    return false;
}

}  // namespace

bool ModelAccess::ClassIsCharacter(const std::uint64_t frame_class_id) const {
    if (!ready_ || frame_class_id == 0 || frame_class_id > 0xFFFFFFFFULL) return false;
    const auto id = static_cast<std::uint32_t>(frame_class_id);
    if (const auto found = character_classes_.find(id); found != character_classes_.end()) {
        return found->second;
    }
    bool character = false;
    std::uintptr_t object{};
    std::uint32_t serial{};
    if (ReadObjectSlot(id - 1U, object, serial) && object != 0) {
        // The engine class is what makes a character a character; this build names its
        // own character classes after the actor (`player_076_shinku_C`) and its crowd
        // actors `CrowdCharacterActor*`/`BP_NPC_*`, so the name is asked as well.
        const std::int64_t super = LayoutOffset(layout_, "ustruct.super_struct", -1);
        const std::string_view character_name = LayoutName(layout_, "character");
        std::uintptr_t current = object;
        for (std::size_t depth = 0; depth < kMaxClassDepth && current != 0; ++depth) {
            std::string text = ReadNameText(current);
            if ((!character_name.empty() && NameMatches(ReadNameId(current), character_name)) ||
                ContainsFold(text, "character") || ContainsFold(text, "npc")) {
                character = true;
                break;
            }
            std::uintptr_t next{};
            if (super < 0 || !AddOffset(current, super, next)) break;
            current = ReadPointer(next);
        }
    }
    if (character_classes_.size() < kMaxCharacterClasses) {
        character_classes_.emplace(id, character);
    }
    return character;
}

bool ModelAccess::ClassIsVehicle(const std::uintptr_t actor_class) const {
    if (actor_class == 0) return false;
    if (const auto found = vehicle_classes_.find(actor_class); found != vehicle_classes_.end()) {
        return found->second;
    }
    bool vehicle = false;
    const auto family = layout_.class_families.find("vehicle");
    const std::int64_t super = LayoutOffset(layout_, "ustruct.super_struct", -1);
    if (family != layout_.class_families.end() && super >= 0 && !family->second.empty()) {
        // One entry has to cover every variant this build names, so the family is
        // matched as a substring of each name along the chain rather than as a whole
        // class name.
        std::uintptr_t current = actor_class;
        for (std::size_t depth = 0; depth < kMaxClassDepth && current != 0; ++depth) {
            const std::string name = ReadNameText(current);
            for (const std::string& fragment : family->second) {
                if (ContainsFold(name, fragment)) {
                    vehicle = true;
                    break;
                }
            }
            if (vehicle) break;
            std::uintptr_t next{};
            if (!AddOffset(current, super, next)) break;
            current = ReadPointer(next);
        }
    }
    if (vehicle_classes_.size() < kMaxVehicleClasses) {
        vehicle_classes_.emplace(actor_class, vehicle);
    }
    return vehicle;
}

bool ModelAccess::ClassIsVehicleById(const std::uint64_t frame_class_id) const {
    if (!ready_ || frame_class_id == 0 || frame_class_id > 0xFFFFFFFFULL) return false;
    const auto id = static_cast<std::uint32_t>(frame_class_id);
    if (const auto found = vehicle_family_ids_.find(id); found != vehicle_family_ids_.end()) {
        return found->second;
    }
    bool vehicle = false;
    std::uintptr_t object{};
    std::uint32_t serial{};
    if (ReadObjectSlot(id - 1U, object, serial) && object != 0) {
        vehicle = ClassIsVehicle(object);
    }
    if (vehicle_family_ids_.size() < kMaxVehicleClasses) {
        vehicle_family_ids_.emplace(id, vehicle);
    }
    return vehicle;
}

bool ModelAccess::AttachedToAnotherVehicle(const std::uintptr_t root) const {
    const auto spec = layout_.properties.find("attach_parent");
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    const std::int64_t outer_offset = LayoutOffset(layout_, "object.outer", -1);
    if (root == 0 || spec == layout_.properties.end() || class_offset < 0 || outer_offset < 0) {
        return false;
    }
    std::uintptr_t address{};
    if (!AddOffset(root, class_offset, address)) return false;
    const std::uintptr_t root_class = ReadPointer(address);
    if (root_class == 0) return false;
    FieldView field;
    // A build that does not reflect AttachParent cannot answer the question, and the
    // answer is only ever used to leave an actor alone: allowing it is the safe way out.
    if (!FindProperty(root_class, "attach_parent", field) ||
        !PropertyMatches(field, spec->second)) {
        return false;
    }
    std::uintptr_t parent{};
    if (!ReadObjectProperty(root, field, parent) || parent == 0) return false;
    // The parent component's outer is the actor it belongs to, so this says whether the
    // root hangs under a component of some *other* actor, and the class of that actor
    // says whether that actor is a vehicle.
    std::uintptr_t parent_actor_address{};
    if (!AddOffset(parent, outer_offset, parent_actor_address)) return false;
    const std::uintptr_t parent_actor = ReadPointer(parent_actor_address);
    if (parent_actor == 0) return false;
    std::uintptr_t parent_class_address{};
    if (!AddOffset(parent_actor, class_offset, parent_class_address)) return false;
    const std::uintptr_t parent_class = ReadPointer(parent_class_address);
    if (parent_class == 0) return false;
    return ClassIsVehicle(parent_class);
}

const ModelAccess::ClassTraits* ModelAccess::TraitsFor(const std::uintptr_t class_object) {
    if (class_object == 0) return nullptr;
    if (const auto found = traits_.find(class_object); found != traits_.end()) {
        return &found->second;
    }
    ClassTraits traits;
    traits.mesh = ClassChainHasName(class_object, "mesh_component");
    traits.character = ClassChainHasName(class_object, "character");
    if (const auto family = layout_.class_families.find("non_visual_mesh");
        family != layout_.class_families.end()) {
        for (const std::string& name : family->second) {
            if (ClassChainContains(class_object, name)) {
                traits.non_visual = true;
                break;
            }
        }
    }
    if (const auto family = layout_.class_families.find("instanced_mesh");
        family != layout_.class_families.end()) {
        for (const std::string& name : family->second) {
            if (ClassChainContains(class_object, name)) {
                traits.instanced = true;
                break;
            }
        }
    }
    const auto inserted = traits_.emplace(class_object, traits);
    return &inserted.first->second;
}

bool ModelAccess::BuildFieldView(const std::uintptr_t field, FieldView& out) const {
    out = FieldView{};
    if (field == 0) return false;
    const std::int64_t class_offset = LayoutOffset(layout_, "ffield.class", -1);
    const std::int64_t name_offset = LayoutOffset(layout_, "ffield_class.name", -1);
    const std::int64_t element_size = LayoutOffset(layout_, "fproperty.element_size", -1);
    const std::int64_t array_dim = LayoutOffset(layout_, "fproperty.array_dim", -1);
    const std::int64_t offset_internal = LayoutOffset(layout_, "fproperty.offset_internal", -1);
    if (class_offset < 0 || name_offset < 0 || element_size < 0 || array_dim < 0 ||
        offset_internal < 0) {
        return false;
    }
    std::uintptr_t address{};
    if (!AddOffset(field, class_offset, address)) return false;
    const std::uintptr_t field_class = ReadPointer(address);
    if (field_class == 0 || !AddOffset(field_class, name_offset, address)) return false;
    std::uint32_t type_id{};
    if (!ReadValue(address, type_id) || !names_.Text(type_id, out.type) || out.type.empty()) {
        return false;
    }
    std::int32_t element{};
    std::int32_t dim{};
    std::int32_t offset{};
    if (!AddOffset(field, element_size, address) || !ReadValue(address, element) ||
        !AddOffset(field, array_dim, address) || !ReadValue(address, dim) ||
        !AddOffset(field, offset_internal, address) || !ReadValue(address, offset)) {
        return false;
    }
    out.field = field;
    out.element_size = element;
    out.array_dim = dim;
    out.offset = offset;

    if (out.type == "StructProperty") {
        const std::int64_t sub = LayoutOffset(layout_, "fstruct_property.struct", -1);
        if (sub < 0 || !AddOffset(field, sub, address)) return false;
        out.sub = ReadPointer(address);
    } else if (out.type == "ObjectProperty" || out.type == "ClassProperty" ||
               out.type == "WeakObjectProperty" || out.type == "SoftObjectProperty") {
        const std::int64_t sub = LayoutOffset(layout_, "fobject_property.property_class", -1);
        if (sub < 0 || !AddOffset(field, sub, address)) return false;
        out.sub = ReadPointer(address);
    }
    out.valid = true;
    return true;
}

bool ModelAccess::FindProperty(
    const std::uintptr_t root_struct, const std::string_view layout_key,
    FieldView& out) const {
    out = FieldView{};
    const auto spec = layout_.properties.find(layout_key);
    if (spec == layout_.properties.end() || spec->second.name.empty() || root_struct == 0) {
        return false;
    }
    const std::string_view expected = spec->second.name;
    const std::int64_t head = LayoutOffset(layout_, "ustruct.property_link", -1);
    const std::int64_t next = LayoutOffset(layout_, "ffield.next", -1);
    const std::int64_t name_offset = LayoutOffset(layout_, "ffield.name", -1);
    const std::int64_t super = LayoutOffset(layout_, "ustruct.super_struct", -1);
    if (head < 0 || next < 0 || name_offset < 0 || super < 0) return false;

    std::uintptr_t current = root_struct;
    for (std::size_t depth = 0; depth < kMaxClassDepth && current != 0; ++depth) {
        std::uintptr_t field{};
        if (AddOffset(current, head, field)) {
            field = ReadPointer(field);
            for (std::size_t walked = 0; field != 0 && walked < kMaxFieldChain; ++walked) {
                std::uintptr_t address{};
                std::uint32_t id{};
                if (AddOffset(field, name_offset, address) && ReadValue(address, id) &&
                    NameMatches(id, expected)) {
                    return BuildFieldView(field, out);
                }
                if (!AddOffset(field, next, address)) break;
                field = ReadPointer(address);
            }
        }
        std::uintptr_t address{};
        if (!AddOffset(current, super, address)) break;
        current = ReadPointer(address);
    }
    return false;
}

bool ModelAccess::ReadFunctionView(
    const std::uintptr_t function, FunctionView& out) const {
    out = FunctionView{};
    if (function == 0) return false;
    const std::int64_t num_parms = LayoutOffset(layout_, "ufunction.num_parms", -1);
    const std::int64_t parms_size = LayoutOffset(layout_, "ufunction.parms_size", -1);
    const std::int64_t return_offset =
        LayoutOffset(layout_, "ufunction.return_value_offset", -1);
    const std::int64_t head = LayoutOffset(layout_, "ustruct.property_link", -1);
    const std::int64_t next = LayoutOffset(layout_, "ffield.next", -1);
    if (num_parms < 0 || parms_size < 0 || return_offset < 0 || head < 0 || next < 0) {
        return false;
    }
    std::uintptr_t address{};
    std::uint8_t parms{};
    std::uint16_t size{};
    std::uint16_t return_value{};
    if (!AddOffset(function, num_parms, address) || !ReadValue(address, parms) ||
        !AddOffset(function, parms_size, address) || !ReadValue(address, size) ||
        !AddOffset(function, return_offset, address) || !ReadValue(address, return_value)) {
        return false;
    }
    out.function = function;
    out.num_parms = parms;
    out.parms_size = size;
    out.return_value_offset = return_value;

    if (!AddOffset(function, head, address)) return false;
    std::uintptr_t param = ReadPointer(address);
    for (std::size_t walked = 0; param != 0 && walked < kMaxParams; ++walked) {
        FieldView field;
        if (!BuildFieldView(param, field)) break;
        ParamView view;
        view.name = ReadFieldNameText(param);
        view.type = field.type;
        view.sub_id = ReadNameId(field.sub);
        view.offset = field.offset;
        view.element_size = field.element_size;
        out.params.push_back(std::move(view));
        if (!AddOffset(param, next, address)) break;
        param = ReadPointer(address);
    }
    out.valid = true;
    return true;
}

bool ModelAccess::FindFunction(
    const std::uintptr_t root_class, const std::string_view expected_name,
    FunctionView& out) const {
    out = FunctionView{};
    if (expected_name.empty() || root_class == 0) return false;
    const std::int64_t children = LayoutOffset(layout_, "ustruct.children", -1);
    const std::int64_t next = LayoutOffset(layout_, "ufield.next", -1);
    const std::int64_t super = LayoutOffset(layout_, "ustruct.super_struct", -1);
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    const std::int64_t name_offset = LayoutOffset(layout_, "object.name_offset", -1);
    if (children < 0 || next < 0 || super < 0 || class_offset < 0 || name_offset < 0) {
        return false;
    }

    std::uintptr_t current = root_class;
    for (std::size_t depth = 0; depth < kMaxClassDepth && current != 0; ++depth) {
        std::uintptr_t address{};
        if (AddOffset(current, children, address)) {
            std::uintptr_t field = ReadPointer(address);
            for (std::size_t walked = 0; field != 0 && walked < kMaxFieldChain; ++walked) {
                std::uintptr_t field_address{};
                std::uint32_t field_name{};
                if (AddOffset(field, name_offset, field_address) &&
                    ReadValue(field_address, field_name) &&
                    NameMatches(field_name, expected_name)) {
                    // A UFunction and not some other UField of the same name.
                    std::uintptr_t class_address{};
                    if (AddOffset(field, class_offset, class_address) &&
                        ClassChainHasName(ReadPointer(class_address), "function")) {
                        return ReadFunctionView(field, out);
                    }
                }
                if (!AddOffset(field, next, field_address)) break;
                field = ReadPointer(field_address);
            }
        }
        if (!AddOffset(current, super, address)) break;
        current = ReadPointer(address);
    }
    return false;
}

bool ModelAccess::PropertyMatches(const FieldView& field, const PropertySpec& spec) const {
    if (!field.valid || field.field == 0) return false;
    if (!spec.type.empty() && field.type != spec.type) return false;
    if (spec.element_size != 0 && field.element_size != spec.element_size) return false;
    if (!spec.struct_name.empty()) {
        if (field.sub == 0 || !NameMatches(ReadNameId(field.sub), spec.struct_name)) {
            return false;
        }
    }
    if (!spec.class_name.empty()) {
        if (field.sub == 0 || !ClassChainContains(field.sub, spec.class_name)) {
            return false;
        }
    }
    if (!spec.inner_type.empty()) {
        const std::int64_t inner = LayoutOffset(layout_, "farray_property.inner", -1);
        std::uintptr_t address{};
        FieldView inner_field;
        if (inner < 0 || !AddOffset(field.field, inner, address) ||
            !BuildFieldView(ReadPointer(address), inner_field) ||
            inner_field.type != spec.inner_type) {
            return false;
        }
        if (!spec.inner_class_name.empty()) {
            if (inner_field.sub == 0 ||
                !ClassChainContains(inner_field.sub, spec.inner_class_name)) {
                return false;
            }
        }
    }
    return true;
}

bool ModelAccess::SignatureMatches(
    const FunctionView& signature, const FunctionSpec& spec) const {
    if (!signature.valid || signature.function == 0 || spec.params.empty()) return false;
    if (spec.exact_size) {
        if (signature.num_parms != spec.num_parms ||
            signature.parms_size != spec.parms_size ||
            signature.params.size() != spec.params.size()) {
            return false;
        }
    } else {
        if (signature.num_parms < spec.params.size() ||
            signature.parms_size == 0 || signature.parms_size > kMaxParmsBytes) {
            return false;
        }
    }
    for (std::size_t index = 0; index < spec.params.size(); ++index) {
        const ParamView& live = signature.params[index];
        const FunctionParamSpec& expected = spec.params[index];
        if (live.name != expected.name || live.offset != expected.offset) return false;
        if (!expected.type.empty() && live.type != expected.type) return false;
        if (expected.element_size != 0 && live.element_size != expected.element_size) {
            return false;
        }
        if (!expected.struct_name.empty()) {
            if (!NameMatches(live.sub_id, expected.struct_name)) return false;
        }
    }
    // The buffer has to cover the parameter the plugin writes, whatever the
    // function's own reported size is.
    const FunctionParamSpec& first = spec.params.front();
    const auto end = first.offset + first.element_size;
    if (end <= 0 || static_cast<std::uint64_t>(end) > signature.parms_size) return false;
    return true;
}

bool ModelAccess::ReadBoolProperty(
    const std::uintptr_t owner, const FieldView& field, bool& out) const noexcept {
    const std::int64_t byte_offset = LayoutOffset(layout_, "fbool_property.byte_offset", -1);
    const std::int64_t byte_mask = LayoutOffset(layout_, "fbool_property.byte_mask", -1);
    if (byte_offset < 0 || byte_mask < 0 || field.field == 0 || owner == 0) return false;
    std::uintptr_t address{};
    std::uint8_t offset{};
    std::uint8_t mask{};
    if (!AddOffset(field.field, byte_offset, address) || !ReadValue(address, offset) ||
        !AddOffset(field.field, byte_mask, address) || !ReadValue(address, mask) ||
        mask == 0) {
        return false;
    }
    if (!AddOffset(owner, field.offset + static_cast<std::int64_t>(offset), address)) {
        return false;
    }
    std::uint8_t value{};
    if (!ReadValue(address, value)) return false;
    out = (value & mask) != 0;
    return true;
}

bool ModelAccess::ReadObjectProperty(
    const std::uintptr_t owner, const FieldView& field, std::uintptr_t& out) const noexcept {
    out = 0;
    std::uintptr_t address{};
    if (owner == 0 || field.field == 0 || !AddOffset(owner, field.offset, address)) {
        return false;
    }
    out = ReadPointer(address);
    return true;
}

bool ModelAccess::ReadScaleProperty(
    const std::uintptr_t owner, const FieldView& field, Vector3& out) const noexcept {
    std::uintptr_t address{};
    if (owner == 0 || !AddOffset(owner, field.offset, address)) return false;
    return ReadBytes(address, out.data(), sizeof(out));
}

bool ModelAccess::ReadRotationProperty(
    const std::uintptr_t owner, const FieldView& field, Vector3& out) const noexcept {
    // An FRotator is three doubles just like an FVector, so the same read does the work.
    return ReadScaleProperty(owner, field, out);
}

bool ModelAccess::ResolveComponentLayout(
    const std::uintptr_t class_object, const std::string_view property_key,
    const std::string_view function_key, ComponentLayout& out, Skip& skip) {
    out = ComponentLayout{};
    const auto property = layout_.properties.find(property_key);
    if (property == layout_.properties.end()) {
        skip = Skip::layout_unavailable;
        return false;
    }
    FieldView value_field;
    skip = Skip::no_visual;
    if (!FindProperty(class_object, property_key, value_field)) {
        NoteMissingName(class_object, "property", property_key);
        return false;
    }
    skip = Skip::signature_mismatch;
    if (!PropertyMatches(value_field, property->second)) return false;

    const auto family = layout_.functions.find(function_key);
    if (family == layout_.functions.end() || family->second.empty()) {
        skip = Skip::layout_unavailable;
        return false;
    }
    // "The build does not reflect this function at all" and "the build reflects a
    // function whose signature is not the one the resource expects" are different
    // problems, so they are reported apart.
    bool found_any_function = false;
    for (const FunctionSpec& spec : family->second) {
        FunctionView signature;
        if (!FindFunction(class_object, spec.name, signature)) continue;
        found_any_function = true;
        if (!SignatureMatches(signature, spec)) continue;
        const FunctionParamSpec& first = spec.params.front();
        const std::uint32_t parms_size =
            spec.exact_size ? spec.parms_size : signature.parms_size;
        if (parms_size < static_cast<std::uint64_t>(first.offset) + sizeof(Vector3)) {
            skip = Skip::parm_too_small;
            continue;
        }
        out.value_offset = value_field.offset;
        out.function = signature.function;
        out.parms_size = parms_size;
        out.param_offset = first.offset;
        out.skip = Skip::none;
        out.valid = true;
        skip = Skip::none;
        return true;
    }
    skip = found_any_function ? Skip::signature_mismatch : Skip::function_missing;
    if (!found_any_function) NoteMissingName(class_object, "function", function_key);
    out.skip = skip;
    return false;
}

const ModelAccess::ComponentLayout* ModelAccess::LayoutFor(
    const std::uintptr_t class_object, Skip& skip) {
    skip = Skip::no_visual;
    if (class_object == 0) return nullptr;
    if (const auto found = component_layouts_.find(class_object);
        found != component_layouts_.end()) {
        skip = found->second.skip;
        return found->second.valid ? &found->second : nullptr;
    }
    ComponentLayout resolved;
    if (!ResolveComponentLayout(
            class_object, "relative_scale", "set_relative_scale", resolved, skip)) {
        // Cache the failure too: a class that does not validate does not become
        // valid a frame later, and retrying its full property walk every update
        // would burn the whole budget.
        ComponentLayout failure;
        failure.skip = skip;
        component_layouts_.emplace(class_object, failure);
        return nullptr;
    }
    const auto inserted = component_layouts_.emplace(class_object, resolved);
    skip = Skip::none;
    return &inserted.first->second;
}

// The rotation path caches separately from the scale path: a class whose rotation cannot
// be written is not a class whose scale cannot be written.
const ModelAccess::ComponentLayout* ModelAccess::RotationLayoutFor(
    const std::uintptr_t class_object, Skip& skip) {
    skip = Skip::no_visual;
    if (class_object == 0) return nullptr;
    if (const auto found = rotation_layouts_.find(class_object);
        found != rotation_layouts_.end()) {
        skip = found->second.skip;
        return found->second.valid ? &found->second : nullptr;
    }
    ComponentLayout resolved;
    if (!ResolveComponentLayout(
            class_object, "relative_rotation", "set_relative_rotation", resolved, skip)) {
        ComponentLayout failure;
        failure.skip = skip;
        rotation_layouts_.emplace(class_object, failure);
        return nullptr;
    }
    const auto inserted = rotation_layouts_.emplace(class_object, resolved);
    skip = Skip::none;
    return &inserted.first->second;
}

bool ModelAccess::ReadScale(
    const std::uintptr_t component, Vector3& scale, Skip& skip) {
    scale = Vector3{1.0, 1.0, 1.0};
    if (!ready_) {
        skip = Skip::layout_unavailable;
        return false;
    }
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    std::uintptr_t address{};
    if (component == 0 || class_offset < 0 || !AddOffset(component, class_offset, address)) {
        skip = Skip::read_failed;
        return false;
    }
    const ComponentLayout* layout = LayoutFor(ReadPointer(address), skip);
    if (layout == nullptr) return false;
    skip = Skip::read_failed;
    if (!ReadScaleProperty(component, FieldView{0, layout->value_offset, 0, 0, {}, 0, true},
                           scale)) {
        return false;
    }
    if (!IsFinite(scale)) return false;
    skip = Skip::none;
    return true;
}

bool ModelAccess::ApplyScale(
    const std::uintptr_t component, const Vector3& scale, Skip& skip) {
    if (!ready_) {
        skip = Skip::layout_unavailable;
        return false;
    }
    if (!IsFinite(scale)) {
        skip = Skip::write_failed;
        return false;
    }
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    std::uintptr_t address{};
    if (component == 0 || class_offset < 0 || !AddOffset(component, class_offset, address)) {
        skip = Skip::write_failed;
        return false;
    }
    const ComponentLayout* layout = LayoutFor(ReadPointer(address), skip);
    if (layout == nullptr) return false;
    if (layout->parms_size == 0 || layout->parms_size > kMaxParmsBytes ||
        static_cast<std::uint64_t>(layout->param_offset) + sizeof(Vector3) >
            layout->parms_size) {
        skip = Skip::parm_too_small;
        return false;
    }
    // The parameter buffer is zeroed, which also leaves a variadic wrapper's sweep
    // and teleport flags false, and only the scale the plugin computed is written
    // into it.
    std::vector<std::uint8_t> parms(layout->parms_size, 0);
    std::memcpy(parms.data() + layout->param_offset, scale.data(), sizeof(Vector3));
    skip = Skip::write_failed;
    if (!call_.ProcessEvent(component, layout->function, parms.data(), parms.size())) {
        return false;
    }
    // Verified by reading the property back: the reflected write is only believed
    // once the component carries it.
    Vector3 live{};
    if (!ReadScaleProperty(component, FieldView{0, layout->value_offset, 0, 0, {}, 0, true},
                           live)) {
        return false;
    }
    if (!Equal(live, scale)) {
        skip = Skip::not_verified;
        return false;
    }
    skip = Skip::none;
    return true;
}

bool ModelAccess::ReadRotation(
    const std::uintptr_t component, Vector3& rotator, Skip& skip) {
    rotator = Vector3{};
    if (!ready_) {
        skip = Skip::layout_unavailable;
        return false;
    }
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    std::uintptr_t address{};
    if (component == 0 || class_offset < 0 || !AddOffset(component, class_offset, address)) {
        skip = Skip::read_failed;
        return false;
    }
    const ComponentLayout* layout = RotationLayoutFor(ReadPointer(address), skip);
    if (layout == nullptr) return false;
    skip = Skip::read_failed;
    if (!ReadRotationProperty(
            component, FieldView{0, layout->value_offset, 0, 0, {}, 0, true}, rotator)) {
        return false;
    }
    if (!IsFinite(rotator)) return false;
    skip = Skip::none;
    return true;
}

bool ModelAccess::ApplyRotation(
    const std::uintptr_t component, const Vector3& rotator, Skip& skip) {
    if (!ready_) {
        skip = Skip::layout_unavailable;
        return false;
    }
    if (!IsFinite(rotator)) {
        skip = Skip::write_failed;
        return false;
    }
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    std::uintptr_t address{};
    if (component == 0 || class_offset < 0 || !AddOffset(component, class_offset, address)) {
        skip = Skip::write_failed;
        return false;
    }
    const ComponentLayout* layout = RotationLayoutFor(ReadPointer(address), skip);
    if (layout == nullptr) return false;
    if (layout->parms_size == 0 || layout->parms_size > kMaxParmsBytes ||
        static_cast<std::uint64_t>(layout->param_offset) + sizeof(Vector3) >
            layout->parms_size) {
        skip = Skip::parm_too_small;
        return false;
    }
    // The buffer is zeroed, which is what keeps the blueprint wrapper's sweep and
    // teleport flags false and its inline hit result an empty struct.
    std::vector<std::uint8_t> parms(layout->parms_size, 0);
    std::memcpy(parms.data() + layout->param_offset, rotator.data(), sizeof(Vector3));
    skip = Skip::write_failed;
    if (!call_.ProcessEvent(component, layout->function, parms.data(), parms.size())) {
        return false;
    }
    Vector3 live{};
    if (!ReadRotationProperty(
            component, FieldView{0, layout->value_offset, 0, 0, {}, 0, true}, live)) {
        return false;
    }
    // The engine is free to hand the same pose back with the yaw wrapped, so the angles
    // are compared as a pose rather than as a number.
    if (!SameRotation(live, rotator)) {
        skip = Skip::not_verified;
        return false;
    }
    skip = Skip::none;
    return true;
}

void ModelAccess::NoteSpinUnavailable(const std::uintptr_t component) {
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    std::uintptr_t address{};
    if (component == 0 || class_offset < 0 || !AddOffset(component, class_offset, address)) {
        return;
    }
    const std::uintptr_t klass = ReadPointer(address);
    if (klass == 0) return;
    NoteUnfiltered(klass, "spin", " cannot be spun; the jelly scale is unaffected");
}

bool ModelAccess::ComponentIsUsable(
    const std::uintptr_t actor, const std::uintptr_t actor_class,
    const std::uintptr_t component, const bool vehicle, const bool allow_simulated_body,
    bool& simulating_part, Skip& skip) {
    skip = Skip::no_visual;
    if (actor == 0 || component == 0) return false;
    // An actor the game has hidden is not in the world: the three party members that
    // stand next to the local player carry AActor::bHidden, and scaling them would
    // only spend the model budget on something nobody can see. The vehicle family is
    // the exception, and deliberately so: every car actor in this build carries the
    // flag while its own meshes report themselves visible and the game renders the car
    // (see the layout resource's vehicle family), so the flag says nothing about what
    // is on screen there.
    if (!vehicle && ActorHidden(actor, actor_class)) {
        skip = Skip::hidden;
        return false;
    }
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    std::uintptr_t address{};
    if (class_offset < 0 || !AddOffset(component, class_offset, address)) {
        skip = Skip::read_failed;
        return false;
    }
    const std::uintptr_t klass = ReadPointer(address);
    const ClassTraits* traits = TraitsFor(klass);
    if (traits == nullptr || klass == 0) {
        skip = Skip::read_failed;
        return false;
    }
    // A mesh-derived component that renders UI in the world is not a model. The crowd
    // NPCs carry a name-plate bubble next to their body mesh, so it is ignored here
    // instead of being counted as a second visual root.
    if (traits->non_visual) return false;
    // A visual model is a mesh component; an instanced mesh cannot be scaled
    // without moving every instance, so it is never a target.
    if (!traits->mesh) return false;
    if (traits->instanced) {
        // Every instance shares one component transform, so scaling it would move
        // instances the user never aimed at.
        skip = Skip::instanced;
        return false;
    }
    if (!OwnedBy(component, actor)) {
        skip = Skip::cross_actor;
        return false;
    }

    // Static scenery is not a target: a level's buildings and props must not squash.
    // The host's entity flags are not usable for this (see jelly_selection.hpp), so the
    // component's own reflected Mobility decides. A build that does not reflect it is
    // reported once per class and then treated as movable, because a scope question
    // must not silently turn the whole effect off.
    if (const auto mobility_spec = layout_.properties.find("mobility");
        mobility_spec != layout_.properties.end()) {
        FieldView mobility;
        std::uintptr_t mobility_address{};
        std::uint8_t value{};
        if (FindProperty(klass, "mobility", mobility) &&
            PropertyMatches(mobility, mobility_spec->second) &&
            AddOffset(component, mobility.offset, mobility_address) &&
            ReadValue(mobility_address, value)) {
            if (value == kComponentMobilityStatic) {
                skip = Skip::static_mesh;
                return false;
            }
        } else {
            NoteUnfilteredClass(klass);
        }
    }

    // A component whose visibility cannot be read is still a target; only one that
    // reports itself hidden is not.
    FieldView visible;
    bool is_visible{true};
    if (FindProperty(klass, "visible", visible) && visible.type == "BoolProperty" &&
        ReadBoolProperty(component, visible, is_visible) && !is_visible) {
        skip = Skip::invisible;
        return false;
    }

    // Collision and physics are read from the component's own FBodyInstance. A mesh
    // that carries either is skipped, and so is a mesh whose state cannot be read at
    // all: scaling a colliding body is what the plan forbids. A vehicle is the one
    // family where collision is not a refusal - its body mesh carries the vehicle's
    // own collision and the whole vehicle is scaled as one unit from its root - but
    // physics simulation still is, and a body that cannot be read is still refused.
    const auto body_spec = layout_.properties.find("body_instance");
    if (body_spec == layout_.properties.end()) {
        skip = Skip::layout_unavailable;
        return false;
    }
    FieldView body_field;
    if (!FindProperty(klass, "body_instance", body_field)) {
        NoteMissingName(klass, "property", "body_instance");
        skip = Skip::collision;
        return false;
    }
    if (!PropertyMatches(body_field, body_spec->second) || body_field.sub == 0) {
        skip = Skip::collision;
        return false;
    }
    std::uintptr_t body{};
    if (!AddOffset(component, body_field.offset, body)) {
        skip = Skip::read_failed;
        return false;
    }
    FieldView collision;
    if (!FindProperty(body_field.sub, "collision_enabled", collision)) {
        NoteMissingName(klass, "property", "collision_enabled");
        skip = Skip::collision;
        return false;
    }
    std::uintptr_t field_address{};
    std::uint8_t collision_enabled{};
    if (!AddOffset(body, collision.offset, field_address) ||
        !ReadValue(field_address, collision_enabled)) {
        skip = Skip::read_failed;
        return false;
    }
    if (collision_enabled != 0 && !vehicle) {
        skip = Skip::collision;
        return false;
    }
    FieldView physics;
    bool simulating{};
    if (!FindProperty(body_field.sub, "simulate_physics", physics) ||
        !ReadBoolProperty(body, physics, simulating)) {
        NoteMissingName(klass, "property", "simulate_physics");
        skip = Skip::physics;
        return false;
    }
    if (simulating) {
        // A simulating body may only ever be scaled as the whole body of a vehicle, and
        // only while the window's switch says so: the car the player drives carries its
        // chassis rigid body on the body mesh, and refusing it left that car still while
        // its accessories moved. The fact is remembered either way, because the
        // whole-model path of the vehicle family decides what to do with it.
        simulating_part = true;
        if (!allow_simulated_body) {
            skip = Skip::physics;
            return false;
        }
    }
    skip = Skip::none;
    return true;
}

bool ModelAccess::CollectAttachTree(
    const std::uintptr_t root, std::vector<std::uintptr_t>& components,
    std::unordered_map<std::uintptr_t, std::uintptr_t>& parents) const {
    components.clear();
    parents.clear();
    if (root == 0) return false;
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    const auto attach_spec = layout_.properties.find("attach_children");
    const std::int64_t data_offset = LayoutOffset(layout_, "tarray.data", -1);
    const std::int64_t num_offset = LayoutOffset(layout_, "tarray.num", -1);
    if (class_offset < 0 || attach_spec == layout_.properties.end() || data_offset < 0 ||
        num_offset < 0) {
        return false;
    }
    std::uintptr_t address{};
    if (!AddOffset(root, class_offset, address)) return false;
    const std::uintptr_t klass = ReadPointer(address);
    FieldView attach;
    if (!FindProperty(klass, "attach_children", attach)) {
        NoteMissingName(klass, "property", "attach_children");
        return false;
    }
    if (!PropertyMatches(attach, attach_spec->second)) {
        return false;
    }

    struct Frame {
        std::uintptr_t component;
        std::size_t depth;
    };
    std::vector<Frame> stack;
    components.push_back(root);
    stack.push_back(Frame{root, 0});
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (frame.depth >= kMaximumVisualDepth) continue;
        std::uintptr_t array{};
        if (!AddOffset(frame.component, attach.offset, array)) return false;
        std::uintptr_t data{};
        std::int32_t count{};
        if (!AddOffset(array, data_offset, address) || !ReadValue(address, data) ||
            !AddOffset(array, num_offset, address) || !ReadValue(address, count)) {
            return false;
        }
        if (data == 0 || count <= 0) continue;
        const std::int32_t bounded = (std::min)(
            count, static_cast<std::int32_t>(kMaximumVisualChildren));
        for (std::int32_t index = 0; index < bounded; ++index) {
            std::uintptr_t slot{};
            if (!AddOffset(data, static_cast<std::int64_t>(index) * 8, slot)) return false;
            const std::uintptr_t child = ReadPointer(slot);
            if (child == 0 || parents.find(child) != parents.end()) continue;
            if (components.size() >= kMaxTreeComponents) return true;
            parents.emplace(child, frame.component);
            components.push_back(child);
            stack.push_back(Frame{child, frame.depth + 1});
        }
    }
    return true;
}

bool ModelAccess::ResolveVisual(
    const std::uintptr_t actor, const bool allow_simulated_body, VisualTarget& out,
    Skip& skip) {
    out = VisualTarget{};
    if (!ready_) {
        skip = Skip::layout_unavailable;
        return false;
    }
    const std::int64_t class_offset = LayoutOffset(layout_, "object.class", -1);
    const auto root_spec = layout_.properties.find("root_component");
    skip = Skip::no_root;
    if (actor == 0 || class_offset < 0 || root_spec == layout_.properties.end()) {
        return false;
    }
    std::uintptr_t address{};
    if (!AddOffset(actor, class_offset, address)) return false;
    const std::uintptr_t klass = ReadPointer(address);
    if (klass == 0) return false;
    FieldView root_field;
    if (!FindProperty(klass, "root_component", root_field)) {
        NoteMissingName(klass, "property", "root_component");
        return false;
    }
    if (!PropertyMatches(root_field, root_spec->second)) {
        return false;
    }
    std::uintptr_t root{};
    if (!ReadObjectProperty(actor, root_field, root)) {
        skip = Skip::read_failed;
        return false;
    }
    if (root == 0) return false;

    // An actor whose root component hangs under another vehicle's component is a part of
    // that vehicle - the spoilers and lamp actors of this build are exactly that - and it
    // follows the vehicle as the vehicle is scaled. It is refused here, before anything
    // is written, so it cannot take a model slot of its own: scaling it separately is
    // what made a car whose body was skipped look as if only its accessories moved.
    if (AttachedToAnotherVehicle(root)) {
        skip = Skip::vehicle_follower;
        return false;
    }

    std::vector<std::uintptr_t> components;
    std::unordered_map<std::uintptr_t, std::uintptr_t> parents;
    if (!CollectAttachTree(root, components, parents)) {
        skip = Skip::read_failed;
        return false;
    }

    // The visual forest: every mesh component that may be scaled, and the reason
    // the first rejected one was rejected, so the window can say why a model the
    // user can see was left alone.
    std::vector<std::uintptr_t> candidates;
    Skip observed = Skip::no_visual;
    const bool vehicle = ClassIsVehicle(klass);
    const bool scale_simulated_body = vehicle && allow_simulated_body;
    // A part that simulates physics is the one thing the whole-model path of the vehicle
    // family has to decide about: scaling the root scales that body too, which is what
    // the window's switch is for.
    bool simulating_part = false;
    for (const std::uintptr_t component : components) {
        Skip why = Skip::none;
        if (ComponentIsUsable(
                actor, klass, component, vehicle, scale_simulated_body, simulating_part,
                why)) {
            candidates.push_back(component);
            continue;
        }
        if (observed == Skip::no_visual && why == Skip::hidden) observed = why;
        if (observed == Skip::no_visual && why == Skip::collision) observed = why;
        if (observed == Skip::no_visual && why == Skip::physics) observed = why;
        if (observed == Skip::no_visual && why == Skip::instanced) observed = why;
        if (observed == Skip::no_visual && why == Skip::static_mesh) observed = why;
        if (observed == Skip::no_visual && why == Skip::invisible) observed = why;
        if (observed == Skip::no_visual && why == Skip::cross_actor) observed = why;
        if (observed == Skip::no_visual && why == Skip::read_failed) observed = why;
    }
    if (candidates.empty()) {
        skip = observed;
        return false;
    }

    // A vehicle is one model, not a forest of parts: its body, wheels and lights hang
    // under a single root - sometimes the body mesh itself, sometimes a plain scene
    // node - and the parts that belong to child actors (kits, lights, spoilers) are
    // attached under that same root. Scaling the root scales all of them together and
    // keeps their offsets, which is what the multi-part rule below cannot do for the
    // sibling layouts of this family.
    if (vehicle) {
        // A simulating body is what the window's switch decides about: with it off the
        // vehicle is refused as before, with it on the whole car is scaled - its physics
        // body included - and the class is named once in the log so the choice is visible.
        if (simulating_part) {
            if (!allow_simulated_body) {
                skip = Skip::physics;
                return false;
            }
            NoteUnfiltered(
                klass, "simulating_body",
                " is scaled with a simulating body; its physics body scales with it");
        }
        Vector3 base{};
        if (!ReadScale(root, base, skip)) return false;
        if (!IsUsableScale(base)) {
            skip = Skip::read_failed;
            return false;
        }
        Vector3 rotation{};
        Skip rotation_skip = Skip::none;
        const bool rotation_valid = ReadRotation(root, rotation, rotation_skip);
        out.component = root;
        out.base = base;
        out.rotation = rotation;
        out.rotation_valid = rotation_valid;
        out.valid = true;
        skip = Skip::none;
        return true;
    }

    const auto is_candidate = [&candidates](const std::uintptr_t component) {
        return std::find(candidates.begin(), candidates.end(), component) !=
            candidates.end();
    };

    // Only the topmost visual components are scaled: a component whose ancestor is
    // also a candidate follows its parent instead of being scaled on its own, and
    // a model whose visual geometry does not hang under one such root is skipped
    // rather than having its parts moved apart.
    std::vector<std::uintptr_t> tops;
    for (const std::uintptr_t component : candidates) {
        bool nested = false;
        for (auto parent = parents.find(component); parent != parents.end();
             parent = parents.find(parent->second)) {
            if (is_candidate(parent->second)) {
                nested = true;
                break;
            }
        }
        if (!nested) tops.push_back(component);
    }
    if (tops.empty()) {
        skip = Skip::no_visual;
        return false;
    }
    if (tops.size() > 1) {
        skip = Skip::multi_part;
        return false;
    }

    const std::uintptr_t component = tops.front();
    Vector3 base{};
    if (!ReadScale(component, base, skip)) return false;
    if (!IsUsableScale(base)) {
        skip = Skip::read_failed;
        return false;
    }
    // The rotation is read here, once, so the spin and its restore work from the value the
    // model carried at takeover. A build that cannot report it simply never spins.
    Vector3 rotation{};
    Skip rotation_skip = Skip::none;
    const bool rotation_valid = ReadRotation(component, rotation, rotation_skip);
    out.component = component;
    out.base = base;
    out.rotation = rotation;
    out.rotation_valid = rotation_valid;
    out.valid = true;
    skip = Skip::none;
    return true;
}

// --- recovery records ------------------------------------------------------------

std::size_t RecoveryLedger::root_count() const noexcept {
    std::size_t count = 0;
    for (const RecoveryRecord& record : records_) {
        if (record.component != 0) ++count;
    }
    return count;
}

std::size_t RecoveryLedger::pending_restore() const noexcept {
    std::size_t count = 0;
    for (const RecoveryRecord& record : records_) {
        if (record.state == RecordState::restore_pending) ++count;
    }
    return count;
}

std::size_t RecoveryLedger::written_count() const noexcept {
    std::size_t count = 0;
    for (const RecoveryRecord& record : records_) {
        if (record.written) ++count;
    }
    return count;
}

RecoveryRecord* RecoveryLedger::Find(const std::uint64_t model_key) noexcept {
    for (RecoveryRecord& record : records_) {
        if (record.model_key == model_key) return &record;
    }
    return nullptr;
}

const RecoveryRecord* RecoveryLedger::Find(const std::uint64_t model_key) const noexcept {
    for (const RecoveryRecord& record : records_) {
        if (record.model_key == model_key) return &record;
    }
    return nullptr;
}

bool RecoveryLedger::Add(
    const RecoveryRecord& record, const std::size_t model_cap) noexcept {
    if (record.model_key == 0 || Find(record.model_key) != nullptr) return false;
    if (records_.size() >= model_cap) return false;
    if (record.component != 0 && root_count() + 1U > RootBudget(model_cap)) return false;
    records_.push_back(record);
    return true;
}

void RecoveryLedger::Erase(const std::uint64_t model_key) noexcept {
    for (auto record = records_.begin(); record != records_.end(); ++record) {
        if (record->model_key == model_key) {
            records_.erase(record);
            return;
        }
    }
}

void RecoveryLedger::Clear() noexcept { records_.clear(); }

void RecoveryLedger::MarkRestore(const std::uint64_t model_key) noexcept {
    if (RecoveryRecord* record = Find(model_key); record != nullptr) {
        record->state = RecordState::restore_pending;
    }
}

std::size_t RecoveryLedger::MarkAllRestore() noexcept {
    for (RecoveryRecord& record : records_) {
        record.state = RecordState::restore_pending;
    }
    return pending_restore();
}

std::vector<std::uint64_t> RecoveryLedger::restore_order() const {
    std::vector<std::uint64_t> keys;
    keys.reserve(records_.size());
    for (const RecoveryRecord& record : records_) {
        if (record.state == RecordState::restore_pending) keys.push_back(record.model_key);
    }
    return keys;
}

std::vector<std::uint64_t> RecoveryLedger::controlled_keys() const {
    std::vector<std::uint64_t> keys;
    keys.reserve(records_.size());
    for (const RecoveryRecord& record : records_) keys.push_back(record.model_key);
    return keys;
}

std::vector<std::uint64_t> RecoveryLedger::held_class_ids() const {
    std::vector<std::uint64_t> classes;
    classes.reserve(records_.size());
    for (const RecoveryRecord& record : records_) classes.push_back(record.class_id);
    return classes;
}

std::size_t RecoveryLedger::MarkLeavers(
    const std::vector<std::uint64_t>& still_wanted) noexcept {
    std::size_t marked = 0;
    for (RecoveryRecord& record : records_) {
        if (std::find(still_wanted.begin(), still_wanted.end(), record.model_key) !=
            still_wanted.end()) {
            continue;
        }
        if (record.state != RecordState::restore_pending) {
            record.state = RecordState::restore_pending;
            ++marked;
        }
    }
    return marked;
}

namespace {

// One spin step of a model the effect owns. Best effort by design: with the switch off a
// rotation the effect wrote is handed back once, and a rotation that cannot be read,
// written or verified stops the spin for this model - it never becomes a conflict, because
// dropping the record would take the jelly scale with it. The class is named once.
bool SpinRecord(
    ModelAccess& model, RecoveryRecord& record, const Settings& settings,
    const double elapsed, Skip& skip) {
    skip = Skip::none;
    if (record.component == 0) return false;
    if (settings.spin) {
        if (record.spin_blocked) return false;
    } else if (!record.rotation_written) {
        // Nothing of ours is on the rotation and nothing is asked for: no read, no write.
        return false;
    }
    if (settings.spin && !record.rotation_valid) {
        record.spin_blocked = true;
        model.NoteSpinUnavailable(record.component);
        return false;
    }
    const Vector3 wanted = settings.spin
        ? SpinRotator(
              record.base_rotation,
              SpinDegrees(settings.spin_degrees_per_second, elapsed, record.phase))
        : record.base_rotation;
    Vector3 live{};
    if (!model.ReadRotation(record.component, live, skip)) {
        if (settings.spin) {
            record.spin_blocked = true;
            model.NoteSpinUnavailable(record.component);
        }
        return false;
    }
    if (SameRotation(live, wanted)) {
        // The pose is already there. With the switch off that means the model carries its
        // own rotation again, so the record no longer holds one of ours.
        record.applied_rotation = wanted;
        record.rotation_written = settings.spin;
        return false;
    }
    if (!model.ApplyRotation(record.component, wanted, skip)) {
        if (settings.spin) {
            record.spin_blocked = true;
            model.NoteSpinUnavailable(record.component);
        }
        return false;
    }
    record.applied_rotation = wanted;
    record.rotation_written = settings.spin;
    return true;
}

}  // namespace

RestoreStep RestoreRecord(
    ModelAccess& model, RecoveryRecord& record, Skip& skip) {
    skip = Skip::none;
    // A destroyed object, or a slot whose serial changed, is never written to: that
    // address is not this model's any more.
    if (!model.IdentityHeld(record.identity, skip)) return RestoreStep::identity_lost;
    // The rotation goes back first, and only while the component still carries what the
    // effect wrote: a rotation the game has taken back is left alone rather than turned
    // into a conflict that would keep the scale from being restored as well.
    if (record.rotation_written) {
        Vector3 live_rotation{};
        Skip rotation_skip = Skip::none;
        if (model.ReadRotation(record.component, live_rotation, rotation_skip)) {
            if (SameRotation(live_rotation, record.applied_rotation)) {
                if (model.ApplyRotation(
                        record.component, record.base_rotation, rotation_skip)) {
                    record.applied_rotation = record.base_rotation;
                    record.rotation_written = false;
                }
            } else if (SameRotation(live_rotation, record.base_rotation)) {
                record.rotation_written = false;
            }
        }
    }
    Vector3 live{};
    if (!model.ReadScale(record.component, live, skip)) return RestoreStep::failed;
    const bool ours = Equal(live, record.applied);
    const bool original = Equal(live, record.base);
    if (!ours && !original) {
        // Someone else owns the component now. Writing the old value over a scale the
        // plugin did not produce is exactly what this rule exists to prevent.
        skip = Skip::conflict;
        return RestoreStep::foreign_value;
    }
    if (original) {
        // The record is left consistent even though the caller normally erases it:
        // nothing the plugin wrote is on the component any more.
        record.applied = record.base;
        record.written = false;
        return RestoreStep::already_back;
    }
    if (!model.ApplyScale(record.component, record.base, skip)) return RestoreStep::failed;
    record.applied = record.base;
    record.written = false;
    return RestoreStep::restored;
}

EffectStep StepRecord(
    ModelAccess& model, RecoveryRecord& record, const Settings& settings, const double clock,
    Skip& skip) {
    skip = Skip::none;
    if (!model.IdentityHeld(record.identity, skip)) return EffectStep::identity_lost;
    Vector3 live{};
    if (!model.ReadScale(record.component, live, skip)) return EffectStep::failed;
    if (!Equal(live, record.applied)) {
        skip = Skip::conflict;
        return EffectStep::foreign_value;
    }
    const double elapsed = clock - record.start_time;
    bool updated = false;
    const Vector3 output = OutputCyclic(
        record.base, settings.amplitude, settings.frequency_hz, elapsed, record.phase,
        settings.pause_seconds);
    if (!Equal(output, record.applied)) {
        if (!model.ApplyScale(record.component, output, skip)) return EffectStep::failed;
        record.applied = output;
        record.written = true;
        updated = true;
    }
    // The spin is best effort and independent of the pause between bounces: it keeps
    // turning while the model rests, and a model whose rotation the game drives itself
    // simply stops spinning instead of losing the jelly scale with it.
    Skip rotation_skip = Skip::none;
    if (SpinRecord(model, record, settings, elapsed, rotation_skip)) updated = true;
    return updated ? EffectStep::updated : EffectStep::unchanged;
}

}  // namespace jelly
