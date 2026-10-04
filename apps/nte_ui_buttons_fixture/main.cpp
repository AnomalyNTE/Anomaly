// Offline fixture for the Host UI button engine behind anomaly.nte.ui-buttons.
//
// The fake widget world below is laid out with the Dumper-7 5.6.1 HT-1.4a SDK offsets written
// out here by hand; the engine reads its offsets from the shipped profiles/nte/nte-current.json.
// A wrong Profile value therefore breaks classification or clicking in this fixture. Requests
// go through the same NteUiButtons entry points the service table forwards to.
#include "anomaly/build_profile.hpp"
#include "anomaly/nte_ui_buttons.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

// UObject / UStruct / UFunction (CoreUObject_classes.hpp, Basic.hpp)
constexpr std::uintptr_t kFlags = 0x08, kIndex = 0x0C, kClass = 0x10, kName = 0x18, kOuter = 0x20;
constexpr std::uintptr_t kSuper = 0x40, kChildren = 0x48, kNext = 0x28;
constexpr std::uintptr_t kNumParms = 0xB4, kParmsSize = 0xB6;
// UMG_classes.hpp
constexpr std::uintptr_t kSlot = 0x30, kWidgetFlags = 0xD9, kVisibility = 0xDC, kOpacity = 0xE0;
constexpr std::uintptr_t kSlotParent = 0x28, kTreeRoot = 0x30, kPanelSlots = 0x168;
constexpr std::uintptr_t kSwitcherActive = 0x180;
constexpr std::uintptr_t kOnClicked = 0x538, kOnPressed = 0x548, kOnReleased = 0x558;
// CommonUI_classes.hpp
constexpr std::uintptr_t kLocked = 0x468, kModal = 0x382, kActive = 0x3E0;
constexpr std::uintptr_t kWidgetList = 0x190, kDisplayed = 0x1A0;
// HTGame_classes.hpp
constexpr std::uintptr_t kButtonText = 0x1750, kPressedFlag = 0x17F1, kLastClick = 0x1840;
constexpr std::uintptr_t kClosing = 0x61A, kHideMain = 0x755, kHideChildren = 0xF84;
// FUObjectItem
constexpr std::uintptr_t kItemStride = 24, kItemSerial = 0x10;

struct World {
    static constexpr std::size_t kArenaSize = 16u << 20;
    std::unique_ptr<std::uint8_t[]> arena{new std::uint8_t[kArenaSize]{}};
    std::size_t used = 0;
    std::uintptr_t items{}, chunk{};
    std::uint32_t count = 0;
    std::vector<std::string> names;
    std::map<std::wstring, std::uintptr_t> paths;
    std::map<std::uintptr_t, std::string> texts;

    std::uintptr_t cls_class{}, cls_function{}, cls_htui{};
    std::uintptr_t fn_is_visible{}, fn_is_in_viewport{}, fn_is_interaction_enabled{},
        fn_pressed{}, fn_released{}, fn_clicked{}, fn_is_locked{}, fn_is_hovered{};
    std::set<std::uintptr_t> in_viewport, not_interactable, slate_missing, hovered, no_arm;
    std::set<std::uintptr_t> accepted;
    // Tabs, check boxes and list entries.
    std::uintptr_t fn_check_is_checked{}, fn_check_set_checked{}, fn_ht_radio_select{},
        fn_radio_select{}, fn_radio_is_checked{}, fn_radio_feature{}, fn_item_locked{};
    std::set<std::uintptr_t> checked, feature_locked, item_locked;
    struct Call {
        std::uintptr_t object, function;
    };
    std::vector<Call> calls;

    World() {
        items = Alloc(8);
        chunk = Alloc(65536u * kItemStride);
        Put(items, chunk);
    }

    std::uintptr_t Alloc(std::size_t size) {
        used = (used + 15u) & ~std::size_t{15};
        if (used + size > kArenaSize) std::abort();
        const auto address = reinterpret_cast<std::uintptr_t>(arena.get()) + used;
        used += size;
        return address;
    }
    template <typename T>
    void Put(std::uintptr_t address, const T& value) {
        std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
    }
    template <typename T>
    T Get(std::uintptr_t address) const {
        T value{};
        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
        return value;
    }
    std::uint64_t Name(const std::string& name) {
        const auto found = std::find(names.begin(), names.end(), name);
        if (found != names.end()) return static_cast<std::uint64_t>(found - names.begin()) + 1U;
        names.push_back(name);
        return names.size();
    }
    std::uintptr_t Object(std::uintptr_t cls, const std::string& name, std::uintptr_t outer,
                          const std::wstring& path = {}, std::size_t size = 0x2000,
                          std::uint32_t flags = 0) {
        const auto object = Alloc(size);
        const auto index = count++;
        Put(object + kFlags, flags);
        Put<std::int32_t>(object + kIndex, static_cast<std::int32_t>(index));
        Put(object + kClass, cls);
        Put(object + kName, Name(name));
        Put(object + kOuter, outer);
        const auto item = chunk + index * kItemStride;
        Put(item, object);
        Put<std::uint32_t>(item + kItemSerial, 1000u + index);
        if (!path.empty()) paths[path] = object;
        return object;
    }
    std::uintptr_t Class(const std::string& name, std::uintptr_t super,
                         const std::wstring& path = {}) {
        const auto cls = Object(cls_class, name, 0, path, 0x200);
        Put(cls + kSuper, super);
        return cls;
    }
    std::uintptr_t Function(std::uintptr_t owner, const std::string& name, std::uint8_t num_parms,
                            std::uint16_t parms_size, const std::wstring& path = {}) {
        const auto function = Object(cls_function, name, owner, path, 0x100);
        Put(function + kNumParms, num_parms);
        Put(function + kParmsSize, parms_size);
        Put(function + kNext, Get<std::uintptr_t>(owner + kChildren));
        Put(owner + kChildren, function);
        return function;
    }
    std::uint32_t Serial(std::uintptr_t object) const {
        return Get<std::uint32_t>(chunk + Get<std::int32_t>(object + kIndex) * kItemStride +
                                  kItemSerial);
    }
    anomaly::NteUiButtonsTickInput Input(std::uint64_t generation = 1) const {
        anomaly::NteUiButtonsTickInput input;
        input.available = true;
        input.registry = {items, count, 1, 65536, static_cast<std::uint32_t>(kItemStride), 0,
                          static_cast<std::uint32_t>(kItemSerial)};
        input.object_generation = generation;
        return input;
    }

