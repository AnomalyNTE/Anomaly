#include "anomaly/nte_ui_buttons.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Engine behind anomaly.nte.ui-buttons. Reflection and Evaluator run on the Game thread
// only; NteUiButtons::Impl owns the cross-thread request state machine and catalog.

namespace anomaly::nte_ui_buttons {

// Profile layout, resolved once. valid is false when any key is missing or out of range.
struct Offsets {
    bool valid{};
    std::int64_t object_flags{}, object_index{}, object_class{}, object_name{}, object_outer{};
    std::int64_t struct_super{}, struct_children{}, field_next{};
    std::int64_t function_num_parms{}, function_parms_size{};
    std::int64_t widget_slot{}, widget_flags{}, widget_visibility{}, widget_opacity{};
    std::uint8_t widget_enabled_mask{};
    std::int64_t slot_parent{}, tree_root{}, panel_slots{}, switcher_active{};
    std::int64_t button_on_pressed{}, button_on_released{}, button_on_clicked{};
    std::int64_t delegate_stride{}, delegate_object_index{}, delegate_serial{};
    std::int64_t delegate_function_name{};
    std::int64_t common_locked{};
    std::uint8_t common_locked_mask{};
    std::int64_t activatable_modal{}, activatable_active{};
    std::int64_t container_list{}, container_displayed{};
    std::int64_t htui_text{}, htui_pressed{}, htui_last_click{};
    std::int64_t base_closing{}, base_pause{}, base_hide_main{}, base_input{};
    std::uint8_t base_input_menu{};
    std::int64_t check_on_changed{}, text_block_text{};
    std::int64_t radio_box{}, radio_text{}, radio_block_button{}, item_click_button{};
};

[[nodiscard]] Offsets LoadOffsets(const BuildProfile& profile);

struct ObjRef {
    std::uintptr_t object{};
    std::uint32_t index{};
    std::uint32_t serial{};
    std::uintptr_t cls{};
};

struct Types {
    std::uintptr_t function{}, button{}, user_widget{}, widget_tree{}, switcher{};
    std::uintptr_t activatable{}, container{}, common_button{}, common_internal{};
    std::uintptr_t htui_button{}, htui_base{}, main_form{};
    std::uintptr_t is_visible{}, is_in_viewport{}, is_interaction_enabled{};
    std::uintptr_t pressed{}, released{}, clicked{}, is_button_locked{}, is_hovered{};
    // Tabs and list entries; all optional.
    std::uintptr_t check_box{}, ht_radio{}, htui_radio{}, list_item{};
    std::uintptr_t check_is_checked{}, check_set_checked{}, ht_radio_select{};
    std::uintptr_t radio_select{}, radio_is_checked{}, radio_feature_active{};
    std::uintptr_t item_pressed{}, item_released{}, item_clicked{}, item_locked{};
};

enum Trait : std::uint8_t {
    kTraitUserWidget = 1u << 0u,
    kTraitActivatable = 1u << 1u,
    kTraitContainer = 1u << 2u,
    kTraitHtuiBase = 1u << 3u,
    kTraitMainForm = 1u << 4u,
    kTraitWidgetTree = 1u << 5u,
    kTraitSwitcher = 1u << 6u,
};

// The engine's view of game memory: reads, the object registry, reflected types and name
// caches.
class Reflection final {
public:
    Reflection(const BuildProfile& profile, std::shared_ptr<const SymbolMemory> memory,
               NteUiButtonsBindings bindings);

    void SetRegistry(const NteUiButtonsRegistryView& registry) noexcept { registry_ = registry; }
    [[nodiscard]] const Offsets& Layout() const noexcept { return offsets_; }
    [[nodiscard]] const Types& Type() const noexcept { return types_; }

    // Resolves the reflected UI types; false with a reason when a required one is missing.
    [[nodiscard]] bool Prepare(std::string& error);
    [[nodiscard]] bool Prepared() const noexcept { return prepared_; }
    // Forgets resolved types and every cache (Host generation change).
    void Reset() noexcept;
    // Forgets per-pass caches: class pointers and objects can be reused after unload.
    void BeginPass();

    template <typename T>
    [[nodiscard]] bool Read(std::uintptr_t address, T& value) const noexcept {
        return address != 0 && memory_->Read(address, &value, sizeof(T));
    }
    // base + offset, or 0 when base is 0 so the following read fails.
    [[nodiscard]] static std::uintptr_t At(std::uintptr_t base, std::int64_t offset) noexcept {
        return base == 0 ? 0 : base + static_cast<std::uintptr_t>(offset);
    }
    [[nodiscard]] std::uintptr_t Pointer(std::uintptr_t address) const noexcept;
    [[nodiscard]] bool Flag(std::uintptr_t address) const noexcept;

    [[nodiscard]] bool ReadSlot(std::uint32_t index, std::uintptr_t& object,
                                std::uint32_t& serial) const noexcept;
    // Object pointer, slot serial, internal index and class all still match, and the object
    // is not a default object or being destroyed.
    [[nodiscard]] bool Alive(const ObjRef& ref) const noexcept;
    [[nodiscard]] bool MakeRef(std::uint32_t index, ObjRef& ref) const noexcept;

    // ABI button kind of a class, 0 when it is not a button.
    [[nodiscard]] std::uint32_t Kind(std::uintptr_t cls);
    // Kind of an object, 0 for a widget that is only a part of another listed control: the
    // click button of a list entry, and the radio box and block button of an HTUI_RadioBox.
    [[nodiscard]] std::uint32_t ObjectKind(const ObjRef& ref);
    [[nodiscard]] bool IsA(std::uintptr_t cls, std::uintptr_t base) const noexcept;
    [[nodiscard]] std::uint8_t Traits(std::uintptr_t cls);
    [[nodiscard]] std::uint8_t ObjectTraits(std::uintptr_t object);

    [[nodiscard]] const std::string& ObjectName(std::uintptr_t object);
    [[nodiscard]] std::string Text(std::uintptr_t ftext_address) const;

    [[nodiscard]] bool Signature(std::uintptr_t function, std::uint8_t num_parms,
                                 std::uint16_t parms_size) const noexcept;
    // UFunction named `fname` (comparison index | number << 32) on cls or a super class.
    [[nodiscard]] std::uintptr_t FindFunction(std::uintptr_t cls, std::uint64_t fname) const
        noexcept;
    [[nodiscard]] bool Invoke(std::uint32_t& calls, std::uintptr_t object, std::uintptr_t function,
                              void* parameters, std::size_t size) const;
    [[nodiscard]] bool QueryBool(std::uint32_t& calls, std::uintptr_t object,
                                 std::uintptr_t function, bool& value) const;

private:
    [[nodiscard]] const std::string& Name(std::uint64_t fname);

    std::shared_ptr<const SymbolMemory> memory_;
    NteUiButtonsBindings bindings_;
    Offsets offsets_;
    NteUiButtonsRegistryView registry_;
    Types types_;
    bool prepared_{};
    std::unordered_map<std::uint64_t, std::string> names_;
    std::unordered_map<std::uintptr_t, std::string> object_names_;
    std::unordered_map<std::uintptr_t, std::uint32_t> kinds_;
    std::unordered_map<std::uintptr_t, std::uint8_t> traits_;
};

// A UI layer (CommonUI activatable-widget container) and the window it shows.
struct WindowRecord {
    std::uintptr_t container{}, window{}, root{};
    std::string layer, name;
    bool active{}, visible{}, modal{}, hides_main_form{}, pauses_game{}, menu_input{}, closing{};
    // Draw order: Slot indices from the root down to the layer; a larger path draws on top.
    std::vector<std::uint32_t> z_path;

    [[nodiscard]] bool Showing() const noexcept {
        return window != 0 && active && visible && !closing;
    }
    // Covers the input of every window drawn beneath it. Over-blocking only hides a
    // clickable button; under-blocking clicks a button the player cannot see.
    [[nodiscard]] bool Blocking() const noexcept {
        return Showing() && (modal || hides_main_form || pauses_game || menu_input);
    }
};

struct Layers {
    std::vector<WindowRecord> windows;
    // Every window listed by any layer -> index into windows.
    std::unordered_map<std::uintptr_t, std::uint32_t> members;
};

struct ButtonRecord {
    ObjRef ref;
    std::uint32_t kind{}, category{}, reasons{}, depth{};
    std::string name, class_name, window, owner, root, cause, text, path;
    std::vector<std::string> path_names;
};

struct ClickOutcome {
    std::uint32_t status{};
    std::uint32_t outcome{};
    std::uint32_t invocations{};
    std::string detail;
};

// Classification, occlusion, clicking and hover checks on top of Reflection.
class Evaluator final {
public:
    explicit Evaluator(Reflection& reflection) : r_(reflection) {}

    void BeginPass() { viewport_.clear(); }
    [[nodiscard]] Layers ReadLayers(const std::vector<ObjRef>& containers);
    // use_cache reuses per-pass viewport results; a click re-queries them.
    [[nodiscard]] bool Evaluate(const Layers& layers, const ObjRef& ref, std::uint32_t kind,
                                bool use_cache, std::uint32_t& calls, ButtonRecord& record);
    [[nodiscard]] ClickOutcome Click(const ButtonRecord& record, std::uint32_t& calls);
    [[nodiscard]] bool Hovered(const ObjRef& ref, std::uint32_t& calls, bool& hovered);

private:
    struct Chain;

    [[nodiscard]] std::uintptr_t ParentOf(std::uintptr_t widget, std::uintptr_t& slot,
                                          std::uintptr_t& panel, bool& via_tree);
    [[nodiscard]] std::uint32_t SlotIndex(std::uintptr_t panel, std::uintptr_t slot) const;
    [[nodiscard]] std::vector<std::uint32_t> ZPath(const Layers& layers, std::uintptr_t widget,
                                                   std::uintptr_t& root, std::uint32_t nesting);
    [[nodiscard]] bool SelfShown(std::uintptr_t widget) const;
    [[nodiscard]] bool ContainerShown(std::uintptr_t container, std::uintptr_t& root);
    [[nodiscard]] Chain Walk(const Layers& layers, std::uintptr_t button);
    [[nodiscard]] const WindowRecord* FindOccluder(const Layers& layers, const Chain& chain,
                                                   std::uintptr_t button);
    [[nodiscard]] bool RootOnScreen(std::uintptr_t root, bool use_cache, std::uint32_t& calls,
                                    bool& on_screen);
    [[nodiscard]] ClickOutcome ClickCommon(const ButtonRecord& record, std::uint32_t& calls);
    [[nodiscard]] ClickOutcome ClickUmg(const ButtonRecord& record, std::uint32_t& calls);
    [[nodiscard]] ClickOutcome ClickRadio(const ButtonRecord& record, std::uint32_t& calls);
    [[nodiscard]] ClickOutcome ClickListEntry(const ButtonRecord& record, std::uint32_t& calls);

    struct Bound {
        std::uintptr_t target, function;
    };
    // Live bindings of the dynamic multicast delegate at object + offset whose function has
    // the given signature.
    [[nodiscard]] std::vector<Bound> Bindings(std::uintptr_t object, std::int64_t offset,
                                              std::uint8_t num_parms, std::uint16_t parms_size);

    Reflection& r_;
    std::unordered_map<std::uintptr_t, std::int8_t> viewport_;
};

}  // namespace anomaly::nte_ui_buttons

