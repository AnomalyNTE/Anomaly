//
// Jelly — 果冻插件.
//
// Squashes and stretches the visible models in view with a volume-preserving
// three-axis scale. The plugin owns three modules:
//
//   * jelly_effect.hpp    效果计算      — the squash/stretch math (pure)
//   * jelly_selection.hpp 目标筛选      — camera, frustum, categories, cadence (pure)
//   * jelly_model.hpp/.cpp UE 模型访问  — registry walk, reflection, identity
//                                          validation and the recovery records
//
// The thread split follows the host contract: on_update runs in the Game domain
// (the UE game thread while the NTE adapter is running) and does every object
// read, every reflected call and every restore; on_draw runs in the Render domain
// and only reads a published snapshot and writes user settings. Configuration and
// the bundled layout resource are touched in the Lifecycle domain only.

#include "anomaly/sdk/cpp.hpp"

#include "jelly_effect.hpp"
#include "jelly_layout.hpp"
#include "jelly_model.hpp"
#include "jelly_selection.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using jelly::Skip;
using jelly::Vector3;

constexpr std::string_view kConfigSchemaId = "anomaly.plugin.jelly.settings";
constexpr std::uint32_t kConfigSchemaVersion = 1;
inline constexpr std::string_view kConfigSchemaJson =
    R"({"type":"object","properties":{"enabled":{"type":"boolean"},)"
    R"("amplitude":{"type":"number","minimum":0.0,"maximum":1.0},)"
    R"("frequency":{"type":"number","minimum":0.2,"maximum":3.0},)"
    R"("pause_seconds":{"type":"number","minimum":0.0,"maximum":5.0},)"
    R"("spin":{"type":"boolean"},)"
    R"("spin_speed":{"type":"number","minimum":0.0,"maximum":720.0},)"
    R"("max_models":{"type":"number","minimum":1,"maximum":128},)"
    R"("local_player":{"type":"boolean"},"characters":{"type":"boolean"},)"
    R"("other_dynamic":{"type":"boolean"},"simulated_vehicles":{"type":"boolean"}},)"
    R"("required":["enabled","amplitude","frequency","pause_seconds","spin",)"
    R"("spin_speed","max_models","local_player","characters","other_dynamic",)"
    R"("simulated_vehicles"],)"
    R"("additionalProperties":false})";

// The host walks its whole actor frame for every page call, so a page as large as it
// allows is what keeps a sweep of thousands of actors down to a few calls.
constexpr std::uint32_t kActorPageCapacity = ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY;
// A pattern scan is expensive, so a failure is retried on this interval instead of
// on every Game update.
constexpr double kSymbolRetrySeconds = 5.0;
// Refusals are counted per class and reason for the sweep log line: bounded so a world
// with hundreds of classes cannot grow the buffer without limit, and only the largest
// few rows are printed.
constexpr std::size_t kMaxRefusalRows = 64;
constexpr std::size_t kMaxRefusalRowsShown = 5;

// --- process adapters ------------------------------------------------------------

struct CoreMemory final : jelly::MemoryAccess {
    const AnomalyCoreServiceV1* core{};

    bool Read(
        const std::uintptr_t address, void* const destination,
        const std::size_t size) noexcept override {
        if (core == nullptr || address == 0 || destination == nullptr || size == 0) {
            return false;
        }
        const AnomalyMutableByteSpanV1 span{static_cast<std::uint8_t*>(destination), size};
        return core->read_memory(core->user, address, span).code == ANOMALY_STATUS_V1_OK;
    }
};

struct NameResolver final : jelly::NameTable {
    const AnomalyUe5NamesServiceV1* names{};

    bool Text(const std::uint32_t id, std::string& name) override {
        name.clear();
        if (names == nullptr || names->resolve_utf8 == nullptr || id == 0) return false;
        std::size_t size = 0;
        if (names->resolve_utf8(names->user, id, nullptr, &size).code != ANOMALY_STATUS_V1_OK ||
            size <= 1 || size > 1024) {
            return false;
        }
        name.assign(size, '\0');
        if (names->resolve_utf8(names->user, id, name.data(), &size).code !=
            ANOMALY_STATUS_V1_OK) {
            name.clear();
            return false;
        }
        if (const std::size_t end = name.find('\0'); end != std::string::npos) {
            name.resize(end);
        }
        return !name.empty();
    }
};

struct ProcessEventCall final : jelly::ReflectionCall {
    const AnomalyUe5FrameworkServiceV1* framework{};
    std::uintptr_t entry{};

    bool ProcessEvent(
        const std::uintptr_t object, const std::uintptr_t function, void* const parms,
        const std::size_t /*parms_size*/) noexcept override {
        if (entry == 0 || object == 0 || function == 0) return false;
        // The Game domain is the only place a UE call is legal; if the host cannot
        // confirm the thread, the call is not made at all.
        if (framework != nullptr && framework->is_game_thread != nullptr &&
            framework->is_game_thread(framework->user) == 0) {
            return false;
        }
        using Entry = void(__fastcall*)(void*, void*, void*);
        reinterpret_cast<Entry>(entry)(
            reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), parms);
        return true;
    }
};

// --- localization -----------------------------------------------------------------

class Localizer final {
public:
    void Bind(const AnomalyHostApiV1* host) {
        const auto queried = anomaly::sdk::Host(host).Query<AnomalyLocalizationServiceV1>(
            ANOMALY_LOCALIZATION_SERVICE_V1_ID, ANOMALY_LOCALIZATION_SERVICE_V1_VERSION);
        service_ = queried.get();
    }

    [[nodiscard]] std::string Text(
        const std::string_view key, const std::string_view fallback) const {
        return Format(key, fallback, {});
    }

    [[nodiscard]] std::string Format(
        const std::string_view key, const std::string_view fallback,
        const std::string_view argument) const {
        if (service_ == nullptr || service_->translate == nullptr) {
            return Substitute(fallback, argument);
        }
        const AnomalyStringViewV1 arguments[1]{anomaly::sdk::StringView(argument)};
        char buffer[512]{};
        std::size_t size = sizeof(buffer);
        const AnomalyStatusV1 status = service_->translate(
            service_->user, anomaly::sdk::StringView(key), anomaly::sdk::StringView(fallback),
            argument.empty() ? nullptr : arguments, argument.empty() ? 0U : 1U, buffer, &size);
        if (status.code != ANOMALY_STATUS_V1_OK || size == 0 || size > sizeof(buffer)) {
            return Substitute(fallback, argument);
        }
        return std::string(buffer, size - 1U);
    }

private:
    [[nodiscard]] static std::string Substitute(
        const std::string_view pattern, const std::string_view argument) {
        std::string result;
        result.reserve(pattern.size() + argument.size());
        for (std::size_t index = 0; index < pattern.size();) {
            if (index + 2U < pattern.size() && pattern[index] == '{' &&
                pattern[index + 1U] == '0' && pattern[index + 2U] == '}') {
                result.append(argument);
                index += 3U;
                continue;
            }
            result.push_back(pattern[index++]);
        }
        return result;
    }

    const AnomalyLocalizationServiceV1* service_{};
};

// --- published state --------------------------------------------------------------

// What the Game domain tells the window, and what the window tells the Game
// domain. Both directions cross the thread boundary as individual atomics, never
// as shared text or shared objects.
struct Published {
    enum class Status : unsigned { disabled = 0, running = 1, restoring = 2, unavailable = 3, waiting = 4 };
    enum class Detail : unsigned {
        none = 0,
        layout_resource,
        symbol_missing,
        // The object registry resolved but the call entry did not, which is a
        // different failure from the entry being there and refused below.
        symbol_scan,
        call_entry,
        game_services,
        // The allocation stage is fine but the game's own actor frame is not: the
        // host gate is closed, the frame is stale, or the page read failed.
        game_frame,
    };

    // What the last selection attempt saw. The window turns this into one line; the
    // numbers behind it say which step of the funnel dropped everything.
    enum class Frame : unsigned {
        none = 0,        // no attempt yet
        usable,
        partial,         // usable, but the host could not read every actor
        stale,
        unavailable,     // the host refused the frame; `frame_status` carries the code
    };

    std::atomic<bool> enabled{false};
    std::atomic<double> amplitude{0.25};
    std::atomic<double> frequency{1.5};
    // The rest between two bounces, and the spin the window can switch on.
    std::atomic<double> pause_seconds{0.0};
    std::atomic<bool> spin{false};
    std::atomic<double> spin_speed{120.0};
    // How many models the effect may own at once. The window's slider is the only
    // writer; everything else reads it through Settings.
    std::atomic<double> max_models{static_cast<double>(jelly::kDefaultModels)};
    std::atomic<bool> local_player{true};
    std::atomic<bool> characters{true};
    std::atomic<bool> other_dynamic{true};
    // Whether a vehicle whose body is the chassis rigid body may be scaled as a whole.
    std::atomic<bool> simulated_vehicles{true};
    std::atomic<bool> restore_all{false};

    std::atomic<unsigned> status{static_cast<unsigned>(Status::disabled)};
    std::atomic<unsigned> detail{static_cast<unsigned>(Detail::none)};
    std::atomic<unsigned> effective{0};
    std::atomic<unsigned> skipped{0};
    std::atomic<unsigned> conflicts{0};
    std::atomic<unsigned> pending{0};
    std::atomic<unsigned> last_skip{0};

