#include "plugin.cpp"

#include <cstdlib>
#include <iostream>

namespace fixture {
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class T, std::size_t N>
void Put(std::array<std::byte, N>& memory, std::size_t offset, T value) {
    std::memcpy(memory.data() + offset, &value, sizeof(value));
}
void ANOMALY_CALL TextSetter(void*, const UnrealText*) {}
void ANOMALY_CALL FreeString(void*) {}
UnrealString* ANOMALY_CALL TextReader(UnrealString* result, const UnrealText* text) {
    result->data = static_cast<wchar_t*>(text->data);
    result->count = static_cast<std::int32_t>(std::wcslen(result->data) + 1);
    result->capacity = result->count;
    return result;
}

struct Registry {
    static constexpr std::uint32_t count = 512;
    std::uint64_t generation{1};
    unsigned snapshots{}, reverse_lookups{}, exact_lookups{};
    std::array<std::array<std::byte, 512>, count> objects{};
    std::array<std::byte, count * 24> slots{};
    std::array<std::uintptr_t, 1> chunks{};
    std::array<std::byte, 32> header{};
    std::array<std::uintptr_t, 108> vtable{};
    std::array<std::byte, 256> visibility_function{};
    AnomalyUe5ObjectsServiceV1 objects_service{};
    AnomalyUe5NamesServiceV1 names_service{};
    Context context{};