namespace anomaly::nte_ui_buttons {
namespace {

constexpr std::uint32_t kSkipObjectFlags = 0x10u | 0x20u | 0x8000u | 0x10000u;
constexpr std::uint32_t kMaxClassDepth = 64;
constexpr std::uint32_t kMaxFieldCount = 8192;
constexpr std::int64_t kMaxOffset = 64LL * 1024LL * 1024LL;

struct KeySpec {
    std::string_view key;
    std::int64_t minimum;
    std::int64_t maximum;
};

// Masks and enum values are single bytes; everything else is a field offset.
constexpr std::array<KeySpec, 46> kKeys{{
    {"object.flags", 0, kMaxOffset},
    {"object.internalIndex", 0, kMaxOffset},
    {"object.class", 0, kMaxOffset},
    {"object.nameOffset", 0, kMaxOffset},
    {"object.outer", 0, kMaxOffset},
    {"ustruct.superStruct", 0, kMaxOffset},
    {"ustruct.children", 0, kMaxOffset},
    {"ufield.next", 0, kMaxOffset},
    {"ufunction.numParms", 0, kMaxOffset},
    {"ufunction.parmsSize", 0, kMaxOffset},
    {"widget.slot", 0, kMaxOffset},
    {"widget.flags", 0, kMaxOffset},
    {"widget.enabledMask", 1, 255},
    {"widget.visibility", 0, kMaxOffset},
    {"widget.renderOpacity", 0, kMaxOffset},
    {"panelSlot.parent", 0, kMaxOffset},
    {"widgetTree.rootWidget", 0, kMaxOffset},
    {"panelWidget.slots", 0, kMaxOffset},
    {"widgetSwitcher.activeWidgetIndex", 0, kMaxOffset},
    {"button.onPressed", 0, kMaxOffset},
    {"button.onReleased", 0, kMaxOffset},
    {"button.onClicked", 0, kMaxOffset},
    {"scriptDelegate.stride", 16, 64},
    {"scriptDelegate.objectIndex", 0, 60},
    {"scriptDelegate.serial", 0, 60},
    {"scriptDelegate.functionName", 0, 56},
    {"commonButton.lockedByte", 0, kMaxOffset},
    {"commonButton.lockedMask", 1, 255},
    {"activatableWidget.isModal", 0, kMaxOffset},
    {"activatableWidget.isActive", 0, kMaxOffset},
    {"activatableContainer.widgetList", 0, kMaxOffset},
    {"activatableContainer.displayedWidget", 0, kMaxOffset},
    {"htuiButton.buttonText", 0, kMaxOffset},
    {"htuiButton.pressedFlag", 0, kMaxOffset},
    {"htuiButton.lastClickTicks", 0, kMaxOffset},
    {"htuiBase.isClosing", 0, kMaxOffset},
    {"htuiBase.pauseGame", 0, kMaxOffset},
    {"htuiBase.hideMainForm", 0, kMaxOffset},
    {"htuiBase.inputConfig", 0, kMaxOffset},
    {"htuiBase.inputConfigMenu", 0, 255},
    {"checkBox.onCheckStateChanged", 0, kMaxOffset},
    {"textBlock.text", 0, kMaxOffset},
    {"htuiRadioBox.radioBox", 0, kMaxOffset},
    {"htuiRadioBox.radioText", 0, kMaxOffset},
    {"htuiRadioBox.blockButton", 0, kMaxOffset},
    {"htuiListItem.clickButton", 0, kMaxOffset},
}};

std::int64_t Value(const BuildProfile& profile, std::string_view key) {
    const auto found = profile.layout.find(key);
    return found == profile.layout.end() ? -1 : found->second;
}

bool CheckLayout(const BuildProfile& profile, std::string& error) {
    for (const auto& spec : kKeys) {
        const auto value = Value(profile, spec.key);
        if (value < spec.minimum || value > spec.maximum) {
            error = "UI button layout key is missing or out of range: " + std::string(spec.key);
            return false;
        }
    }
    const auto stride = Value(profile, "scriptDelegate.stride");
    if (Value(profile, "scriptDelegate.objectIndex") + 4 > stride ||
        Value(profile, "scriptDelegate.serial") + 4 > stride ||
        Value(profile, "scriptDelegate.functionName") + 8 > stride) {
        error = "UI button script delegate layout does not fit its stride";
        return false;
    }
    return true;
}

}  // namespace

Offsets LoadOffsets(const BuildProfile& profile) {
    Offsets o;
    std::string error;
    if (!CheckLayout(profile, error)) return o;
    const auto v = [&](std::string_view key) { return Value(profile, key); };
    o.object_flags = v("object.flags");
    o.object_index = v("object.internalIndex");
    o.object_class = v("object.class");
    o.object_name = v("object.nameOffset");
    o.object_outer = v("object.outer");
    o.struct_super = v("ustruct.superStruct");
    o.struct_children = v("ustruct.children");
    o.field_next = v("ufield.next");
    o.function_num_parms = v("ufunction.numParms");
    o.function_parms_size = v("ufunction.parmsSize");
    o.widget_slot = v("widget.slot");
    o.widget_flags = v("widget.flags");
    o.widget_enabled_mask = static_cast<std::uint8_t>(v("widget.enabledMask"));
    o.widget_visibility = v("widget.visibility");
    o.widget_opacity = v("widget.renderOpacity");
    o.slot_parent = v("panelSlot.parent");
    o.tree_root = v("widgetTree.rootWidget");
    o.panel_slots = v("panelWidget.slots");
    o.switcher_active = v("widgetSwitcher.activeWidgetIndex");
    o.button_on_pressed = v("button.onPressed");
    o.button_on_released = v("button.onReleased");
    o.button_on_clicked = v("button.onClicked");
    o.delegate_stride = v("scriptDelegate.stride");
    o.delegate_object_index = v("scriptDelegate.objectIndex");
    o.delegate_serial = v("scriptDelegate.serial");
    o.delegate_function_name = v("scriptDelegate.functionName");
    o.common_locked = v("commonButton.lockedByte");
    o.common_locked_mask = static_cast<std::uint8_t>(v("commonButton.lockedMask"));
    o.activatable_modal = v("activatableWidget.isModal");
    o.activatable_active = v("activatableWidget.isActive");
    o.container_list = v("activatableContainer.widgetList");
    o.container_displayed = v("activatableContainer.displayedWidget");
    o.htui_text = v("htuiButton.buttonText");
    o.htui_pressed = v("htuiButton.pressedFlag");
    o.htui_last_click = v("htuiButton.lastClickTicks");
    o.base_closing = v("htuiBase.isClosing");
    o.base_pause = v("htuiBase.pauseGame");
    o.base_hide_main = v("htuiBase.hideMainForm");
    o.base_input = v("htuiBase.inputConfig");
    o.base_input_menu = static_cast<std::uint8_t>(v("htuiBase.inputConfigMenu"));
    o.check_on_changed = v("checkBox.onCheckStateChanged");
    o.text_block_text = v("textBlock.text");
    o.radio_box = v("htuiRadioBox.radioBox");
    o.radio_text = v("htuiRadioBox.radioText");
    o.radio_block_button = v("htuiRadioBox.blockButton");
    o.item_click_button = v("htuiListItem.clickButton");
    o.valid = true;
    return o;
}

}  // namespace anomaly::nte_ui_buttons

