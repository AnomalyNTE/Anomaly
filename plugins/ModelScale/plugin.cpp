#include "anomaly/sdk/cpp.hpp"
#include "plugins/common/localization.hpp"
#include "plugins/ModelScale/model_scale_profile.hpp"
#include "plugins/ModelScale/scale_plan.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

namespace profile = model_scale_profile;

constexpr std::string_view kConfigSchemaId = "anomaly.builtin.model-scale.settings";
constexpr std::uint32_t kConfigSchemaVersion = 1;
inline constexpr std::string_view kConfigSchemaJson =
    R"({"type":"object","properties":{"enabled":{"type":"boolean"},)"
    R"("scale":{"type":"number","minimum":0.1,"maximum":10.0}},)"
    R"("required":["enabled","scale"],"additionalProperties":false})";

// The scale the plugin wants is the factor times whatever the game itself put on the
// component. What the plugin writes is decided by model_scale::PlanScale.
struct AppliedState {
    // The component the plugin last wrote to, so a disable can put it back even after the
    // character moved to another one.
    std::uintptr_t component{};
    model_scale::TargetScaleState scale{};
};

// Why the last game-thread pass did what it did. The Game thread publishes one of these
// and the Render thread turns it into localized text, so the status line crosses the
// thread boundary as a single atomic word instead of a shared string buffer.
enum class DetailCode : unsigned {
    resized = 0,
    applied = 1,
    restored = 2,
    restore_failed = 3,
    no_pawn = 4,
    no_component = 5,
    unreadable = 6,
    unwritable = 7,
    not_markable = 8,
    idle = 9,
};

struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyUiServiceV1* ui{};
    const AnomalySignatureServiceV1* signatures{};
    const AnomalyUe5AhudServiceV1* ahud{};
    const AnomalyConfigServiceV1* config{};
    anomaly::plugins::Localizer localizer;

    std::uintptr_t g_world_address{};
    AnomalyGenerationHandleV1 ahud_subscription{};
    AnomalyGenerationHandleV1 config_schema{};

    // Written by the render thread (UI), consumed by the Game thread.
    std::atomic<bool> enabled{false};
    std::atomic<double> factor{1.0};
    std::atomic<bool> sync_requested{false};

    // The component's scale before the plugin touched it, shared with the Game thread and
    // persisted to configuration. Held across a plugin reload so the plugin never has to
    // infer the original scale from a component that already carries its own output.
    std::atomic<double> saved_base{1.0};
    std::atomic<bool> saved_base_known{false};
    // Set when the base is measured, so the next tick can write it to configuration.
    std::atomic<bool> base_measured{false};

    // Published by the Game thread for the UI to display. The status line crosses the
    // thread boundary as one word; the UI renders it in the active locale.
    std::atomic<bool> publish_applied{false};
    std::atomic<double> publish_base_scale{1.0};
    std::atomic<double> publish_effective_factor{1.0};
    std::atomic<unsigned> publish_detail{static_cast<unsigned>(DetailCode::idle)};

    // Owned by the Game thread only.
    AppliedState applied{};
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

constexpr AnomalyStatusV1 StatusCode(const std::uint32_t code) noexcept {
    return {code, 0, {}};
}

template <typename Service>
bool ServiceTableReady(const Service* service, const std::uint32_t version) noexcept {
    constexpr std::size_t kPrefix = offsetof(Service, user) + sizeof(void*);
    return service != nullptr && service->struct_size >= kPrefix &&
        service->service_version >= version;
}

bool CoreReady(const AnomalyCoreServiceV1* service) noexcept {
    return ServiceTableReady(service, ANOMALY_CORE_SERVICE_V1_VERSION) &&
        HasField<AnomalyCoreServiceV1, decltype(AnomalyCoreServiceV1::write_memory)>(
            service, offsetof(AnomalyCoreServiceV1, write_memory)) &&
        service->read_memory != nullptr && service->write_memory != nullptr;
}

bool SignaturesReady(const AnomalySignatureServiceV1* service) noexcept {
    return ServiceTableReady(service, ANOMALY_SIGNATURE_SERVICE_V1_VERSION) &&
        service->resolve != nullptr;
}