    Registry() {
        vtable[107] = reinterpret_cast<std::uintptr_t>(&TextSetter);
        chunks[0] = reinterpret_cast<std::uintptr_t>(slots.data());
        Put(header, 16, reinterpret_cast<std::uintptr_t>(chunks.data()));
        for (std::uint32_t i = 0; i < count; ++i) {
            Put(slots, i * 24, reinterpret_cast<std::uintptr_t>(objects[i].data()));
            Put(slots, i * 24 + 16, std::uint32_t{7});
            Put(objects[i], 0, reinterpret_cast<std::uintptr_t>(vtable.data()));
            Put(objects[i], 24, std::uint32_t{99});
        }
        objects_service = {sizeof(objects_service), 1, this, Generation, Count,
            Snapshot, ByHandle, FindExact};
        names_service = {sizeof(names_service), 2, this, Name, nullptr, ReverseName};
        context.objects = &objects_service;
        context.names = &names_service;
        context.object_registry = reinterpret_cast<std::uintptr_t>(header.data());
        context.set_text = TextSetter;
        context.text_to_string = TextReader;
        context.free_string = FreeString;
        context.text_field_offset = 392;
        context.process_event = Event;
        Put(visibility_function, 16, std::uintptr_t{1});
        Put(visibility_function, 32, std::uintptr_t{1});
        Put(visibility_function, 0xD8, std::uintptr_t{1});
        Put(visibility_function, 180, std::uint8_t{1});
        Put(visibility_function, 182, std::uint16_t{1});
        Put(visibility_function, 184, std::uint16_t{0xFFFF});
        Put(slots, 0, reinterpret_cast<std::uintptr_t>(visibility_function.data()));
    }
    static std::uint64_t ANOMALY_CALL Generation(void* user) {
        return static_cast<Registry*>(user)->generation;
    }
    static std::uint32_t ANOMALY_CALL Count(void*) { return count; }
    static AnomalyStatusV1 ANOMALY_CALL Snapshot(void* user, std::uint32_t i,
                                                AnomalyUe5ObjectSnapshotV1* snapshot) {
        auto& registry = *static_cast<Registry*>(user);
        ++registry.snapshots;
        snapshot->handle = {(std::uint64_t{7} << 32) | (i + 1), registry.generation};
        std::memcpy(&snapshot->name_id, registry.objects[i].data() + 24, 4);
        return anomaly::sdk::Ok();
    }
    static AnomalyStatusV1 ANOMALY_CALL ByHandle(void* user, AnomalyGenerationHandleV1 handle,
                                                AnomalyUe5ObjectSnapshotV1* snapshot) {
        return Snapshot(user, static_cast<std::uint32_t>(handle.id) - 1, snapshot);
    }
    static AnomalyStatusV1 ANOMALY_CALL FindExact(void* user, AnomalyStringViewV1 path,
                                                 AnomalyGenerationHandleV1* handle) {
        auto& registry = *static_cast<Registry*>(user);
        ++registry.exact_lookups;
        if (std::string_view(path.data, path.size) != "/Script/UMG.Widget:SetVisibility")
            return {ANOMALY_STATUS_V1_NOT_FOUND, 0, {}};
        *handle = {(std::uint64_t{7} << 32) | 1, registry.generation};
        return anomaly::sdk::Ok();
    }
    static AnomalyStatusV1 ANOMALY_CALL Name(void*, std::uint32_t id, char* data,
                                           std::size_t* size) {
        const char* name = id == 11 ? "TextPing" : id == 12 ? "ImagePing" :
            id == 13 ? "TextBlock_RoleID" : "Other";
        const auto bytes = std::strlen(name) + 1;
        if (data == nullptr) { *size = bytes; return anomaly::sdk::Ok(); }
        if (*size < bytes) { *size = bytes; return {ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, 0, {}}; }
        std::memcpy(data, name, bytes);
        *size = bytes;
        return anomaly::sdk::Ok();
    }
    static AnomalyStatusV1 ANOMALY_CALL ReverseName(void* user, AnomalyStringViewV1,
                                                   std::uint32_t*) {
        ++static_cast<Registry*>(user)->reverse_lookups;
        return {ANOMALY_STATUS_V1_NOT_FOUND, 0, {}};
    }
    static void ANOMALY_CALL Event(void* object, void*, void* parameter) {
        std::memcpy(static_cast<std::byte*>(object) + 0xDC, parameter, 1);
    }
    void Widget(std::uint32_t index, std::uint32_t name, std::uintptr_t outer,
                wchar_t* text = nullptr, std::uint8_t visibility = 0) {
        Put(objects[index], 24, name);
        Put(objects[index], 32, outer);
        Put(objects[index], 392, UnrealText{text});
        Put(objects[index], 0xDC, visibility);
    }
    void Probe() { ++context.update_tick; ProbeLatencyWidget(context); }
};

struct PrefixFixture : Registry {
    struct TextData { std::uintptr_t vtable; std::wstring value; };
    std::vector<std::unique_ptr<TextData>> text_data;
    std::array<std::byte, 256> convert{}, set{}, get_position{}, set_position{};
    std::array<std::byte, 128> prefix_slot{}, value_slot{};
    AnomalyGenerationHandleV1 prefix_handle{}, value_handle{};
    PrefixFixture() {
        convert[0] = std::byte{1}; set[0] = std::byte{2};
        get_position[0] = std::byte{3}; set_position[0] = std::byte{4};
        context.text_to_string = ReadText;
        context.assign_string = Assign;
        context.process_event = Dispatch;
        context.string_to_text_function = reinterpret_cast<std::uintptr_t>(convert.data());
        context.text_block_set_text_function = reinterpret_cast<std::uintptr_t>(set.data());
        context.kismet_text_library_cdo = reinterpret_cast<std::uintptr_t>(this);
        context.text_write_generation = generation;
        context.slot_get_position_function = reinterpret_cast<std::uintptr_t>(get_position.data());
        context.slot_set_position_function = reinterpret_cast<std::uintptr_t>(set_position.data());
        context.slot_set_position_generation = generation;
        context.roleid_outer = 100; context.roleid_panel = 200;
        Put(prefix_slot, 32, std::uintptr_t{200});
        Put(value_slot, 32, std::uintptr_t{200});
        Put(value_slot, 64, double{43}); Put(value_slot, 72, double{9});
        Widget(300, 13, 100); Widget(301, 14, 100);
        Put(objects[300], fake_uid_profile::kWidgetSlotOffset,
            reinterpret_cast<std::uintptr_t>(value_slot.data()));
        Put(objects[301], fake_uid_profile::kWidgetSlotOffset,
            reinterpret_cast<std::uintptr_t>(prefix_slot.data()));
        prefix_handle = {(std::uint64_t{7} << 32) | 302, generation};
        value_handle = {(std::uint64_t{7} << 32) | 301, generation};
        context.widgets[0] = {value_handle, false};
        context.widgets[1] = {prefix_handle, true};
        context.widget_count = 2; context.target_name_id = 13;
        context.detected_uid.store(1337);
        Write(301, L"UID:"); Write(300, L"1337");
    }
    UnrealText MakeText(std::wstring value) {
        auto data = std::make_unique<TextData>();
        data->vtable = reinterpret_cast<std::uintptr_t>(vtable.data());
        data->value = std::move(value);
        UnrealText result{data.get()}; text_data.push_back(std::move(data));
        return result;
    }
    void Write(unsigned index, std::wstring value) {
        Put(objects[index], 392, MakeText(std::move(value)));
    }
    std::wstring Read(unsigned index) {
        std::wstring value;
        Check(ReadWidgetText(context, reinterpret_cast<std::uintptr_t>(objects[index].data()), value),
            "text readback must succeed");
        return value;
    }
    static UnrealString* ANOMALY_CALL Assign(UnrealString* out, const wchar_t* value) {
        *out = {const_cast<wchar_t*>(value), static_cast<std::int32_t>(std::wcslen(value) + 1),
            static_cast<std::int32_t>(std::wcslen(value) + 1)};
        return out;
    }
    static UnrealString* ANOMALY_CALL ReadText(UnrealString* out, const UnrealText* text) {
        auto& value = static_cast<TextData*>(text->data)->value;
        *out = {value.data(), static_cast<std::int32_t>(value.size() + 1),
            static_cast<std::int32_t>(value.size() + 1)};
        return out;
    }
    static void ANOMALY_CALL Dispatch(void* object, void* function, void* parameters) {
        const auto tag = *static_cast<std::byte*>(function);
        if (tag == std::byte{1}) {
            struct Conversion { UnrealString input; UnrealText result; };
            auto& p = *static_cast<Conversion*>(parameters);
            p.result = static_cast<PrefixFixture*>(object)->MakeText(p.input.data);
        } else if (tag == std::byte{2}) {
            std::memcpy(static_cast<std::byte*>(object) + 392, parameters, sizeof(UnrealText));
        } else if (tag == std::byte{3}) {
            std::memcpy(parameters, static_cast<std::byte*>(object) + 64, sizeof(double) * 2);
        } else if (tag == std::byte{4}) {
            std::memcpy(static_cast<std::byte*>(object) + 64, parameters, sizeof(double) * 2);
        }
    }
    void Apply(const SettingsSnapshot& settings) {
        ++context.update_tick;
        Check(ApplyToWidget(context, prefix_handle, settings, context.update_tick) == ApplyResult::Applied,
            "prefix must apply");
        Check(ApplyToWidget(context, value_handle, settings, context.update_tick) == ApplyResult::Applied,
            "value must apply");
    }
};
}