namespace anomaly {

const std::vector<std::string_view>& NteUiButtonsLayoutKeys() {
    static const std::vector<std::string_view> keys = [] {
        std::vector<std::string_view> result;
        for (const auto& spec : nte_ui_buttons::kKeys) result.push_back(spec.key);
        return result;
    }();
    return keys;
}

bool ValidateNteUiButtonsLayout(const BuildProfile& profile, std::string& error) {
    return nte_ui_buttons::CheckLayout(profile, error);
}

namespace {

class InProcessSymbolMemory final : public SymbolMemory {
public:
    std::optional<ue5mem::ModuleInfo> FindModule(std::wstring_view) const override {
        return std::nullopt;
    }
    std::vector<ue5mem::SectionInfo> Sections(const ue5mem::ModuleInfo&) const override {
        return {};
    }
    std::vector<std::uintptr_t> Scan(
        const ue5mem::ModuleInfo&, std::string_view, std::string_view,
        std::size_t) const override {
        return {};
    }
    // A widget scan reads every GObjects slot. ReadProcessMemory per read costs seconds of
    // Game-thread time, so objects owned by this thread are copied directly under SEH.
    bool Read(std::uintptr_t address, void* destination, std::size_t size) const override {
        if (address == 0 || destination == nullptr) return false;
        __try {
            std::memcpy(destination, reinterpret_cast<const void*>(address), size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    std::optional<SymbolMemoryRegion> Query(std::uintptr_t) const override {
        return std::nullopt;
    }
};

}  // namespace

std::shared_ptr<const SymbolMemory> CreateInProcessSymbolMemory() {
    return std::make_shared<InProcessSymbolMemory>();
}

}  // namespace anomaly

namespace anomaly::nte_ui_buttons {

Reflection::Reflection(const BuildProfile& profile, std::shared_ptr<const SymbolMemory> memory,
                       NteUiButtonsBindings bindings)
    : memory_(std::move(memory)), bindings_(std::move(bindings)),
      offsets_(LoadOffsets(profile)) {}

std::uintptr_t Reflection::Pointer(std::uintptr_t address) const noexcept {
    std::uintptr_t value{};
    return Read(address, value) ? value : 0;
}

bool Reflection::Flag(std::uintptr_t address) const noexcept {
    std::uint8_t value{};
    return Read(address, value) && value != 0;
}

bool Reflection::ReadSlot(std::uint32_t index, std::uintptr_t& object,
                          std::uint32_t& serial) const noexcept {
    object = 0;
    const auto& reg = registry_;
    if (reg.items == 0 || reg.chunk_size == 0 || reg.item_stride == 0 || index >= reg.count) {
        return false;
    }
    const auto page = index / reg.chunk_size;
    if (page >= reg.num_chunks) return false;
    const auto chunk = Pointer(reg.items + static_cast<std::uintptr_t>(page) * sizeof(void*));
    if (chunk == 0) return false;
    const auto item =
        chunk + static_cast<std::uintptr_t>(index % reg.chunk_size) * reg.item_stride;
    return Read(item + reg.object_offset, object) && Read(item + reg.serial_offset, serial);
}

bool Reflection::MakeRef(std::uint32_t index, ObjRef& ref) const noexcept {
    ref = {};
    std::uintptr_t object{};
    std::uint32_t serial{};
    std::uint32_t flags{};
    std::uintptr_t cls{};
    if (!ReadSlot(index, object, serial) || object == 0 ||
        !Read(At(object, offsets_.object_flags), flags) || (flags & kSkipObjectFlags) != 0 ||
        !Read(At(object, offsets_.object_class), cls) || cls == 0) {
        return false;
    }
    ref = {object, index, serial, cls};
    return true;
}

bool Reflection::Alive(const ObjRef& ref) const noexcept {
    ObjRef current;
    std::int32_t internal{-1};
    return ref.object != 0 && MakeRef(ref.index, current) && current.object == ref.object &&
        current.serial == ref.serial && current.cls == ref.cls &&
        Read(At(ref.object, offsets_.object_index), internal) &&
        internal == static_cast<std::int32_t>(ref.index);
}

bool Reflection::IsA(std::uintptr_t cls, std::uintptr_t base) const noexcept {
    if (cls == 0 || base == 0) return false;
    for (std::uint32_t depth = 0; cls != 0 && depth < kMaxClassDepth; ++depth) {
        if (cls == base) return true;
        const auto super = Pointer(At(cls, offsets_.struct_super));
        if (super == cls) return false;
        cls = super;
    }
    return false;
}

std::uint32_t Reflection::Kind(std::uintptr_t cls) {
    const auto [entry, inserted] = kinds_.try_emplace(cls, 0u);
    if (!inserted) return entry->second;
    // HTUI_RadioBox and HTUI_ListItem are UserWidgets, not buttons; test them first.
    if (IsA(cls, types_.htui_radio)) {
        entry->second = ANOMALY_NTE_UI_BUTTON_KIND_V1_RADIO;
    } else if (IsA(cls, types_.list_item)) {
        entry->second = ANOMALY_NTE_UI_BUTTON_KIND_V1_LIST_ENTRY;
    } else if (IsA(cls, types_.check_box)) {
        // HTRadioBox, HTCheckBox and plain UMG CheckBox.
        entry->second = ANOMALY_NTE_UI_BUTTON_KIND_V1_RADIO;
    } else if (IsA(cls, types_.htui_button)) {
        entry->second = ANOMALY_NTE_UI_BUTTON_KIND_V1_HTUI;
    } else if (IsA(cls, types_.common_button)) {
        entry->second = ANOMALY_NTE_UI_BUTTON_KIND_V1_COMMON;
    } else if (IsA(cls, types_.button) && !IsA(cls, types_.common_internal)) {
        // CommonButtonInternalBase is the Slate button inside every CommonButtonBase.
        entry->second = ANOMALY_NTE_UI_BUTTON_KIND_V1_UMG;
    }
    return entry->second;
}

std::uint32_t Reflection::ObjectKind(const ObjRef& ref) {
    const auto kind = Kind(ref.cls);
    if (kind == 0 || kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_LIST_ENTRY ||
        (types_.list_item == 0 && types_.htui_radio == 0)) {
        return kind;
    }
    if (kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_RADIO && IsA(ref.cls, types_.htui_radio)) {
        return kind;
    }
    // The owning UserWidget: widget -> WidgetTree -> UserWidget.
    const auto tree = Pointer(At(ref.object, offsets_.object_outer));
    if (tree == 0 || (ObjectTraits(tree) & kTraitWidgetTree) == 0) return kind;
    const auto owner = Pointer(At(tree, offsets_.object_outer));
    const auto owner_cls = Pointer(At(owner, offsets_.object_class));
    if (owner_cls == 0) return kind;
    const auto owner_kind = Kind(owner_cls);
    if (owner_kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_LIST_ENTRY &&
        Pointer(At(owner, offsets_.item_click_button)) == ref.object) {
        return 0;
    }
    if (owner_kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_RADIO && IsA(owner_cls, types_.htui_radio) &&
        (Pointer(At(owner, offsets_.radio_box)) == ref.object ||
         Pointer(At(owner, offsets_.radio_block_button)) == ref.object)) {
        return 0;
    }
    return kind;
}

std::uint8_t Reflection::Traits(std::uintptr_t cls) {
    const auto [entry, inserted] = traits_.try_emplace(cls, std::uint8_t{0});
    if (!inserted) return entry->second;
    std::uint8_t traits = 0;
    if (IsA(cls, types_.user_widget)) traits |= kTraitUserWidget;
    if (IsA(cls, types_.activatable)) traits |= kTraitActivatable;
    if (IsA(cls, types_.container)) traits |= kTraitContainer;
    if (IsA(cls, types_.htui_base)) traits |= kTraitHtuiBase;
    if (IsA(cls, types_.main_form)) traits |= kTraitMainForm;
    if (IsA(cls, types_.widget_tree)) traits |= kTraitWidgetTree;
    if (IsA(cls, types_.switcher)) traits |= kTraitSwitcher;
    entry->second = traits;
    return traits;
}

std::uint8_t Reflection::ObjectTraits(std::uintptr_t object) {
    const auto cls = Pointer(At(object, offsets_.object_class));
    return cls == 0 ? 0 : Traits(cls);
}

const std::string& Reflection::Name(std::uint64_t fname) {
    const auto [entry, inserted] = names_.try_emplace(fname);
    if (!inserted) return entry->second;
    const auto index = static_cast<std::uint32_t>(fname);
    const auto number = static_cast<std::uint32_t>(fname >> 32u);
    try {
        entry->second = bindings_.resolve_name ? bindings_.resolve_name(index) : std::string{};
    } catch (...) {
        entry->second.clear();
    }
    if (!entry->second.empty() && number != 0) {
        entry->second += "_" + std::to_string(number - 1U);
    }
    return entry->second;
}

const std::string& Reflection::ObjectName(std::uintptr_t object) {
    const auto [entry, inserted] = object_names_.try_emplace(object);
    if (inserted) {
        std::uint64_t fname{};
        if (Read(At(object, offsets_.object_name), fname)) entry->second = Name(fname);
    }
    return entry->second;
}

std::string Reflection::Text(std::uintptr_t ftext_address) const {
    if (!bindings_.read_ftext || ftext_address == 0) return {};
    try {
        return bindings_.read_ftext(ftext_address);
    } catch (...) {
        return {};
    }
}

bool Reflection::Signature(std::uintptr_t function, std::uint8_t num_parms,
                           std::uint16_t parms_size) const noexcept {
    std::uint8_t actual_parms{};
    std::uint16_t actual_size{};
    return function != 0 && Read(At(function, offsets_.function_num_parms), actual_parms) &&
        Read(At(function, offsets_.function_parms_size), actual_size) &&
        actual_parms == num_parms && actual_size == parms_size;
}

std::uintptr_t Reflection::FindFunction(std::uintptr_t cls, std::uint64_t fname) const noexcept {
    for (std::uint32_t depth = 0; cls != 0 && depth < kMaxClassDepth; ++depth) {
        auto field = Pointer(At(cls, offsets_.struct_children));
        for (std::uint32_t count = 0; field != 0 && count < kMaxFieldCount; ++count) {
            std::uint64_t name{};
            if (Read(At(field, offsets_.object_name), name) && name == fname &&
                Pointer(At(field, offsets_.object_class)) == types_.function) {
                return field;
            }
            const auto next = Pointer(At(field, offsets_.field_next));
            if (next == field) break;
            field = next;
        }
        const auto super = Pointer(At(cls, offsets_.struct_super));
        if (super == cls) break;
        cls = super;
    }
    return 0;
}

bool Reflection::Invoke(std::uint32_t& calls, std::uintptr_t object, std::uintptr_t function,
                        void* parameters, std::size_t size) const {
    if (!bindings_.invoke || object == 0 || function == 0) return false;
    ++calls;
    try {
        return bindings_.invoke(object, function, parameters, size);
    } catch (...) {
        return false;
    }
}

bool Reflection::QueryBool(std::uint32_t& calls, std::uintptr_t object, std::uintptr_t function,
                           bool& value) const {
    // Room past the one-byte return value keeps a mismatched signature from writing past
    // the buffer; the signature itself was checked when the function was resolved.
    std::array<std::uint8_t, 16> parameters{};
    if (!Invoke(calls, object, function, parameters.data(), parameters.size())) return false;
    value = parameters[0] != 0;
    return true;
}

bool Reflection::Prepare(std::string& error) {
    if (prepared_) return true;
    if (!offsets_.valid) {
        error = "UI button Profile layout is incomplete";
        return false;
    }
    if (!bindings_.find_object || !bindings_.invoke || !bindings_.resolve_name) {
        error = "object lookup, names or ProcessEvent is unavailable";
        return false;
    }
    const auto find = [&](const wchar_t* path) -> std::uintptr_t {
        try {
            return bindings_.find_object(path);
        } catch (...) {
            return 0;
        }
    };
    struct Required {
        const wchar_t* path;
        std::uintptr_t* slot;
    };
    Types t;
    const std::array<Required, 15> required{{
        {L"/Script/CoreUObject.Function", &t.function},
        {L"/Script/UMG.Button", &t.button},
        {L"/Script/UMG.UserWidget", &t.user_widget},
        {L"/Script/UMG.WidgetTree", &t.widget_tree},
        {L"/Script/UMG.WidgetSwitcher", &t.switcher},
        {L"/Script/UMG.Widget.IsVisible", &t.is_visible},
        {L"/Script/UMG.Widget.IsInViewport", &t.is_in_viewport},
        {L"/Script/CommonUI.CommonActivatableWidget", &t.activatable},
        {L"/Script/CommonUI.CommonActivatableWidgetContainerBase", &t.container},
        {L"/Script/CommonUI.CommonButtonBase", &t.common_button},
        {L"/Script/CommonUI.CommonButtonInternalBase", &t.common_internal},
        {L"/Script/CommonUI.CommonButtonBase.IsInteractionEnabled", &t.is_interaction_enabled},
        {L"/Script/CommonUI.CommonButtonBase.HandleButtonPressed", &t.pressed},
        {L"/Script/CommonUI.CommonButtonBase.HandleButtonReleased", &t.released},
        {L"/Script/CommonUI.CommonButtonBase.HandleButtonClicked", &t.clicked},
    }};
    for (const auto& item : required) {
        *item.slot = find(item.path);
        if (*item.slot == 0) {
            // Script paths are ASCII.
            std::string narrow;
            for (const wchar_t* c = item.path; *c != 0; ++c) {
                narrow.push_back(static_cast<char>(*c));
            }
            error = "reflected UI type is missing: " + narrow;
            return false;
        }
    }
    // HTGame types only refine the checks; a build without them still scans and clicks.
    t.htui_button = find(L"/Script/HTGame.HTUI_Button");
    t.htui_base = find(L"/Script/HTGame.HTUIBase");
    t.main_form = find(L"/Script/HTGame.HTUI_MainForm");
    t.is_button_locked = find(L"/Script/HTGame.HTUI_Button.IsButtonLocked");
    t.is_hovered = find(L"/Script/UMG.Widget.IsHovered");
    t.check_box = find(L"/Script/UMG.CheckBox");
    t.check_is_checked = find(L"/Script/UMG.CheckBox.IsChecked");
    t.check_set_checked = find(L"/Script/UMG.CheckBox.SetIsChecked");
    t.ht_radio = find(L"/Script/HTGame.HTRadioBox");
    t.ht_radio_select = find(L"/Script/HTGame.HTRadioBox.SetSelected");
    t.htui_radio = find(L"/Script/HTGame.HTUI_RadioBox");
    t.radio_select = find(L"/Script/HTGame.HTUI_RadioBox.SetSelected");
    t.radio_is_checked = find(L"/Script/HTGame.HTUI_RadioBox.IsChecked");
    t.radio_feature_active =
        find(L"/Script/HTGame.HTUI_RadioBox.IsSystematicGameFeatureActivated");
    t.list_item = find(L"/Script/HTGame.HTUI_ListItem");
    t.item_pressed = find(L"/Script/HTGame.HTUI_ListItem.OnBtnPressed");
    t.item_released = find(L"/Script/HTGame.HTUI_ListItem.OnBtnReleased");
    t.item_clicked = find(L"/Script/HTGame.HTUI_ListItem.OnBtnClicked");
    t.item_locked = find(L"/Script/HTGame.HTUI_ListItem.IsItemLocked");
    types_ = t;
    if (!Signature(t.is_visible, 1, 1) || !Signature(t.is_in_viewport, 1, 1) ||
        !Signature(t.is_interaction_enabled, 1, 1) || !Signature(t.pressed, 0, 0) ||
        !Signature(t.released, 0, 0) || !Signature(t.clicked, 0, 0)) {
        types_ = {};
        error = "reflected UI function signatures do not match";
        return false;
    }
    if (types_.is_button_locked != 0 && !Signature(types_.is_button_locked, 1, 1)) {
        types_.is_button_locked = 0;
    }
    if (types_.is_hovered != 0 && !Signature(types_.is_hovered, 1, 1)) types_.is_hovered = 0;
    const auto keep = [this](std::uintptr_t& function, std::uint8_t num_parms,
                             std::uint16_t parms_size) {
        if (function != 0 && !Signature(function, num_parms, parms_size)) function = 0;
    };
    keep(types_.check_is_checked, 1, 1);
    keep(types_.check_set_checked, 1, 1);
    keep(types_.ht_radio_select, 2, 2);
    keep(types_.radio_select, 2, 2);
    keep(types_.radio_is_checked, 1, 1);
    keep(types_.radio_feature_active, 1, 1);
    keep(types_.item_pressed, 0, 0);
    keep(types_.item_released, 0, 0);
    keep(types_.item_clicked, 0, 0);
    keep(types_.item_locked, 1, 1);
    // A kind is only listed when it can be clicked.
    if (types_.check_set_checked == 0) types_.check_box = 0;
    if (types_.radio_select == 0) types_.htui_radio = 0;
    if (types_.item_pressed == 0 || types_.item_released == 0 || types_.item_clicked == 0) {
        types_.list_item = 0;
    }
    prepared_ = true;
    return true;
}

void Reflection::Reset() noexcept {
    prepared_ = false;
    types_ = {};
    names_.clear();
    object_names_.clear();
    kinds_.clear();
    traits_.clear();
}

void Reflection::BeginPass() {
    object_names_.clear();
    kinds_.clear();
    traits_.clear();
}

}  // namespace anomaly::nte_ui_buttons

namespace anomaly::nte_ui_buttons {
namespace {

constexpr std::uint8_t kVisibilityCollapsed = 1;
constexpr std::uint8_t kVisibilityHidden = 2;
constexpr std::uint8_t kVisibilityHitTestInvisible = 3;
constexpr float kTransparentOpacity = 0.01F;
constexpr std::uint32_t kMaxChainDepth = 128;
constexpr std::uint32_t kMaxDelegateEntries = 64;
constexpr std::uint32_t kMaxPanelSlots = 4096;
constexpr std::uint32_t kMaxLayerWidgets = 256;
constexpr std::uint32_t kMaxLayerNesting = 8;
constexpr std::uint32_t kZWindowContent = 0xFFFFFFFFu;

constexpr std::uint32_t kVisibilityReasons = ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_SELF |
    ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_ANCESTOR |
    ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_IN_VIEWPORT | ANOMALY_NTE_UI_BUTTON_REASON_V1_DETACHED |
    ANOMALY_NTE_UI_BUTTON_REASON_V1_INACTIVE_PAGE | ANOMALY_NTE_UI_BUTTON_REASON_V1_TRANSPARENT;
// Past these reasons a button cannot become clickable, so no game function is called for it.
constexpr std::uint32_t kSkipQueryReasons = kVisibilityReasons |
    ANOMALY_NTE_UI_BUTTON_REASON_V1_OCCLUDED | ANOMALY_NTE_UI_BUTTON_REASON_V1_CLOSING;

struct TArrayHeader {
    std::uintptr_t data;
    std::int32_t count;
    std::int32_t capacity;
};

}  // namespace

struct Evaluator::Chain {
    std::uint32_t reasons{};
    std::uintptr_t owner{}, root{}, cause{}, unslotted{}, window{};
    std::string cause_name;
    bool in_main_form{}, ok{};
    std::uint32_t depth{};  // parent widgets above the button
    std::vector<std::uintptr_t> windows;
    std::vector<std::uintptr_t> user_widgets;  // innermost first
};

std::uintptr_t Evaluator::ParentOf(std::uintptr_t widget, std::uintptr_t& slot,
                                   std::uintptr_t& panel, bool& via_tree) {
    const auto& o = r_.Layout();
    slot = r_.Pointer(Reflection::At(widget, o.widget_slot));
    panel = r_.Pointer(Reflection::At(slot, o.slot_parent));
    via_tree = false;
    if (panel != 0 && panel != widget) return panel;
    panel = 0;
    // WidgetTree roots (and Slate-hosted list entries) have no Slot; their parent is the
    // UserWidget that owns the widget tree.
    const auto outer = r_.Pointer(Reflection::At(widget, o.object_outer));
    if (outer == 0 || (r_.ObjectTraits(outer) & kTraitWidgetTree) == 0) return 0;
    const auto user_widget = r_.Pointer(Reflection::At(outer, o.object_outer));
    if (user_widget == 0 || user_widget == widget ||
        (r_.ObjectTraits(user_widget) & kTraitUserWidget) == 0) {
        return 0;
    }
    via_tree = true;
    return user_widget;
}

std::uint32_t Evaluator::SlotIndex(std::uintptr_t panel, std::uintptr_t slot) const {
    TArrayHeader slots{};
    if (!r_.Read(Reflection::At(panel, r_.Layout().panel_slots), slots) || slots.data == 0 ||
        slots.count <= 0) {
        return 0;
    }
    const auto limit = std::min<std::uint32_t>(static_cast<std::uint32_t>(slots.count),
                                               kMaxPanelSlots);
    for (std::uint32_t i = 0; i < limit; ++i) {
        if (r_.Pointer(slots.data + static_cast<std::uintptr_t>(i) * sizeof(void*)) == slot) {
            return i;
        }
    }
    return 0;
}

std::vector<std::uint32_t> Evaluator::ZPath(const Layers& layers, std::uintptr_t widget,
                                            std::uintptr_t& root, std::uint32_t nesting) {
    // Windows in a layer have no Slot (Slate hosts them); a window draws at its layer's
    // position plus one level for the layer's content.
    std::vector<std::uint32_t> reversed;
    root = 0;
    for (std::uint32_t depth = 0; widget != 0 && depth < kMaxChainDepth; ++depth) {
        const auto member = layers.members.find(widget);
        if (member != layers.members.end() && nesting < kMaxLayerNesting) {
            auto path =
                ZPath(layers, layers.windows[member->second].container, root, nesting + 1);
            path.push_back(kZWindowContent);
            path.insert(path.end(), reversed.rbegin(), reversed.rend());
            return path;
        }
        std::uintptr_t slot{}, panel{};
        bool via_tree{};
        const auto parent = ParentOf(widget, slot, panel, via_tree);
        if (parent == 0) {
            root = widget;
            break;
        }
        reversed.push_back(panel != 0 ? SlotIndex(panel, slot) : 0);
        widget = parent;
    }
    return {reversed.rbegin(), reversed.rend()};
}

bool Evaluator::SelfShown(std::uintptr_t widget) const {
    const auto& o = r_.Layout();
    std::uint8_t visibility{};
    float opacity{1.0F};
    return r_.Read(Reflection::At(widget, o.widget_visibility), visibility) &&
        visibility != kVisibilityCollapsed && visibility != kVisibilityHidden &&
        (!r_.Read(Reflection::At(widget, o.widget_opacity), opacity) ||
         opacity > kTransparentOpacity);
}

bool Evaluator::ContainerShown(std::uintptr_t container, std::uintptr_t& root) {
    root = 0;
    auto widget = container;
    for (std::uint32_t depth = 0; widget != 0 && depth < kMaxChainDepth; ++depth) {
        if (!SelfShown(widget)) return false;
        std::uintptr_t slot{}, panel{};
        bool via_tree{};
        const auto parent = ParentOf(widget, slot, panel, via_tree);
        if (parent == 0) {
            root = widget;
            break;
        }
        widget = parent;
    }
    return true;
}

Layers Evaluator::ReadLayers(const std::vector<ObjRef>& containers) {
    const auto& o = r_.Layout();
    Layers layers;
    for (const auto& ref : containers) {
        if (!r_.Alive(ref)) continue;
        WindowRecord record;
        record.container = ref.object;
        record.layer = r_.ObjectName(ref.object);
        const bool container_shown = ContainerShown(ref.object, record.root);
        record.window = r_.Pointer(Reflection::At(ref.object, o.container_displayed));
        if (record.window != 0) {
            const auto traits = r_.ObjectTraits(record.window);
            record.name = r_.ObjectName(record.window);
            record.visible = container_shown && SelfShown(record.window);
            if ((traits & kTraitActivatable) != 0) {
                record.active = r_.Flag(Reflection::At(record.window, o.activatable_active));
                record.modal = r_.Flag(Reflection::At(record.window, o.activatable_modal));
            }
            if ((traits & kTraitHtuiBase) != 0) {
                record.closing = r_.Flag(Reflection::At(record.window, o.base_closing));
                record.hides_main_form =
                    r_.Flag(Reflection::At(record.window, o.base_hide_main));
                record.pauses_game = r_.Flag(Reflection::At(record.window, o.base_pause));
                std::uint8_t input{};
                record.menu_input =
                    r_.Read(Reflection::At(record.window, o.base_input), input) &&
                    input == o.base_input_menu;
            }
        }
        const auto index = static_cast<std::uint32_t>(layers.windows.size());
        TArrayHeader list{};
        if (r_.Read(Reflection::At(ref.object, o.container_list), list) && list.data != 0 &&
            list.count > 0) {
            const auto limit = std::min<std::uint32_t>(static_cast<std::uint32_t>(list.count),
                                                       kMaxLayerWidgets);
            for (std::uint32_t i = 0; i < limit; ++i) {
                const auto member =
                    r_.Pointer(list.data + static_cast<std::uintptr_t>(i) * sizeof(void*));
                if (member != 0) layers.members.emplace(member, index);
            }
        }
        if (record.window != 0) layers.members.emplace(record.window, index);
        layers.windows.push_back(std::move(record));
    }
    for (auto& window : layers.windows) {
        std::uintptr_t root{};
        window.z_path = ZPath(layers, window.container, root, 0);
        window.z_path.push_back(kZWindowContent);
    }
    return layers;
}

Evaluator::Chain Evaluator::Walk(const Layers& layers, std::uintptr_t button) {
    const auto& o = r_.Layout();
    Chain chain;
    const auto set_cause = [&chain](std::uint32_t reason, std::uintptr_t cause) {
        chain.reasons |= reason;
        if (chain.cause == 0 && chain.cause_name.empty()) chain.cause = cause;
    };
    auto widget = button;
    for (std::uint32_t depth = 0; depth < kMaxChainDepth; ++depth) {
        std::uint8_t visibility{};
        std::uint8_t flags{};
        float opacity{1.0F};
        if (!r_.Read(Reflection::At(widget, o.widget_visibility), visibility) ||
            !r_.Read(Reflection::At(widget, o.widget_flags), flags)) {
            return chain;
        }
        if (visibility == kVisibilityCollapsed || visibility == kVisibilityHidden) {
            set_cause(widget == button ? ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_SELF
                                       : ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_ANCESTOR,
                      widget);
        } else if (visibility == kVisibilityHitTestInvisible) {
            set_cause(ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_HIT_TESTABLE, widget);
        }
        if (r_.Read(Reflection::At(widget, o.widget_opacity), opacity) &&
            opacity <= kTransparentOpacity) {
            set_cause(ANOMALY_NTE_UI_BUTTON_REASON_V1_TRANSPARENT, widget);
        }
        if ((flags & o.widget_enabled_mask) == 0) {
            chain.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_DISABLED;
        }

        const auto traits = r_.ObjectTraits(widget);
        if ((traits & kTraitMainForm) != 0) chain.in_main_form = true;
        if ((traits & kTraitHtuiBase) != 0 && widget != button) {
            if (r_.Flag(Reflection::At(widget, o.base_closing))) {
                set_cause(ANOMALY_NTE_UI_BUTTON_REASON_V1_CLOSING, widget);
            }
        }
        const auto member = layers.members.find(widget);
        if (member != layers.members.end()) {
            const auto& layer = layers.windows[member->second];
            if (chain.window == 0) chain.window = widget;
            chain.windows.push_back(widget);
            if (layer.window != widget || !layer.active) {
                chain.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_INACTIVE_PAGE;
                if (chain.cause == 0 && chain.cause_name.empty()) {
                    chain.cause_name = layer.layer + " shows " +
                        (layer.name.empty() ? std::string("nothing") : layer.name);
                }
            } else if (!layer.visible) {
                set_cause(ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_ANCESTOR, layer.container);
            }
        }

        std::uintptr_t slot{}, panel{};
        bool via_tree{};
        const auto parent = ParentOf(widget, slot, panel, via_tree);
        if (parent == 0) {
            const auto outer = r_.Pointer(Reflection::At(widget, o.object_outer));
            if (outer != 0 && (r_.ObjectTraits(outer) & kTraitWidgetTree) != 0 &&
                member == layers.members.end()) {
                const auto owner = r_.Pointer(Reflection::At(outer, o.object_outer));
                if (owner == 0 || owner == widget) {
                    set_cause(ANOMALY_NTE_UI_BUTTON_REASON_V1_DETACHED, widget);
                }
            }
            chain.root = widget;
            chain.ok = true;
            return chain;
        }
        if (panel != 0) {
            // A WidgetSwitcher's inactive pages stay Visible but are not drawn.
            if ((r_.ObjectTraits(panel) & kTraitSwitcher) != 0) {
                std::int32_t active{-1};
                TArrayHeader slots{};
                if (r_.Read(Reflection::At(panel, o.switcher_active), active) &&
                    r_.Read(Reflection::At(panel, o.panel_slots), slots) && active >= 0 &&
                    active < slots.count &&
                    r_.Pointer(slots.data + static_cast<std::uintptr_t>(active) *
                               sizeof(void*)) != slot) {
                    set_cause(ANOMALY_NTE_UI_BUTTON_REASON_V1_INACTIVE_PAGE, widget);
                }
            }
        } else {
            if (chain.owner == 0) chain.owner = parent;
            chain.user_widgets.push_back(parent);
            // Neither the tree root nor a layer window, yet no Slot: a Slate-hosted list
            // entry or a removed widget. Memory cannot tell them apart; IsVisible can.
            const auto outer = r_.Pointer(Reflection::At(widget, o.object_outer));
            if (chain.unslotted == 0 && member == layers.members.end() &&
                r_.Pointer(Reflection::At(outer, o.tree_root)) != widget) {
                chain.unslotted = widget;
            }
        }
        ++chain.depth;
        widget = parent;
    }
    return chain;
}

const WindowRecord* Evaluator::FindOccluder(const Layers& layers, const Chain& chain,
                                            std::uintptr_t button) {
    std::uintptr_t root{};
    const auto button_z = ZPath(layers, button, root, 0);
    for (const auto& window : layers.windows) {
        if (!window.Showing()) continue;
        if (std::find(chain.windows.begin(), chain.windows.end(), window.window) !=
            chain.windows.end()) {
            continue;
        }
        if (chain.in_main_form && window.hides_main_form) return &window;
        if (window.Blocking() && root != 0 && window.root == root &&
            std::lexicographical_compare(button_z.begin(), button_z.end(),
                                         window.z_path.begin(), window.z_path.end())) {
            return &window;
        }
    }
    return nullptr;
}

bool Evaluator::RootOnScreen(std::uintptr_t root, bool use_cache, std::uint32_t& calls,
                             bool& on_screen) {
    if (use_cache) {
        const auto found = viewport_.find(root);
        if (found != viewport_.end()) {
            on_screen = found->second > 0;
            return found->second >= 0;
        }
    }
    bool in_viewport{};
    const bool queried = r_.QueryBool(calls, root, r_.Type().is_in_viewport, in_viewport);
    // CommonUI stack windows are not added to the viewport; activation is what shows them.
    const bool active = (r_.ObjectTraits(root) & kTraitActivatable) != 0 &&
        r_.Flag(Reflection::At(root, r_.Layout().activatable_active));
    on_screen = (queried && in_viewport) || active;
    const bool ok = queried || active;
    viewport_[root] = ok ? (on_screen ? 1 : 0) : -1;
    return ok;
}

bool Evaluator::Evaluate(const Layers& layers, const ObjRef& ref, std::uint32_t kind,
                         bool use_cache, std::uint32_t& calls, ButtonRecord& record) {
    if (!r_.Alive(ref)) return false;
    const auto& o = r_.Layout();
    const auto& t = r_.Type();
    record = {};
    record.ref = ref;
    record.kind = kind;
    const auto chain = Walk(layers, ref.object);
    if (!chain.ok) record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_QUERY_FAILED;
    record.reasons |= chain.reasons;
    std::uintptr_t cause = chain.cause;
    record.cause = chain.cause_name;

    const bool common =
        kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_COMMON || kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_HTUI;
    const bool htui_radio =
        kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_RADIO && r_.IsA(ref.cls, t.htui_radio);
    if (common) {
        std::uint8_t locked{};
        if (r_.Read(Reflection::At(ref.object, o.common_locked), locked) &&
            (locked & o.common_locked_mask) != 0) {
            record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED;
        }
    }
    if ((record.reasons & kVisibilityReasons) == 0 && chain.ok) {
        if (const auto* occluder = FindOccluder(layers, chain, ref.object)) {
            record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_OCCLUDED;
            if (cause == 0 && record.cause.empty()) {
                record.cause = occluder->name + " (" + occluder->layer + ")";
            }
        }
    }
    const auto skip = [&record] { return (record.reasons & kSkipQueryReasons) != 0; };
    if (!skip() && chain.ok && chain.unslotted != 0) {
        bool visible{};
        if (!r_.QueryBool(calls, chain.unslotted, t.is_visible, visible)) {
            record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_QUERY_FAILED;
        } else if (!visible) {
            record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_DETACHED;
        }
    }
    if (!skip() && chain.ok) {
        bool on_screen{};
        if (!RootOnScreen(chain.root, use_cache, calls, on_screen)) {
            record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_QUERY_FAILED;
        } else if (!on_screen) {
            record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_IN_VIEWPORT;
        }
    }
    if (!skip()) {
        if (common) {
            bool interactable{};
            if (!r_.QueryBool(calls, ref.object, t.is_interaction_enabled, interactable)) {
                record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_QUERY_FAILED;
            } else if (!interactable) {
                record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_INTERACTABLE;
            }
        }
        if (kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_HTUI && t.is_button_locked != 0) {
            bool locked{};
            if (r_.QueryBool(calls, ref.object, t.is_button_locked, locked) && locked) {
                record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED;
            }
        }
        // A selected tab stays clickable: what a click leads to is decided by the window, not
        // by the tab's checked state.
        if (htui_radio && t.radio_feature_active != 0) {
            bool active{};
            if (r_.QueryBool(calls, ref.object, t.radio_feature_active, active) && !active) {
                record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED;
            }
        }
        if (kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_LIST_ENTRY && t.item_locked != 0) {
            bool locked{};
            if (r_.QueryBool(calls, ref.object, t.item_locked, locked) && locked) {
                record.reasons |= ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED;
            }
        }
    }
    if ((record.reasons & kVisibilityReasons) != 0) {
        record.category = ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN;
    } else if (record.reasons != 0) {
        record.category = ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_BLOCKED;
    } else {
        record.category = ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE;
    }

    record.name = r_.ObjectName(ref.object);
    record.class_name = r_.ObjectName(ref.cls);
    if (chain.owner != 0) record.owner = r_.ObjectName(chain.owner);
    if (chain.window != 0) record.window = r_.ObjectName(chain.window);
    if (chain.root != 0) record.root = r_.ObjectName(chain.root);
    if (record.cause.empty() && cause != 0 && cause != ref.object) {
        record.cause = r_.ObjectName(cause);
    }
    record.depth = chain.depth;
    for (auto it = chain.user_widgets.rbegin(); it != chain.user_widgets.rend(); ++it) {
        record.path_names.push_back(r_.ObjectName(*it));
    }
    for (const auto& name : record.path_names) {
        if (!record.path.empty()) record.path += " / ";
        record.path += name;
    }
    if (record.category != ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN) {
        if (kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_HTUI) {
            record.text = r_.Text(Reflection::At(ref.object, o.htui_text));
        } else if (htui_radio) {
            const auto text_block = r_.Pointer(Reflection::At(ref.object, o.radio_text));
            if (text_block != 0) record.text = r_.Text(Reflection::At(text_block, o.text_block_text));
        } else if (kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_LIST_ENTRY) {
            const auto button = r_.Pointer(Reflection::At(ref.object, o.item_click_button));
            if (button != 0 && r_.IsA(r_.Pointer(Reflection::At(button, o.object_class)),
                                      t.htui_button)) {
                record.text = r_.Text(Reflection::At(button, o.htui_text));
            }
        }
    }
    return true;
}

ClickOutcome Evaluator::ClickCommon(const ButtonRecord& record, std::uint32_t& calls) {
    const auto& o = r_.Layout();
    const auto& t = r_.Type();
    const auto object = record.ref.object;
    const bool htui = record.kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_HTUI;
    ClickOutcome result;
    std::int64_t ticks_before{};
    if (htui) static_cast<void>(r_.Read(Reflection::At(object, o.htui_last_click), ticks_before));
    const std::array<std::pair<std::uintptr_t, const char*>, 3> steps{{
        {t.pressed, "HandleButtonPressed"},
        {t.released, "HandleButtonReleased"},
        {t.clicked, "HandleButtonClicked"},
    }};
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (!r_.Invoke(calls, object, steps[i].first, nullptr, 0)) {
            result.status = ANOMALY_STATUS_V1_FAILED;
            result.detail = std::string(steps[i].second) + " faulted";
            return result;
        }
        ++result.invocations;
        // HTUI_Button::NativeOnClicked runs only after NativeOnPressed armed this flag.
        if (i == 0 && htui && r_.Flag(Reflection::At(object, o.htui_pressed))) {
            result.outcome |= ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_PRESS_ARMED;
        }
    }
    result.status = ANOMALY_STATUS_V1_OK;
    result.detail = "press, release, click";
    if (htui) {
        std::int64_t ticks_after{};
        static_cast<void>(r_.Read(Reflection::At(object, o.htui_last_click), ticks_after));
        if (ticks_after != ticks_before) {
            result.outcome |= ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_CLICK_ACCEPTED;
        } else {
            result.status = ANOMALY_STATUS_V1_FAILED;
            result.detail = (result.outcome & ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_PRESS_ARMED) != 0
                ? "the button discarded the click"
                : "the button did not arm (click interval or not interactable)";
        }
    }
    return result;
}

std::vector<Evaluator::Bound> Evaluator::Bindings(std::uintptr_t object, std::int64_t offset,
                                                  std::uint8_t num_parms,
                                                  std::uint16_t parms_size) {
    const auto& o = r_.Layout();
    std::vector<Bound> bound;
    TArrayHeader list{};
    if (!r_.Read(Reflection::At(object, offset), list) || list.data == 0 || list.count <= 0) {
        return bound;
    }
    const auto limit =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(list.count), kMaxDelegateEntries);
    for (std::uint32_t i = 0; i < limit; ++i) {
        const auto entry = list.data + static_cast<std::uintptr_t>(i) *
            static_cast<std::uintptr_t>(o.delegate_stride);
        std::int32_t index{-1};
        std::int32_t serial{};
        std::uint64_t fname{};
        if (!r_.Read(entry + static_cast<std::uintptr_t>(o.delegate_object_index), index) ||
            !r_.Read(entry + static_cast<std::uintptr_t>(o.delegate_serial), serial) ||
            !r_.Read(entry + static_cast<std::uintptr_t>(o.delegate_function_name), fname) ||
            index < 0) {
            continue;
        }
        // Resolve the weak target like FWeakObjectPtr::Get: slot serial must match.
        std::uintptr_t target{};
        std::uint32_t slot_serial{};
        if (!r_.ReadSlot(static_cast<std::uint32_t>(index), target, slot_serial) ||
            target == 0 || slot_serial == 0 ||
            slot_serial != static_cast<std::uint32_t>(serial)) {
            continue;
        }
        const auto function =
            r_.FindFunction(r_.Pointer(Reflection::At(target, o.object_class)), fname);
        if (r_.Signature(function, num_parms, parms_size)) bound.push_back({target, function});
    }
    return bound;
}

ClickOutcome Evaluator::ClickUmg(const ButtonRecord& record, std::uint32_t& calls) {
    const auto& o = r_.Layout();
    ClickOutcome result;
    const std::array<std::pair<std::int64_t, const char*>, 3> steps{{
        {o.button_on_pressed, "OnPressed"},
        {o.button_on_released, "OnReleased"},
        {o.button_on_clicked, "OnClicked"},
    }};
    std::array<std::vector<Bound>, 3> bound;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        bound[i] = Bindings(record.ref.object, steps[i].first, 0, 0);
    }
    if (bound[0].empty() && bound[1].empty() && bound[2].empty()) {
        result.status = ANOMALY_STATUS_V1_UNAVAILABLE;
        result.detail = "OnPressed, OnReleased and OnClicked have no bindings";
        return result;
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        for (const auto& entry : bound[i]) {
            if (!r_.Invoke(calls, entry.target, entry.function, nullptr, 0)) {
                result.status = ANOMALY_STATUS_V1_FAILED;
                result.detail = std::string(steps[i].second) + " binding faulted";
                return result;
            }
            ++result.invocations;
        }
    }
    result.status = ANOMALY_STATUS_V1_OK;
    result.detail = "press, release, click";
    return result;
}

ClickOutcome Evaluator::ClickRadio(const ButtonRecord& record, std::uint32_t& calls) {
    const auto& o = r_.Layout();
    const auto& t = r_.Type();
    const auto object = record.ref.object;
    ClickOutcome result;
    const auto fail = [&result](const char* detail) {
        result.status = ANOMALY_STATUS_V1_FAILED;
        result.detail = detail;
        return result;
    };
    // SetSelected(bSelected, bSendEvent). HTUI_RadioBox forwards to its HTRadioBox, whose
    // OnRadioBoxSeleced callback checks the feature lock and broadcasts OnChecked to the
    // window; an unchanged state is a no-op, which is still a successful click.
    struct SelectParms {
        bool selected;
        bool send_event;
    };
    const bool htui = r_.IsA(record.ref.cls, t.htui_radio);
    const bool ht = !htui && r_.IsA(record.ref.cls, t.ht_radio) && t.ht_radio_select != 0;
    bool checked{};
    if (htui || ht) {
        SelectParms parms{true, true};
        if (!r_.Invoke(calls, object, htui ? t.radio_select : t.ht_radio_select, &parms,
                       sizeof(parms))) {
            return fail("SetSelected faulted");
        }
        ++result.invocations;
        result.detail = "SetSelected(true, true)";
        const auto query = htui ? t.radio_is_checked : t.check_is_checked;
        if (query != 0 && r_.QueryBool(calls, object, query, checked)) {
            result.outcome |= checked ? ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_CLICK_ACCEPTED : 0u;
        }
    } else {
        // Plain CheckBox: SetIsChecked does not broadcast, so the bindings of
        // OnCheckStateChanged are called as Slate would after a click.
        std::array<std::uint8_t, 8> parms{};
        parms[0] = 1;
        if (!r_.Invoke(calls, object, t.check_set_checked, parms.data(), parms.size())) {
            return fail("SetIsChecked faulted");
        }
        ++result.invocations;
        for (const auto& entry : Bindings(object, o.check_on_changed, 1, 1)) {
            std::array<std::uint8_t, 8> event{};
            event[0] = 1;
            if (!r_.Invoke(calls, entry.target, entry.function, event.data(), event.size())) {
                return fail("OnCheckStateChanged binding faulted");
            }
            ++result.invocations;
        }
        result.detail = "SetIsChecked(true), OnCheckStateChanged(true)";
        if (t.check_is_checked != 0 && r_.QueryBool(calls, object, t.check_is_checked, checked)) {
            result.outcome |= checked ? ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_CLICK_ACCEPTED : 0u;
        }
    }
    result.status = ANOMALY_STATUS_V1_OK;
    return result;
}

ClickOutcome Evaluator::ClickListEntry(const ButtonRecord& record, std::uint32_t& calls) {
    const auto& t = r_.Type();
    ClickOutcome result;
    // HTUI_ListItem's Btn_Click handlers; OnBtnClicked checks the entry and forwards the click
    // to the owning list view's item-click handler.
    const std::array<std::pair<std::uintptr_t, const char*>, 3> steps{{
        {t.item_pressed, "OnBtnPressed"},
        {t.item_released, "OnBtnReleased"},
        {t.item_clicked, "OnBtnClicked"},
    }};
    for (const auto& [function, name] : steps) {
        if (!r_.Invoke(calls, record.ref.object, function, nullptr, 0)) {
            result.status = ANOMALY_STATUS_V1_FAILED;
            result.detail = std::string(name) + " faulted";
            return result;
        }
        ++result.invocations;
    }
    result.status = ANOMALY_STATUS_V1_OK;
    result.detail = "press, release, click";
    return result;
}

ClickOutcome Evaluator::Click(const ButtonRecord& record, std::uint32_t& calls) {
    switch (record.kind) {
    case ANOMALY_NTE_UI_BUTTON_KIND_V1_UMG:
        return ClickUmg(record, calls);
    case ANOMALY_NTE_UI_BUTTON_KIND_V1_RADIO:
        return ClickRadio(record, calls);
    case ANOMALY_NTE_UI_BUTTON_KIND_V1_LIST_ENTRY:
        return ClickListEntry(record, calls);
    default:
        return ClickCommon(record, calls);
    }
}

bool Evaluator::Hovered(const ObjRef& ref, std::uint32_t& calls, bool& hovered) {
    hovered = false;
    return r_.Alive(ref) && r_.QueryBool(calls, ref.object, r_.Type().is_hovered, hovered);
}

}  // namespace anomaly::nte_ui_buttons