bool AhudReady(const AnomalyUe5AhudServiceV1* service) noexcept {
    return ServiceTableReady(service, ANOMALY_UE5_AHUD_SERVICE_V1_VERSION) &&
        HasField<AnomalyUe5AhudServiceV1, decltype(AnomalyUe5AhudServiceV1::unsubscribe)>(
            service, offsetof(AnomalyUe5AhudServiceV1, unsubscribe)) &&
        service->subscribe != nullptr && service->unsubscribe != nullptr;
}

bool ConfigReady(const AnomalyConfigServiceV1* service) noexcept {
    return ServiceTableReady(service, ANOMALY_CONFIG_SERVICE_V1_VERSION) &&
        HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::write_atomic)>(
            service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
        service->register_schema != nullptr && service->read != nullptr &&
        service->write_atomic != nullptr && service->unregister_schema != nullptr;
}

bool HasUiFrame(const AnomalyUiServiceV1* ui) noexcept {
    return ServiceTableReady(ui, ANOMALY_UI_SERVICE_V1_VERSION) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_double)>(
            ui, offsetof(AnomalyUiServiceV1, input_double)) &&
        ui->begin_window != nullptr && ui->end_window != nullptr &&
        ui->text != nullptr && ui->checkbox != nullptr &&
        ui->input_double != nullptr && ui->button != nullptr &&
        ui->separator != nullptr;
}

std::uintptr_t AddOffset(const std::uintptr_t base, const std::uint32_t offset) noexcept {
    const auto sum = static_cast<std::uint64_t>(base) + offset;
    return sum < base ? 0 : static_cast<std::uintptr_t>(sum);
}

bool ReadBytes(
    const std::uintptr_t address, void* const destination, const std::size_t size) noexcept {
    if (g_context.core == nullptr || address == 0 || destination == nullptr || size == 0) {
        return false;
    }
    const AnomalyMutableByteSpanV1 output{static_cast<std::uint8_t*>(destination), size};
    return g_context.core->read_memory(g_context.core->user, address, output).code ==
        ANOMALY_STATUS_V1_OK;
}

std::uintptr_t ReadPointer(const std::uintptr_t base, const std::uint32_t offset) noexcept {
    std::uintptr_t value{};
    const std::uintptr_t address = AddOffset(base, offset);
    if (address == 0 || !ReadBytes(address, &value, sizeof(value))) return 0;
    return value;
}

bool WriteBytes(
    const std::uintptr_t address, const void* const source, const std::size_t size) noexcept {
    if (g_context.core == nullptr || address == 0 || source == nullptr || size == 0) {
        return false;
    }
    const AnomalyByteSpanV1 input{static_cast<const std::uint8_t*>(source), size};
    return g_context.core->write_memory(g_context.core->user, address, input).code ==
        ANOMALY_STATUS_V1_OK;
}

bool ResolveGWorld() noexcept {
    if (g_context.g_world_address != 0) return true;
    if (!SignaturesReady(g_context.signatures)) return false;
    std::uintptr_t instruction{};
    const AnomalyStatusV1 status = g_context.signatures->resolve(
        g_context.signatures->user, anomaly::sdk::StringView("HTGame.exe"),
        anomaly::sdk::StringView(".text"),
        anomaly::sdk::StringView(profile::kGWorldPattern), &instruction);
    if (status.code != ANOMALY_STATUS_V1_OK || instruction == 0) return false;
    std::int32_t displacement{};
    if (!ReadBytes(instruction + profile::kGWorldResolveOffset, &displacement,
                   sizeof(displacement))) {
        return false;
    }
    const auto resolved = static_cast<std::intptr_t>(instruction) +
        profile::kGWorldInstructionSize + displacement;
    if (resolved <= 0) return false;
    g_context.g_world_address = static_cast<std::uintptr_t>(resolved);
    return true;
}

// Why the local component could not be resolved, so the UI can tell "not in the world yet"
// apart from "this pawn has nothing to scale".
enum class ComponentStatus { resolved, no_pawn, no_component };

struct LocalComponent {
    std::uintptr_t component{};
    ComponentStatus status{ComponentStatus::no_pawn};
};