int main() {
    using namespace fixture;
    Registry registry;
    // Two interleaved HUD trees: the older algorithm overwrote both handles
    // and lost the matching image before reaching its text widget.
    wchar_t ping[] = L"1 ms";
    registry.Widget(490, 12, 100);
    registry.Widget(450, 12, 200);
    registry.Widget(340, 11, 100, ping, 4);
    for (unsigned i = 0; i < 20 && !registry.context.latency_probe_found; ++i) {
        const auto before = registry.snapshots;
        registry.Probe();
        Check(registry.snapshots - before <= 128, "discovery must remain bounded per update");
    }
    Check(registry.context.latency_probe_found, "pair must be found without reverse name lookup");
    Check(registry.reverse_lookups == 0, "discovery must not scan the FName pool");
    ApplyLatencyVisibility(registry.context, true);
    Check(ReadVisibilityField(reinterpret_cast<std::uintptr_t>(registry.objects[340].data())) == 1,
          "latency text must collapse");
    Check(ReadVisibilityField(reinterpret_cast<std::uintptr_t>(registry.objects[490].data())) == 1,
          "matching image must collapse");
    Check(ReadVisibilityField(reinterpret_cast<std::uintptr_t>(registry.objects[450].data())) == 0,
          "foreign HUD image must remain unchanged");
    ++registry.context.update_tick;
    ApplyLatencyVisibility(registry.context, false);
    Check(!registry.context.latency_original_captured, "restoration must complete on the next tick");
    Check(ReadVisibilityField(reinterpret_cast<std::uintptr_t>(registry.objects[340].data())) == 4,
          "original noninteractive visibility must be restored exactly");
    Check(registry.exact_lookups == 1, "successful visibility binding must be reused");
    const auto before = registry.snapshots;
    for (unsigned i = 0; i < 100; ++i) registry.Probe();
    Check(registry.snapshots == before, "bound HUD must not rescan the registry");

    Registry missing;
    for (unsigned i = 0; i < 50 && (missing.context.latency_probe_cursor != 0 || i == 0); ++i)
        missing.Probe();
    const auto completed = missing.snapshots;
    for (unsigned i = 0; i < 10; ++i) missing.Probe();
    Check(missing.snapshots == completed, "missing widgets must back off after a completed pass");
    Check(missing.reverse_lookups == 0, "missing widgets must not trigger reverse lookup retries");

    auto prefix_storage = std::make_unique<PrefixFixture>();
    auto& prefix = *prefix_storage;
    auto shown = MakeSettings(true, false, "1337");
    auto hidden = MakeSettings(true, true, "1337");
    prefix.Apply(*shown);
    prefix.Write(301, L"UID"); // the animation frame just before the colon
    prefix.Apply(*hidden);
    prefix.Apply(*shown);
    Check(prefix.Read(301) == L"UID:", "hide/show must restore the colon after a partial frame");
    auto custom = MakeSettings(true, false, "https://github.com/example", "a very long prefix: ");
    prefix.Apply(*custom);
    Check(prefix.Read(301) == kHiddenPrefixText, "custom line must use a single visible TextBlock");
    Check(prefix.Read(300) == L"a very long prefix: https://github.com/example",
        "long prefix and value must retain their order without overlay");
    double position[2]{};
    std::memcpy(position, prefix.value_slot.data() + 64, sizeof(position));
    Check(position[0] == 1 && position[1] == 9, "composed line must align left and preserve Y");
    prefix.Apply(*shown);
    Check(prefix.Read(301) == L"UID:", "clearing the custom prefix must restore the complete label");
    std::memcpy(position, prefix.value_slot.data() + 64, sizeof(position));
    Check(position[0] == 43 && position[1] == 9, "restoring separate labels must restore the value slot");
    prefix.context.typing_enabled.store(true);
    prefix.Apply(*shown);
    for (unsigned i = 0; i <= 8; ++i) {
        prefix.context.typing.next_frame = {};
        UpdateTypingAnimation(prefix.context, *shown, 1);
        Check(prefix.Read(301) == kHiddenPrefixText, "dynamic UID must keep its sibling label empty");
    }
    Check(prefix.Read(300) == L"UID:1337", "dynamic UID must include the complete prefix and colon");
    prefix.context.typing_enabled.store(false);
    UpdateTypingAnimation(prefix.context, *shown, 1);
    prefix.Apply(*shown);
    Check(prefix.Read(301) == L"UID:", "stopping animation must restore the complete label");
    std::cout << "Latency discovery, work budget, visibility and restoration passed\n";
    std::cout << "Prefix restoration, long text layout and dynamic UID passed\n";
}