    std::atomic<unsigned> frame_state{static_cast<unsigned>(Frame::none)};
    std::atomic<unsigned> frame_status{0};
    std::atomic<unsigned> frame_entities{0};
    std::atomic<unsigned> page_total{0};
    std::atomic<unsigned> page_returned{0};
    std::atomic<unsigned> in_view{0};
    std::atomic<unsigned> kept{0};
    // Models the effect actually owns, which is what the shortlist is resolved into.
    std::atomic<unsigned> models{0};
    std::atomic<unsigned> first_item_flags{0};
    std::atomic<unsigned> frame_restarts{0};
    std::atomic<bool> player_camera{false};
};

// --- context ----------------------------------------------------------------------

struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyUiServiceV1* ui{};
    const AnomalySignatureServiceV1* signatures{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyNteActorsServiceV1* actors{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyConfigServiceV1* config{};
    AnomalyGenerationHandleV1 config_schema{};
    Localizer localizer;
    Published published;

    CoreMemory memory;
    NameResolver name_table;
    ProcessEventCall call;

    // Lifecycle domain: the bundled resource is parsed before any game object is
    // touched, and never from the Game or Render domain.
    jelly::Layout layout;
    bool layout_ready{};
    std::string layout_error;

    // Game domain only.
    std::unique_ptr<jelly::ModelAccess> model;
    bool symbols_ready{};
    double last_symbol_attempt{-1.0};
    std::uintptr_t gobjects{};
    std::uintptr_t process_event{};
    jelly::RecoveryLedger ledger;
    std::vector<jelly::Candidate> candidates;
    std::vector<jelly::Candidate> sweep;
    // How far the resolution pass has walked the candidate list of this sweep, and the
    // refusals it produced, which is what the window and the log report.
    std::size_t candidate_cursor{};
    bool shortlist_walked{};
    // "class:reason" -> how many candidates of this sweep were refused that way.
    std::vector<std::pair<std::string, unsigned>> refusals;
    // The same for the vehicle family alone, and the vehicle models that were taken over
    // (named with the component they are scaled through). Kept apart so the general
    // top-five cut can never hide what happened to the cars.
    std::vector<std::pair<std::string, unsigned>> vehicle_refusals;
    std::vector<std::pair<std::string, unsigned>> vehicle_scaled;
    double clock{};
    double next_selection{};
    double next_effect{};
    bool sweep_running{};
    std::uint32_t sweep_offset{};
    std::uint64_t world_generation{};
    std::uint64_t world_change{};
    unsigned skipped{};
    unsigned conflicts{};
    Skip last_skip{Skip::none};
    // Which layer refused the two process-global entry points, so the window can
    // tell a signature that did not match from an entry that did not validate.
    Published::Detail symbol_failure{Published::Detail::call_entry};
    // The allocation stage is behind us; this says whether the game's own actor frame
    // is usable, so an empty candidate list is not reported as a healthy run.
    Published::Detail frame_detail{Published::Detail::none};
    // Funnel counters of the sweep in progress (or of the last completed one).
    unsigned frame_in_view{};
    unsigned frame_kept{};
    unsigned frame_restarts{};
    // The spin outcome of this sweep: models whose rotation the effect verified, and models
    // that stopped spinning (their rotation is driven by the game, or cannot be written).
    unsigned spin_applied{};
    unsigned spin_blocked{};
    // The last selection summary written to the host log, so only changes are logged.
    std::string selection_note;
};

Context g_context;

// The model module's naming notes go to the host log. The window can only show how
// many targets were refused; the log says which name this build does not carry, so a
// build whose reflection differs is diagnosable from one file.
struct CoreLogSink final : jelly::DiagnosticSink {
    void Note(const std::string_view message) noexcept override {
        const AnomalyCoreServiceV1* core = g_context.core;
        if (core == nullptr || core->log == nullptr) return;
        core->log(
            core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            anomaly::sdk::StringView(message));
    }
};

CoreLogSink g_model_log;

AnomalyStatusV1 StatusCode(const std::uint32_t code) noexcept {
    return {code, 0, {}};
}

template <typename Service>
bool ServiceReady(const Service* service, const std::uint32_t version) noexcept {
    constexpr std::size_t kPrefix = offsetof(Service, user) + sizeof(void*);
    return service != nullptr && service->struct_size >= kPrefix &&
        service->service_version >= version;
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

bool CoreReady(const AnomalyCoreServiceV1* service) noexcept {
    return ServiceReady(service, ANOMALY_CORE_SERVICE_V1_VERSION) &&
        service->read_memory != nullptr && service->plugin_directory != nullptr;
}

bool UiReady(const AnomalyUiServiceV1* service) noexcept {
    return ServiceReady(service, ANOMALY_UI_SERVICE_V1_VERSION) &&
        service->begin_window != nullptr && service->end_window != nullptr &&
        service->text != nullptr && service->checkbox != nullptr &&
        service->button != nullptr && service->separator != nullptr &&
        service->slider_float != nullptr;
}

bool ConfigReady(const AnomalyConfigServiceV1* service) noexcept {
    return ServiceReady(service, ANOMALY_CONFIG_SERVICE_V1_VERSION) &&
        service->register_schema != nullptr && service->unregister_schema != nullptr &&
        service->read != nullptr && service->write_atomic != nullptr;
}

bool ActorsReady(const AnomalyNteActorsServiceV1* service) noexcept {
    return ServiceReady(service, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION) &&
        service->frame != nullptr && service->page != nullptr &&
        service->class_name_utf8 != nullptr;
}

bool PlayerReady(const AnomalyNtePlayerServiceV1* service) noexcept {
    return ServiceReady(service, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION) &&
        service->camera_snapshot != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* service) noexcept {
    // Only the id-to-text direction is used: names are compared as text, because the
    // pool lookup (find_utf8, names service version 2) searches only the newest
    // blocks of the pool and misses names the engine registered long ago.
    return ServiceReady(service, 1U) && service->resolve_utf8 != nullptr;
}

bool FrameworkReady(const AnomalyUe5FrameworkServiceV1* service) noexcept {
    return ServiceReady(service, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION) &&
        service->is_game_thread != nullptr;
}

bool ObjectsReady(const AnomalyUe5ObjectsServiceV1* service) noexcept {
    return ServiceReady(service, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION) &&
        service->snapshot_by_handle != nullptr;
}

bool SignaturesReady(const AnomalySignatureServiceV1* service) noexcept {
    return ServiceReady(service, ANOMALY_SIGNATURE_SERVICE_V1_VERSION) &&
        service->resolve != nullptr;
}

// The snapshot policy lives in the tested module; these keep the mirror there honest
// against the SDK the plugin is actually built with.
static_assert(
    jelly::kSnapshotValid == ANOMALY_NTE_SNAPSHOT_V1_VALID, "snapshot VALID flag changed");
static_assert(
    jelly::kSnapshotStale == ANOMALY_NTE_SNAPSHOT_V1_STALE, "snapshot STALE flag changed");
static_assert(
    jelly::kSnapshotPartial == ANOMALY_NTE_SNAPSHOT_V1_PARTIAL, "snapshot PARTIAL flag changed");
static_assert(
    jelly::kEntityFlagStatic == ANOMALY_NTE_ENTITY_V1_STATIC, "entity STATIC flag changed");
static_assert(
    jelly::kEntityFlagStationary == ANOMALY_NTE_ENTITY_V1_STATIONARY,
    "entity STATIONARY flag changed");
static_assert(
    jelly::kEntityFlagMovable == ANOMALY_NTE_ENTITY_V1_MOVABLE, "entity MOVABLE flag changed");

// The page invariants themselves live in the tested module; this is only the SDK view
// of one page.
bool SanePage(
    const AnomalyNteEntityPageResultV1& result, const std::uint32_t offset,
    const std::size_t capacity) noexcept {
    return jelly::SanePage(
        jelly::PageResult{result.total_matches, result.returned, result.next_offset},
        offset, capacity);
}

jelly::Settings ReadSettings() noexcept {
    jelly::Settings settings;
    settings.enabled = g_context.published.enabled.load(std::memory_order_acquire);
    settings.amplitude =
        jelly::ClampAmplitude(g_context.published.amplitude.load(std::memory_order_acquire));
    settings.frequency_hz =
        jelly::ClampFrequency(g_context.published.frequency.load(std::memory_order_acquire));
    settings.pause_seconds =
        jelly::ClampPause(g_context.published.pause_seconds.load(std::memory_order_acquire));
    settings.spin = g_context.published.spin.load(std::memory_order_acquire);
    settings.spin_degrees_per_second = jelly::ClampSpin(
        g_context.published.spin_speed.load(std::memory_order_acquire));
    settings.max_models =
        jelly::ClampModels(g_context.published.max_models.load(std::memory_order_acquire));
    settings.local_player =
        g_context.published.local_player.load(std::memory_order_acquire);
    settings.characters = g_context.published.characters.load(std::memory_order_acquire);
    settings.other_dynamic =
        g_context.published.other_dynamic.load(std::memory_order_acquire);
    settings.simulated_vehicles =
        g_context.published.simulated_vehicles.load(std::memory_order_acquire);
    return settings;
}

void PublishStatus(const Published::Status status, const Published::Detail detail) noexcept {
    g_context.published.status.store(static_cast<unsigned>(status), std::memory_order_release);
    g_context.published.detail.store(static_cast<unsigned>(detail), std::memory_order_release);
}

void PublishCounters() noexcept {
    g_context.published.effective.store(
        static_cast<unsigned>(g_context.ledger.written_count()), std::memory_order_release);
    g_context.published.skipped.store(g_context.skipped, std::memory_order_release);
    g_context.published.conflicts.store(g_context.conflicts, std::memory_order_release);
    g_context.published.pending.store(
        static_cast<unsigned>(g_context.ledger.pending_restore()), std::memory_order_release);
    g_context.published.models.store(
        static_cast<unsigned>(g_context.ledger.model_count()), std::memory_order_release);
    g_context.published.last_skip.store(
        static_cast<unsigned>(g_context.last_skip), std::memory_order_release);
}

void PublishSelection() noexcept {
    g_context.published.in_view.store(g_context.frame_in_view, std::memory_order_release);
    g_context.published.kept.store(g_context.frame_kept, std::memory_order_release);
    g_context.published.frame_restarts.store(
        g_context.frame_restarts, std::memory_order_release);
}

// The frame's own state, so an empty candidate list can be told from a healthy run.
// PARTIAL is a state here, not a refusal: the host sets it when it could not read
// every actor or when the player-ESP feature is unavailable, neither of which makes
// the actor list unusable.
void PublishFrame(
    const AnomalyStatusV1 status, const AnomalyNteEntityFrameV1& frame) noexcept {
    Published::Frame state = Published::Frame::none;
    if (status.code != ANOMALY_STATUS_V1_OK) {
        state = Published::Frame::unavailable;
    } else if ((frame.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) != 0) {
        state = Published::Frame::stale;
    } else if ((frame.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) {
        state = Published::Frame::unavailable;
    } else if ((frame.flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) != 0) {
        state = Published::Frame::partial;
    } else {
        state = Published::Frame::usable;
    }
    g_context.published.frame_state.store(
        static_cast<unsigned>(state), std::memory_order_release);
    g_context.published.frame_status.store(status.code, std::memory_order_release);
    g_context.published.frame_entities.store(
        status.code == ANOMALY_STATUS_V1_OK ? frame.entity_count : 0U,
        std::memory_order_release);
}

// One line per change of what the selection stage sees. It follows the *state* and
// whether anything was kept, not the counts, so a run that finds nothing says so once
// instead of five times a second.
void NoteSelection() noexcept {
    const auto state = static_cast<Published::Frame>(
        g_context.published.frame_state.load(std::memory_order_acquire));
    const char* name = "none";
    switch (state) {
        case Published::Frame::usable: name = "usable"; break;
        case Published::Frame::partial: name = "partial"; break;
        case Published::Frame::stale: name = "stale"; break;
        case Published::Frame::unavailable: name = "unavailable"; break;
        case Published::Frame::none: break;
    }
    const unsigned status = g_context.published.frame_status.load(std::memory_order_acquire);
    const bool from_player = g_context.published.player_camera.load(std::memory_order_acquire);
    char key[96]{};
    std::snprintf(
        key, sizeof(key), "%s/%u/%s/%s", name, status, from_player ? "player" : "frame",
        g_context.frame_kept != 0 ? "kept" : "empty");
    if (g_context.selection_note == key) return;
    g_context.selection_note = key;

    const AnomalyCoreServiceV1* core = g_context.core;
    if (core == nullptr || core->log == nullptr) return;
    char line[256]{};
    std::snprintf(
        line, sizeof(line),
        "jelly: selection frame=%s status=%u entities=%u page=%u/%u frustum=%u candidates=%u "
        "camera=%s",
        name, status, g_context.published.frame_entities.load(std::memory_order_acquire),
        g_context.published.page_returned.load(std::memory_order_acquire),
        g_context.published.page_total.load(std::memory_order_acquire), g_context.frame_in_view,
        g_context.frame_kept, from_player ? "player" : "frame");
    core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, anomaly::sdk::StringView(line));
}

// --- layout resource ---------------------------------------------------------------

bool LoadLayoutResource(jelly::Layout& layout, std::string& error) {
    const AnomalyCoreServiceV1* core = g_context.core;
    if (core == nullptr || core->plugin_directory == nullptr) {
        error = "the core service is unavailable";
        return false;
    }
    std::size_t size = 0;
    if (core->plugin_directory(core->user, nullptr, &size).code != ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > 4096) {
        error = "the plugin directory is unavailable";
        return false;
    }
    std::string directory(size, '\0');
    if (core->plugin_directory(core->user, directory.data(), &size).code !=
        ANOMALY_STATUS_V1_OK) {
        error = "the plugin directory is unavailable";
        return false;
    }
    if (const std::size_t end = directory.find('\0'); end != std::string::npos) {
        directory.resize(end);
    }
    const std::string path = directory + "/data/jelly-layout.json";
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "the layout resource is missing";
        return false;
    }
    const std::string document{
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (document.empty()) {
        error = "the layout resource is empty";
        return false;
    }
    if (!jelly::ParseLayout(document, layout, error)) return false;
    const std::vector<std::string> problems = jelly::LayoutProblems(layout);
    if (!problems.empty()) {
        error = "the layout resource is incomplete: " + problems.front();
        return false;
    }
    return true;
}

// --- settings ----------------------------------------------------------------------

void PersistSettings() noexcept {
    const AnomalyConfigServiceV1* config = g_context.config;
    if (!ConfigReady(config) || g_context.config_schema.id == 0) return;
    char document[384]{};
    const int written = std::snprintf(
        document, sizeof(document),
        "{\"enabled\":%s,\"amplitude\":%.4f,\"frequency\":%.4f,"
        "\"pause_seconds\":%.4f,\"spin\":%s,\"spin_speed\":%.4f,"
        "\"max_models\":%u,\"local_player\":%s,\"characters\":%s,\"other_dynamic\":%s,"
        "\"simulated_vehicles\":%s}",
        g_context.published.enabled.load(std::memory_order_acquire) ? "true" : "false",
        jelly::ClampAmplitude(g_context.published.amplitude.load(std::memory_order_acquire)),
        jelly::ClampFrequency(g_context.published.frequency.load(std::memory_order_acquire)),
        jelly::ClampPause(g_context.published.pause_seconds.load(std::memory_order_acquire)),
        g_context.published.spin.load(std::memory_order_acquire) ? "true" : "false",
        jelly::ClampSpin(g_context.published.spin_speed.load(std::memory_order_acquire)),
        static_cast<unsigned>(jelly::ClampModels(
            g_context.published.max_models.load(std::memory_order_acquire))),
        g_context.published.local_player.load(std::memory_order_acquire) ? "true" : "false",
        g_context.published.characters.load(std::memory_order_acquire) ? "true" : "false",
        g_context.published.other_dynamic.load(std::memory_order_acquire) ? "true" : "false",
        g_context.published.simulated_vehicles.load(std::memory_order_acquire) ? "true"
                                                                              : "false");
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(document)) return;
    const AnomalyByteSpanV1 payload{
        reinterpret_cast<const std::uint8_t*>(document), static_cast<std::size_t>(written)};
    static_cast<void>(g_context.config->write_atomic(
        g_context.config->user, anomaly::sdk::StringView(kConfigSchemaId),
        kConfigSchemaVersion, payload));
}

bool ReadBoolean(const std::string_view document, const std::string_view key) noexcept {
    const std::size_t key_at = document.find(key);
    if (key_at == std::string_view::npos) return false;
    const std::size_t true_at = document.find("true", key_at);
    const std::size_t false_at = document.find("false", key_at);
    return true_at != std::string_view::npos &&
        (false_at == std::string_view::npos || true_at < false_at);
}

// A switch that is on by default: a document written before it existed has no such key,
// and reading that as "off" would silently change the behaviour of an upgrade.
bool ReadBooleanOr(
    const std::string_view document, const std::string_view key,
    const bool fallback) noexcept {
    if (document.find(key) == std::string_view::npos) return fallback;
    return ReadBoolean(document, key);
}

double ReadNumber(
    const std::string_view document, const std::string_view key,
    const double fallback) noexcept {
    const std::size_t key_at = document.find(key);
    if (key_at == std::string_view::npos) return fallback;
    const std::size_t colon = document.find(':', key_at);
    if (colon == std::string_view::npos) return fallback;
    const std::string value(document.substr(colon + 1U, 32U));
    return std::strtod(value.c_str(), nullptr);
}

void LoadSettings() noexcept {
    const AnomalyConfigServiceV1* config = g_context.config;
    if (!ConfigReady(config) || g_context.config_schema.id == 0) return;
    char document[384]{};
    std::size_t size = sizeof(document);
    std::uint32_t version{};
    const AnomalyMutableByteSpanV1 destination{
        reinterpret_cast<std::uint8_t*>(document), size};
    if (config->read(
            config->user, anomaly::sdk::StringView(kConfigSchemaId), &version, destination,
            &size)
            .code != ANOMALY_STATUS_V1_OK ||
        size == 0 || size > sizeof(document)) {
        return;
    }
    const std::string_view text(document, size);
    g_context.published.enabled.store(ReadBoolean(text, "\"enabled\""),
                                      std::memory_order_release);
    g_context.published.local_player.store(ReadBoolean(text, "\"local_player\""),
                                           std::memory_order_release);
    g_context.published.characters.store(ReadBoolean(text, "\"characters\""),
                                         std::memory_order_release);
    g_context.published.other_dynamic.store(ReadBoolean(text, "\"other_dynamic\""),
                                            std::memory_order_release);
    g_context.published.simulated_vehicles.store(
        ReadBooleanOr(text, "\"simulated_vehicles\"", true), std::memory_order_release);
    g_context.published.amplitude.store(
        jelly::ClampAmplitude(ReadNumber(text, "\"amplitude\"", 0.25)),
        std::memory_order_release);
    g_context.published.frequency.store(
        jelly::ClampFrequency(ReadNumber(text, "\"frequency\"", 1.5)),
        std::memory_order_release);
    g_context.published.pause_seconds.store(
        jelly::ClampPause(ReadNumber(text, "\"pause_seconds\"", 0.0)),
        std::memory_order_release);
    g_context.published.spin.store(
        ReadBooleanOr(text, "\"spin\"", false), std::memory_order_release);
    g_context.published.spin_speed.store(
        jelly::ClampSpin(ReadNumber(
            text, "\"spin_speed\"", jelly::kDefaultSpinDegreesPerSecond)),
        std::memory_order_release);
    // A document written before this setting existed has no such key, so the default
    // stands and the next save writes it.
    g_context.published.max_models.store(
        static_cast<double>(jelly::ClampModels(ReadNumber(
            text, "\"max_models\"", static_cast<double>(jelly::kDefaultModels)))),
        std::memory_order_release);
}

// --- Game domain -------------------------------------------------------------------

// Resolves the two process-global entry points through the Profile's signature
// service. The GObjects pattern is a RIP-relative load, so its displacement and
// the recorded addend are applied here exactly as the host applies them.
bool ResolveSymbols() {
    const AnomalySignatureServiceV1* signatures = g_context.signatures;
    g_context.symbol_failure = Published::Detail::call_entry;
    if (!SignaturesReady(signatures) || !g_context.layout_ready) return false;

    const auto resolve = [signatures](
                             const jelly::SymbolSpec& spec,
                             const std::string_view pattern, std::uintptr_t& out) {
        out = 0;
        std::uintptr_t instruction{};
        if (signatures->resolve(
                signatures->user, anomaly::sdk::StringView(spec.module),
                anomaly::sdk::StringView(spec.section),
                anomaly::sdk::StringView(pattern), &instruction)
                .code != ANOMALY_STATUS_V1_OK ||
            instruction == 0) {
            return false;
        }
        if (!spec.IsRipRelative()) {
            out = instruction;
            return true;
        }
        std::int32_t displacement{};
        if (!g_context.memory.Read(
                instruction + static_cast<std::uintptr_t>(spec.resolve_offset), &displacement,
                sizeof(displacement))) {
            return false;
        }
        const auto target = static_cast<std::intptr_t>(instruction) +
            spec.instruction_size + displacement + spec.addend;
        if (target <= 0) return false;
        out = static_cast<std::uintptr_t>(target);
        return true;
    };

    const auto gobjects = g_context.layout.symbols.find("gobjects");
    const auto process_event = g_context.layout.symbols.find("process_event");
    if (gobjects == g_context.layout.symbols.end() ||
        process_event == g_context.layout.symbols.end()) {
        return false;
    }
    if (!resolve(gobjects->second, gobjects->second.pattern, g_context.gobjects)) {
        return false;
    }
    // ProcessEvent first as the resource records it, which is what the active
    // Profile carries and what an unhooked process still shows at the entry. Once
    // Anomaly's own observer has replaced the first bytes with its detour the
    // recorded bytes are gone, so the entry is then found from the bytes that
    // survive it and sits that far in front of the match.
    const std::int64_t detour_bytes = process_event->second.detour_bytes;
    if (!resolve(
            process_event->second, process_event->second.pattern,
            g_context.process_event)) {
        std::string anchored;
        if (!jelly::DropLeadingBytes(process_event->second.pattern, detour_bytes, anchored) ||
            !resolve(process_event->second, anchored, g_context.process_event) ||
            g_context.process_event <= static_cast<std::uintptr_t>(detour_bytes)) {
            g_context.symbol_failure = Published::Detail::symbol_scan;
            g_context.process_event = 0;
            return false;
        }
        g_context.process_event -= static_cast<std::uintptr_t>(detour_bytes);
    }
    g_context.call.framework = g_context.framework;
    g_context.call.entry = g_context.process_event;
    auto model = std::make_unique<jelly::ModelAccess>(
        g_context.layout, g_context.memory, g_context.name_table, g_context.call,
        g_model_log);
    Skip skip = Skip::none;
    if (!model->Prepare(g_context.gobjects, g_context.process_event, skip)) {
        g_context.last_skip = skip;
        g_context.call.entry = 0;
        return false;
    }
    g_context.model = std::move(model);
    return true;
}

jelly::Camera CurrentCamera(const AnomalyNteEntityFrameV1& frame, bool& from_player) noexcept {
    from_player = false;
    jelly::Camera camera;
    camera.position = Vector3{frame.camera_position[0], frame.camera_position[1],
                             frame.camera_position[2]};
    camera.rotation = Vector3{frame.camera_rotation[0], frame.camera_rotation[1],
                             frame.camera_rotation[2]};
    camera.horizontal_fov_degrees = frame.horizontal_fov_degrees;
    if (PlayerReady(g_context.player)) {
        AnomalyNteCameraSnapshotV1 snapshot{sizeof(snapshot)};
        if (g_context.player->camera_snapshot(g_context.player->user, &snapshot).code ==
                ANOMALY_STATUS_V1_OK &&
            (snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0) {
            camera.position =
                Vector3{snapshot.position[0], snapshot.position[1], snapshot.position[2]};
            camera.rotation =
                Vector3{snapshot.rotation[0], snapshot.rotation[1], snapshot.rotation[2]};
            camera.horizontal_fov_degrees = snapshot.horizontal_fov_degrees;
            from_player = true;
        }
    }
    return camera;
}

// The text the actor frame's class id names, for the diagnostics that have to say
// *which* class a refusal was about. Whether that class is a character is a different
// question, and it is answered from the live class chain by the model module.
bool FrameClassName(const std::uint64_t class_id, std::string& name) {
    name.clear();
    if (!ActorsReady(g_context.actors) || class_id == 0) return false;
    std::size_t size = 0;
    if (g_context.actors->class_name_utf8(g_context.actors->user, class_id, nullptr, &size)
            .code != ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > 512) {
        return false;
    }
    name.assign(size, '\0');
    if (g_context.actors->class_name_utf8(
            g_context.actors->user, class_id, name.data(), &size)
            .code != ANOMALY_STATUS_V1_OK) {
        name.clear();
        return false;
    }
    if (const std::size_t end = name.find('\0'); end != std::string::npos) {
        name.resize(end);
    }
    return !name.empty();
}

void NoteSkip(const Skip skip) noexcept {
    g_context.last_skip = skip;
    if (g_context.skipped != 0xFFFFFFFFU) ++g_context.skipped;
}

// The Host owns the object registry and its generation counter, so it can confirm
// the identity the plugin derived from the snapshot before anything is written.
bool IdentityConfirmedByHost(const jelly::ActorIdentity& identity) noexcept {
    if (!ObjectsReady(g_context.objects) || g_context.objects->generation == nullptr) {
        // Without the service the plugin's own registry and serial checks remain,
        // which is why this is a corroboration and not the only gate.
        return true;
    }
    const AnomalyGenerationHandleV1 handle{
        identity.handle(), g_context.objects->generation(g_context.objects->user)};
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    return g_context.objects->snapshot_by_handle(
               g_context.objects->user, handle, &snapshot)
               .code == ANOMALY_STATUS_V1_OK;
}

// The world can be swapped underneath the plugin; every record then belongs to a
// scene that no longer exists, so it is put back before anything else happens.
void DetectWorldChange() {
    if (!ServiceReady(g_context.session, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION) ||
        g_context.session->snapshot == nullptr) {
        return;
    }
    AnomalyNteSessionSnapshotV1 snapshot{sizeof(snapshot)};
    if (g_context.session->snapshot(g_context.session->user, &snapshot).code !=
        ANOMALY_STATUS_V1_OK) {
        return;
    }
    const std::uint64_t world_generation = snapshot.world.generation;
    const bool changed =
        (world_generation != g_context.world_generation && g_context.world_generation != 0) ||
        (snapshot.sequence != g_context.world_change && g_context.world_change != 0);
    if (changed) {
        if (g_context.sweep_running) ++g_context.frame_restarts;
        // The candidates described the scene that just went away, so nothing from that
        // selection may be acted on again; the next sweep starts fresh. This is the one
        // restart the sweep still does, and it is counted.
        g_context.candidates.clear();
        g_context.sweep.clear();
        g_context.candidate_cursor = 0;
        g_context.shortlist_walked = false;
        g_context.refusals.clear();
        g_context.vehicle_refusals.clear();
        g_context.vehicle_scaled.clear();
        g_context.sweep_running = false;
        g_context.sweep_offset = 0;
        if (!g_context.ledger.empty()) static_cast<void>(g_context.ledger.MarkAllRestore());
    }
    g_context.world_generation = world_generation;
    g_context.world_change = snapshot.sequence;
}

void RestorePending(const std::chrono::steady_clock::time_point deadline) {
    const std::vector<std::uint64_t> keys = g_context.ledger.restore_order();
    for (const std::uint64_t key : keys) {
        if (std::chrono::steady_clock::now() >= deadline) return;
        jelly::RecoveryRecord* record = g_context.ledger.Find(key);
        if (record == nullptr) continue;
        Skip skip = Skip::none;
        switch (jelly::RestoreRecord(*g_context.model, *record, skip)) {
            case jelly::RestoreStep::restored:
            case jelly::RestoreStep::already_back:
                g_context.ledger.Erase(key);
                break;
            case jelly::RestoreStep::identity_lost:
                g_context.ledger.Erase(key);
                NoteSkip(skip);
                break;
            case jelly::RestoreStep::foreign_value:
                g_context.ledger.Erase(key);
                ++g_context.conflicts;
                NoteSkip(skip);
                break;
            case jelly::RestoreStep::failed:
                // The record stays pending: Stop then reports it instead of letting
                // the generation unload over a component that is still squashed.
                NoteSkip(skip);
                break;
        }
    }
}

// One row of a per-class-and-reason histogram, counted in place.
void Bump(
    std::vector<std::pair<std::string, unsigned>>& rows, std::string entry) {
    for (auto& row : rows) {
        if (row.first == entry) {
            ++row.second;
            return;
        }
    }
    if (rows.size() < kMaxRefusalRows) rows.emplace_back(std::move(entry), 1U);
}

// One refusal, counted by the class it was about and the reason it was refused for. The
// window can only show how many candidates came up empty; this is what names them. A
// vehicle-family refusal is counted a second time, on its own, because those rows are the
// ones the general top-five cut would otherwise hide.
void NoteRefusal(const std::uint64_t class_id, const Skip skip, const bool vehicle) {
    std::string name;
    if (!FrameClassName(class_id, name)) name = "?";
    std::string entry = name;
    entry.push_back(':');
    entry.append(SkipKey(skip));
    if (vehicle) Bump(g_context.vehicle_refusals, entry);
    Bump(g_context.refusals, std::move(entry));
}

// The component a vehicle was scaled through, read from the object's own name, so the
// sweep line shows that the whole car (its root) is the target rather than one of its
// parts.
bool ComponentName(const std::uintptr_t component, std::string& name) {
    name.clear();
    const std::int64_t offset = jelly::LayoutOffset(g_context.layout, "object.name_offset", -1);
    if (component == 0 || offset < 0) return false;
    std::uint32_t id{};
    const auto address = component + static_cast<std::uintptr_t>(offset);
    if (!g_context.memory.Read(address, &id, sizeof(id)) || id == 0) return false;
    return g_context.name_table.Text(id, name) && !name.empty();
}

// A vehicle-family model that was taken over, named with the component it is scaled
// through. Same reason as above: the general scaled rows are cut to five.
void NoteVehicleScaled(
    const std::uint64_t class_id, const std::uintptr_t component) {
    std::string entry;
    if (!FrameClassName(class_id, entry)) entry = "?";
    std::string name;
    if (ComponentName(component, name)) {
        entry.push_back(':');
        entry.append(name);
    }
    Bump(g_context.vehicle_scaled, std::move(entry));
}

// Which classes the model budget actually went to, largest first. The refusals below say
// what was left alone; this is the other half of the same question, and it is what tells
// a run that only finds the local player whether the cars are in that set or refused.
std::vector<std::pair<std::string, unsigned>> HeldClassRows() {
    std::vector<std::pair<std::string, unsigned>> rows;
    for (const std::uint64_t class_id : g_context.ledger.held_class_ids()) {
        std::string name;
        if (!FrameClassName(class_id, name)) name = "?";
        bool counted = false;
        for (auto& row : rows) {
            if (row.first == name) {
                ++row.second;
                counted = true;
                break;
            }
        }
        if (!counted && rows.size() < kMaxRefusalRows) {
            rows.emplace_back(std::move(name), 1U);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        return left.second > right.second;
    });
    return rows;
}

// One line per sweep that says which classes the model budget went to and what the
// resolution pass refused instead. Deliberately not noexcept: it assembles text, and an
// allocation failure has to reach the caller's catch clause rather than terminating the
// game thread.
void NoteSweepRefusals() {
    const AnomalyCoreServiceV1* core = g_context.core;
    if (core == nullptr || core->log == nullptr) return;
    std::vector<const std::pair<std::string, unsigned>*> rows;
    rows.reserve(g_context.refusals.size());
    unsigned refused = 0;
    for (const auto& row : g_context.refusals) {
        rows.push_back(&row);
        refused += row.second;
    }
    std::sort(rows.begin(), rows.end(), [](const auto* left, const auto* right) {
        return left->second > right->second;
    });
    std::vector<std::pair<std::string, unsigned>> ordered;
    ordered.reserve(rows.size());
    for (const auto* row : rows) ordered.push_back(*row);
    const std::vector<std::pair<std::string, unsigned>> held = HeldClassRows();
    char line[1024]{};
    std::size_t used = 0;
    const auto append = [&line, &used](const std::string_view text) {
        if (used + text.size() >= sizeof(line)) return;
        std::memcpy(line + used, text.data(), text.size());
        used += text.size();
        line[used] = '\0';
    };
    const auto append_rows =
        [&append](const std::vector<std::pair<std::string, unsigned>>& source) {
            const std::size_t shown = (std::min)(source.size(), kMaxRefusalRowsShown);
            for (std::size_t index = 0; index < shown; ++index) {
                if (index != 0) append(",");
                append(source[index].first);
                char count[24]{};
                std::snprintf(count, sizeof(count), "x%u", source[index].second);
                append(count);
            }
            if (shown == 0) append("none");
        };
    char head[192]{};
    std::snprintf(
        head, sizeof(head),
        "jelly: sweep inView=%u candidates=%u models=%u refused=%u scaled=",
        g_context.frame_in_view, g_context.frame_kept,
        static_cast<unsigned>(g_context.ledger.model_count()), refused);
    append(head);
    append_rows(held);
    append(" top=");
    append_rows(ordered);
    // The vehicle family on its own, untruncated by the top-five cut above: which cars
    // were taken over (and through which component) and which were refused, and why.
    append(" veh=");
    append_rows(g_context.vehicle_scaled);
    append(" vehref=");
    append_rows(g_context.vehicle_refusals);
    // How many models the spin actually reaches: a model whose rotation the game drives
    // itself stops spinning and shows up on the right of the slash.
    char spin[48]{};
    std::snprintf(
        spin, sizeof(spin), " spin=%u/%u", g_context.spin_applied, g_context.spin_blocked);
    append(spin);
    core->log(
        core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        anomaly::sdk::StringView(std::string_view(line, used)));
}

// The effect step of a model the plugin already owns. Resolution is a separate pass, so
// a model that is already moving keeps moving while newly arrived candidates are still
// being resolved.
void StepTarget(const jelly::Candidate& candidate, const jelly::Settings& settings) {
    jelly::RecoveryRecord* record = g_context.ledger.Find(candidate.frame_handle_id);
    if (record == nullptr || record->state != jelly::RecordState::controlling) return;
    Skip skip = Skip::none;
    switch (jelly::StepRecord(*g_context.model, *record, settings, g_context.clock, skip)) {
        case jelly::EffectStep::identity_lost:
            g_context.ledger.Erase(record->model_key);
            NoteSkip(skip);
            break;
        case jelly::EffectStep::foreign_value:
            // Another writer owns this component: the plugin stops controlling it,
            // reports the conflict, and leaves that value alone.
            g_context.ledger.Erase(record->model_key);
            ++g_context.conflicts;
            NoteSkip(skip);
            break;
        case jelly::EffectStep::failed:
            NoteSkip(skip);
            break;
        case jelly::EffectStep::updated:
        case jelly::EffectStep::unchanged:
            break;
    }
    // The spin is counted from the record as it stands now: a record that is gone (the
    // object was lost or another writer took it) is no longer a model that turns.
    if (!settings.spin) return;
    const jelly::RecoveryRecord* now =
        g_context.ledger.Find(candidate.frame_handle_id);
    if (now == nullptr || now->spin_blocked) {
        ++g_context.spin_blocked;
    } else if (now->rotation_written) {
        ++g_context.spin_applied;
    }
}

// Turns the next entries of the sweep's shortlist into models the effect owns. An entry
// is attempted once per sweep: what is not a model now is not retried on every update,
// and the next sweep asks again. The model cap is what the budget is spent on, so a
// hidden actor, a name plate or a building no longer takes a slot away from a character.
void ResolveCandidate(
    const jelly::Candidate& candidate, const std::size_t model_cap,
    const bool simulated_vehicles) {
    // Whether this entry is a car decides how its fate is counted, and it is the same
    // answer the model module gives, asked from the class id the frame carried.
    const bool vehicle =
        g_context.model != nullptr && g_context.model->ClassIsVehicleById(candidate.class_id);
    jelly::ActorIdentity identity;
    Skip skip = Skip::none;
    if (!g_context.model->ResolveActor(
            candidate.frame_handle_id, candidate.class_id, identity, skip)) {
        NoteSkip(skip);
        NoteRefusal(candidate.class_id, skip, vehicle);
        return;
    }
    // The Host's registry is the authority on which object a slot holds; if it does not
    // confirm the identity, the frame handle is stale and the model is left alone.
    if (!IdentityConfirmedByHost(identity)) {
        NoteSkip(Skip::identity_stale);
        NoteRefusal(candidate.class_id, Skip::identity_stale, vehicle);
        return;
    }
    jelly::VisualTarget target;
    if (!g_context.model->ResolveVisual(
            identity.actor, simulated_vehicles, target, skip)) {
        NoteSkip(skip);
        NoteRefusal(candidate.class_id, skip, vehicle);
        return;
    }
    jelly::RecoveryRecord fresh;
    fresh.model_key = candidate.frame_handle_id;
    fresh.class_id = candidate.class_id;
    fresh.identity = identity;
    fresh.component = target.component;
    fresh.base = target.base;
    fresh.applied = target.base;
    fresh.base_rotation = target.rotation;
    fresh.applied_rotation = target.rotation;
    fresh.rotation_valid = target.rotation_valid;
    fresh.phase = jelly::PhaseForIdentity(candidate.frame_handle_id);
    fresh.start_time = g_context.clock;
    fresh.state = jelly::RecordState::controlling;
    if (!g_context.ledger.Add(fresh, model_cap)) {
        NoteSkip(Skip::capacity);
        NoteRefusal(candidate.class_id, Skip::capacity, vehicle);
        return;
    }
    if (vehicle) NoteVehicleScaled(candidate.class_id, target.component);
}

void ResolveNextCandidates(
    const jelly::Settings& settings, const std::chrono::steady_clock::time_point deadline) {
    while (g_context.candidate_cursor < g_context.candidates.size() &&
           g_context.ledger.model_count() < settings.max_models) {
        if (std::chrono::steady_clock::now() >= deadline) return;  // deferred, cursor kept
        jelly::Candidate& candidate =
            g_context.candidates[g_context.candidate_cursor++];
        if (candidate.attempted) continue;
        candidate.attempted = true;
        if (!jelly::CategoryEnabled(candidate.category, settings)) continue;
        if (g_context.ledger.Find(candidate.frame_handle_id) != nullptr) continue;
        ResolveCandidate(candidate, settings.max_models, settings.simulated_vehicles);
    }
    if (g_context.shortlist_walked) return;
    g_context.shortlist_walked = true;
    NoteSweepRefusals();
}

void EffectPass(
    const jelly::Settings& settings, const std::chrono::steady_clock::time_point deadline) {
    for (const jelly::Candidate& candidate : g_context.candidates) {
        if (std::chrono::steady_clock::now() >= deadline) return;  // deferred to the next update
        if (!jelly::CategoryEnabled(candidate.category, settings)) continue;
        StepTarget(candidate, settings);
    }
    ResolveNextCandidates(settings, deadline);
}

void FinishSweep(const Vector3& camera_position);

// Reads one bounded page of the actor frame into the sweep in progress. Either the
// sweep completes (and becomes the candidate set) or it continues on a later pass.
void SelectionPass(
    const AnomalyNteEntityFrameV1& frame, const jelly::Settings& settings,
    const std::chrono::steady_clock::time_point deadline) {
    bool from_player = false;
    const jelly::Camera camera = CurrentCamera(frame, from_player);
    g_context.published.player_camera.store(from_player, std::memory_order_release);
    const jelly::CameraBasis basis = jelly::MakeCameraBasis(camera);

    std::size_t inspected = 0;
    while (inspected < jelly::kSelectionCandidateBudget) {
        // The sweep is also inside the update's soft budget: the rest of the frame
        // is read on a later pass.
        if (std::chrono::steady_clock::now() >= deadline) return;
        std::vector<AnomalyNteEntitySnapshotV1> items(kActorPageCapacity);
        for (auto& item : items) item = {sizeof(item)};
        AnomalyNteEntityPageResultV1 result{sizeof(result)};
        // No flag filter: the host's mobility bits are not usable (see
        // jelly_selection.hpp). The sweep instead continues across frame generations
        // and reads the frame in the largest pages the host allows, so a table of
        // thousands is still walked in a few passes; scope is decided per component
        // later, from the component's own reflected Mobility.
        const AnomalyNteEntityPageRequestV1 request{
            sizeof(request), 0, frame.generation, g_context.sweep_offset, kActorPageCapacity,
            0, 0, 0, 0, 0};
        const AnomalyStatusV1 status = g_context.actors->page(
            g_context.actors->user, &request, items.data(), &result);
        if (status.code != ANOMALY_STATUS_V1_OK) {
            // A generation change makes the saved offset meaningless; restart from
            // the first page on the next pass.
            if (g_context.sweep_running) ++g_context.frame_restarts;
            g_context.sweep_running = false;
            g_context.sweep.clear();
            g_context.sweep_offset = 0;
            g_context.frame_detail = Published::Detail::game_frame;
            PublishSelection();
            return;
        }
        if (result.generation != frame.generation ||
            !jelly::IsUsableSnapshot(result.flags) ||
            !SanePage(result, g_context.sweep_offset, items.size())) {
            if (g_context.sweep_running) ++g_context.frame_restarts;
            g_context.sweep_running = false;
            g_context.sweep.clear();
            g_context.sweep_offset = 0;
            g_context.frame_detail = Published::Detail::game_frame;
            PublishSelection();
            return;
        }
        g_context.published.page_total.store(result.total_matches, std::memory_order_release);
        g_context.published.page_returned.store(result.returned, std::memory_order_release);
        for (std::uint32_t index = 0; index < result.returned; ++index) {
            const AnomalyNteEntitySnapshotV1& item = items[index];
            ++inspected;
            if (index == 0) {
                g_context.published.first_item_flags.store(
                    item.flags, std::memory_order_release);
            }
            if (!jelly::IsUsableSnapshot(item.flags) ||
                item.handle.generation != frame.generation) {
                continue;
            }
            const jelly::Bounds bounds{
                Vector3{item.bounds_center[0], item.bounds_center[1], item.bounds_center[2]},
                Vector3{item.bounds_extent[0], item.bounds_extent[1], item.bounds_extent[2]}};
            if (jelly::Screen(basis, bounds) == jelly::ScreenResult::culled) continue;
            ++g_context.frame_in_view;
            // Whether this build calls the class a character is asked of the live class
            // chain (an engine `Character`, or a name that says NPC/Character): the
            // classes here are `player_076_shinku_C` and `BP_NPC_Mass_fm_Sub_C`, so the
            // class *name* never carries the word the window's switch means.
            const bool character =
                g_context.model != nullptr &&
                g_context.model->ClassIsCharacter(item.class_id);
            jelly::Candidate candidate;
            candidate.frame_handle_id = item.handle.id;
            candidate.class_id = item.class_id;
            candidate.entity_flags = item.flags;
            candidate.bounds = bounds;
            candidate.category = jelly::Categorize(item.flags, character);
            if (candidate.category == jelly::Category::unsupported) continue;
            // The sweep is a bounded shortlist of the nearest in-view models, not the
            // model set itself: only what the effect can actually scale may occupy a
            // model slot, and which entries those are is decided when they are resolved.
            if (!jelly::KeepCandidate(
                    g_context.sweep, candidate, camera.position,
                    jelly::ShortlistCapacity(settings.max_models))) {
                continue;
            }
            ++g_context.frame_kept;
        }

        g_context.sweep_offset = result.next_offset;
        if (result.next_offset >= result.total_matches) {
            FinishSweep(camera.position);
            PublishSelection();
            return;
        }
    }
    PublishSelection();
}

void FinishSweep(const Vector3& camera_position) {
    jelly::SortForUpdate(g_context.sweep, camera_position);
    // The list is deliberately not cut down to the model cap here. The cap is about
    // models, and which of these entries are models is only known once each one has been
    // resolved; cutting by distance first is what let three hidden party members, their
    // weapons and a handful of non-visual actors take every model slot.
    const jelly::Settings settings = ReadSettings();
    // Anything the plugin owns that is not a candidate any more leaves the effect
    // and is queued for restore: out of view, no longer dynamic, or its category
    // switched off.
    std::vector<std::uint64_t> still_wanted;
    still_wanted.reserve(g_context.sweep.size());
    for (const jelly::Candidate& candidate : g_context.sweep) {
        if (jelly::CategoryEnabled(candidate.category, settings)) {
            still_wanted.push_back(candidate.frame_handle_id);
        }
    }
    static_cast<void>(g_context.ledger.MarkLeavers(still_wanted));
    g_context.candidates = g_context.sweep;
    g_context.candidate_cursor = 0;
    g_context.shortlist_walked = false;
    g_context.refusals.clear();
    g_context.vehicle_refusals.clear();
    g_context.vehicle_scaled.clear();
    g_context.sweep.clear();
    g_context.sweep_running = false;
    g_context.sweep_offset = 0;
}

void UpdateGame(const double delta_seconds) {
    g_context.clock += delta_seconds;
    const jelly::Settings settings = ReadSettings();

    if (!g_context.layout_ready) {
        PublishStatus(Published::Status::unavailable, Published::Detail::layout_resource);
        PublishCounters();
        return;
    }
    if (!SignaturesReady(g_context.signatures) || !NamesReady(g_context.names) ||
        !ObjectsReady(g_context.objects) || !ActorsReady(g_context.actors) ||
        !PlayerReady(g_context.player)) {
        PublishStatus(
            g_context.ledger.empty() ? Published::Status::unavailable
                                     : Published::Status::restoring,
            Published::Detail::game_services);
        PublishCounters();
        return;
    }

    const bool restore_all = g_context.published.restore_all.exchange(
        false, std::memory_order_acq_rel);
    if (restore_all) {
        // 恢复全部: the effect stops first and every record is queued, so the
        // pending count is the only thing left to watch.
        g_context.published.enabled.store(false, std::memory_order_release);
        g_context.candidates.clear();
        g_context.sweep.clear();
        g_context.candidate_cursor = 0;
        g_context.shortlist_walked = false;
        g_context.refusals.clear();
        g_context.vehicle_refusals.clear();
        g_context.vehicle_scaled.clear();
        g_context.sweep_running = false;
        static_cast<void>(g_context.ledger.MarkAllRestore());
    }

    const bool enabled =
        g_context.published.enabled.load(std::memory_order_acquire) && !restore_all;
    if (!enabled && !g_context.ledger.empty()) {
        // Turning the effect off is the accepted way to hand the components back:
        // every record is queued here and the restore pass puts them back on the
        // following updates, which is what the window's pending count tracks.
        static_cast<void>(g_context.ledger.MarkAllRestore());
    }
    if (!enabled && g_context.ledger.empty()) {
        PublishStatus(Published::Status::disabled, Published::Detail::none);
        PublishCounters();
        return;
    }

    DetectWorldChange();
    if (!g_context.symbols_ready &&
        jelly::IsDue(g_context.clock, g_context.last_symbol_attempt, kSymbolRetrySeconds)) {
        g_context.last_symbol_attempt = g_context.clock;
        g_context.symbols_ready = ResolveSymbols();
    }
    if (!g_context.symbols_ready) {
        // Two different failures share this branch: the signature scan never
        // matched, or it matched and the entry/registry behind it did not validate.
        // The window reports which one, because only the second one is about the
        // process the plugin is looking at rather than about the resource.
        PublishStatus(
            g_context.ledger.empty() ? Published::Status::unavailable
                                     : Published::Status::restoring,
            g_context.gobjects == 0 ? Published::Detail::symbol_missing
                                    : g_context.symbol_failure);
        PublishCounters();
        return;
    }
    if (g_context.framework != nullptr && g_context.framework->is_game_thread != nullptr &&
        g_context.framework->is_game_thread(g_context.framework->user) == 0) {
        PublishStatus(Published::Status::waiting, Published::Detail::game_services);
        PublishCounters();
        return;
    }

    const double budget_seconds = jelly::kEffectSoftBudgetMilliseconds / 1000.0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(budget_seconds));
    // Restores come first: a component the effect has left must not stay squashed
    // while the remaining budget goes to models that are still in view.
    RestorePending(deadline);

    if (enabled && jelly::IsDue(g_context.clock, g_context.next_selection,
                                jelly::kSelectionIntervalSeconds)) {
        g_context.next_selection = g_context.clock;
        AnomalyNteEntityFrameV1 frame{sizeof(frame)};
        const AnomalyStatusV1 frame_status =
            g_context.actors->frame(g_context.actors->user, &frame);
        PublishFrame(frame_status, frame);
        if (frame_status.code == ANOMALY_STATUS_V1_OK &&
            jelly::IsUsableSnapshot(frame.flags)) {
            g_context.frame_detail = Published::Detail::none;
            if (!g_context.sweep_running) {
                // A fresh sweep. The host rebuilds the actor frame about once a second
                // and the frame holds thousands of actors, so the sweep deliberately
                // *continues* across generations instead of starting over: it keeps the
                // offset it reached and asks the next page from the newest generation.
                // Restarting only happens when the world changes or a page read fails.
                g_context.sweep_running = false;
                g_context.sweep.clear();
                g_context.sweep_offset = 0;
                g_context.skipped = 0;
                g_context.frame_in_view = 0;
                g_context.frame_kept = 0;
                g_context.spin_applied = 0;
                g_context.spin_blocked = 0;
                g_context.published.page_total.store(0, std::memory_order_release);
                g_context.published.page_returned.store(0, std::memory_order_release);
                g_context.published.first_item_flags.store(0, std::memory_order_release);
            }
            g_context.sweep_running = true;
            SelectionPass(frame, settings, deadline);
        } else {
            // Nothing can be selected from this frame. Say so instead of reporting a
            // healthy run that quietly does nothing; the numbers beside it say which
            // step of the funnel is the one that came up empty.
            g_context.frame_detail = Published::Detail::game_frame;
            g_context.sweep_running = false;
            g_context.sweep.clear();
            g_context.sweep_offset = 0;
            g_context.frame_in_view = 0;
            g_context.frame_kept = 0;
        }
        PublishSelection();
        NoteSelection();
    }

    if (enabled && jelly::IsDue(g_context.clock, g_context.next_effect,
                                jelly::kEffectIntervalSeconds)) {
        g_context.next_effect = g_context.clock;
        EffectPass(settings, deadline);
    }

    PublishStatus(
        !enabled ? (g_context.ledger.empty() ? Published::Status::disabled
                                             : Published::Status::restoring)
                 : Published::Status::running,
        enabled ? g_context.frame_detail : Published::Detail::none);
    PublishCounters();
}

void ANOMALY_CALL OnUpdate(void* /*context*/, const double delta_seconds) {
    try {
        UpdateGame(delta_seconds);
    } catch (...) {
        // A plugin fault must never escape into the host's game thread.
    }
}

// --- Render domain -----------------------------------------------------------------

const char* StatusKey(const Published::Status status) noexcept {
    switch (status) {
        case Published::Status::running: return "status.running";
        case Published::Status::restoring: return "status.restoring";
        case Published::Status::unavailable: return "status.unavailable";
        case Published::Status::waiting: return "status.waiting_frame";
        case Published::Status::disabled: break;
    }
    return "status.disabled";
}

const char* DetailKey(const Published::Detail detail) noexcept {
    switch (detail) {
        case Published::Detail::layout_resource: return "detail.layout_resource";
        case Published::Detail::symbol_missing: return "detail.symbol_missing";
        case Published::Detail::symbol_scan: return "detail.symbol_scan";
        case Published::Detail::call_entry: return "detail.call_entry";
        case Published::Detail::game_services: return "detail.game_services";
        case Published::Detail::game_frame: return "detail.game_frame";
        case Published::Detail::none: break;
    }
    return "";
}

const char* FrameStateKey(const Published::Frame state) noexcept {
    switch (state) {
        case Published::Frame::usable: return "frame.usable";
        case Published::Frame::partial: return "frame.partial";
        case Published::Frame::stale: return "frame.stale";
        case Published::Frame::unavailable: return "frame.unavailable";
        case Published::Frame::none: break;
    }
    return "frame.none";
}

std::string Numbered(
    const std::string_view key, const std::string_view fallback, const unsigned value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%u", value);
    return g_context.localizer.Format(key, fallback, buffer);
}

void Draw(const AnomalyUiServiceV1* ui) {
    const std::string title = g_context.localizer.Text("window.title", "Jelly");
    int open = 1;
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    int enabled = g_context.published.enabled.load(std::memory_order_acquire) ? 1 : 0;
    if (ui->checkbox(
            ui->user, anomaly::sdk::StringView(g_context.localizer.Text("setting.enabled", "Enable")),
            &enabled) != 0) {
        g_context.published.enabled.store(enabled != 0, std::memory_order_release);
    }

    float amplitude =
        static_cast<float>(jelly::ClampAmplitude(
            g_context.published.amplitude.load(std::memory_order_acquire)));
    if (ui->slider_float(
            ui->user,
            anomaly::sdk::StringView(g_context.localizer.Text("setting.amplitude", "Amplitude")),
            &amplitude, static_cast<float>(jelly::kMinimumAmplitude),
            static_cast<float>(jelly::kMaximumAmplitude)) != 0) {
        g_context.published.amplitude.store(
            jelly::ClampAmplitude(amplitude), std::memory_order_release);
    }

    float frequency =
        static_cast<float>(jelly::ClampFrequency(
            g_context.published.frequency.load(std::memory_order_acquire)));
    if (ui->slider_float(
            ui->user,
            anomaly::sdk::StringView(
                g_context.localizer.Text("setting.frequency", "Frequency (Hz)")),
            &frequency, static_cast<float>(jelly::kMinimumFrequencyHz),
            static_cast<float>(jelly::kMaximumFrequencyHz)) != 0) {
        g_context.published.frequency.store(
            jelly::ClampFrequency(frequency), std::memory_order_release);
    }

    // The one cap the window owns. The UI service only offers a float slider, so the
    // whole number is taken here and the module clamps and rounds it either way.
    float max_models = static_cast<float>(jelly::ClampModels(
        g_context.published.max_models.load(std::memory_order_acquire)));
    if (ui->slider_float(
            ui->user,
            anomaly::sdk::StringView(
                g_context.localizer.Text("setting.max_models", "Max models")),
            &max_models, static_cast<float>(jelly::kMinimumModels),
            static_cast<float>(jelly::kMaximumModelsLimit)) != 0) {
        g_context.published.max_models.store(
            static_cast<double>(jelly::ClampModels(max_models)),
            std::memory_order_release);
    }

    float pause_seconds = static_cast<float>(jelly::ClampPause(
        g_context.published.pause_seconds.load(std::memory_order_acquire)));
    if (ui->slider_float(
            ui->user,
            anomaly::sdk::StringView(
                g_context.localizer.Text("setting.pause_seconds", "Pause between bounces (s)")),
            &pause_seconds, static_cast<float>(jelly::kMinimumPauseSeconds),
            static_cast<float>(jelly::kMaximumPauseSeconds)) != 0) {
        g_context.published.pause_seconds.store(
            jelly::ClampPause(pause_seconds), std::memory_order_release);
    }

    int spin = g_context.published.spin.load(std::memory_order_acquire) ? 1 : 0;
    if (ui->checkbox(
            ui->user,
            anomaly::sdk::StringView(
                g_context.localizer.Text("setting.spin", "Spin models (up axis)")),
            &spin) != 0) {
        g_context.published.spin.store(spin != 0, std::memory_order_release);
    }
    float spin_speed = static_cast<float>(jelly::ClampSpin(
        g_context.published.spin_speed.load(std::memory_order_acquire)));
    if (ui->slider_float(
            ui->user,
            anomaly::sdk::StringView(
                g_context.localizer.Text("setting.spin_speed", "Spin speed (deg/s)")),
            &spin_speed, static_cast<float>(jelly::kMinimumSpinDegreesPerSecond),
            static_cast<float>(jelly::kMaximumSpinDegreesPerSecond)) != 0) {
        g_context.published.spin_speed.store(
            jelly::ClampSpin(spin_speed), std::memory_order_release);
    }

    ui->separator(ui->user);

    const auto filter = [ui](const char* key, const char* fallback, std::atomic<bool>& value) {
        int checked = value.load(std::memory_order_acquire) ? 1 : 0;
        if (ui->checkbox(
                ui->user, anomaly::sdk::StringView(g_context.localizer.Text(key, fallback)),
                &checked) != 0) {
            value.store(checked != 0, std::memory_order_release);
        }
    };
    filter("filter.local_player", "Local player", g_context.published.local_player);
    filter("filter.characters", "Characters", g_context.published.characters);
    filter("filter.other_dynamic", "Other dynamic models", g_context.published.other_dynamic);
    // The car the player drives carries its chassis rigid body on the body mesh, so it is
    // the one model whose physics body has to scale with it to be seen at all. The switch
    // is what makes that trade reversible without a rebuild.
    filter(
        "filter.simulated_vehicles", "Include physics-driven vehicles",
        g_context.published.simulated_vehicles);

    if (ui->button(
            ui->user,
            anomaly::sdk::StringView(
                g_context.localizer.Text("action.restore_all", "Restore all")),
            0.0F, 0.0F) != 0) {
        g_context.published.restore_all.store(true, std::memory_order_release);
    }

    ui->separator(ui->user);

    const auto status = static_cast<Published::Status>(
        g_context.published.status.load(std::memory_order_acquire));
    const auto detail = static_cast<Published::Detail>(
        g_context.published.detail.load(std::memory_order_acquire));
    std::string status_line = g_context.localizer.Text(StatusKey(status), "");
    if (const char* detail_key = DetailKey(detail); detail_key[0] != '\0') {
        status_line.append(": ");
        status_line.append(g_context.localizer.Text(
            detail_key, detail == Published::Detail::game_services
                    ? "the game-side services are unavailable"
                    : "unavailable"));
    }
    ui->text(ui->user, anomaly::sdk::StringView(status_line));

    const auto skip = static_cast<Skip>(
        g_context.published.last_skip.load(std::memory_order_acquire));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(g_context.localizer.Format(
            "stats.last_skip", "Last skip: {0}",
            g_context.localizer.Text(SkipKey(skip), "unknown"))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.effective", "Effective: {0}",
            g_context.published.effective.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.skipped", "Skipped: {0}",
            g_context.published.skipped.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.conflict", "Conflicts: {0}",
            g_context.published.conflicts.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.pending_restore", "Pending restore: {0}",
            g_context.published.pending.load(std::memory_order_acquire))));

    // The selection funnel: what the last actor frame and page actually held, so a run
    // that finds nothing says which step came up empty instead of only showing zeros.
    ui->separator(ui->user);
    ui->text(
        ui->user,
        anomaly::sdk::StringView(g_context.localizer.Text(
            FrameStateKey(static_cast<Published::Frame>(
                g_context.published.frame_state.load(std::memory_order_acquire))),
            "Snapshot: unknown")));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.frame_status", "Snapshot status: {0}",
            g_context.published.frame_status.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.frame_entities", "Snapshot actors: {0}",
            g_context.published.frame_entities.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.page_total", "Page matched: {0}",
            g_context.published.page_total.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.page_returned", "Page read: {0}",
            g_context.published.page_returned.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.frame_in_view", "In view: {0}",
            g_context.published.in_view.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.frame_kept", "Candidates: {0}",
            g_context.published.kept.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.models", "Models: {0}",
            g_context.published.models.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.model_cap", "Model cap: {0}",
            static_cast<unsigned>(jelly::ClampModels(
                g_context.published.max_models.load(std::memory_order_acquire))))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.first_item_flags", "First item flags: {0}",
            g_context.published.first_item_flags.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(Numbered(
            "stats.frame_restarts", "Sweeps restarted: {0}",
            g_context.published.frame_restarts.load(std::memory_order_acquire))));
    ui->text(
        ui->user,
        anomaly::sdk::StringView(g_context.localizer.Text(
            g_context.published.player_camera.load(std::memory_order_acquire)
                ? "stats.camera_player"
                : "stats.camera_frame",
            "Camera: actor frame")));
}