namespace anomaly {
namespace {

using Clock = std::chrono::steady_clock;
using nte_ui_buttons::ButtonRecord;
using nte_ui_buttons::ClickOutcome;
using nte_ui_buttons::Layers;
using nte_ui_buttons::ObjRef;
using nte_ui_buttons::WindowRecord;

constexpr std::size_t kMaxOpenRequests = 32;
constexpr std::size_t kRetainedCompletedRequests = 64;

AnomalyStatusV1 Result(std::uint32_t code, std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
}

// Copies up to N bytes, cutting on a UTF-8 code point boundary, and null-terminates.
template <std::size_t N>
void CopyText(char (&destination)[N], std::string_view value) noexcept {
    std::size_t size = std::min(value.size(), N - 1U);
    if (size < value.size()) {
        while (size > 0 && (static_cast<unsigned char>(value[size]) & 0xC0u) == 0x80u) --size;
    }
    std::memcpy(destination, value.data(), size);
    destination[size] = '\0';
}

template <typename T>
bool Covers(const T* value) noexcept {
    return value != nullptr && value->struct_size >= sizeof(T);
}

std::string_view View(AnomalyStringViewV1 value) noexcept {
    return value.data == nullptr ? std::string_view{} : std::string_view(value.data, value.size);
}

struct Catalog {
    std::uint64_t sequence{};
    std::uint64_t object_generation{};
    bool truncated{};
    std::vector<ButtonRecord> buttons;  // clickable, blocked, hidden; then by window and name
    std::vector<WindowRecord> windows;
    std::vector<ObjRef> containers;
    std::array<std::uint32_t, 4> counts{};
    std::uint32_t objects_scanned{}, process_event_calls{}, ticks{}, milliseconds{};
};

struct Request {
    std::uint64_t id{};
    std::uint32_t kind{};
    std::uint32_t state{ANOMALY_NTE_UI_BUTTON_REQUEST_V1_QUEUED};
    std::uint32_t status{ANOMALY_STATUS_V1_OK};
    std::uint64_t catalog_sequence{};
    std::uint32_t reasons{}, outcome{}, invocations{}, checked{}, calls{};
    std::string detail;
    AnomalyGenerationHandleV1 button{};
    std::uint32_t click_flags{};
    std::vector<ButtonRecord> hits;
    std::uint64_t hits_generation{};
    // Completion order; the oldest completed request is dropped first.
    std::uint64_t completed_order{};
};

void FillButton(const ButtonRecord& record, std::uint64_t catalog_sequence,
                std::uint64_t object_generation, std::uint32_t index,
                AnomalyNteUiButtonSnapshotV1& out) noexcept {
    const auto struct_size = out.struct_size;
    out = {};
    out.struct_size = struct_size;
    out.kind = record.kind;
    out.catalog_sequence = catalog_sequence;
    out.button = {(static_cast<std::uint64_t>(record.ref.serial) << 32u) |
                      (static_cast<std::uint64_t>(record.ref.index) + 1U),
                  object_generation};
    out.index = index;
    out.category = record.category;
    out.reasons = record.reasons;
    out.depth = record.depth;
    CopyText(out.name, record.name);
    CopyText(out.class_name, record.class_name);
    CopyText(out.window, record.window);
    CopyText(out.owner, record.owner);
    CopyText(out.root, record.root);
    CopyText(out.cause, record.cause);
    CopyText(out.text, record.text);
    CopyText(out.path, record.path);
}

bool Matches(const ButtonRecord& record, const AnomalyNteUiButtonQueryV1& query) {
    if (query.category_mask != 0 &&
        (query.category_mask & ANOMALY_NTE_UI_BUTTON_QUERY_V1_CATEGORY(record.category)) == 0) {
        return false;
    }
    const auto name = View(query.name);
    const auto text = View(query.text);
    const auto window = View(query.window);
    if (!name.empty() && record.name != name) return false;
    if (!text.empty() && record.text != text) return false;
    if (window.empty()) return true;
    return record.window == window || record.owner == window || record.root == window ||
        std::find(record.path_names.begin(), record.path_names.end(), window) !=
            record.path_names.end();
}

}  // namespace

struct NteUiButtons::Impl {
    Impl(const BuildProfile& profile, std::shared_ptr<const SymbolMemory> memory,
         NteUiButtonsBindings bindings, NteUiButtonsBudget budget_value)
        : reflection(profile, std::move(memory), std::move(bindings)),
          evaluator(reflection), budget(budget_value) {}

