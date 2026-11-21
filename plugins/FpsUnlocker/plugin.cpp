#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/localization.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>

namespace {
using namespace anomaly::sdk;

constexpr auto Id = "b1ank.fps-unlocker";
constexpr auto Title = "FPS Unlocker";
constexpr auto Version = "0.1.5";
constexpr auto ConfigId = "settings";
constexpr uint32_t ConfigVersion = 1;
constexpr std::string_view ConfigSchema =
    R"({"type":"object","required":["enabled"],"additionalProperties":false,"properties":{"enabled":{"type":"boolean"}}})";
constexpr std::string_view EnabledConfig = R"({"enabled":true})";
constexpr std::string_view DisabledConfig = R"({"enabled":false})";
using Clock = std::chrono::steady_clock;
constexpr auto CheckInterval = std::chrono::seconds(1);
constexpr float Unknown = std::numeric_limits<float>::quiet_NaN();

enum class Result { Idle, Waiting, Unavailable, NoPlayer, Unlocked, Restored, Unconfirmed };

struct PacingSample {
    std::optional<float> cap_before;
    std::optional<float> cap_after;
    std::optional<float> vsync;
    Result result{};

    bool operator==(const PacingSample&) const = default;
};

struct Context {
    Context(const AnomalyHostApiV1* raw, const AnomalyWindowServiceV1* windows, const AnomalyConfigServiceV1* config,
            const AnomalySchedulerServiceV1* scheduler) noexcept
        : host(raw), localizer(raw), windows(windows), config(config), scheduler(scheduler) {}

    Host host;
    Localizer localizer;
    const AnomalyWindowServiceV1* windows;
    const AnomalyConfigServiceV1* config;
    const AnomalySchedulerServiceV1* scheduler;
    AnomalyGenerationHandleV1 window{};
    std::atomic<bool> enabled{};
    std::atomic<int> pending_save{-1};
    std::mutex save_mutex;
    std::optional<float> original_limit;
    Clock::time_point next_check{};
    bool last_requested_enabled{};
    std::optional<PacingSample> last_sample;
};

AnomalyByteSpanV1 Bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

void ReadSettings(Context& ctx) noexcept {
    // The host returns the parsed configuration as compact JSON.
    std::array<uint8_t, DisabledConfig.size()> bytes{};
    size_t size = bytes.size();
    uint32_t version{};
    const auto status = ctx.config->read(ctx.config->user, StringView(ConfigId), &version,
                                         {bytes.data(), bytes.size()}, &size);
    if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) return;
    if (Succeeded(status) && version == ConfigVersion && size <= bytes.size()) {
        const std::string_view document{reinterpret_cast<const char*>(bytes.data()), size};
        if (document == EnabledConfig || document == DisabledConfig) {
            ctx.enabled = document == EnabledConfig;
            return;
        }
    }
}

void SaveSettings(Context& ctx) noexcept {
    std::scoped_lock lock(ctx.save_mutex);
    const int setting = ctx.pending_save.exchange(-1);
    if (setting < 0) return;
    const auto status = ctx.config->write_atomic(ctx.config->user, StringView(ConfigId), ConfigVersion,
                                                 Bytes(setting ? EnabledConfig : DisabledConfig));
    if (!Succeeded(status)) {
        int empty = -1;
        ctx.pending_save.compare_exchange_strong(empty, setting);
    }
}

void ANOMALY_CALL SaveSettingsTask(void* raw, AnomalyGenerationHandleV1) noexcept {
    SaveSettings(*static_cast<Context*>(raw));
}

void SetEnabled(Context& ctx, bool enabled) noexcept {
    ctx.enabled = enabled;
    ctx.pending_save = enabled;
    AnomalyGenerationHandleV1 task{};
    static_cast<void>(ctx.scheduler->schedule(ctx.scheduler->user, 0, SaveSettingsTask, &ctx, &task));
}

// Parameter layouts from the UE 5.6.1 Windows x64 SDK. The FString inputs
// borrow null-terminated buffers for the duration of synchronous dispatch.
struct StringInput {
    const wchar_t* data;
    int32_t count;
    int32_t capacity;
};

struct ReadLimitParams {
    StringInput name;
    float value;
};

struct ExecuteCommandParams {
    uintptr_t world_context;
    StringInput command;
    uintptr_t specific_player;
};