// UWorld -> GameInstance -> LocalPlayers[0] -> LocalPlayer -> Controller -> Pawn -> Mesh.
//
// The skeletal mesh is the target: it is the visible model, and its own RelativeScale3D is
// not touched by the game's gameplay-scale path, which drives the capsule instead. Writing
// the capsule was measured to be reverted by the game within a frame.
//
// Game thread only: the head of the chain is written by the game.
LocalComponent ResolveLocalComponent() noexcept {
    std::uintptr_t world{};
    if (!ResolveGWorld() || !ReadBytes(g_context.g_world_address, &world, sizeof(world)) ||
        world == 0) {
        return {};
    }
    const std::uintptr_t game_instance = ReadPointer(world, profile::kWorldGameInstanceOffset);
    if (game_instance == 0) return {};
    const std::uintptr_t local_players =
        ReadPointer(game_instance, profile::kGameInstanceLocalPlayersOffset);
    if (local_players == 0) return {};
    std::uintptr_t local_player{};
    if (!ReadBytes(local_players, &local_player, sizeof(local_player)) ||
        local_player == 0) {
        return {};
    }
    const std::uintptr_t controller =
        ReadPointer(local_player, profile::kLocalPlayerControllerOffset);
    if (controller == 0) return {};
    const std::uintptr_t character = ReadPointer(controller, profile::kControllerPawnOffset);
    if (character == 0) return {};

    const std::uintptr_t mesh = ReadPointer(character, profile::kCharacterMeshOffset);
    if (mesh == 0) return {0, ComponentStatus::no_component};
    return {mesh, ComponentStatus::resolved};
}

bool ReadScale(const std::uintptr_t component, model_scale::ScaleVector& scale) noexcept {
    return ReadBytes(AddOffset(component, profile::kSceneComponentRelativeScale3D),
                     scale.data(), profile::kVectorSize);
}

bool WriteScale(
    const std::uintptr_t component, const model_scale::ScaleVector& scale) noexcept {
    return WriteBytes(AddOffset(component, profile::kSceneComponentRelativeScale3D),
                      scale.data(), profile::kVectorSize);
}

// Clear the bit the engine's own SetRelativeScale3D clears after it writes the vector.
// Without this the cached component-to-world transform stays in place and the new scale
// never reaches the mesh that is attached underneath.
bool MarkTransformDirty(const std::uintptr_t component) noexcept {
    const std::uintptr_t address =
        AddOffset(component, profile::kSceneComponentToWorldUpdated);
    std::uint8_t flags{};
    if (!ReadBytes(address, &flags, sizeof(flags))) return false;
    flags = static_cast<std::uint8_t>(
        flags & ~static_cast<std::uint8_t>(1U << profile::kSceneComponentToWorldUpdatedBit));
    return WriteBytes(address, &flags, sizeof(flags));
}

// Game thread only. The Render thread reads the published word, so the status never
// crosses the boundary as shared text.
void SetDetail(const DetailCode code) noexcept {
    g_context.publish_detail.store(static_cast<unsigned>(code), std::memory_order_release);
}

const char* DetailKey(const DetailCode code) noexcept {
    switch (code) {
        case DetailCode::resized: return "detail.resized";
        case DetailCode::applied: return "detail.applied";
        case DetailCode::restored: return "detail.restored";
        case DetailCode::restore_failed: return "detail.restore_failed";
        case DetailCode::no_pawn: return "detail.no_pawn";
        case DetailCode::no_component: return "detail.no_component";
        case DetailCode::unreadable: return "detail.unreadable";
        case DetailCode::unwritable: return "detail.unwritable";
        case DetailCode::not_markable: return "detail.not_markable";
        case DetailCode::idle: break;
    }
    return "status.idle";
}

void ForgetApplied() noexcept {
    g_context.applied = AppliedState{};
    g_context.publish_applied.store(false, std::memory_order_release);
}