    // ---- Game thread only -------------------------------------------------------------
    nte_ui_buttons::Reflection reflection;
    nte_ui_buttons::Evaluator evaluator;
    NteUiButtonsBudget budget;

    enum class Phase : std::uint8_t { Idle, Collect, Evaluate, Hover };
    struct Work {
        Phase phase{Phase::Idle};
        std::uint64_t request{};
        std::uint32_t cursor{};
        std::vector<std::pair<ObjRef, std::uint32_t>> candidates;
        std::vector<ObjRef> containers;
        Layers layers;
        std::vector<ButtonRecord> records;
        std::vector<ObjRef> hover_candidates;
        std::vector<ObjRef> hover_matches;
        std::shared_ptr<const Catalog> hover_catalog;
        Clock::time_point started{}, phase_started{};
        std::uint32_t calls{}, ticks{}, objects_total{}, objects_scanned{};
        bool truncated{};
    } work;
    std::uint64_t object_generation{};
    // Prepare is attempted once per registry generation, and again for each request while
    // it keeps failing.
    bool prepare_attempted{};
    std::string prepare_error;
    std::uint64_t seen_epoch{};

    // ---- Shared, under mutex ---------------------------------------------------------
    mutable std::mutex mutex;
    std::uint64_t epoch{1};
    std::uint64_t next_request{1};
    std::uint64_t next_completion{1};
    std::uint64_t catalog_sequence{};
    std::shared_ptr<const Catalog> catalog;
    std::deque<Request> requests;  // open requests in FIFO order, then retained completed ones
    bool prepared{};
    bool pick_available{};
    bool scanning{};