static_assert(sizeof(wchar_t) == 2 && sizeof(StringInput) == 0x10);
static_assert(sizeof(ReadLimitParams) == 0x18 && offsetof(ReadLimitParams, value) == 0x10);
static_assert(sizeof(ExecuteCommandParams) == 0x20 && offsetof(ExecuteCommandParams, command) == 8);

AnomalyGenerationHandleV1 Find(const AnomalyUe5ObjectsServiceV1& objects, std::string_view path) noexcept {
    AnomalyGenerationHandleV1 handle{};
    if (Succeeded(objects.find_exact(objects.user, StringView(path), &handle))) return handle;
    return {};
}

std::optional<float> ReadCVar(const AnomalyUe5ProcessEventServiceV1& events,
                             AnomalyGenerationHandleV1 library, AnomalyGenerationHandleV1 getter,
                             const wchar_t* name, int32_t size) noexcept {
    ReadLimitParams args{{name, size, size}, 0.0f};
    if (!Succeeded(events.invoke(events.user, library, getter, &args, sizeof(args))) || !std::isfinite(args.value))
        return {};
    return args.value;
}

Result Apply(Context& ctx, bool unlock, PacingSample& sample) noexcept {
    if (!unlock && !ctx.original_limit) return Result::Idle;
    const bool first_unlock = unlock && !ctx.original_limit;

    const auto objects = ctx.host.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID);
    const auto events = ctx.host.Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID);
    if (!objects || objects->struct_size < sizeof(*objects.get()) || !objects->find_exact ||
        !events || events->struct_size < sizeof(*events.get()) || !events->invoke)
        return Result::Unavailable;

    const auto library = Find(*objects.get(), "/Script/Engine.Default__KismetSystemLibrary");
    const auto getter = Find(*objects.get(), "/Script/Engine.KismetSystemLibrary.GetConsoleVariableFloatValue");
    if (!library.id || !getter.id) return Result::Unavailable;

    constexpr wchar_t cap_name[] = L"t.MaxFPS";
    constexpr wchar_t vsync_name[] = L"r.VSync";
    const auto current = ReadCVar(*events.get(), library, getter, cap_name, std::size(cap_name));
    sample.cap_before = sample.cap_after = current;
    if (unlock) sample.vsync = ReadCVar(*events.get(), library, getter, vsync_name, std::size(vsync_name));
    if (!current) return Result::Unavailable;
    if (unlock && *current <= 0) {
        if (first_unlock) ctx.original_limit = current;
        return Result::Unlocked;
    }
    const float requested = unlock ? 0.0f : *ctx.original_limit;

    if (*current != requested) {
        const auto players = ctx.host.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID);
        const auto execute = Find(*objects.get(), "/Script/Engine.KismetSystemLibrary.ExecuteConsoleCommand");
        const auto get_pawn = Find(*objects.get(), "/Script/Engine.Controller.K2_GetPawn");
        if (!players || players->struct_size < sizeof(*players.get()) || !players->get_object_handles ||
            !execute.id || !get_pawn.id)
            return Result::Unavailable;

        AnomalyGenerationHandleV1 controller{};
        uintptr_t pawn{};
        if (!Succeeded(players->get_object_handles(players->user, nullptr, &controller)) ||
            !Succeeded(events->invoke(events->user, controller, get_pawn, &pawn, sizeof(pawn))) || !pawn)
            return Result::NoPlayer;

        char number[32]{};
        const auto written = std::to_chars(number, number + sizeof(number), requested);
        if (written.ec != std::errc{}) return Result::Unavailable;
        constexpr std::wstring_view prefix = L"t.MaxFPS ";
        wchar_t command[prefix.size() + sizeof(number) + 1]{};
        std::copy(prefix.begin(), prefix.end(), command);
        std::copy(number, written.ptr, command + prefix.size());
        const auto size = static_cast<int32_t>(prefix.size() + (written.ptr - number) + 1);
        ExecuteCommandParams args{pawn, {command, size, size}, 0};
        if (first_unlock) ctx.original_limit = current;
        events->invoke(events->user, library, execute, &args, sizeof(args));
        const auto actual = ReadCVar(*events.get(), library, getter, cap_name, std::size(cap_name));
        sample.cap_after = actual;
        if (!actual) return Result::Unconfirmed;
        if (*actual != requested) {
            if (first_unlock) ctx.original_limit.reset();
            return Result::Unconfirmed;
        }
    }

    if (unlock) return Result::Unlocked;
    ctx.original_limit.reset();
    return Result::Restored;
}