// Write the scale and then clear the cached-transform bit, which is the pair of writes the
// engine's own SetRelativeScale3D performs. Doing it by hand keeps the plugin on the same
// path the game uses for its own gameplay scale, without needing a reflected call.
//
// The decision of what to write belongs to model_scale::PlanScale; this function only
// performs the reads and writes that plan asks for.
bool ApplyFactor(const std::uintptr_t component, const double factor) noexcept {
    AppliedState& applied = g_context.applied;
    model_scale::TargetScaleState prepared = applied.scale;

    model_scale::ScaleVector live{1.0, 1.0, 1.0};
    if (!ReadScale(component, live)) {
        SetDetail(DetailCode::unreadable);
        return false;
    }

    // The base is measured only when there is no remembered one, and only from a component
    // the plugin has never written to. Re-measuring it on every reload is what produced the
    // "stuck at 10x" bug: the component still carried the plugin's own last write, the
    // plugin could not tell that value apart from the game's scale, adopted it as the base,
    // and from then on multiplied every factor by it -- so setting the factor back to 1.0
    // restored 10x instead of the original size. The measured base is persisted, so a reload
    // restores the original scale instead of inheriting the inflated one.
    if (!g_context.saved_base_known.load(std::memory_order_acquire)) {
        if (applied.component != component) {
            g_context.saved_base.store(live[0], std::memory_order_release);
            g_context.saved_base_known.store(true, std::memory_order_release);
            g_context.base_measured.store(true, std::memory_order_release);
        }
    }
    const double base = g_context.saved_base.load(std::memory_order_acquire);
    prepared.base = model_scale::UniformScale(base);

    const model_scale::ScalePlan plan =
        model_scale::PlanScale(prepared, live, factor, profile::kScaleFactorEpsilon);

    // A resync means the component no longer carries what the plugin last wrote, so the
    // recorded output is dropped. The base is deliberately kept: it is the component's
    // original scale, which does not change because something else moved the value.
    if (plan.resynced) {
        prepared.valid = false;
    }

    if (plan.write) {
        if (!WriteScale(component, plan.scale)) {
            SetDetail(DetailCode::unwritable);
            return false;
        }
        // The vector is on the component now; the dirty bit is what makes the engine act
        // on it. If clearing it fails the resize would silently do nothing, so report it
        // rather than claiming success.
        if (!MarkTransformDirty(component)) {
            SetDetail(DetailCode::not_markable);
            return false;
        }
        prepared.applied = plan.scale;
        prepared.valid = true;
    }

    applied.component = component;
    applied.scale = prepared;
    SetDetail(plan.write ? DetailCode::resized : DetailCode::applied);
    return true;
}

// Put the component back on the base scale it had before the factor was applied.
bool ReleaseScale() noexcept {
    AppliedState& applied = g_context.applied;
    const std::uintptr_t component = applied.component;
    if (!applied.scale.valid || component == 0) {
        ForgetApplied();
        return true;
    }
    const bool okay =
        WriteScale(component, applied.scale.base) && MarkTransformDirty(component);
    ForgetApplied();
    SetDetail(okay ? DetailCode::restored : DetailCode::restore_failed);
    return okay;
}

void PersistSettings() noexcept;

// Game thread, once per processed frame through the AHUD draw endpoint.
void ApplyPendingScale() noexcept {
    const bool enabled = g_context.enabled.load(std::memory_order_acquire);
    const double requested_factor = g_context.factor.load(std::memory_order_acquire);
    const bool requested = g_context.sync_requested.exchange(false, std::memory_order_acq_rel);

    if (!enabled) {
        if (requested || g_context.applied.scale.valid) {
            static_cast<void>(ReleaseScale());
        }
        return;
    }

    const LocalComponent local = ResolveLocalComponent();
    if (local.status != ComponentStatus::resolved) {
        SetDetail(local.status == ComponentStatus::no_pawn ? DetailCode::no_pawn
                                                           : DetailCode::no_component);
        ForgetApplied();
        return;
    }

    // A base measured on this pass has to reach configuration before the next reload drops
    // it, or the plugin would measure the component again and adopt its own output.
    if (g_context.base_measured.exchange(false, std::memory_order_acq_rel)) {
        PersistSettings();
    }

    const double factor = requested_factor < profile::kMinimumScaleFactor
        ? profile::kMinimumScaleFactor
        : (requested_factor > profile::kMaximumScaleFactor
               ? profile::kMaximumScaleFactor
               : requested_factor);

    // Reinforced every frame, not only on change: the character, its components and its
    // animation state are all replaced on a world change, and a single write would
    // otherwise be lost while the UI still reports the factor as active.
    static_cast<void>(ApplyFactor(local.component, factor));

    g_context.publish_applied.store(
        g_context.applied.scale.valid, std::memory_order_release);
    if (g_context.applied.scale.valid) {
        g_context.publish_base_scale.store(
            g_context.applied.scale.base[0], std::memory_order_release);
        g_context.publish_effective_factor.store(factor, std::memory_order_release);
    }
}