    [[nodiscard]] Request* FindLocked(AnomalyGenerationHandleV1 handle) {
        if (handle.generation != epoch) return nullptr;
        const auto found = std::find_if(requests.begin(), requests.end(),
                                        [&](const Request& r) { return r.id == handle.id; });
        return found == requests.end() ? nullptr : &*found;
    }

    [[nodiscard]] std::size_t OpenCountLocked() const {
        return static_cast<std::size_t>(std::count_if(
            requests.begin(), requests.end(), [](const Request& r) {
                return r.state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE;
            }));
    }

    // Marks a request complete without touching the container, so it is safe inside loops.
    void MarkCompleteLocked(Request& request, std::uint32_t status, std::string detail) {
        request.state = ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE;
        request.status = status;
        request.completed_order = next_completion++;
        if (!detail.empty()) request.detail = std::move(detail);
    }

    void TrimLocked() {
        std::size_t completed = 0;
        for (const auto& r : requests) {
            if (r.state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) ++completed;
        }
        while (completed > kRetainedCompletedRequests) {
            auto oldest = requests.end();
            for (auto it = requests.begin(); it != requests.end(); ++it) {
                if (it->state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE &&
                    (oldest == requests.end() || it->completed_order < oldest->completed_order)) {
                    oldest = it;
                }
            }
            requests.erase(oldest);
            --completed;
        }
    }

    // Invalidates pointers into requests.
    void CompleteLocked(Request& request, std::uint32_t status, std::string detail) {
        MarkCompleteLocked(request, status, std::move(detail));
        TrimLocked();
    }