void PublishPacing(Context& ctx, const PacingSample& sample) noexcept {
    if (ctx.last_sample == sample) return;
    ctx.last_sample = sample;

    const auto core = ctx.host.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID);
    if (!core || core->struct_size < offsetof(AnomalyCoreServiceV1, log) + sizeof(core->log) || !core->log) return;
    char message[160]{};
    std::snprintf(message, sizeof(message), "FPS Unlocker: t.MaxFPS before=%g after=%g r.VSync=%g result=%d",
                  sample.cap_before.value_or(Unknown), sample.cap_after.value_or(Unknown),
                  sample.vsync.value_or(Unknown), static_cast<int>(sample.result));
    core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(message));
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* raw, void** out) noexcept {
    if (!out) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *out = nullptr;
    const Host host(raw);
    const auto windows = host.Query<AnomalyWindowServiceV1>(ANOMALY_WINDOW_SERVICE_V1_ID);
    if (!windows || windows->struct_size < sizeof(*windows.get()) || !windows->register_window ||
        !windows->release_window || !windows->begin || !windows->end)
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, StringView("Anomaly window service unavailable")};
    const auto config = host.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID);
    const auto scheduler = host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID);
    if (!config || config->struct_size < sizeof(*config.get()) || !config->register_schema ||
        !config->read || !config->write_atomic || !scheduler ||
        scheduler->struct_size < sizeof(*scheduler.get()) || !scheduler->schedule)
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, StringView("Anomaly configuration services unavailable")};

    AnomalyGenerationHandleV1 schema{};
    const auto registered = config->register_schema(config->user, StringView(ConfigId), ConfigVersion,
                                                     Bytes(ConfigSchema), &schema);
    if (!Succeeded(registered)) return registered;

    auto* ctx = new (std::nothrow) Context{raw, windows.get(), config.get(), scheduler.get()};
    if (!ctx) return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    ReadSettings(*ctx);
    *out = ctx;
    return Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* raw) noexcept {
    try {
        auto& ctx = *static_cast<Context*>(raw);
        AnomalyWindowSpecV1 spec{sizeof(spec)};
        spec.id = StringView("fps-unlocker-main");
        const auto title = ctx.localizer.Text("window.title", "FPS Unlocker");
        spec.title = StringView(title);
        spec.initial_width = 350;
        spec.initial_height = 90;
        spec.default_open = 1;
        return ctx.windows->register_window(ctx.windows->user, &spec, &ctx.window);
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED, 0, {}}; }
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* raw, uint32_t) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    SaveSettings(ctx);
    if (!ctx.window.id) return Ok();
    const auto result = ctx.windows->release_window(ctx.windows->user, ctx.window);
    if (!Succeeded(result) && result.code != ANOMALY_STATUS_V1_NOT_FOUND) return result;
    ctx.window = {};
    return Ok();
}

void ANOMALY_CALL Unload(void* raw) noexcept { delete static_cast<Context*>(raw); }

void ANOMALY_CALL Update(void* raw, double) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    const bool enabled = ctx.enabled.load();
    if (!enabled && !ctx.original_limit) {
        ctx.last_requested_enabled = false;
        return;
    }
    const auto now = Clock::now();
    if (enabled == ctx.last_requested_enabled && now < ctx.next_check) return;
    ctx.last_requested_enabled = enabled;
    ctx.next_check = now + CheckInterval;
    PacingSample sample;
    sample.result = Apply(ctx, enabled, sample);
    PublishPacing(ctx, sample);
}

void ANOMALY_CALL Draw(void* raw, const AnomalyUiServiceV1* ui) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    if (!ui || !ui->checkbox || !ctx.window.id) return;
    int32_t visible{};
    if (!Succeeded(ctx.windows->begin(ctx.windows->user, ctx.window, 0, &visible))) return;
    try {
        if (visible) {
            int enabled = ctx.enabled.load();
            const auto label = ctx.localizer.Text("settings.enabled", "Unlock frame rate");
            const bool changed = ui->checkbox(ui->user, StringView(label), &enabled) != 0;
            if (changed) SetEnabled(ctx, enabled != 0);
        }
    } catch (...) {}
    ctx.windows->end(ctx.windows->user, ctx.window);
}
}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
                   StringView(Id), StringView(Title), StringView("b1ank"), StringView(Version),
                   Load, Start, Stop, Unload, Update, Draw, nullptr};
    return Ok();
}