void ANOMALY_CALL OnAhudDraw(void* /*user*/, const AnomalyUe5AhudFrameV1* /*frame*/) {
    try {
        ApplyPendingScale();
    } catch (...) {
        // Never let a plugin fault escape into the host's frame.
    }
}

std::string Text(const std::string_view key, const std::string_view fallback) {
    return g_context.localizer.Text(key, fallback);
}

std::string Line(const std::string_view key, const std::string_view fallback,
                 const double value) {
    const std::string pattern = Text(key, fallback);
    char buffer[192]{};
    std::snprintf(buffer, sizeof(buffer), pattern.c_str(), value);
    return buffer;
}

void Draw(const AnomalyUiServiceV1* ui) {
    const std::string title = Text("window.title", "Model Scale");
    int open = 1;
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    int enabled = g_context.enabled.load(std::memory_order_acquire) ? 1 : 0;
    const std::string enable_label = Text("setting.enabled", "Enable");
    if (ui->checkbox(ui->user, anomaly::sdk::StringView(enable_label), &enabled) != 0) {
        g_context.enabled.store(enabled != 0, std::memory_order_release);
        g_context.sync_requested.store(true, std::memory_order_release);
    }

    // input_double takes no range, so the factor is clamped here before it is stored: the
    // planner multiplies it into the component's scale, and a zero or negative factor would
    // collapse the transform.
    double factor = g_context.factor.load(std::memory_order_acquire);
    const std::string factor_label = Text("setting.factor", "Model scale");
    if (ui->input_double(ui->user, anomaly::sdk::StringView(factor_label), &factor,
                         0.1, 1.0) != 0) {
        factor = model_scale::ClampFactor(
            factor, profile::kMinimumScaleFactor, profile::kMaximumScaleFactor);
        g_context.factor.store(factor, std::memory_order_release);
        g_context.sync_requested.store(true, std::memory_order_release);
    }

    const std::string reset_label = Text("action.reset", "Reset to 1.0x");
    if (ui->button(ui->user, anomaly::sdk::StringView(reset_label), 0.0F, 0.0F) != 0) {
        g_context.factor.store(1.0, std::memory_order_release);
        g_context.sync_requested.store(true, std::memory_order_release);
    }

    ui->separator(ui->user);

    const char* status_key = "status.idle";
    if (!g_context.enabled.load(std::memory_order_acquire)) {
        status_key = "status.disabled";
    } else if (g_context.publish_applied.load(std::memory_order_acquire)) {
        status_key = "status.applied";
    } else {
        status_key = "status.pending";
    }
    const auto detail = static_cast<DetailCode>(
        g_context.publish_detail.load(std::memory_order_acquire));
    const std::string status_line =
        Text(status_key, "") + ": " + Text(DetailKey(detail), "");
    ui->text(ui->user, anomaly::sdk::StringView(status_line));

    const double base = g_context.publish_base_scale.load(std::memory_order_acquire);
    const double effective = g_context.publish_effective_factor.load(std::memory_order_acquire);
    ui->text(ui->user, anomaly::sdk::StringView(Line(
        "status.base_scale", "Component base scale: %.4g", base)));
    ui->text(ui->user, anomaly::sdk::StringView(Line(
        "status.effective_scale", "Effective component scale: %.4g", base * effective)));
}

void PersistSettings() noexcept {
    if (!ConfigReady(g_context.config) || g_context.config_schema.id == 0) return;
    // `%g` rather than a fixed number of decimals: the factor now spans the whole float
    // range, and a fixed-width format would need 40 characters for a legal value.
    char document[256]{};
    const int written = std::snprintf(
        document, sizeof(document), "{\"enabled\":%s,\"scale\":%g,\"base\":%g}",
        g_context.enabled.load(std::memory_order_acquire) ? "true" : "false",
        g_context.factor.load(std::memory_order_acquire),
        g_context.saved_base.load(std::memory_order_acquire));
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(document)) return;
    const AnomalyByteSpanV1 payload{
        reinterpret_cast<const std::uint8_t*>(document), static_cast<std::size_t>(written)};
    static_cast<void>(g_context.config->write_atomic(
        g_context.config->user, anomaly::sdk::StringView(kConfigSchemaId),
        kConfigSchemaVersion, payload));
}