    AnomalyStatusV1 Enqueue(Request request, AnomalyGenerationHandleV1* handle) {
        if (handle == nullptr) return Result(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        *handle = {};
        std::scoped_lock lock(mutex);
        if (OpenCountLocked() >= kMaxOpenRequests) {
            return Result(ANOMALY_STATUS_V1_CONFLICT, "too many open UI button requests");
        }
        request.id = next_request++;
        *handle = {request.id, epoch};
        requests.push_back(std::move(request));
        return Result(ANOMALY_STATUS_V1_OK);
    }

    // The oldest open request, copied so the Game thread can work without the lock.
    [[nodiscard]] std::optional<Request> FrontOpen() {
        std::scoped_lock lock(mutex);
        for (auto& request : requests) {
            if (request.state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) continue;
            request.state = ANOMALY_NTE_UI_BUTTON_REQUEST_V1_RUNNING;
            return request;
        }
        return std::nullopt;
    }

    // False when the request was cancelled or invalidated meanwhile.
    template <typename Update>
    bool UpdateRequest(std::uint64_t id, Update&& update) {
        std::scoped_lock lock(mutex);
        const auto found = std::find_if(requests.begin(), requests.end(),
                                        [&](const Request& r) { return r.id == id; });
        if (found == requests.end() || found->state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
            return false;
        }
        update(*found);
        return true;
    }

    void Finish(std::uint64_t id, std::uint32_t status, std::string detail) {
        std::scoped_lock lock(mutex);
        const auto found = std::find_if(requests.begin(), requests.end(),
                                        [&](const Request& r) { return r.id == id; });
        if (found != requests.end() && found->state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
            CompleteLocked(*found, status, std::move(detail));
        }
    }

    void AbortWork() {
        work = {};
        std::scoped_lock lock(mutex);
        scanning = false;
    }

    void BeginScan(std::uint64_t request) {
        work = {};
        work.phase = Phase::Collect;
        work.request = request;
        work.started = work.phase_started = Clock::now();
        reflection.BeginPass();
        evaluator.BeginPass();
        std::scoped_lock lock(mutex);
        scanning = true;
    }

    [[nodiscard]] bool OverSlice(Clock::time_point slice_start, std::uint32_t calls_at_start) const {
        return Clock::now() - slice_start > std::chrono::microseconds(budget.slice_microseconds) ||
            work.calls - calls_at_start >= budget.max_calls_per_tick;
    }

    [[nodiscard]] bool OverPhase() const {
        return Clock::now() - work.phase_started >
            std::chrono::milliseconds(budget.phase_limit_milliseconds);
    }

    // Advances the scan one slice; returns the finished catalog.
    std::shared_ptr<Catalog> StepScan(const NteUiButtonsTickInput& input) {
        const auto slice_start = Clock::now();
        const auto calls_at_start = work.calls;
        ++work.ticks;
        if (work.phase == Phase::Collect) {
            const auto count = input.registry.count;
            work.objects_total = count;
            while (work.cursor < count) {
                if ((work.cursor & 1023u) == 0 && work.cursor != 0 &&
                    Clock::now() - slice_start >
                        std::chrono::microseconds(budget.slice_microseconds)) {
                    break;
                }
                ObjRef ref;
                if (!reflection.MakeRef(work.cursor++, ref)) continue;
                if (const auto kind = reflection.ObjectKind(ref); kind != 0) {
                    work.candidates.emplace_back(ref, kind);
                    if (work.candidates.size() >= budget.max_buttons) {
                        work.truncated = true;
                        work.cursor = count;
                    }
                } else if ((reflection.Traits(ref.cls) & nte_ui_buttons::kTraitContainer) != 0 &&
                           work.containers.size() < budget.max_containers) {
                    work.containers.push_back(ref);
                }
            }
            work.objects_scanned = work.cursor;
            const bool collected = work.cursor >= count;
            if (!collected && OverPhase()) work.truncated = true;
            if (collected || work.truncated) {
                work.phase = Phase::Evaluate;
                work.cursor = 0;
                work.phase_started = Clock::now();
                work.layers = evaluator.ReadLayers(work.containers);
            }
            // Evaluation starts next tick so this slice does not also spend game calls.
            return nullptr;
        }
        while (work.cursor < work.candidates.size() && !OverSlice(slice_start, calls_at_start)) {
            const auto& [ref, kind] = work.candidates[work.cursor++];
            ButtonRecord record;
            if (evaluator.Evaluate(work.layers, ref, kind, true, work.calls, record)) {
                work.records.push_back(std::move(record));
            }
        }
        if (work.cursor < work.candidates.size() && !OverPhase()) return nullptr;
        if (work.cursor < work.candidates.size()) work.truncated = true;

        auto result = std::make_shared<Catalog>();
        std::stable_sort(work.records.begin(), work.records.end(),
                         [](const ButtonRecord& a, const ButtonRecord& b) {
                             if (a.category != b.category) return a.category < b.category;
                             if (a.root != b.root) return a.root < b.root;
                             if (a.window != b.window) return a.window < b.window;
                             if (a.owner != b.owner) return a.owner < b.owner;
                             return a.name < b.name;
                         });
        for (const auto& record : work.records) ++result->counts[record.category];
        result->object_generation = input.object_generation;
        result->truncated = work.truncated;
        result->buttons = std::move(work.records);
        result->windows = std::move(work.layers.windows);
        result->containers = std::move(work.containers);
        result->objects_scanned = work.objects_scanned;
        result->process_event_calls = work.calls;
        result->ticks = work.ticks;
        result->milliseconds = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - work.started)
                .count());
        return result;
    }

    std::uint64_t Publish(std::shared_ptr<Catalog> next) {
        std::scoped_lock lock(mutex);
        next->sequence = ++catalog_sequence;
        catalog = std::move(next);
        scanning = false;
        return catalog->sequence;
    }

    void StartHover(std::shared_ptr<const Catalog> source) {
        const auto request = work.request;
        const auto calls = work.calls;
        work = {};
        work.phase = Phase::Hover;
        work.request = request;
        work.calls = calls;
        work.started = work.phase_started = Clock::now();
        // Hidden buttons have no Slate widget on the hit path. Blocked ones are still asked:
        // a hovered button the Host calls occluded exposes a wrong occlusion verdict.
        for (const auto& record : source->buttons) {
            if (record.category != ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN) {
                work.hover_candidates.push_back(record.ref);
            }
        }
        work.hover_catalog = std::move(source);
    }

    // True when the pick finished (hits evaluated) this tick.
    bool StepHover() {
        const auto slice_start = Clock::now();
        const auto calls_at_start = work.calls;
        while (work.cursor < work.hover_candidates.size()) {
            if (OverSlice(slice_start, calls_at_start)) return false;
            const auto& ref = work.hover_candidates[work.cursor++];
            bool hovered{};
            if (evaluator.Hovered(ref, work.calls, hovered) && hovered) {
                work.hover_matches.push_back(ref);
            }
        }
        return true;
    }

    [[nodiscard]] bool ResolveButton(AnomalyGenerationHandleV1 handle, ObjRef& ref,
                                     std::uint32_t& kind) {
        const auto encoded = static_cast<std::uint32_t>(handle.id);
        if (handle.generation != object_generation || encoded == 0) return false;
        if (!reflection.MakeRef(encoded - 1U, ref) ||
            ref.serial != static_cast<std::uint32_t>(handle.id >> 32u)) {
            return false;
        }
        kind = reflection.ObjectKind(ref);
        return kind != 0 && reflection.Alive(ref);
    }

    void RunClick(const Request& request, const std::shared_ptr<const Catalog>& current) {
        ObjRef ref;
        std::uint32_t kind{};
        if (current == nullptr || !ResolveButton(request.button, ref, kind)) {
            Finish(request.id, ANOMALY_STATUS_V1_NOT_FOUND, "button handle is stale");
            return;
        }
        std::uint32_t calls{};
        // Live re-evaluation: windows opened since the scan must still block the click.
        const auto layers = evaluator.ReadLayers(current->containers);
        ButtonRecord record;
        if (!evaluator.Evaluate(layers, ref, kind, false, calls, record)) {
            Finish(request.id, ANOMALY_STATUS_V1_NOT_FOUND, "button handle is stale");
            return;
        }
        const bool force = (request.click_flags & ANOMALY_NTE_UI_BUTTON_CLICK_V1_FORCE) != 0;
        if (!force && record.category != ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE) {
            std::scoped_lock lock(mutex);
            if (auto* open = FindLocked({request.id, epoch});
                open != nullptr && open->state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
                open->reasons = record.reasons;
                open->calls = calls;
                open->catalog_sequence = current->sequence;
                CompleteLocked(*open, ANOMALY_STATUS_V1_CONFLICT,
                               "button is not clickable" +
                                   (record.cause.empty() ? std::string{} : ": " + record.cause));
            }
            return;
        }
        const auto outcome = evaluator.Click(record, calls);
        std::scoped_lock lock(mutex);
        if (auto* open = FindLocked({request.id, epoch});
            open != nullptr && open->state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
            open->reasons = record.reasons;
            open->outcome = outcome.outcome | (force ? ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_FORCED : 0u);
            open->invocations = outcome.invocations;
            open->calls = calls;
            open->catalog_sequence = current->sequence;
            CompleteLocked(*open, outcome.status, outcome.detail);
        }
    }

    void FinishPick() {
        std::uint32_t calls = work.calls;
        const auto layers = evaluator.ReadLayers(work.hover_catalog->containers);
        std::vector<ButtonRecord> hits;
        for (const auto& ref : work.hover_matches) {
            ButtonRecord record;
            const auto kind = reflection.ObjectKind(ref);
            if (kind != 0 && evaluator.Evaluate(layers, ref, kind, false, calls, record)) {
                hits.push_back(std::move(record));
            }
        }
        // Slate marks the whole path under the cursor as hovered; the deepest button is the
        // one the cursor points at.
        std::stable_sort(hits.begin(), hits.end(), [](const ButtonRecord& a, const ButtonRecord& b) {
            return a.depth > b.depth;
        });
        const auto catalog_used = work.hover_catalog;
        const auto checked = static_cast<std::uint32_t>(work.hover_candidates.size());
        const auto id = work.request;
        work = {};
        std::scoped_lock lock(mutex);
        if (auto* open = FindLocked({id, epoch});
            open != nullptr && open->state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
            open->hits = std::move(hits);
            open->hits_generation = object_generation;
            open->checked = checked;
            open->calls = calls;
            open->catalog_sequence = catalog_used->sequence;
            if (catalog_used->truncated) open->outcome |= ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_TRUNCATED;
            CompleteLocked(*open, ANOMALY_STATUS_V1_OK,
                           open->hits.empty() ? "no hovered button" : std::string{});
        }
    }

    // Runs the oldest open request for one slice. Returns false when nothing progressed.
    bool StepFront(const NteUiButtonsTickInput& input) {
        const auto front = FrontOpen();
        if (!front.has_value()) return false;
        // A cancelled or superseded request loses its in-flight work.
        if (work.phase != Phase::Idle && work.request != front->id) AbortWork();

        if (!reflection.Prepared()) {
            Finish(front->id, ANOMALY_STATUS_V1_UNAVAILABLE, prepare_error);
            return true;
        }
        std::shared_ptr<const Catalog> current;
        {
            std::scoped_lock lock(mutex);
            current = catalog;
        }
        if (front->kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_CLICK) {
            RunClick(*front, current);
            return true;
        }
        if (front->kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_PICK &&
            reflection.Type().is_hovered == 0) {
            Finish(front->id, ANOMALY_STATUS_V1_UNAVAILABLE, "UMG.Widget.IsHovered is unavailable");
            return true;
        }
        if (work.phase == Phase::Idle) BeginScan(front->id);
        if (work.phase == Phase::Collect || work.phase == Phase::Evaluate) {
            auto next = StepScan(input);
            if (next == nullptr) {
                static_cast<void>(UpdateRequest(front->id, [&](Request& r) { r.calls = work.calls; }));
                return true;
            }
            const bool truncated = next->truncated;
            std::shared_ptr<const Catalog> published = next;
            const auto sequence = Publish(std::move(next));
            if (front->kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_SCAN) {
                const auto calls = work.calls;
                work = {};
                std::scoped_lock lock(mutex);
                if (auto* open = FindLocked({front->id, epoch});
                    open != nullptr && open->state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
                    open->catalog_sequence = sequence;
                    open->calls = calls;
                    open->checked = published->objects_scanned;
                    if (truncated) open->outcome |= ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_TRUNCATED;
                    CompleteLocked(*open, ANOMALY_STATUS_V1_OK, {});
                }
                return true;
            }
            StartHover(published);
            return true;
        }
        if (work.phase == Phase::Hover) {
            if (StepHover()) {
                FinishPick();
            } else {
                static_cast<void>(UpdateRequest(front->id, [&](Request& r) {
                    r.calls = work.calls;
                    r.checked = work.cursor;
                }));
            }
        }
        return true;
    }

