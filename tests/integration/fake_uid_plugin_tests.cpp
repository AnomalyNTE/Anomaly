#include "anomaly/sdk/anomaly_sdk.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <string_view>

namespace {

constexpr std::size_t kObjectCount = 65545;
constexpr std::size_t kWidgetCount = 9;
constexpr std::size_t kInitialWidgetCount = 8;
constexpr std::size_t kInitialWidgetSlotStart = kObjectCount - kInitialWidgetCount;
constexpr std::uint32_t kTargetNameId = 37;
constexpr std::uint32_t kOtherNameId = 91;
constexpr std::uint64_t kObjectGeneration = 5;
constexpr std::size_t kObjectNameOffset = 24;
constexpr std::size_t kTextFieldOffset = 392;
constexpr std::size_t kSetTextVtableOffset = 856;
constexpr std::size_t kObjectRegistryItemsOffset = 16;
constexpr std::size_t kObjectChunkSize = 65536;
constexpr std::size_t kObjectItemSerialOffset = 16;
constexpr std::int32_t kGObjectsAddend = -16;
constexpr std::wstring_view kOriginalUid = L"UID: 216065736008";
constexpr std::wstring_view kDefaultUid = L"UID: 000000000000";
constexpr std::wstring_view kChangedUid = L"UID: 987654321";

struct UnrealString final {
    wchar_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

struct UnrealText final {
    void* data{};
    std::uint32_t flags{};
    std::uint32_t padding{};
};

static_assert(sizeof(UnrealString) == 16);
static_assert(sizeof(UnrealText) == 16);

std::array<std::uint8_t, 64> g_from_string_match{};
std::array<std::uint8_t, 32> g_gobjects_accessor{};
std::array<std::uint8_t, 64> g_object_registry{};
std::array<std::uintptr_t, 2> g_chunks{};
std::array<std::array<std::uint8_t, 24>, kObjectCount> g_object_items{};
std::array<std::array<std::uint8_t, 512>, kWidgetCount> g_objects{};
std::array<std::uint8_t, 864> g_vtable{};
std::array<std::array<wchar_t, 512>, kWidgetCount> g_current_texts{};
std::array<wchar_t, 512> g_owned_text{};
std::array<unsigned, kWidgetCount> g_set_text_calls{};
std::array<std::uint32_t, kObjectCount> g_object_serials{};
std::array<std::uint32_t, kObjectCount> g_object_name_ids{};
unsigned g_signature_calls{};
unsigned g_config_writes{};
unsigned g_scheduled_tasks{};
unsigned g_set_window_open_calls{};
unsigned g_window_begin_calls{};
std::uint32_t g_object_count{};
bool g_apply_clicked{};
bool g_window_open{};

AnomalyConfigServiceV1 g_config{};
AnomalySchedulerServiceV1 g_scheduler{};
AnomalySignatureServiceV1 g_signature{};
AnomalyWindowServiceV1 g_window{};
AnomalyUe5ObjectsServiceV1 g_objects_service{};
AnomalyUe5NamesServiceV1 g_names{};

AnomalyStatusV1 Status(const std::uint32_t code) {
    return {code, 0, {nullptr, 0}};
}

template <typename Value, std::size_t Size>
void Store(
    std::array<std::uint8_t, Size>& destination,
    const std::size_t offset,
    const Value& value) {
    std::memcpy(destination.data() + offset, &value, sizeof(value));
}

bool PutRelative32(
    std::uint8_t* instruction, const std::size_t instruction_size,
    const std::uintptr_t target) {
    const auto displacement = static_cast<std::intptr_t>(target) -
        reinterpret_cast<std::intptr_t>(instruction + instruction_size);
    if (displacement < INT32_MIN || displacement > INT32_MAX) return false;
    const auto relative = static_cast<std::int32_t>(displacement);
    std::memcpy(instruction + instruction_size - sizeof(relative), &relative, sizeof(relative));
    return true;
}

std::size_t ObjectSlotForWidget(const std::size_t widget_index) {
    return widget_index == 0
        ? 0
        : kInitialWidgetSlotStart + widget_index - 1;
}

std::size_t WidgetIndex(const void* const widget) {
    for (std::size_t index = 0; index < g_objects.size(); ++index) {
        if (widget == g_objects[index].data()) return index;
    }
    return g_objects.size();
}

UnrealString* ANOMALY_CALL AssignString(
    UnrealString* const output, const wchar_t* const source) {
    if (output == nullptr || source == nullptr) return nullptr;
    const std::size_t length = std::wcslen(source);
    if (length + 1 > g_owned_text.size()) return nullptr;
    std::copy_n(source, length + 1, g_owned_text.data());
    *output = {g_owned_text.data(), static_cast<std::int32_t>(length + 1),
        static_cast<std::int32_t>(length + 1)};
    return output;
}

UnrealText* ANOMALY_CALL FromString(
    UnrealText* const output, UnrealString* const input) {
    if (output == nullptr || input == nullptr || input->data == nullptr) return nullptr;
    output->data = input->data;
    output->flags = 0x12;
    return output;
}

void ANOMALY_CALL FreeString(void*) {}

UnrealString* ANOMALY_CALL TextToString(
    UnrealString* const output, const UnrealText* const text) {
    if (output == nullptr || text == nullptr) return nullptr;
    for (std::size_t index = 0; index < g_objects.size(); ++index) {
        const auto* const expected = reinterpret_cast<const UnrealText*>(
            g_objects[index].data() + kTextFieldOffset);
        if (text != expected) continue;
        const std::size_t length = std::wcslen(g_current_texts[index].data());
        *output = {g_current_texts[index].data(), static_cast<std::int32_t>(length + 1),
            static_cast<std::int32_t>(length + 1)};
        return output;
    }
    return nullptr;
}

void ANOMALY_CALL SetText(void* const widget, const UnrealText* const text) {
    const std::size_t index = WidgetIndex(widget);
    if (index == g_objects.size() || text == nullptr || text->data == nullptr) return;
    const auto* const value = static_cast<const wchar_t*>(text->data);
    const std::size_t length = std::wcslen(value);
    if (length + 1 > g_current_texts[index].size()) return;
    std::copy_n(value, length + 1, g_current_texts[index].data());
    ++g_set_text_calls[index];
}

AnomalyStatusV1 ANOMALY_CALL RegisterSchema(
    void*, AnomalyStringViewV1, std::uint32_t, AnomalyByteSpanV1,
    AnomalyGenerationHandleV1* const handle) {
    if (handle == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *handle = {1, 1};
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL ReadConfig(
    void*, AnomalyStringViewV1, std::uint32_t* const version,
    AnomalyMutableByteSpanV1, std::size_t* const size) {
    if (version == nullptr || size == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *version = 0;
    *size = 0;
    return Status(ANOMALY_STATUS_V1_NOT_FOUND);
}

AnomalyStatusV1 ANOMALY_CALL WriteConfig(
    void*, AnomalyStringViewV1, std::uint32_t, AnomalyByteSpanV1) {
    ++g_config_writes;
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL Schedule(
    void*, std::uint32_t, AnomalyTaskCallbackV1, void*,
    AnomalyGenerationHandleV1* const handle) {
    if (handle == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *handle = {++g_scheduled_tasks, 1};
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL Cancel(void*, AnomalyGenerationHandleV1) {
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL ResolveSignature(
    void*, AnomalyStringViewV1, AnomalyStringViewV1, AnomalyStringViewV1,
    std::uintptr_t* const address) {
    if (address == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    switch (g_signature_calls++) {
    case 0: *address = reinterpret_cast<std::uintptr_t>(&SetText); break;
    case 1: *address = reinterpret_cast<std::uintptr_t>(&AssignString); break;
    case 2: *address = reinterpret_cast<std::uintptr_t>(g_from_string_match.data()); break;
    case 3: *address = reinterpret_cast<std::uintptr_t>(&FreeString); break;
    case 4: *address = reinterpret_cast<std::uintptr_t>(&TextToString); break;
    case 5: *address = reinterpret_cast<std::uintptr_t>(g_gobjects_accessor.data()); break;
    default: return Status(ANOMALY_STATUS_V1_CONFLICT);
    }
    return Status(ANOMALY_STATUS_V1_OK);
}

std::uint64_t ANOMALY_CALL ObjectGeneration(void*) { return kObjectGeneration; }

std::uint32_t ANOMALY_CALL ObjectCount(void*) {
    return g_object_count;
}

AnomalyGenerationHandleV1 ObjectHandle(const std::uint32_t index) {
    const std::uint64_t serial = g_object_serials[index];
    return {(serial << 32U) | (static_cast<std::uint64_t>(index) + 1U),
        kObjectGeneration};
}

AnomalyStatusV1 CopyObjectSnapshot(
    const std::uint32_t index, AnomalyUe5ObjectSnapshotV1* const snapshot) {
    if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot) ||
        index >= kObjectCount) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *snapshot = {sizeof(*snapshot), 0, ObjectHandle(index), g_object_name_ids[index], 0};
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL SnapshotAt(
    void*, const std::uint32_t index, AnomalyUe5ObjectSnapshotV1* const snapshot) {
    return index < g_object_count ? CopyObjectSnapshot(index, snapshot) :
                                    Status(ANOMALY_STATUS_V1_NOT_FOUND);
}

AnomalyStatusV1 ANOMALY_CALL SnapshotByHandle(
    void*, const AnomalyGenerationHandleV1 handle,
    AnomalyUe5ObjectSnapshotV1* const snapshot) {
    const std::uint32_t encoded_index = static_cast<std::uint32_t>(handle.id);
    if (encoded_index == 0) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
    const std::uint32_t index = encoded_index - 1U;
    const auto expected = index < g_object_count ?
        ObjectHandle(index) : AnomalyGenerationHandleV1{};
    return handle.id == expected.id && handle.generation == expected.generation
        ? CopyObjectSnapshot(index, snapshot)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND);
}

AnomalyStatusV1 ANOMALY_CALL ResolveName(
    void*, const std::uint32_t name_id, char* const destination,
    std::size_t* const size) {
    constexpr std::string_view name = "TextBlock_RoleID";
    if (name_id != kTargetNameId || size == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    const std::size_t required = name.size() + 1;
    if (destination == nullptr) {
        *size = required;
        return Status(ANOMALY_STATUS_V1_OK);
    }
    if (*size < required) return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    std::memcpy(destination, name.data(), name.size());
    destination[name.size()] = '\0';
    *size = required;
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL RegisterWindow(
    void*, const AnomalyWindowSpecV1*, AnomalyGenerationHandleV1* const handle) {
    if (handle == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *handle = {1, 1};
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL WindowHandle(
    void*, const AnomalyGenerationHandleV1 handle) {
    return handle.id == 1 ? Status(ANOMALY_STATUS_V1_OK) :
                            Status(ANOMALY_STATUS_V1_NOT_FOUND);
}

AnomalyStatusV1 ANOMALY_CALL SetWindowOpen(
    void* user, const AnomalyGenerationHandleV1 handle, const std::int32_t open) {
    ++g_set_window_open_calls;
    g_window_open = open != 0;
    return WindowHandle(user, handle);
}

AnomalyStatusV1 ANOMALY_CALL WindowState(
    void*, const AnomalyGenerationHandleV1 handle, AnomalyWindowStateV1* const state) {
    if (handle.id != 1 || state == nullptr || state->struct_size < sizeof(*state)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *state = {sizeof(*state), 0, 240.0F, 170.0F, 1, g_window_open ? 1 : 0, 0};
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL BeginWindow(
    void*, const AnomalyGenerationHandleV1 handle, std::uint32_t,
    std::int32_t* const visible) {
    if (handle.id != 1 || visible == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    ++g_window_begin_calls;
    *visible = 1;
    return Status(ANOMALY_STATUS_V1_OK);
}

void ANOMALY_CALL UiText(void*, AnomalyStringViewV1) {}

int ANOMALY_CALL UiInputText(
    void*, AnomalyStringViewV1, char* const buffer,
    const std::size_t capacity, std::uint32_t) {
    constexpr std::string_view replacement = "987654321";
    if (buffer == nullptr || capacity <= replacement.size()) return 0;
    std::copy(replacement.begin(), replacement.end(), buffer);
    buffer[replacement.size()] = '\0';
    return 1;
}

int ANOMALY_CALL UiButton(
    void*, const AnomalyStringViewV1 label, float, float) {
    const std::string_view value(label.data, label.size);
    if (!g_apply_clicked && value.find("Apply") != std::string_view::npos) {
        g_apply_clicked = true;
        return 1;
    }
    return 0;
}

AnomalyStatusV1 ANOMALY_CALL Query(
    void*, const AnomalyStringViewV1 id, const std::uint32_t minimum,
    const void** const service) {
    if (id.data == nullptr || service == nullptr || minimum > 1) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *service = nullptr;
    const std::string_view value(id.data, id.size);
    if (value == ANOMALY_CONFIG_SERVICE_V1_ID) *service = &g_config;
    else if (value == ANOMALY_SCHEDULER_SERVICE_V1_ID) *service = &g_scheduler;
    else if (value == ANOMALY_SIGNATURE_SERVICE_V1_ID) *service = &g_signature;
    else if (value == ANOMALY_WINDOW_SERVICE_V1_ID) *service = &g_window;
    else if (value == ANOMALY_UE5_OBJECTS_SERVICE_V1_ID) *service = &g_objects_service;
    else if (value == ANOMALY_UE5_NAMES_SERVICE_V1_ID) *service = &g_names;
    return *service == nullptr ? Status(ANOMALY_STATUS_V1_UNAVAILABLE) :
                                 Status(ANOMALY_STATUS_V1_OK);
}

bool ResetFixture() {
    g_from_string_match.fill(0);
    g_gobjects_accessor.fill(0);
    g_object_registry.fill(0);
    std::memset(g_object_items.data(), 0, sizeof(g_object_items));
    g_objects = {};
    g_vtable.fill(0);
    g_current_texts = {};
    g_owned_text.fill(L'\0');
    g_set_text_calls.fill(0);
    g_object_serials.fill(0);
    g_object_name_ids.fill(kOtherNameId);
    g_signature_calls = 0;
    g_config_writes = 0;
    g_scheduled_tasks = 0;
    g_set_window_open_calls = 0;
    g_window_begin_calls = 0;
    g_object_count = static_cast<std::uint32_t>(kObjectCount);
    std::fill(
        g_object_name_ids.begin() + kInitialWidgetSlotStart,
        g_object_name_ids.end(), kTargetNameId);
    g_apply_clicked = false;
    g_window_open = false;

    if (!PutRelative32(
            g_from_string_match.data() + 31, 5,
            reinterpret_cast<std::uintptr_t>(&FromString))) {
        return false;
    }
    const auto registry = reinterpret_cast<std::uintptr_t>(g_object_registry.data());
    const auto accessor = reinterpret_cast<std::uintptr_t>(g_gobjects_accessor.data());
    const auto registry_displacement = static_cast<std::intptr_t>(registry) -
        static_cast<std::intptr_t>(accessor + 7U) - kGObjectsAddend;
    if (registry_displacement < INT32_MIN || registry_displacement > INT32_MAX) return false;
    const auto relative = static_cast<std::int32_t>(registry_displacement);
    std::memcpy(g_gobjects_accessor.data() + 3, &relative, sizeof(relative));

    g_chunks[0] = reinterpret_cast<std::uintptr_t>(g_object_items[0].data());
    g_chunks[1] = reinterpret_cast<std::uintptr_t>(
        g_object_items[kObjectChunkSize].data());
    const auto chunk_table = reinterpret_cast<std::uintptr_t>(g_chunks.data());
    Store(g_object_registry, kObjectRegistryItemsOffset, chunk_table);
    const auto vtable = reinterpret_cast<std::uintptr_t>(g_vtable.data());
    const auto set_text = reinterpret_cast<std::uintptr_t>(&SetText);
    Store(g_vtable, kSetTextVtableOffset, set_text);
    for (std::uint32_t index = 0; index < kObjectCount; ++index) {
        const std::uint32_t serial = 71U + index;
        g_object_serials[index] = serial;
    }
    for (std::size_t widget_index = 0; widget_index < kWidgetCount; ++widget_index) {
        const std::size_t object_slot = ObjectSlotForWidget(widget_index);
        const auto object = reinterpret_cast<std::uintptr_t>(
            g_objects[widget_index].data());
        Store(g_object_items[object_slot], 0, object);
        Store(
            g_object_items[object_slot], kObjectItemSerialOffset,
            g_object_serials[object_slot]);
        Store(g_objects[widget_index], 0, vtable);
        Store(
            g_objects[widget_index], kObjectNameOffset,
            g_object_name_ids[object_slot]);
        const UnrealText current_text{};
        Store(g_objects[widget_index], kTextFieldOffset, current_text);
        std::copy(
            kOriginalUid.begin(), kOriginalUid.end(),
            g_current_texts[widget_index].begin());
        g_current_texts[widget_index][kOriginalUid.size()] = L'\0';
    }

    g_config = {sizeof(g_config), ANOMALY_CONFIG_SERVICE_V1_VERSION, nullptr,
        RegisterSchema, nullptr, ReadConfig, WriteConfig, nullptr};
    g_scheduler = {sizeof(g_scheduler), ANOMALY_SCHEDULER_SERVICE_V1_VERSION,
        nullptr, Schedule, Cancel};
    g_signature = {sizeof(g_signature), ANOMALY_SIGNATURE_SERVICE_V1_VERSION,
        nullptr, ResolveSignature};
    g_window = {sizeof(g_window), ANOMALY_WINDOW_SERVICE_V1_VERSION, nullptr,
        RegisterWindow, WindowHandle, SetWindowOpen, nullptr, WindowState,
        BeginWindow, WindowHandle};
    g_objects_service = {sizeof(g_objects_service), ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION,
        nullptr, ObjectGeneration, ObjectCount, SnapshotAt, SnapshotByHandle};
    g_names = {sizeof(g_names), ANOMALY_UE5_NAMES_SERVICE_V1_VERSION,
        nullptr, ResolveName};
    return true;
}

bool InitialWidgetsEqual() {
    if (std::wstring_view(g_current_texts.front().data()) != kOriginalUid ||
        g_set_text_calls.front() != 0) {
        return false;
    }
    for (std::size_t index = 1; index < kWidgetCount; ++index) {
        if (std::wstring_view(g_current_texts[index].data()) != kDefaultUid ||
            g_set_text_calls[index] != 1) {
            return false;
        }
    }
    return true;
}

bool ReusedSlotWidgetsEqual() {
    if (std::wstring_view(g_current_texts.front().data()) != kChangedUid ||
        g_set_text_calls.front() != 1) {
        return false;
    }
    for (std::size_t index = 1; index < kWidgetCount; ++index) {
        if (std::wstring_view(g_current_texts[index].data()) != kChangedUid ||
            g_set_text_calls[index] != 2) {
            return false;
        }
    }
    return true;
}

bool RunUpdatesUntil(
    const AnomalyPluginDescriptorV1& descriptor, void* context,
    bool (*condition)()) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        descriptor.on_update(context, 1.0 / 60.0);
        if (condition()) return true;
    }
    return false;
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
    if (argc != 2 || !ResetFixture()) return 1;
    const HMODULE module = LoadLibraryW(argv[1]);
    if (module == nullptr) return 2;
    const auto entry = reinterpret_cast<AnomalyPluginEntryV1Fn>(
        GetProcAddress(module, "AnomalyPluginEntryV1"));
    if (entry == nullptr) return 3;

    AnomalyPluginDescriptorV1 descriptor{sizeof(descriptor)};
    if (entry(&descriptor).code != ANOMALY_STATUS_V1_OK ||
        descriptor.on_load == nullptr || descriptor.on_start == nullptr ||
        descriptor.on_stop == nullptr || descriptor.on_unload == nullptr ||
        descriptor.on_update == nullptr || descriptor.on_draw == nullptr) {
        return 4;
    }

    AnomalyHostApiV1 host{sizeof(host), ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR, nullptr, {}, Query};
    void* context{};
    if (descriptor.on_load(&host, &context).code != ANOMALY_STATUS_V1_OK ||
        context == nullptr ||
        descriptor.on_start(context).code != ANOMALY_STATUS_V1_OK ||
        g_signature_calls != 6 || g_config_writes != 1) {
        return 5;
    }

    if (!RunUpdatesUntil(descriptor, context, InitialWidgetsEqual)) {
        std::cerr << "FakeUID did not initialize the first live UID widgets\n";
        return 6;
    }

    g_object_name_ids[0] = kTargetNameId;
    g_object_serials[0] = 1000000;
    Store(g_object_items[0], kObjectItemSerialOffset, g_object_serials[0]);
    Store(g_objects[0], kObjectNameOffset, g_object_name_ids[0]);
    AnomalyUiServiceV1 ui{};
    ui.struct_size = sizeof(ui);
    ui.service_version = ANOMALY_UI_SERVICE_V1_VERSION;
    ui.text = UiText;
    ui.button = UiButton;
    ui.input_text = UiInputText;
    descriptor.on_draw(context, &ui);
    if (g_set_window_open_calls != 0 || g_window_begin_calls != 0 || g_apply_clicked) {
        std::cerr << "FakeUID reopened a host-persisted closed window\n";
        return 7;
    }
    g_window_open = true;
    descriptor.on_draw(context, &ui);
    if (!g_apply_clicked || g_window_begin_calls != 1 ||
        !RunUpdatesUntil(descriptor, context, ReusedSlotWidgetsEqual)) {
        std::cerr << "FakeUID Apply did not discover a newer widget in a reused slot\n";
        return 8;
    }

    if (descriptor.on_stop(context, 1000).code != ANOMALY_STATUS_V1_OK) return 9;
    descriptor.on_unload(context);
    FreeLibrary(module);
    return 0;
}