void LoadSettings() noexcept {
    if (!ConfigReady(g_context.config) || g_context.config_schema.id == 0) return;
    std::array<char, 256> document{};
    std::size_t size = document.size();
    std::uint32_t version{};
    AnomalyMutableByteSpanV1 destination{
        reinterpret_cast<std::uint8_t*>(document.data()), size};
    const AnomalyStatusV1 status = g_context.config->read(
        g_context.config->user, anomaly::sdk::StringView(kConfigSchemaId), &version,
        destination, &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size == 0 || size > document.size()) return;
    const std::string_view text(document.data(), size);

    const auto enabled_at = text.find("\"enabled\"");
    if (enabled_at != std::string_view::npos) {
        const auto true_at = text.find("true", enabled_at);
        const auto false_at = text.find("false", enabled_at);
        if (true_at != std::string_view::npos &&
            (false_at == std::string_view::npos || true_at < false_at)) {
            g_context.enabled.store(true, std::memory_order_release);
        }
    }
    // The remembered base, so a reload applies the factor to the original scale instead of
    // to the component's current value.
    //
    // A save without a base was written before the base was remembered. Such a save cannot be
    // trusted as a pair: the version that wrote it could not tell the component's original
    // scale from its own previous output, so the character may be sitting at an inflated size
    // that "reset to 1.0x" could not undo, and the stored factor multiplies that inflation.
    // The original size of a character mesh is its unscaled size, so a repaired save starts
    // from a factor of 1.0 and leaves the character at its original size.
    bool base_remembered = false;
    double parsed_base = 1.0;
    const auto base_at = text.find("\"base\"");
    if (base_at != std::string_view::npos) {
        const auto colon = text.find(':', base_at);
        if (colon != std::string_view::npos) {
            const std::string value(text.substr(colon + 1U, 32U));
            const double parsed = std::strtod(value.c_str(), nullptr);
            if (model_scale::FactorInRange(
                    parsed, profile::kMinimumScaleFactor, profile::kMaximumScaleFactor)) {
                parsed_base = parsed;
                base_remembered = true;
            }
        }
    }
    g_context.saved_base.store(parsed_base, std::memory_order_release);
    g_context.saved_base_known.store(true, std::memory_order_release);

    if (base_remembered) {
        const auto scale_at = text.find("\"scale\"");
        if (scale_at != std::string_view::npos) {
            const auto colon = text.find(':', scale_at);
            if (colon != std::string_view::npos) {
                const std::string value(text.substr(colon + 1U, 32U));
                const double parsed = std::strtod(value.c_str(), nullptr);
                if (model_scale::FactorInRange(
                        parsed, profile::kMinimumScaleFactor, profile::kMaximumScaleFactor)) {
                    g_context.factor.store(parsed, std::memory_order_release);
                }
            }
        }
    }

    g_context.sync_requested.store(true, std::memory_order_release);
}