    void Tick(const NteUiButtonsTickInput& input) {
        std::uint64_t current_epoch{};
        bool has_open{};
        {
            std::scoped_lock lock(mutex);
            current_epoch = epoch;
            has_open = OpenCountLocked() != 0;
        }
        if (current_epoch != seen_epoch) {
            // Invalidate ran: forget everything bound to the previous Host generation.
            seen_epoch = current_epoch;
            work = {};
            reflection.Reset();
            prepare_attempted = false;
        }
        if (!input.available) {
            if (has_open || work.phase != Phase::Idle) {
                AbortWork();
                std::scoped_lock lock(mutex);
                catalog.reset();
                for (auto& request : requests) {
                    if (request.state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
                        MarkCompleteLocked(request, ANOMALY_STATUS_V1_UNAVAILABLE,
                                           "object registry is unavailable");
                    }
                }
                TrimLocked();
            }
            return;
        }
        reflection.SetRegistry(input.registry);
        if (input.object_generation != object_generation) {
            // Objects and classes may have been reallocated: the catalog and any in-flight
            // pass describe a registry that no longer exists.
            object_generation = input.object_generation;
            work = {};
            reflection.Reset();
            prepare_attempted = false;
            std::scoped_lock lock(mutex);
            catalog.reset();
            scanning = false;
            prepared = false;
            pick_available = false;
        }
        // Once per registry generation so status reports readiness before any request, and
        // again whenever a request is waiting on a failed preparation.
        if (!reflection.Prepared() && (!prepare_attempted || has_open)) {
            prepare_attempted = true;
            prepare_error.clear();
            const bool ready = reflection.Prepare(prepare_error);
            std::scoped_lock lock(mutex);
            prepared = ready;
            pick_available = ready && reflection.Type().is_hovered != 0;
        }
        if (!has_open) return;
        const auto slice_start = Clock::now();
        // Clicks are cheap: several may complete in one tick, a scan slice ends the tick.
        for (std::uint32_t step = 0; step < 8; ++step) {
            const auto before = work.phase;
            if (!StepFront(input)) break;
            if (work.phase != Phase::Idle || before != Phase::Idle) break;
            if (Clock::now() - slice_start > std::chrono::microseconds(budget.slice_microseconds)) {
                break;
            }
        }
    }
};

NteUiButtons::NteUiButtons(const BuildProfile& profile, std::shared_ptr<const SymbolMemory> memory,
                           NteUiButtonsBindings bindings, NteUiButtonsBudget budget)
    : impl_(std::make_unique<Impl>(profile, std::move(memory), std::move(bindings), budget)) {}

NteUiButtons::~NteUiButtons() = default;

void NteUiButtons::Tick(const NteUiButtonsTickInput& input) noexcept {
    try {
        impl_->Tick(input);
    } catch (...) {
        // A failed allocation drops the pass; the open request completes on a later tick.
        impl_->work = {};
    }
}

void NteUiButtons::Invalidate(std::uint32_t status) noexcept {
    std::scoped_lock lock(impl_->mutex);
    ++impl_->epoch;
    impl_->catalog.reset();
    impl_->scanning = false;
    impl_->prepared = false;
    impl_->pick_available = false;
    for (auto& request : impl_->requests) {
        if (request.state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
            request.state = ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE;
            request.status = status;
            request.detail = "UI button service was reset";
        }
    }
    // Handles carry the epoch, so nothing from before this point is addressable.
    impl_->requests.clear();
}

AnomalyStatusV1 NteUiButtons::Status(AnomalyNteUiButtonsStatusV1* status) const noexcept {
    if (!Covers(status)) return Result(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const auto struct_size = status->struct_size;
    *status = {};
    status->struct_size = struct_size;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->prepared) status->flags |= ANOMALY_NTE_UI_BUTTONS_STATUS_V1_READY;
    if (impl_->pick_available) status->flags |= ANOMALY_NTE_UI_BUTTONS_STATUS_V1_PICK_AVAILABLE;
    if (impl_->scanning) status->flags |= ANOMALY_NTE_UI_BUTTONS_STATUS_V1_SCANNING;
    status->open_requests = static_cast<std::uint32_t>(impl_->OpenCountLocked());
    if (const auto& current = impl_->catalog) {
        status->flags |= ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG;
        if (current->truncated) status->flags |= ANOMALY_NTE_UI_BUTTONS_STATUS_V1_TRUNCATED;
        status->catalog_sequence = current->sequence;
        status->button_count = static_cast<std::uint32_t>(current->buttons.size());
        status->window_count = static_cast<std::uint32_t>(current->windows.size());
        status->clickable_count = current->counts[ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE];
        status->blocked_count = current->counts[ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_BLOCKED];
        status->hidden_count = current->counts[ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN];
        status->objects_scanned = current->objects_scanned;
        status->process_event_calls = current->process_event_calls;
        status->scan_ticks = current->ticks;
        status->scan_milliseconds = current->milliseconds;
    }
    return Result(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 NteUiButtons::ButtonAt(std::uint64_t catalog_sequence, std::uint32_t index,
                                       AnomalyNteUiButtonSnapshotV1* snapshot) const noexcept {
    if (!Covers(snapshot)) return Result(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(impl_->mutex);
    const auto& current = impl_->catalog;
    if (current == nullptr || current->sequence != catalog_sequence) {
        return Result(ANOMALY_STATUS_V1_NOT_FOUND, "catalog sequence is not current");
    }
    if (index >= current->buttons.size()) return Result(ANOMALY_STATUS_V1_NOT_FOUND);
    FillButton(current->buttons[index], current->sequence, current->object_generation, index,
               *snapshot);
    return Result(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 NteUiButtons::WindowAt(std::uint64_t catalog_sequence, std::uint32_t index,
                                       AnomalyNteUiWindowSnapshotV1* snapshot) const noexcept {
    if (!Covers(snapshot)) return Result(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(impl_->mutex);
    const auto& current = impl_->catalog;
    if (current == nullptr || current->sequence != catalog_sequence) {
        return Result(ANOMALY_STATUS_V1_NOT_FOUND, "catalog sequence is not current");
    }
    if (index >= current->windows.size()) return Result(ANOMALY_STATUS_V1_NOT_FOUND);
    const auto& window = current->windows[index];
    const auto struct_size = snapshot->struct_size;
    *snapshot = {};
    snapshot->struct_size = struct_size;
    snapshot->catalog_sequence = current->sequence;
    snapshot->index = index;
    if (window.active) snapshot->flags |= ANOMALY_NTE_UI_WINDOW_V1_ACTIVE;
    if (window.visible) snapshot->flags |= ANOMALY_NTE_UI_WINDOW_V1_VISIBLE;
    if (window.closing) snapshot->flags |= ANOMALY_NTE_UI_WINDOW_V1_CLOSING;
    if (window.modal) snapshot->flags |= ANOMALY_NTE_UI_WINDOW_V1_MODAL;
    if (window.hides_main_form) snapshot->flags |= ANOMALY_NTE_UI_WINDOW_V1_HIDES_MAIN_FORM;
    if (window.pauses_game) snapshot->flags |= ANOMALY_NTE_UI_WINDOW_V1_PAUSES_GAME;
    if (window.menu_input) snapshot->flags |= ANOMALY_NTE_UI_WINDOW_V1_MENU_INPUT;
    if (window.Blocking()) snapshot->flags |= ANOMALY_NTE_UI_WINDOW_V1_BLOCKING;
    CopyText(snapshot->layer, window.layer);
    CopyText(snapshot->window, window.name);
    return Result(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 NteUiButtons::Find(const AnomalyNteUiButtonQueryV1* query,
                                   AnomalyNteUiButtonSnapshotV1* snapshot,
                                   std::uint32_t* match_count) const noexcept {
    if (!Covers(query) || !Covers(snapshot) || match_count == nullptr) {
        return Result(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *match_count = 0;
    try {
        std::scoped_lock lock(impl_->mutex);
        const auto& current = impl_->catalog;
        if (current == nullptr ||
            (query->catalog_sequence != 0 && current->sequence != query->catalog_sequence)) {
            return Result(ANOMALY_STATUS_V1_NOT_FOUND, "catalog sequence is not current");
        }
        std::optional<std::uint32_t> first;
        for (std::uint32_t i = 0; i < current->buttons.size(); ++i) {
            if (!Matches(current->buttons[i], *query)) continue;
            if (!first) first = i;
            ++*match_count;
        }
        if (!first) return Result(ANOMALY_STATUS_V1_NOT_FOUND, "no button matches");
        FillButton(current->buttons[*first], current->sequence, current->object_generation,
                   *first, *snapshot);
        return Result(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Result(ANOMALY_STATUS_V1_FAILED);
    }
}

AnomalyStatusV1 NteUiButtons::RequestScan(AnomalyGenerationHandleV1* request) noexcept {
    try {
        Request r;
        r.kind = ANOMALY_NTE_UI_BUTTON_REQUEST_V1_SCAN;
        return impl_->Enqueue(std::move(r), request);
    } catch (...) {
        return Result(ANOMALY_STATUS_V1_FAILED);
    }
}

AnomalyStatusV1 NteUiButtons::RequestPick(AnomalyGenerationHandleV1* request) noexcept {
    try {
        Request r;
        r.kind = ANOMALY_NTE_UI_BUTTON_REQUEST_V1_PICK;
        return impl_->Enqueue(std::move(r), request);
    } catch (...) {
        return Result(ANOMALY_STATUS_V1_FAILED);
    }
}

AnomalyStatusV1 NteUiButtons::RequestClick(const AnomalyNteUiButtonClickRequestV1* click,
                                           AnomalyGenerationHandleV1* request) noexcept {
    if (!Covers(click) || (click->flags & ~ANOMALY_NTE_UI_BUTTON_CLICK_V1_FORCE) != 0 ||
        static_cast<std::uint32_t>(click->button.id) == 0) {
        return Result(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    try {
        Request r;
        r.kind = ANOMALY_NTE_UI_BUTTON_REQUEST_V1_CLICK;
        r.button = click->button;
        r.click_flags = click->flags;
        return impl_->Enqueue(std::move(r), request);
    } catch (...) {
        return Result(ANOMALY_STATUS_V1_FAILED);
    }
}

AnomalyStatusV1 NteUiButtons::RequestSnapshot(
    AnomalyGenerationHandleV1 request, AnomalyNteUiButtonRequestSnapshotV1* snapshot) const noexcept {
    if (!Covers(snapshot)) return Result(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(impl_->mutex);
    const auto* found = impl_->FindLocked(request);
    if (found == nullptr) return Result(ANOMALY_STATUS_V1_NOT_FOUND, "request is not retained");
    const auto struct_size = snapshot->struct_size;
    *snapshot = {};
    snapshot->struct_size = struct_size;
    snapshot->kind = found->kind;
    snapshot->request = request;
    snapshot->state = found->state;
    snapshot->status = found->status;
    snapshot->catalog_sequence = found->catalog_sequence;
    snapshot->reasons = found->reasons;
    snapshot->outcome = found->outcome;
    snapshot->invocations = found->invocations;
    snapshot->hit_count = static_cast<std::uint32_t>(found->hits.size());
    snapshot->checked = found->checked;
    snapshot->process_event_calls = found->calls;
    CopyText(snapshot->detail, found->detail);
    return Result(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 NteUiButtons::PickHitAt(AnomalyGenerationHandleV1 request, std::uint32_t index,
                                        AnomalyNteUiButtonSnapshotV1* snapshot) const noexcept {
    if (!Covers(snapshot)) return Result(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(impl_->mutex);
    const auto* found = impl_->FindLocked(request);
    if (found == nullptr || found->kind != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_PICK ||
        index >= found->hits.size()) {
        return Result(ANOMALY_STATUS_V1_NOT_FOUND);
    }
    // Hits keep the index of the catalog their pick scanned only while it is current.
    FillButton(found->hits[index], found->catalog_sequence, found->hits_generation, index,
               *snapshot);
    return Result(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 NteUiButtons::Cancel(AnomalyGenerationHandleV1 request) noexcept {
    std::scoped_lock lock(impl_->mutex);
    auto* found = impl_->FindLocked(request);
    if (found == nullptr) return Result(ANOMALY_STATUS_V1_NOT_FOUND, "request is not retained");
    if (found->state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
        return Result(ANOMALY_STATUS_V1_CONFLICT, "request is already complete");
    }
    try {
        impl_->CompleteLocked(*found, ANOMALY_STATUS_V1_CANCELLED, "cancelled");
    } catch (...) {
        return Result(ANOMALY_STATUS_V1_FAILED);
    }
    return Result(ANOMALY_STATUS_V1_OK);
}

}  // namespace anomaly