    bool Invoke(std::uintptr_t self, std::uintptr_t fn, void* parameters) {
        calls.push_back({self, fn});
        auto* result = static_cast<std::uint8_t*>(parameters);
        if (fn == fn_is_visible) {
            const auto visibility = Get<std::uint8_t>(self + kVisibility);
            *result = visibility != 1 && visibility != 2 && !slate_missing.contains(self);
        } else if (fn == fn_is_in_viewport) {
            *result = in_viewport.contains(self);
        } else if (fn == fn_is_interaction_enabled) {
            *result = !not_interactable.contains(self);
        } else if (fn == fn_is_locked) {
            *result = 0;
        } else if (fn == fn_is_hovered) {
            *result = hovered.contains(self);
        } else if (fn == fn_check_is_checked || fn == fn_radio_is_checked) {
            *result = checked.contains(self);
        } else if (fn == fn_radio_feature) {
            *result = !feature_locked.contains(self);
        } else if (fn == fn_item_locked) {
            *result = item_locked.contains(self);
        } else if (fn == fn_radio_select || fn == fn_ht_radio_select ||
                   fn == fn_check_set_checked) {
            // SetSelected(bSelected, bSendEvent) / SetIsChecked(bIsChecked).
            if (result[0] != 0) {
                checked.insert(self);
            } else {
                checked.erase(self);
            }
        } else if (Get<std::uintptr_t>(self + kClass) == cls_htui) {
            // HTUI_Button: NativeOnPressed arms +0x17F1; NativeOnClicked runs only when armed,
            // then records the click time and disarms.
            if (fn == fn_pressed && !no_arm.contains(self)) {
                Put<std::uint8_t>(self + kPressedFlag, 1);
            } else if (fn == fn_clicked && Get<std::uint8_t>(self + kPressedFlag) != 0) {
                Put<std::uint8_t>(self + kPressedFlag, 0);
                Put<std::int64_t>(self + kLastClick, Get<std::int64_t>(self + kLastClick) + 1);
                accepted.insert(self);
            }
        }
        return true;
    }
    bool Called(std::uintptr_t object, std::uintptr_t function) const {
        return std::any_of(calls.begin(), calls.end(), [&](const Call& call) {
            return call.object == object && call.function == function;
        });
    }
    bool Touched(std::uintptr_t object) const {
        return std::any_of(calls.begin(), calls.end(),
                           [&](const Call& call) { return call.object == object; });
    }
};

struct DelegateEntry {
    std::int32_t index;
    std::int32_t serial;
    std::uint64_t name;
};

void Bind(World& world, std::uintptr_t button, std::uintptr_t offset, std::uintptr_t target,
          const std::string& function) {
    const auto data = world.Alloc(sizeof(DelegateEntry));
    world.Put(data, DelegateEntry{world.Get<std::int32_t>(target + kIndex),
                                  static_cast<std::int32_t>(world.Serial(target)),
                                  world.Name(function)});
    world.Put(button + offset, data);
    world.Put<std::int32_t>(button + offset + 8, 1);
    world.Put<std::int32_t>(button + offset + 12, 1);
}

using anomaly::NteUiButtons;

AnomalyNteUiButtonRequestSnapshotV1 Wait(NteUiButtons& engine, World& world,
                                         AnomalyGenerationHandleV1 request,
                                         std::uint64_t generation = 1) {
    AnomalyNteUiButtonRequestSnapshotV1 snapshot{sizeof(snapshot)};
    for (int tick = 0; tick < 4000; ++tick) {
        engine.Tick(world.Input(generation));
        if (engine.RequestSnapshot(request, &snapshot).code != ANOMALY_STATUS_V1_OK) break;
        if (snapshot.state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) return snapshot;
    }
    Check(false, "request did not complete");
    return snapshot;
}

std::uint64_t Scan(NteUiButtons& engine, World& world, std::uint64_t generation = 1) {
    AnomalyGenerationHandleV1 request{};
    Check(engine.RequestScan(&request).code == ANOMALY_STATUS_V1_OK, "scan queued");
    const auto done = Wait(engine, world, request, generation);
    Check(done.status == ANOMALY_STATUS_V1_OK, "scan status " + std::to_string(done.status));
    Check((done.outcome & ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_TRUNCATED) == 0, "scan not truncated");
    return done.catalog_sequence;
}

bool FindButton(NteUiButtons& engine, const std::string& name,
                AnomalyNteUiButtonSnapshotV1& out) {
    AnomalyNteUiButtonQueryV1 query{sizeof(query)};
    query.name = {name.data(), name.size()};
    std::uint32_t matches{};
    out = {sizeof(out)};
    return engine.Find(&query, &out, &matches).code == ANOMALY_STATUS_V1_OK && matches == 1;
}

}  // namespace