void ANOMALY_CALL OnDraw(void* /*context*/, const AnomalyUiServiceV1* ui) {
    try {
        if (ui == nullptr) ui = g_context.ui;
        if (!UiReady(ui)) return;
        Draw(ui);
    } catch (...) {
    }
}

// --- lifecycle ---------------------------------------------------------------------

void ResetContext() noexcept {
    g_context.host = nullptr;
    g_context.core = nullptr;
    g_context.ui = nullptr;
    g_context.signatures = nullptr;
    g_context.names = nullptr;
    g_context.objects = nullptr;
    g_context.framework = nullptr;
    g_context.actors = nullptr;
    g_context.player = nullptr;
    g_context.session = nullptr;
    g_context.config = nullptr;
    g_context.config_schema = {};
    g_context.localizer = Localizer{};
    g_context.memory = CoreMemory{};
    g_context.name_table = NameResolver{};
    g_context.call = ProcessEventCall{};
    g_context.layout = jelly::Layout{};
    g_context.layout_ready = false;
    g_context.layout_error.clear();
    g_context.model.reset();
    g_context.symbols_ready = false;
    g_context.last_symbol_attempt = -1.0;
    g_context.gobjects = 0;
    g_context.process_event = 0;
    g_context.ledger.Clear();
    g_context.candidates.clear();
    g_context.sweep.clear();
    g_context.candidate_cursor = 0;
    g_context.shortlist_walked = false;
    g_context.refusals.clear();
    g_context.vehicle_refusals.clear();
    g_context.vehicle_scaled.clear();
    g_context.clock = 0.0;
    g_context.next_selection = -1.0;
    g_context.next_effect = -1.0;
    g_context.sweep_running = false;
    g_context.sweep_offset = 0;
    g_context.world_generation = 0;
    g_context.world_change = 0;
    g_context.skipped = 0;
    g_context.conflicts = 0;
    g_context.last_skip = Skip::none;
    g_context.symbol_failure = Published::Detail::call_entry;
    g_context.frame_detail = Published::Detail::none;
    g_context.frame_in_view = 0;
    g_context.frame_kept = 0;
    g_context.frame_restarts = 0;
    g_context.selection_note.clear();
    g_context.published.frame_state.store(
        static_cast<unsigned>(Published::Frame::none), std::memory_order_release);
    g_context.published.frame_status.store(0, std::memory_order_release);
    g_context.published.frame_entities.store(0, std::memory_order_release);
    g_context.published.page_total.store(0, std::memory_order_release);
    g_context.published.page_returned.store(0, std::memory_order_release);
    g_context.published.in_view.store(0, std::memory_order_release);
    g_context.published.kept.store(0, std::memory_order_release);
    g_context.published.models.store(0, std::memory_order_release);
    g_context.published.first_item_flags.store(0, std::memory_order_release);
    g_context.published.frame_restarts.store(0, std::memory_order_release);
    g_context.published.player_camera.store(false, std::memory_order_release);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    const auto sdk_host = anomaly::sdk::Host(host);
    const auto core = sdk_host.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    const auto ui = sdk_host.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    // Only the memory view and the settings window are required to load: every
    // game-side service may legitimately be missing (no profile, no adapter, an
    // offline TestHost), and the window then reports it while the effect stays off.
    if (!CoreReady(core.get()) || !UiReady(ui.get())) {
        return StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    ResetContext();
    g_context.host = host;
    g_context.core = core.get();
    g_context.ui = ui.get();
    g_context.memory.core = core.get();
    g_context.signatures = sdk_host
        .Query<AnomalySignatureServiceV1>(
            ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION)
        .get();
    g_context.names = sdk_host
        .Query<AnomalyUe5NamesServiceV1>(
            ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1U)
        .get();
    g_context.objects = sdk_host
        .Query<AnomalyUe5ObjectsServiceV1>(
            ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION)
        .get();
    g_context.framework = sdk_host
        .Query<AnomalyUe5FrameworkServiceV1>(
            ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION)
        .get();
    g_context.actors = sdk_host
        .Query<AnomalyNteActorsServiceV1>(
            ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION)
        .get();
    g_context.player = sdk_host
        .Query<AnomalyNtePlayerServiceV1>(
            ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION)
        .get();
    g_context.session = sdk_host
        .Query<AnomalyNteSessionServiceV1>(
            ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION)
        .get();
    const auto config = sdk_host.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    g_context.config = ConfigReady(config.get()) ? config.get() : nullptr;
    g_context.name_table.names = NamesReady(g_context.names) ? g_context.names : nullptr;
    g_context.localizer.Bind(host);
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    auto* state = static_cast<Context*>(context);
    if (state == nullptr || state != &g_context) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    // Lifecycle domain: the bundled resource is read and validated here, and only
    // its parsed expectations reach the Game domain.
    std::string error;
    state->layout_ready = LoadLayoutResource(state->layout, error);
    state->layout_error = error;

    if (ConfigReady(state->config)) {
        AnomalyGenerationHandleV1 schema{};
        const AnomalyByteSpanV1 document{
            reinterpret_cast<const std::uint8_t*>(kConfigSchemaJson.data()),
            kConfigSchemaJson.size()};
        if (state->config
                ->register_schema(
                    state->config->user, anomaly::sdk::StringView(kConfigSchemaId),
                    kConfigSchemaVersion, document, &schema)
                .code == ANOMALY_STATUS_V1_OK) {
            state->config_schema = schema;
        }
    }
    LoadSettings();
    state->next_selection = -1.0;
    state->next_effect = -1.0;
    PublishStatus(
        Published::Status::disabled,
        state->layout_ready ? Published::Detail::none
                            : Published::Detail::layout_resource);
    PublishCounters();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t /*deadline_milliseconds*/) {
    auto* state = static_cast<Context*>(context);
    if (state == nullptr || state != &g_context) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    // Game objects must not be touched from here and no new Game work is queued.
    // The plugin's own rule is to be stopped with the effect already off and the
    // pending count back to zero; anything still owned is reported as a failure so
    // the host isolates this generation instead of unloading it over a live write.
    PersistSettings();
    if (!state->ledger.empty()) {
        return StatusCode(ANOMALY_STATUS_V1_FAILED);
    }
    state->candidates.clear();
    state->sweep.clear();
    state->model.reset();
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

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.plugin.jelly"),
        anomaly::sdk::StringView("Jelly"),
        anomaly::sdk::StringView("Shikari"), anomaly::sdk::StringView("0.1.12"),
        Load, Start, Stop, Unload, OnUpdate, OnDraw};
    return anomaly::sdk::Ok();
}