void ResetContext() noexcept {
    // Assigned while nothing is subscribed, so the plugin's own state is the only
    // thing being replaced. Atomics are not copy-assignable, hence the explicit reload.
    g_context.host = nullptr;
    g_context.core = nullptr;
    g_context.ui = nullptr;
    g_context.signatures = nullptr;
    g_context.ahud = nullptr;
    g_context.config = nullptr;
    g_context.localizer = anomaly::plugins::Localizer{};
    g_context.g_world_address = 0;
    g_context.ahud_subscription = {};
    g_context.config_schema = {};
    g_context.enabled.store(false, std::memory_order_relaxed);
    g_context.factor.store(1.0, std::memory_order_relaxed);
    g_context.sync_requested.store(false, std::memory_order_relaxed);
    // Cleared here and restored by LoadSettings; a load that finds nothing stored measures
    // the base from the component on the first pass.
    g_context.saved_base.store(1.0, std::memory_order_relaxed);
    g_context.saved_base_known.store(false, std::memory_order_relaxed);
    g_context.base_measured.store(false, std::memory_order_relaxed);
    g_context.publish_applied.store(false, std::memory_order_relaxed);
    g_context.publish_base_scale.store(1.0, std::memory_order_relaxed);
    g_context.publish_effective_factor.store(1.0, std::memory_order_relaxed);
    g_context.publish_detail.store(static_cast<unsigned>(DetailCode::idle),
                                   std::memory_order_relaxed);

    ForgetApplied();
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr || host == nullptr) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    const auto sdk_host = anomaly::sdk::Host(host);
    const auto core = sdk_host.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    if (!core) return StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE);
    const auto signatures = sdk_host.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    if (!signatures) return StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE);
    const auto ui = sdk_host.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui) return StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE);
    const auto ahud = sdk_host.Query<AnomalyUe5AhudServiceV1>(
        ANOMALY_UE5_AHUD_SERVICE_V1_ID, ANOMALY_UE5_AHUD_SERVICE_V1_VERSION);
    if (!ahud) return StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE);

    ResetContext();
    g_context.host = host;
    g_context.core = core.get();
    g_context.signatures = signatures.get();
    g_context.ui = ui.get();
    g_context.ahud = ahud.get();
    g_context.config = sdk_host.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION).get();
    g_context.localizer = anomaly::plugins::Localizer(host);

    if (!CoreReady(g_context.core) || !SignaturesReady(g_context.signatures) ||
        !HasUiFrame(g_context.ui) || !AhudReady(g_context.ahud)) {
        ResetContext();
        return StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    if (!ConfigReady(g_context.config)) g_context.config = nullptr;

    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    auto* state = static_cast<Context*>(context);
    if (state == nullptr || state != &g_context) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    ForgetApplied();
    state->sync_requested.store(true, std::memory_order_release);

    if (ConfigReady(state->config)) {
        AnomalyGenerationHandleV1 schema{};
        const AnomalyByteSpanV1 document{
            reinterpret_cast<const std::uint8_t*>(kConfigSchemaJson.data()),
            kConfigSchemaJson.size()};
        const AnomalyStatusV1 status = state->config->register_schema(
            state->config->user, anomaly::sdk::StringView(kConfigSchemaId),
            kConfigSchemaVersion, document, &schema);
        if (status.code == ANOMALY_STATUS_V1_OK) state->config_schema = schema;
    }
    LoadSettings();

    AnomalyGenerationHandleV1 subscription{};
    const AnomalyStatusV1 status = state->ahud->subscribe(
        state->ahud->user, OnAhudDraw, nullptr, &subscription);
    if (status.code != ANOMALY_STATUS_V1_OK || subscription.id == 0) {
        return StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    state->ahud_subscription = subscription;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t /*deadline_milliseconds*/) {
    auto* state = static_cast<Context*>(context);
    if (state == nullptr || state != &g_context) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    PersistSettings();
    if (state->ahud_subscription.id != 0 && AhudReady(state->ahud)) {
        // A successful unsubscribe drains a callback already in flight.
        const AnomalyGenerationHandleV1 handle = state->ahud_subscription;
        state->ahud_subscription = {};
        static_cast<void>(state->ahud->unsubscribe(state->ahud->user, handle));
    }
    // The component keeps the scale that was last written. The next Start re-derives the
    // base from the live value, so an applied factor is not a leak.
    ForgetApplied();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* context) {
    auto* state = static_cast<Context*>(context);
    if (state == nullptr || state != &g_context) return;
    if (state->config_schema.id != 0 && ConfigReady(state->config)) {
        static_cast<void>(state->config->unregister_schema(
            state->config->user, state->config_schema));
    }
    ResetContext();
}

void ANOMALY_CALL Update(void* /*context*/, double /*delta_seconds*/) {}

void ANOMALY_CALL OnDraw(void* /*context*/, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    if (!HasUiFrame(ui)) return;
    Draw(ui);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.model-scale"),
        anomaly::sdk::StringView("Model Scale"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, OnDraw};
    return anomaly::sdk::Ok();
}