int main() {
    const auto profile_path =
        std::filesystem::path(ANOMALY_SOURCE_DIR) / "profiles" / "nte" / "nte-current.json";
    auto loaded = anomaly::LoadBuildProfile(profile_path);
    if (!loaded.Ok()) {
        std::cerr << "cannot load " << profile_path.string() << '\n';
        return EXIT_FAILURE;
    }
    const anomaly::BuildProfile profile = *loaded.profile;
    std::string error;
    Check(anomaly::ValidateNteUiButtonsLayout(profile, error), "shipped layout validates: " + error);
    {
        auto missing = profile;
        missing.layout.erase("htuiButton.pressedFlag");
        Check(!anomaly::ValidateNteUiButtonsLayout(missing, error), "missing key rejected");
        auto narrow = profile;
        narrow.layout["scriptDelegate.stride"] = 12;
        Check(!anomaly::ValidateNteUiButtonsLayout(narrow, error), "narrow delegate stride rejected");
    }

    World world;
    world.cls_class = world.Class("Class", 0);
    world.Put(world.cls_class + kClass, world.cls_class);
    world.cls_function = world.Class("Function", 0, L"/Script/CoreUObject.Function");
    const auto widget = world.Class("Widget", 0);
    const auto panel = world.Class("PanelWidget", widget);
    const auto canvas_cls = world.Class("CanvasPanel", panel);
    const auto switcher_cls = world.Class("WidgetSwitcher", panel, L"/Script/UMG.WidgetSwitcher");
    const auto slot_cls = world.Class("PanelSlot", 0);
    const auto tree_cls = world.Class("WidgetTree", 0, L"/Script/UMG.WidgetTree");
    const auto user_widget = world.Class("UserWidget", widget, L"/Script/UMG.UserWidget");
    const auto activatable = world.Class("CommonActivatableWidget", user_widget,
                                         L"/Script/CommonUI.CommonActivatableWidget");
    const auto container_cls =
        world.Class("CommonActivatableWidgetContainerBase", widget,
                    L"/Script/CommonUI.CommonActivatableWidgetContainerBase");
    const auto htui_base = world.Class("HTUIBase", activatable, L"/Script/HTGame.HTUIBase");
    const auto main_form_cls =
        world.Class("HTUI_MainForm", htui_base, L"/Script/HTGame.HTUI_MainForm");
    const auto button_cls = world.Class("Button", panel, L"/Script/UMG.Button");
    const auto ht_button_cls = world.Class("HTButton", button_cls);
    const auto internal_cls = world.Class("CommonButtonInternalBase", button_cls,
                                          L"/Script/CommonUI.CommonButtonInternalBase");
    const auto common_cls =
        world.Class("CommonButtonBase", user_widget, L"/Script/CommonUI.CommonButtonBase");
    const auto htui_cls = world.Class("HTUI_Button", common_cls, L"/Script/HTGame.HTUI_Button");
    world.cls_htui = htui_cls;
    const auto award_cls = world.Class("WBP_Award_C", htui_base);
    const auto popup_cls = world.Class("BPUI_Popup_C", htui_base);
    const auto layout_cls = world.Class("W_OverallUILayout_C", user_widget);
    const auto settle_cls = world.Class("WBP_Settle_C", activatable);

    world.fn_is_visible = world.Function(widget, "IsVisible", 1, 1, L"/Script/UMG.Widget.IsVisible");
    world.fn_is_in_viewport =
        world.Function(widget, "IsInViewport", 1, 1, L"/Script/UMG.Widget.IsInViewport");
    world.fn_is_hovered = world.Function(widget, "IsHovered", 1, 1, L"/Script/UMG.Widget.IsHovered");
    world.fn_is_interaction_enabled =
        world.Function(common_cls, "IsInteractionEnabled", 1, 1,
                       L"/Script/CommonUI.CommonButtonBase.IsInteractionEnabled");
    world.fn_pressed = world.Function(common_cls, "HandleButtonPressed", 0, 0,
                                      L"/Script/CommonUI.CommonButtonBase.HandleButtonPressed");
    world.fn_released = world.Function(common_cls, "HandleButtonReleased", 0, 0,
                                       L"/Script/CommonUI.CommonButtonBase.HandleButtonReleased");
    world.fn_clicked = world.Function(common_cls, "HandleButtonClicked", 0, 0,
                                      L"/Script/CommonUI.CommonButtonBase.HandleButtonClicked");
    world.fn_is_locked = world.Function(htui_cls, "IsButtonLocked", 1, 1,
                                        L"/Script/HTGame.HTUI_Button.IsButtonLocked");
    const auto fn_exit_pressed = world.Function(settle_cls, "OnExitPressed", 0, 0);
    const auto fn_exit_released = world.Function(settle_cls, "OnExitReleased", 0, 0);
    const auto fn_exit = world.Function(settle_cls, "OnExitClicked", 0, 0);

    const auto set_widget = [&](std::uintptr_t object, bool enabled = true,
                                std::uint8_t visibility = 0, float opacity = 1.0F) {
        world.Put<std::uint8_t>(object + kWidgetFlags, enabled ? 0x04 : 0x00);
        world.Put<std::uint8_t>(object + kVisibility, visibility);
        world.Put<float>(object + kOpacity, opacity);
    };
    std::map<std::uintptr_t, std::uintptr_t> panel_slots;
    const auto attach = [&](std::uintptr_t child, std::uintptr_t parent, std::uintptr_t tree) {
        const auto slot = world.Object(slot_cls, "Slot", tree, {}, 0x80);
        world.Put(slot + kSlotParent, parent);
        world.Put(child + kSlot, slot);
        auto& slots = panel_slots[parent];
        if (slots == 0) {
            slots = world.Alloc(16 * 8);
            world.Put(parent + kPanelSlots, slots);
        }
        const auto n = world.Get<std::int32_t>(parent + kPanelSlots + 8);
        world.Put(slots + static_cast<std::uintptr_t>(n) * 8, slot);
        world.Put<std::int32_t>(parent + kPanelSlots + 8, n + 1);
        return slot;
    };
    const auto make_tree = [&](std::uintptr_t owner) {
        const auto tree = world.Object(tree_cls, "WidgetTree", owner, {}, 0x80);
        const auto root = world.Object(canvas_cls, "RootCanvas", tree);
        set_widget(root);
        world.Put(tree + kTreeRoot, root);
        return std::pair{tree, root};
    };
    const auto set_layer = [&](std::uintptr_t layer, const std::vector<std::uintptr_t>& list,
                               std::uintptr_t displayed) {
        const auto data = world.Alloc(16 * 8);
        for (std::size_t i = 0; i < list.size(); ++i) world.Put(data + i * 8, list[i]);
        world.Put(layer + kWidgetList, data);
        world.Put<std::int32_t>(layer + kWidgetList + 8, static_cast<std::int32_t>(list.size()));
        world.Put(layer + kDisplayed, displayed);
    };
    const auto activate = [&](std::uintptr_t window, bool active) {
        world.Put<std::uint8_t>(window + kActive, active ? 1 : 0);
    };

    // Root layout with three layers drawn bottom to top: main form, menus, popups.
    const auto layout = world.Object(layout_cls, "W_OverallUILayout_C_0", 0);
    set_widget(layout);
    world.in_viewport.insert(layout);
    const auto [layout_tree, layout_root] = make_tree(layout);
    const auto main_layer = world.Object(container_cls, "GameplayMain_Stack", layout_tree);
    const auto menu_layer = world.Object(container_cls, "Menu_Stack", layout_tree);
    const auto popup_layer = world.Object(container_cls, "Popup_Stack", layout_tree);
    for (const auto layer : {main_layer, menu_layer, popup_layer}) {
        set_widget(layer);
        attach(layer, layout_root, layout_tree);
    }
    const auto main_form = world.Object(main_form_cls, "BPUI_MainForm_C_0", layout_tree);
    set_widget(main_form);
    activate(main_form, true);
    const auto [main_tree, main_root] = make_tree(main_form);
    const auto character_info = world.Object(htui_cls, "ButtonCharacterInfo2", main_tree);
    set_widget(character_info);
    attach(character_info, main_root, main_tree);
    set_layer(main_layer, {main_form}, main_form);

    // Full-screen reward window shown by the menu layer; it hides the main form.
    const auto award = world.Object(award_cls, "WBP_Award_C_0", layout_tree);
    set_widget(award);
    activate(award, true);
    world.Put<std::uint8_t>(award + kHideMain, 1);
    const auto [tree, root] = make_tree(award);
    const auto confirm = world.Object(htui_cls, "BtnConfirm", tree);
    set_widget(confirm);
    attach(confirm, root, tree);
    world.texts[confirm + kButtonText] = "Confirm";
    const auto locked = world.Object(common_cls, "BtnLocked", tree);
    set_widget(locked);
    attach(locked, root, tree);
    world.Put<std::uint8_t>(locked + kLocked, 0x02);
    const auto disabled = world.Object(htui_cls, "BtnDisabled", tree);
    set_widget(disabled, false);
    attach(disabled, root, tree);
    const auto no_interact = world.Object(common_cls, "BtnNoInteract", tree);
    set_widget(no_interact);
    attach(no_interact, root, tree);
    world.not_interactable.insert(no_interact);
    const auto hidden_panel = world.Object(canvas_cls, "HiddenPanel", tree);
    set_widget(hidden_panel, true, 1);
    attach(hidden_panel, root, tree);
    const auto in_hidden = world.Object(common_cls, "BtnInHidden", tree);
    set_widget(in_hidden);
    attach(in_hidden, hidden_panel, tree);
    const auto faded = world.Object(common_cls, "BtnFaded", tree);
    set_widget(faded, true, 0, 0.0F);
    attach(faded, root, tree);
    const auto no_hit = world.Object(canvas_cls, "NoHitPanel", tree);
    set_widget(no_hit, true, 3);
    attach(no_hit, root, tree);
    const auto in_no_hit = world.Object(common_cls, "BtnNoHit", tree);
    set_widget(in_no_hit);
    attach(in_no_hit, no_hit, tree);
    const auto switcher = world.Object(switcher_cls, "Switcher", tree);
    set_widget(switcher);
    attach(switcher, root, tree);
    const auto page_a = world.Object(canvas_cls, "PageA", tree);
    const auto page_b = world.Object(canvas_cls, "PageB", tree);
    set_widget(page_a);
    set_widget(page_b);
    attach(page_a, switcher, tree);
    attach(page_b, switcher, tree);
    world.Put<std::int32_t>(switcher + kSwitcherActive, 0);
    const auto on_page_a = world.Object(common_cls, "BtnPageA", tree);
    set_widget(on_page_a);
    attach(on_page_a, page_a, tree);
    const auto on_page_b = world.Object(common_cls, "BtnPageB", tree);
    set_widget(on_page_b);
    attach(on_page_b, page_b, tree);
    const auto inner = world.Object(common_cls, "BtnInner", tree);
    set_widget(inner);
    attach(inner, on_page_a, tree);
    const auto floating = world.Object(htui_cls, "BtnFloating", tree);
    set_widget(floating);
    world.slate_missing.insert(floating);
    const auto internal = world.Object(internal_cls, "InternalRootButton", tree);
    set_widget(internal);
    attach(internal, root, tree);
    world.Object(htui_cls, "Default__HTUI_Button", 0, {}, 0x2000, 0x10);

    const auto popup = world.Object(popup_cls, "BPUI_Popup_C_0", layout_tree);
    set_widget(popup);
    world.Put<std::uint8_t>(popup + kModal, 1);
    const auto [popup_tree, popup_root] = make_tree(popup);
    const auto popup_ok = world.Object(htui_cls, "BtnPopupOk", popup_tree);
    set_widget(popup_ok);
    attach(popup_ok, popup_root, popup_tree);
    set_layer(menu_layer, {award}, award);
    set_layer(popup_layer, {popup}, 0);

    // Standalone window that is neither in a layer nor in the viewport, with a UMG button.
    const auto settle = world.Object(settle_cls, "WBP_Settle_C_0", 0);
    set_widget(settle);
    const auto [settle_tree, settle_root] = make_tree(settle);
    const auto exit_button = world.Object(ht_button_cls, "Button_Exit", settle_tree);
    set_widget(exit_button);
    attach(exit_button, settle_root, settle_tree);
    Bind(world, exit_button, kOnPressed, settle, "OnExitPressed");
    Bind(world, exit_button, kOnReleased, settle, "OnExitReleased");
    Bind(world, exit_button, kOnClicked, settle, "OnExitClicked");

    // Tabs (HTUI_RadioBox wrapping an HTRadioBox), a plain CheckBox and list entries
    // (HTUI_ListItem wrapping an HTUI_Button) in the award window.
    const auto text_block_cls = world.Class("TextBlock", widget);
    const auto check_box_cls = world.Class("CheckBox", widget, L"/Script/UMG.CheckBox");
    const auto ht_check_cls = world.Class("HTCheckBox", check_box_cls);
    const auto ht_radio_cls = world.Class("HTRadioBox", ht_check_cls, L"/Script/HTGame.HTRadioBox");
    const auto radio_cls = world.Class("HTUI_RadioBox", user_widget, L"/Script/HTGame.HTUI_RadioBox");
    const auto tab_cls = world.Class("WBP_Tab_C", radio_cls);
    const auto item_cls = world.Class("HTUI_ListItem", user_widget, L"/Script/HTGame.HTUI_ListItem");
    const auto entry_cls = world.Class("WBP_CharacterEntry_C", item_cls);
    world.fn_check_is_checked =
        world.Function(check_box_cls, "IsChecked", 1, 1, L"/Script/UMG.CheckBox.IsChecked");
    world.fn_check_set_checked =
        world.Function(check_box_cls, "SetIsChecked", 1, 1, L"/Script/UMG.CheckBox.SetIsChecked");
    world.fn_ht_radio_select = world.Function(ht_radio_cls, "SetSelected", 2, 2,
                                              L"/Script/HTGame.HTRadioBox.SetSelected");
    world.fn_radio_select = world.Function(radio_cls, "SetSelected", 2, 2,
                                           L"/Script/HTGame.HTUI_RadioBox.SetSelected");
    world.fn_radio_is_checked = world.Function(radio_cls, "IsChecked", 1, 1,
                                               L"/Script/HTGame.HTUI_RadioBox.IsChecked");
    world.fn_radio_feature =
        world.Function(radio_cls, "IsSystematicGameFeatureActivated", 1, 1,
                       L"/Script/HTGame.HTUI_RadioBox.IsSystematicGameFeatureActivated");
    const auto fn_item_pressed = world.Function(item_cls, "OnBtnPressed", 0, 0,
                                                L"/Script/HTGame.HTUI_ListItem.OnBtnPressed");
    const auto fn_item_released = world.Function(item_cls, "OnBtnReleased", 0, 0,
                                                 L"/Script/HTGame.HTUI_ListItem.OnBtnReleased");
    const auto fn_item_clicked = world.Function(item_cls, "OnBtnClicked", 0, 0,
                                                L"/Script/HTGame.HTUI_ListItem.OnBtnClicked");
    world.fn_item_locked = world.Function(item_cls, "IsItemLocked", 1, 1,
                                          L"/Script/HTGame.HTUI_ListItem.IsItemLocked");
    const auto fn_option_changed = world.Function(award_cls, "OnOptionChanged", 1, 1);
    // HTGame_classes.hpp / UMG_classes.hpp
    constexpr std::uintptr_t kRadioBox = 0x1360, kRadioText = 0x1368, kBlockBtn = 0x1370;
    constexpr std::uintptr_t kItemButton = 0x650, kTextBlockText = 0x188;
    constexpr std::uintptr_t kOnCheckStateChanged = 0xAF8;

    struct Tab {
        std::uintptr_t tab, radio, block;
    };
    const auto make_tab = [&](const std::string& name, const std::string& text) {
        const auto tab = world.Object(tab_cls, name, tree);
        set_widget(tab);
        attach(tab, root, tree);
        const auto [tab_tree, tab_root] = make_tree(tab);
        const auto radio = world.Object(ht_radio_cls, "RadioBox", tab_tree);
        set_widget(radio);
        attach(radio, tab_root, tab_tree);
        const auto label = world.Object(text_block_cls, "RadioText", tab_tree);
        set_widget(label);
        attach(label, tab_root, tab_tree);
        world.texts[label + kTextBlockText] = text;
        const auto block = world.Object(htui_cls, "BlockBtn", tab_tree);
        set_widget(block);
        attach(block, tab_root, tab_tree);
        world.Put(tab + kRadioBox, radio);
        world.Put(tab + kRadioText, label);
        world.Put(tab + kBlockBtn, block);
        return Tab{tab, radio, block};
    };
    const auto tab_characters = make_tab("TabCharacters", "Characters");
    const auto tab_selected = make_tab("TabSelected", "Monsters");
    world.checked.insert(tab_selected.tab);
    const auto tab_locked = make_tab("TabLocked", "Locked");
    world.feature_locked.insert(tab_locked.tab);

    const auto option = world.Object(check_box_cls, "ChkOption", tree);
    set_widget(option);
    attach(option, root, tree);
    Bind(world, option, kOnCheckStateChanged, award, "OnOptionChanged");

    const auto make_entry = [&](const std::string& name, const std::string& text) {
        const auto entry = world.Object(entry_cls, name, tree);
        set_widget(entry);
        attach(entry, root, tree);
        const auto [entry_tree, entry_root] = make_tree(entry);
        const auto button = world.Object(htui_cls, "Btn_Click", entry_tree);
        set_widget(button);
        attach(button, entry_root, entry_tree);
        world.texts[button + kButtonText] = text;
        world.Put(entry + kItemButton, button);
        return entry;
    };
    const auto character_entry = make_entry("CharacterEntry", "Entry A");
    const auto entry_locked = make_entry("EntryLocked", "Entry B");
    world.item_locked.insert(entry_locked);

    anomaly::NteUiButtonsBindings bindings;
    bindings.resolve_name = [&world](std::uint32_t id) {
        return id == 0 || id > world.names.size() ? std::string{} : world.names[id - 1U];
    };
    bindings.read_ftext = [&world](std::uintptr_t address) {
        const auto found = world.texts.find(address);
        return found == world.texts.end() ? std::string{} : found->second;
    };
    bindings.find_object = [&world](const wchar_t* path) {
        const auto found = world.paths.find(path);
        return found == world.paths.end() ? std::uintptr_t{0} : found->second;
    };
    bindings.invoke = [&world](std::uintptr_t object, std::uintptr_t function, void* parameters,
                               std::size_t) { return world.Invoke(object, function, parameters); };
    anomaly::NteUiButtonsBudget budget;
    budget.max_calls_per_tick = 1;  // forces every pass across several ticks
    NteUiButtons engine(profile, anomaly::CreateInProcessSymbolMemory(), bindings, budget);

    // Readiness is reported before any request.
    engine.Tick(world.Input());
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    Check(engine.Status(&status).code == ANOMALY_STATUS_V1_OK, "status");
    Check((status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_READY) != 0, "ready");
    Check((status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_PICK_AVAILABLE) != 0, "pick available");
    Check((status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG) == 0, "no catalog yet");

    // Request lifecycle: queued until a tick runs it.
    AnomalyGenerationHandleV1 first{};
    Check(engine.RequestScan(&first).code == ANOMALY_STATUS_V1_OK, "scan queued");
    AnomalyNteUiButtonRequestSnapshotV1 request{sizeof(request)};
    Check(engine.RequestSnapshot(first, &request).code == ANOMALY_STATUS_V1_OK &&
              request.state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_QUEUED &&
              request.kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_SCAN,
          "new request is queued");
    const auto done = Wait(engine, world, first);
    Check(done.status == ANOMALY_STATUS_V1_OK && done.catalog_sequence != 0, "first scan ok");
    Check(engine.Cancel(first).code == ANOMALY_STATUS_V1_CONFLICT, "cannot cancel a complete scan");
    const auto sequence = done.catalog_sequence;
    Check(engine.Status(&status).code == ANOMALY_STATUS_V1_OK && status.catalog_sequence == sequence &&
              status.window_count == 3,
          "status reports the catalog and its three layers");

    const auto expect = [&](const std::string& name, std::uint32_t category, std::uint32_t reasons) {
        AnomalyNteUiButtonSnapshotV1 button{};
        if (!FindButton(engine, name, button)) {
            Check(false, name + " listed once");
            return button;
        }
        Check(button.category == category, name + " category " + std::to_string(button.category));
        Check(button.reasons == reasons, name + " reasons " + std::to_string(button.reasons) +
                                             " cause=" + button.cause);
        return button;
    };
    using R = std::uint32_t;
    constexpr R clickable = ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE;
    constexpr R blocked = ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_BLOCKED;
    constexpr R hidden = ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN;
    const auto confirm_button = expect("BtnConfirm", clickable, 0);
    expect("BtnPageA", clickable, 0);
    expect("BtnInner", clickable, 0);
    expect("BtnLocked", blocked, ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED);
    expect("BtnDisabled", blocked, ANOMALY_NTE_UI_BUTTON_REASON_V1_DISABLED);
    expect("BtnNoInteract", blocked, ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_INTERACTABLE);
    expect("BtnNoHit", blocked, ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_HIT_TESTABLE);
    expect("BtnInHidden", hidden, ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_ANCESTOR);
    expect("BtnFaded", hidden, ANOMALY_NTE_UI_BUTTON_REASON_V1_TRANSPARENT);
    expect("BtnPageB", hidden, ANOMALY_NTE_UI_BUTTON_REASON_V1_INACTIVE_PAGE);
    expect("BtnFloating", hidden, ANOMALY_NTE_UI_BUTTON_REASON_V1_DETACHED);
    expect("Button_Exit", hidden, ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_IN_VIEWPORT);
    expect("BtnPopupOk", hidden, ANOMALY_NTE_UI_BUTTON_REASON_V1_INACTIVE_PAGE);
    // The regression from the live freeze: a full-screen window hides the main form.
    const auto character_button =
        expect("ButtonCharacterInfo2", blocked, ANOMALY_NTE_UI_BUTTON_REASON_V1_OCCLUDED);
    Check(std::string(character_button.cause).find("WBP_Award_C_0") != std::string::npos,
          "occlusion names the covering window: " + std::string(character_button.cause));
    Check(std::string(confirm_button.text) == "Confirm", "HTUI text");
    Check(std::string(confirm_button.window) == "WBP_Award_C_0", "window name");
    Check(std::string(confirm_button.root) == "W_OverallUILayout_C_0", "root name");
    Check(confirm_button.kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_HTUI, "HTUI kind");
    AnomalyNteUiButtonSnapshotV1 scratch{};
    Check(!FindButton(engine, "InternalRootButton", scratch), "CommonButtonInternalBase excluded");
    Check(!FindButton(engine, "Default__HTUI_Button", scratch), "class default object excluded");
    // Tabs, check boxes and list entries are listed once, as themselves; the buttons inside
    // them are not listed separately. A selected tab stays clickable.
    const auto tab_button = expect("TabCharacters", clickable, 0);
    Check(tab_button.kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_RADIO &&
              std::string(tab_button.text) == "Characters",
          "tab kind and text: " + std::string(tab_button.text));
    expect("TabSelected", clickable, 0);
    expect("TabLocked", blocked, ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED);
    const auto option_button = expect("ChkOption", clickable, 0);
    Check(option_button.kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_RADIO, "check box kind");
    const auto entry_button = expect("CharacterEntry", clickable, 0);
    Check(entry_button.kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_LIST_ENTRY &&
              std::string(entry_button.text) == "Entry A",
          "list entry kind and text: " + std::string(entry_button.text));
    expect("EntryLocked", blocked, ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED);
    Check(!FindButton(engine, "RadioBox", scratch) && !FindButton(engine, "BlockBtn", scratch) &&
              !FindButton(engine, "Btn_Click", scratch),
          "buttons inside tabs and list entries excluded");
    Check(status.button_count == 20, "button count " + std::to_string(status.button_count));
    Check(!world.Touched(in_hidden) && !world.Touched(on_page_b) && !world.Touched(character_info),
          "hidden and occluded buttons cost no game call");

    {
        AnomalyNteUiButtonSnapshotV1 first_button{sizeof(first_button)};
        Check(engine.ButtonAt(sequence, 0, &first_button).code == ANOMALY_STATUS_V1_OK &&
                  first_button.category == clickable,
              "catalog lists clickable buttons first");
        AnomalyNteUiButtonQueryV1 query{sizeof(query)};
        query.category_mask = ANOMALY_NTE_UI_BUTTON_QUERY_V1_CATEGORY(clickable);
        const std::string window = "WBP_Award_C_0";
        query.window = {window.data(), window.size()};
        std::uint32_t matches{};
        Check(engine.Find(&query, &first_button, &matches).code == ANOMALY_STATUS_V1_OK &&
                  matches == 7,
              "seven clickable buttons in the award window: " + std::to_string(matches));
        AnomalyNteUiWindowSnapshotV1 window_snapshot{sizeof(window_snapshot)};
        bool award_blocks = false;
        for (std::uint32_t i = 0; i < status.window_count; ++i) {
            if (engine.WindowAt(sequence, i, &window_snapshot).code == ANOMALY_STATUS_V1_OK &&
                std::string(window_snapshot.window) == "WBP_Award_C_0") {
                award_blocks = (window_snapshot.flags & ANOMALY_NTE_UI_WINDOW_V1_BLOCKING) != 0;
            }
        }
        Check(award_blocks, "award window reported as blocking");
    }

    const auto click = [&](const AnomalyNteUiButtonSnapshotV1& button, std::uint32_t flags = 0) {
        AnomalyNteUiButtonClickRequestV1 click_request{sizeof(click_request)};
        click_request.button = button.button;
        click_request.flags = flags;
        AnomalyGenerationHandleV1 handle{};
        Check(engine.RequestClick(&click_request, &handle).code == ANOMALY_STATUS_V1_OK,
              "click queued");
        return Wait(engine, world, handle);
    };

    // HTUI click: press, release, click in order, accepted by the button's own gate.
    world.calls.clear();
    auto result = click(confirm_button);
    Check(result.status == ANOMALY_STATUS_V1_OK && result.invocations == 3,
          "HTUI click ok: " + std::string(result.detail));
    Check((result.outcome & ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_PRESS_ARMED) != 0 &&
              (result.outcome & ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_CLICK_ACCEPTED) != 0 &&
              world.accepted.contains(confirm),
          "HTUI click armed and accepted");
    {
        std::vector<std::uintptr_t> order;
        for (const auto& call : world.calls) {
            if (call.object == confirm && (call.function == world.fn_pressed ||
                                           call.function == world.fn_released ||
                                           call.function == world.fn_clicked)) {
                order.push_back(call.function);
            }
        }
        Check(order == std::vector<std::uintptr_t>{world.fn_pressed, world.fn_released,
                                                   world.fn_clicked},
              "press, release, click order");
    }
    // An HTUI click the game discards is a failure, not a silent success.
    world.no_arm.insert(confirm);
    result = click(confirm_button);
    Check(result.status == ANOMALY_STATUS_V1_FAILED &&
              (result.outcome & ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_PRESS_ARMED) == 0,
          "discarded HTUI click fails: " + std::string(result.detail));
    world.no_arm.erase(confirm);

    // Occluded: refused without any game call.
    world.calls.clear();
    result = click(character_button);
    Check(result.status == ANOMALY_STATUS_V1_CONFLICT &&
              result.reasons == ANOMALY_NTE_UI_BUTTON_REASON_V1_OCCLUDED,
          "occluded click refused: " + std::string(result.detail));
    Check(!world.Called(character_info, world.fn_pressed), "refused click invokes nothing");

    // Live re-evaluation: a modal opened after the scan blocks a click on the old catalog.
    set_layer(popup_layer, {popup}, popup);
    activate(popup, true);
    world.calls.clear();
    result = click(confirm_button);
    Check(result.status == ANOMALY_STATUS_V1_CONFLICT &&
              std::string(result.detail).find("BPUI_Popup_C_0") != std::string::npos,
          "modal opened after the scan blocks the click: " + std::string(result.detail));
    Check(!world.Called(confirm, world.fn_pressed), "blocked click invokes nothing");
    Scan(engine, world);
    expect("BtnPopupOk", clickable, 0);
    expect("BtnConfirm", blocked, ANOMALY_NTE_UI_BUTTON_REASON_V1_OCCLUDED);
    // A closing window's buttons are blocked and it no longer covers anything.
    world.Put<std::uint8_t>(popup + kClosing, 1);
    Scan(engine, world);
    expect("BtnPopupOk", blocked, ANOMALY_NTE_UI_BUTTON_REASON_V1_CLOSING);
    expect("BtnConfirm", clickable, 0);
    world.Put<std::uint8_t>(popup + kClosing, 0);
    activate(popup, false);
    set_layer(popup_layer, {popup}, 0);

    // Closing the award window uncovers the main form.
    activate(award, false);
    set_layer(menu_layer, {}, 0);
    world.slate_missing.insert(award);
    Scan(engine, world);
    expect("ButtonCharacterInfo2", clickable, 0);
    expect("BtnConfirm", hidden, ANOMALY_NTE_UI_BUTTON_REASON_V1_DETACHED);
    // BeHideChildrenReason is only a tag (the open world leaves it set on the main
    // form); HideChildren collapses what it hides, so visible buttons stay clickable.
    world.Put<std::uint32_t>(main_form + kHideChildren, 2);
    Scan(engine, world);
    expect("ButtonCharacterInfo2", clickable, 0);
    world.Put<std::uint32_t>(main_form + kHideChildren, 0);
    world.slate_missing.erase(award);
    activate(award, true);
    set_layer(menu_layer, {award}, award);
    Scan(engine, world);

    // Forced click on a locked button, then a UMG button's delegate chain.
    AnomalyNteUiButtonSnapshotV1 locked_button{};
    Check(FindButton(engine, "BtnLocked", locked_button), "locked button listed");
    result = click(locked_button);
    Check(result.status == ANOMALY_STATUS_V1_CONFLICT, "locked button refused");
    world.calls.clear();
    result = click(locked_button, ANOMALY_NTE_UI_BUTTON_CLICK_V1_FORCE);
    Check(result.status == ANOMALY_STATUS_V1_OK &&
              (result.outcome & ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_FORCED) != 0 &&
              world.Called(locked, world.fn_pressed),
          "forced click runs");
    AnomalyNteUiButtonSnapshotV1 exit{};
    Check(FindButton(engine, "Button_Exit", exit), "exit listed");
    world.calls.clear();
    result = click(exit, ANOMALY_NTE_UI_BUTTON_CLICK_V1_FORCE);
    Check(result.status == ANOMALY_STATUS_V1_OK && result.invocations == 3 &&
              world.calls.size() >= 3 &&
              world.calls[world.calls.size() - 3].function == fn_exit_pressed &&
              world.calls[world.calls.size() - 2].function == fn_exit_released &&
              world.calls.back().function == fn_exit,
          "UMG delegates run pressed, released, clicked");

    // Tab: SetSelected(true, true) on the HTUI_RadioBox, never on the inner radio box.
    world.calls.clear();
    result = click(tab_button);
    Check(result.status == ANOMALY_STATUS_V1_OK && result.invocations == 1 &&
              (result.outcome & ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_CLICK_ACCEPTED) != 0 &&
              world.Called(tab_characters.tab, world.fn_radio_select) &&
              world.checked.contains(tab_characters.tab),
          "tab click selects: " + std::string(result.detail));
    Check(!world.Touched(tab_characters.radio) && !world.Touched(tab_characters.block),
          "tab click leaves the inner widgets alone");
    // Clicking the already selected tab is still a successful click.
    AnomalyNteUiButtonSnapshotV1 selected_button{};
    Check(FindButton(engine, "TabSelected", selected_button), "selected tab listed");
    result = click(selected_button);
    Check(result.status == ANOMALY_STATUS_V1_OK &&
              world.Called(tab_selected.tab, world.fn_radio_select),
          "selected tab click ok: " + std::string(result.detail));
    AnomalyNteUiButtonSnapshotV1 locked_tab{};
    Check(FindButton(engine, "TabLocked", locked_tab), "locked tab listed");
    world.calls.clear();
    result = click(locked_tab);
    Check(result.status == ANOMALY_STATUS_V1_CONFLICT &&
              !world.Called(tab_locked.tab, world.fn_radio_select),
          "locked tab refused without a click: " + std::string(result.detail));

    // Plain CheckBox: SetIsChecked(true), then its OnCheckStateChanged bindings with true.
    world.calls.clear();
    result = click(option_button);
    Check(result.status == ANOMALY_STATUS_V1_OK && result.invocations == 2 &&
              world.checked.contains(option) &&
              world.Called(option, world.fn_check_set_checked) &&
              world.Called(award, fn_option_changed),
          "check box click checks and notifies: " + std::string(result.detail));

    // List entry: the entry's own press, release, click handlers, in order.
    world.calls.clear();
    result = click(entry_button);
    Check(result.status == ANOMALY_STATUS_V1_OK && result.invocations == 3,
          "list entry click ok: " + std::string(result.detail));
    {
        std::vector<std::uintptr_t> order;
        for (const auto& call : world.calls) {
            if (call.object == character_entry) order.push_back(call.function);
        }
        const auto clicks = std::find(order.begin(), order.end(), fn_item_pressed);
        Check(clicks != order.end() &&
                  std::vector<std::uintptr_t>(clicks, order.end()) ==
                      std::vector<std::uintptr_t>{fn_item_pressed, fn_item_released,
                                                  fn_item_clicked},
              "list entry press, release, click order");
    }
    AnomalyNteUiButtonSnapshotV1 locked_entry{};
    Check(FindButton(engine, "EntryLocked", locked_entry), "locked entry listed");
    world.calls.clear();
    result = click(locked_entry);
    Check(result.status == ANOMALY_STATUS_V1_CONFLICT &&
              !world.Called(entry_locked, fn_item_clicked),
          "locked entry refused");

    // Hover pick: the whole path under the cursor is hovered; innermost first.
    world.hovered = {on_page_a, inner, page_a, switcher, root, award};
    world.calls.clear();
    AnomalyGenerationHandleV1 pick{};
    Check(engine.RequestPick(&pick).code == ANOMALY_STATUS_V1_OK, "pick queued");
    const auto picked = Wait(engine, world, pick);
    Check(picked.status == ANOMALY_STATUS_V1_OK && picked.hit_count == 2,
          "two hovered buttons: " + std::to_string(picked.hit_count));
    AnomalyNteUiButtonSnapshotV1 hit{sizeof(hit)};
    Check(engine.PickHitAt(pick, 0, &hit).code == ANOMALY_STATUS_V1_OK &&
              std::string(hit.name) == "BtnInner",
          "innermost hit first: " + std::string(hit.name));
    Check(engine.PickHitAt(pick, 1, &hit).code == ANOMALY_STATUS_V1_OK &&
              std::string(hit.name) == "BtnPageA",
          "outer hit second");
    Check(!world.Called(in_hidden, world.fn_is_hovered), "hidden buttons are not asked");
    world.hovered = {character_info};
    Check(engine.RequestPick(&pick).code == ANOMALY_STATUS_V1_OK, "second pick queued");
    Check(Wait(engine, world, pick).hit_count == 1 &&
              engine.PickHitAt(pick, 0, &hit).code == ANOMALY_STATUS_V1_OK &&
              hit.category == blocked,
          "a hovered button that is occluded is still reported, as blocked");
    world.hovered.clear();

    // Catalog sequences: an old sequence is refused once a newer catalog replaces it.
    Check(engine.ButtonAt(sequence, 0, &scratch).code == ANOMALY_STATUS_V1_NOT_FOUND,
          "stale catalog sequence refused");

    // Cancel a queued request; the queue bound.
    AnomalyGenerationHandleV1 cancelled{};
    Check(engine.RequestScan(&cancelled).code == ANOMALY_STATUS_V1_OK, "scan to cancel");
    Check(engine.Cancel(cancelled).code == ANOMALY_STATUS_V1_OK, "cancel queued scan");
    Check(engine.RequestSnapshot(cancelled, &request).code == ANOMALY_STATUS_V1_OK &&
              request.state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE &&
              request.status == ANOMALY_STATUS_V1_CANCELLED,
          "cancelled request completes with CANCELLED");
    std::vector<AnomalyGenerationHandleV1> flood(32);
    for (auto& handle : flood) Check(engine.RequestScan(&handle).code == ANOMALY_STATUS_V1_OK, "fill");
    AnomalyGenerationHandleV1 overflow{};
    Check(engine.RequestScan(&overflow).code == ANOMALY_STATUS_V1_CONFLICT, "open request bound");
    for (const auto& handle : flood) static_cast<void>(engine.Cancel(handle));

    // A new object-registry generation invalidates button handles from the previous one.
    AnomalyNteUiButtonSnapshotV1 fresh{};
    Check(FindButton(engine, "BtnPageA", fresh), "button before generation change");
    engine.Tick(world.Input(2));
    Check(engine.Status(&status).code == ANOMALY_STATUS_V1_OK &&
              (status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG) == 0,
          "catalog dropped with the registry generation");
    {
        AnomalyNteUiButtonClickRequestV1 click_request{sizeof(click_request)};
        click_request.button = fresh.button;
        AnomalyGenerationHandleV1 handle{};
        Check(engine.RequestClick(&click_request, &handle).code == ANOMALY_STATUS_V1_OK, "queued");
        Check(Wait(engine, world, handle, 2).status == ANOMALY_STATUS_V1_NOT_FOUND,
              "handle from the previous generation is stale");
    }
    // A reused object slot (new serial) is also stale.
    Scan(engine, world, 2);
    Check(FindButton(engine, "BtnPageA", fresh), "button in generation 2");
    world.Put<std::uint32_t>(
        world.chunk + world.Get<std::int32_t>(on_page_a + kIndex) * kItemStride + kItemSerial, 7u);
    world.calls.clear();
    {
        AnomalyNteUiButtonClickRequestV1 click_request{sizeof(click_request)};
        click_request.button = fresh.button;
        AnomalyGenerationHandleV1 handle{};
        Check(engine.RequestClick(&click_request, &handle).code == ANOMALY_STATUS_V1_OK, "queued");
        Check(Wait(engine, world, handle, 2).status == ANOMALY_STATUS_V1_NOT_FOUND &&
                  world.calls.empty(),
              "reused slot refused without a game call");
    }

    // Host reset: every earlier request handle becomes unknown.
    AnomalyGenerationHandleV1 before_reset{};
    Check(engine.RequestScan(&before_reset).code == ANOMALY_STATUS_V1_OK, "queued before reset");
    engine.Invalidate(ANOMALY_STATUS_V1_UNAVAILABLE);
    Check(engine.RequestSnapshot(before_reset, &request).code == ANOMALY_STATUS_V1_NOT_FOUND,
          "requests from before the reset are gone");

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "nte ui buttons fixture: all checks passed\n";
    return EXIT_SUCCESS;
}
