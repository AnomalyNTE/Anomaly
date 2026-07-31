#include "../common/localization.hpp"
#include "anomaly/sdk/cpp.hpp"
#include "camera_tools_profile.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>

namespace {

using namespace camera_tools_profile;

constexpr std::string_view kSettingsSchemaId = "camera-tools-settings-v1";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 2048;
constexpr std::uint64_t kSettingsSaveDelayMilliseconds = 500;
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
constexpr double kDefaultDistance = 0.0;
constexpr float kDefaultSpeed = 800.0F;
constexpr float kMinimumSpeed = 100.0F;
constexpr float kMaximumSpeed = 5000.0F;
constexpr double kBoostMultiplier = 4.0;
constexpr std::uint32_t kForwardKey = 'W';
constexpr std::uint32_t kBackwardKey = 'S';
constexpr std::uint32_t kLeftKey = 'A';
constexpr std::uint32_t kRightKey = 'D';
constexpr std::uint32_t kUpKey = VK_SPACE;
constexpr std::uint32_t kDownKey = VK_SHIFT;
constexpr std::uint32_t kBoostKey = VK_CONTROL;

constexpr std::string_view kSettingsSchema = R"json(
{
  "type":"object",
  "additionalProperties":false,
  "required":["distance","freeCameraEnabled","speed","toggle"],
  "properties":{
    "distance":{"type":"number","minimum":0.0},
    "freeCameraEnabled":{"type":"boolean"},
    "speed":{"type":"number","minimum":100.0,"maximum":5000.0},
    "toggle":{"type":"integer","minimum":1,"maximum":255}
  }
}
)json";

using CameraViewPointFn = void(ANOMALY_CALL *)(void *, double *, double *);
using PlayerInputKeyFn = bool(ANOMALY_CALL *)(void *, const void *);

struct Context final {
  anomaly::plugins::Localizer localizer;
  const AnomalyCoreServiceV1 *core{};
  const AnomalyConfigServiceV1 *config{};
  const AnomalyInputServiceV1 *input{};
  const AnomalyUiServiceV1 *ui{};
  const AnomalySignatureServiceV1 *signature{};
  const AnomalyHookServiceV1 *hook{};
  AnomalyGenerationHandleV1 settings_schema{};
  AnomalyGenerationHandleV1 toggle_hotkey{};
  AnomalyGenerationHandleV1 view_point_hook{};
  AnomalyGenerationHandleV1 input_key_hook{};
  std::atomic<double> distance{kDefaultDistance};
  std::atomic<float> speed{kDefaultSpeed};
  std::atomic<std::uint32_t> toggle_key{VK_F6};
  std::atomic_bool capturing_toggle{};
  std::atomic_bool enabled{};
  std::atomic_bool configured_enabled{};
  std::atomic_bool active{};
  std::atomic<std::uint64_t> settings_revision{};
  std::atomic<std::uint64_t> persisted_settings_revision{};
  std::atomic<std::uint64_t> settings_changed_at{};
  std::uintptr_t view_point_original{};
  std::uintptr_t input_key_original{};
  std::uintptr_t g_world_address{};
  std::uintptr_t f_name_pool_address{};
  std::uintptr_t view_point_target{};
  std::uintptr_t input_key_target{};
  std::atomic<std::uintptr_t> camera_manager{};
  std::atomic<std::uintptr_t> player_input{};
  std::array<std::atomic<double>, 3> position{};
  std::array<std::atomic<double>, 3> rotation{};
  std::array<std::atomic<double>, 3> observed_rotation{};
};

std::atomic<Context *> g_active{};

template <typename Struct, typename Field>
bool HasField(const Struct *value, const std::size_t offset) noexcept {
  return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

AnomalyStatusV1 Status(const std::uint32_t code,
                       const std::string_view message = {}) noexcept {
  return {code, 0, {message.data(), message.size()}};
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
  return {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()};
}

template <typename Service>
const Service *Query(const AnomalyHostApiV1 *host, const char *id,
                     const std::uint32_t version) noexcept {
  if (!HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
          host, offsetof(AnomalyHostApiV1, query_service)) ||
      host->query_service == nullptr) {
    return nullptr;
  }
  const void *table{};
  if (host->query_service(host->host_context, anomaly::sdk::StringView(id),
                          version, &table)
              .code != ANOMALY_STATUS_V1_OK ||
      table == nullptr) {
    return nullptr;
  }
  const auto *service = static_cast<const Service *>(table);
  constexpr std::size_t prefix = offsetof(Service, user) + sizeof(void *);
  return service->struct_size >= prefix && service->service_version >= version
             ? service
             : nullptr;
}

bool ConfigReady(const AnomalyConfigServiceV1 *service) noexcept {
  return HasField<AnomalyConfigServiceV1,
                  decltype(AnomalyConfigServiceV1::write_atomic)>(
             service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
         service->register_schema != nullptr && service->read != nullptr &&
         service->write_atomic != nullptr;
}

bool InputReady(const AnomalyInputServiceV1 *service) noexcept {
  return HasField<AnomalyInputServiceV1,
                  decltype(AnomalyInputServiceV1::release_hotkey)>(
             service, offsetof(AnomalyInputServiceV1, release_hotkey)) &&
         service->snapshot != nullptr && service->was_pressed != nullptr &&
         service->register_hotkey != nullptr &&
         service->release_hotkey != nullptr;
}

bool UiReady(const AnomalyUiServiceV1 *service) noexcept {
  return HasField<AnomalyUiServiceV1,
                  decltype(AnomalyUiServiceV1::input_double)>(
             service, offsetof(AnomalyUiServiceV1, input_double)) &&
         service->set_next_window_size != nullptr &&
         service->begin_window != nullptr && service->end_window != nullptr &&
         service->text != nullptr && service->button != nullptr &&
         service->checkbox != nullptr && service->input_uint32 != nullptr &&
         service->separator != nullptr && service->begin_table != nullptr &&
         service->table_next_row != nullptr &&
         service->table_next_column != nullptr &&
         service->end_table != nullptr && service->input_double != nullptr;
}

bool SignatureReady(const AnomalySignatureServiceV1 *service) noexcept {
  return HasField<AnomalySignatureServiceV1,
                  decltype(AnomalySignatureServiceV1::resolve)>(
             service, offsetof(AnomalySignatureServiceV1, resolve)) &&
         service->resolve != nullptr;
}

bool HookReady(const AnomalyHookServiceV1 *service) noexcept {
  return HasField<AnomalyHookServiceV1,
                  decltype(AnomalyHookServiceV1::end_callback)>(
             service, offsetof(AnomalyHookServiceV1, end_callback)) &&
         service->create != nullptr && service->release != nullptr &&
         service->begin_callback != nullptr && service->end_callback != nullptr;
}

void Log(Context &context, const std::uint32_t level,
         const std::string_view message) noexcept {
  if (context.core != nullptr && context.core->log != nullptr) {
    context.core->log(context.core->user, level,
                      anomaly::sdk::StringView(message));
  }
}

template <typename T>
bool Read(Context &context, const std::uintptr_t address, T &value) noexcept {
  if (context.core == nullptr || context.core->read_memory == nullptr ||
      address == 0) {
    return false;
  }
  AnomalyMutableByteSpanV1 destination{reinterpret_cast<std::uint8_t *>(&value),
                                       sizeof(value)};
  return context.core->read_memory(context.core->user, address, destination)
             .code == ANOMALY_STATUS_V1_OK;
}

bool AddAddress(const std::uintptr_t base, const std::uint64_t offset,
                std::uintptr_t &result) noexcept {
  if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base)
    return false;
  result = base + static_cast<std::uintptr_t>(offset);
  return true;
}

bool ResolveSignature(Context &context, const std::string_view pattern,
                      std::uintptr_t &address) noexcept {
  address = 0;
  return SignatureReady(context.signature) &&
         context.signature
                 ->resolve(context.signature->user,
                           anomaly::sdk::StringView("HTGame.exe"),
                           anomaly::sdk::StringView(".text"),
                           anomaly::sdk::StringView(pattern), &address)
                 .code == ANOMALY_STATUS_V1_OK &&
         address != 0;
}

bool ResolveRipRelative(Context &context, const std::string_view pattern,
                        const std::uint32_t displacement_offset,
                        const std::uint32_t instruction_size,
                        std::uintptr_t &address) noexcept {
  std::uintptr_t instruction{};
  if (!ResolveSignature(context, pattern, instruction) ||
      displacement_offset > instruction_size ||
      instruction_size - displacement_offset < sizeof(std::int32_t)) {
    return false;
  }
  std::int32_t displacement{};
  if (!Read(context, instruction + displacement_offset, displacement))
    return false;
  const auto resolved =
      static_cast<std::intptr_t>(instruction) + instruction_size + displacement;
  if (resolved <= 0)
    return false;
  address = static_cast<std::uintptr_t>(resolved);
  return true;
}

bool ReadPointerAtOffset(Context &context, const std::uintptr_t base,
                         const std::uint32_t offset,
                         std::uintptr_t &value) noexcept {
  std::uintptr_t address{};
  return AddAddress(base, offset, address) && Read(context, address, value) &&
         value != 0;
}

bool NameEquals(Context &context, const std::uint32_t name_id,
                const std::string_view expected) noexcept {
  if (expected.empty() || expected.size() > 15U)
    return false;
  const auto block_index = name_id >> kNameBlockBits;
  const auto entry_offset = name_id & ((1U << kNameBlockBits) - 1U);
  std::uintptr_t block{};
  if (!ReadPointerAtOffset(context, context.f_name_pool_address,
                           kNamePoolBlocksOffset +
                               block_index * sizeof(std::uintptr_t),
                           block)) {
    return false;
  }
  std::uintptr_t entry{};
  if (!AddAddress(block,
                  static_cast<std::uint64_t>(entry_offset) * kNameEntryStride,
                  entry)) {
    return false;
  }
  std::uint16_t header{};
  std::array<char, 16> name{};
  if (!Read(context, entry, header) || (header & 1U) != 0 ||
      (header >> kNameLengthShift) != expected.size() ||
      !Read(context, entry + sizeof(header), name)) {
    return false;
  }
  return std::equal(expected.begin(), expected.end(), name.begin());
}

bool MouseAxisNamesValid(Context &context) noexcept {
  return NameEquals(context, kMouseXNameId, "MouseX") &&
         NameEquals(context, kMouseYNameId, "MouseY") &&
         NameEquals(context, kMouse2DNameId, "Mouse2D");
}

bool IsMouseAxisInput(const void *parameters) noexcept {
  if (parameters == nullptr)
    return false;
  const auto *bytes = static_cast<const std::uint8_t *>(parameters);
  const auto name_id = *reinterpret_cast<const std::uint32_t *>(
      bytes + kInputKeyEventArgsKeyOffset);
  return name_id == kMouseXNameId || name_id == kMouseYNameId ||
         name_id == kMouse2DNameId;
}

bool ResolveLocalPlayerController(Context &context,
                                  std::uintptr_t &controller) noexcept {
  controller = 0;
  std::uintptr_t world{};
  std::uintptr_t game_instance{};
  std::uintptr_t local_players{};
  std::uintptr_t local_player{};
  return Read(context, context.g_world_address, world) && world != 0 &&
         ReadPointerAtOffset(context, world, kWorldGameInstanceOffset,
                             game_instance) &&
         ReadPointerAtOffset(context, game_instance,
                             kGameInstanceLocalPlayersOffset, local_players) &&
         Read(context, local_players, local_player) && local_player != 0 &&
         ReadPointerAtOffset(context, local_player,
                             kLocalPlayerControllerOffset, controller);
}

bool ResolveActiveCameraManager(Context &context,
                                std::uintptr_t &manager) noexcept {
  manager = 0;
  std::uintptr_t controller{};
  std::uintptr_t vtable{};
  std::uintptr_t view_point{};
  return ResolveLocalPlayerController(context, controller) &&
         ReadPointerAtOffset(context, controller,
                             kControllerCameraManagerOffset, manager) &&
         Read(context, manager, vtable) && vtable != 0 &&
         ReadPointerAtOffset(context, vtable, kCameraViewPointVtableOffset,
                             view_point) &&
         view_point == context.view_point_target;
}

bool ResolveActivePlayerInput(Context &context,
                              std::uintptr_t &player_input) noexcept {
  player_input = 0;
  std::uintptr_t controller{};
  std::uintptr_t vtable{};
  std::uintptr_t input_key{};
  return ResolveLocalPlayerController(context, controller) &&
         ReadPointerAtOffset(context, controller, kControllerPlayerInputOffset,
                             player_input) &&
         Read(context, player_input, vtable) && vtable != 0 &&
         ReadPointerAtOffset(context, vtable, kPlayerInputKeyVtableOffset,
                             input_key) &&
         input_key == context.input_key_target;
}

void RefreshCameraManager(Context &context) noexcept {
  std::uintptr_t manager{};
  if (!ResolveActiveCameraManager(context, manager)) {
    context.camera_manager.store(0, std::memory_order_release);
    context.active.store(false, std::memory_order_release);
    return;
  }
  const auto previous =
      context.camera_manager.exchange(manager, std::memory_order_acq_rel);
  if (previous != manager) {
    context.active.store(false, std::memory_order_release);
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "camera tools active CameraManager validated");
  }
}

void RefreshPlayerInput(Context &context) noexcept {
  std::uintptr_t player_input{};
  if (!ResolveActivePlayerInput(context, player_input)) {
    context.player_input.store(0, std::memory_order_release);
    return;
  }
  const auto previous =
      context.player_input.exchange(player_input, std::memory_order_acq_rel);
  if (previous != player_input) {
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "camera tools active EnhancedPlayerInput validated");
  }
}

bool DistanceValid(const double distance) noexcept {
  return std::isfinite(distance) && distance >= 0.0;
}

bool KeyDown(const AnomalyInputSnapshotV1 &input,
             const std::uint32_t key) noexcept {
  return key < 256U && (input.keys[key / 8U] &
                        static_cast<std::uint8_t>(1U << (key % 8U))) != 0;
}

std::string VirtualKeyName(const Context &context, const std::uint32_t key) {
  if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
    return std::string(1, static_cast<char>(key));
  if (key >= VK_F1 && key <= VK_F24)
    return "F" + std::to_string(key - VK_F1 + 1U);
  switch (key) {
  case VK_BACK:
    return context.localizer.Text("key.backspace", "Backspace");
  case VK_TAB:
    return context.localizer.Text("key.tab", "Tab");
  case VK_RETURN:
    return context.localizer.Text("key.enter", "Enter");
  case VK_SHIFT:
    return context.localizer.Text("key.shift", "Shift");
  case VK_CONTROL:
    return context.localizer.Text("key.ctrl", "Ctrl");
  case VK_MENU:
    return context.localizer.Text("key.alt", "Alt");
  case VK_PAUSE:
    return context.localizer.Text("key.pause", "Pause");
  case VK_CAPITAL:
    return context.localizer.Text("key.caps_lock", "Caps Lock");
  case VK_ESCAPE:
    return context.localizer.Text("key.escape", "Escape");
  case VK_SPACE:
    return context.localizer.Text("key.space", "Space");
  case VK_PRIOR:
    return context.localizer.Text("key.page_up", "Page Up");
  case VK_NEXT:
    return context.localizer.Text("key.page_down", "Page Down");
  case VK_END:
    return context.localizer.Text("key.end", "End");
  case VK_HOME:
    return context.localizer.Text("key.home", "Home");
  case VK_LEFT:
    return context.localizer.Text("key.left_arrow", "Left Arrow");
  case VK_UP:
    return context.localizer.Text("key.up_arrow", "Up Arrow");
  case VK_RIGHT:
    return context.localizer.Text("key.right_arrow", "Right Arrow");
  case VK_DOWN:
    return context.localizer.Text("key.down_arrow", "Down Arrow");
  case VK_INSERT:
    return context.localizer.Text("key.insert", "Insert");
  case VK_DELETE:
    return context.localizer.Text("key.delete", "Delete");
  case VK_LWIN:
    return context.localizer.Text("key.left_windows", "Left Windows");
  case VK_RWIN:
    return context.localizer.Text("key.right_windows", "Right Windows");
  case VK_NUMLOCK:
    return context.localizer.Text("key.num_lock", "Num Lock");
  case VK_SCROLL:
    return context.localizer.Text("key.scroll_lock", "Scroll Lock");
  case VK_LSHIFT:
    return context.localizer.Text("key.left_shift", "Left Shift");
  case VK_RSHIFT:
    return context.localizer.Text("key.right_shift", "Right Shift");
  case VK_LCONTROL:
    return context.localizer.Text("key.left_ctrl", "Left Ctrl");
  case VK_RCONTROL:
    return context.localizer.Text("key.right_ctrl", "Right Ctrl");
  case VK_LMENU:
    return context.localizer.Text("key.left_alt", "Left Alt");
  case VK_RMENU:
    return context.localizer.Text("key.right_alt", "Right Alt");
  default:
    break;
  }
  if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
    const std::string number = std::to_string(key - VK_NUMPAD0);
    const std::array<std::string_view, 1> arguments{number};
    return context.localizer.Format("key.numpad", "Numpad {0}", arguments);
  }
  const auto scan_code = MapVirtualKeyA(key, MAPVK_VK_TO_VSC_EX);
  LONG key_data = static_cast<LONG>((scan_code & 0xFFU) << 16U);
  if ((scan_code & 0xFF00U) == 0xE000U)
    key_data |= 1L << 24U;
  std::array<char, 64> name{};
  const int length =
      GetKeyNameTextA(key_data, name.data(), static_cast<int>(name.size()));
  return length > 0 ? std::string(name.data(), static_cast<std::size_t>(length))
                    : context.localizer.Text("key.unknown", "Unknown key");
}

bool SettingsValid(const double distance, const float speed,
                   const std::uint32_t toggle) noexcept {
  return DistanceValid(distance) && std::isfinite(speed) &&
         speed >= kMinimumSpeed && speed <= kMaximumSpeed && toggle > 0 &&
         toggle < 256U;
}

void MarkSettingsDirty(Context &context) noexcept {
  context.settings_changed_at.store(GetTickCount64(),
                                    std::memory_order_release);
  context.settings_revision.fetch_add(1, std::memory_order_acq_rel);
}

bool PersistSettings(Context &context) noexcept {
  const double distance = context.distance.load(std::memory_order_acquire);
  const float speed = context.speed.load(std::memory_order_acquire);
  const auto toggle = context.toggle_key.load(std::memory_order_acquire);
  if (!ConfigReady(context.config) ||
      !SettingsValid(distance, speed, toggle)) {
    return false;
  }
  const auto revision =
      context.settings_revision.load(std::memory_order_acquire);
  try {
    const auto document =
        nlohmann::json{
            {"distance", distance},
            {"freeCameraEnabled",
             context.configured_enabled.load(std::memory_order_acquire)},
            {"speed", speed},
            {"toggle", toggle}}
            .dump();
    if (context.config
            ->write_atomic(context.config->user,
                           anomaly::sdk::StringView(kSettingsSchemaId),
                           kSettingsSchemaVersion, Bytes(document))
            .code != ANOMALY_STATUS_V1_OK) {
      return false;
    }
    context.persisted_settings_revision.store(revision,
                                              std::memory_order_release);
    return true;
  } catch (...) {
    return false;
  }
}

bool LoadSettings(Context &context) noexcept {
  try {
    std::uint32_t version{};
    std::size_t size{};
    const auto status = context.config->read(
        context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
        &version, {nullptr, 0}, &size);
    if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
      context.distance.store(kDefaultDistance, std::memory_order_release);
      context.speed.store(kDefaultSpeed, std::memory_order_release);
      context.toggle_key.store(VK_F6, std::memory_order_release);
      context.configured_enabled.store(false, std::memory_order_release);
      context.enabled.store(false, std::memory_order_release);
      return PersistSettings(context);
    }
    if (status.code != ANOMALY_STATUS_V1_OK ||
        version != kSettingsSchemaVersion || size == 0 ||
        size > kMaximumSettingsBytes) {
      return false;
    }
    std::string document(size, '\0');
    std::size_t copied = size;
    if (context.config
                ->read(context.config->user,
                       anomaly::sdk::StringView(kSettingsSchemaId), &version,
                       {reinterpret_cast<std::uint8_t *>(document.data()),
                        document.size()},
                       &copied)
                .code != ANOMALY_STATUS_V1_OK ||
        copied == 0 || copied > document.size()) {
      return false;
    }
    const auto json =
        nlohmann::json::parse(document.begin(), document.begin() + copied);
    if (!json.is_object() || json.size() != 4)
      return false;
    const double distance = json.at("distance").get<double>();
    const float speed = json.at("speed").get<float>();
    const auto toggle = json.at("toggle").get<std::uint32_t>();
    const bool enabled = json.at("freeCameraEnabled").get<bool>();
    if (!SettingsValid(distance, speed, toggle))
      return false;
    context.distance.store(distance, std::memory_order_release);
    context.speed.store(speed, std::memory_order_release);
    context.toggle_key.store(toggle, std::memory_order_release);
    context.configured_enabled.store(enabled, std::memory_order_release);
    context.enabled.store(enabled, std::memory_order_release);
    return true;
  } catch (...) {
    return false;
  }
}

void SetFreeCameraEnabled(Context &context, const bool enabled) noexcept {
  const bool changed = context.configured_enabled.exchange(
                           enabled, std::memory_order_acq_rel) != enabled;
  context.enabled.store(enabled, std::memory_order_release);
  context.active.store(false, std::memory_order_release);
  if (changed)
    MarkSettingsDirty(context);
}

void ANOMALY_CALL ToggleHotkey(void *user, AnomalyGenerationHandleV1,
                               const AnomalyInputSnapshotV1 *) noexcept {
  auto *context = static_cast<Context *>(user);
  if (context != nullptr &&
      !context->capturing_toggle.load(std::memory_order_acquire)) {
    SetFreeCameraEnabled(
        *context, !context->enabled.load(std::memory_order_acquire));
  }
}

bool RegisterToggleHotkey(Context &context, const std::uint32_t key,
                          AnomalyGenerationHandleV1 &handle) noexcept {
  if (!InputReady(context.input) || key == 0 || key >= 256U)
    return false;
  try {
    const std::string id = "camera-tools-free-camera-toggle-" +
                           std::to_string(key);
    AnomalyHotkeySpecV1 spec{sizeof(spec)};
    spec.virtual_key = key;
    spec.flags = ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS |
                 ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED;
    spec.id = anomaly::sdk::StringView(id);
    handle = {};
    return context.input
                   ->register_hotkey(context.input->user, &spec, ToggleHotkey,
                                     &context, &handle)
                   .code == ANOMALY_STATUS_V1_OK &&
           handle.id != 0;
  } catch (...) {
    handle = {};
    return false;
  }
}

void ReleaseToggleHotkey(Context &context) noexcept {
  if (context.toggle_hotkey.id != 0 && InputReady(context.input)) {
    static_cast<void>(context.input->release_hotkey(context.input->user,
                                                    context.toggle_hotkey));
  }
  context.toggle_hotkey = {};
}

bool ReplaceToggleHotkey(Context &context, const std::uint32_t key) noexcept {
  const auto current = context.toggle_key.load(std::memory_order_acquire);
  if (key == current)
    return true;
  AnomalyGenerationHandleV1 replacement{};
  if (!RegisterToggleHotkey(context, key, replacement))
    return false;
  const auto previous = context.toggle_hotkey;
  if (previous.id == 0 ||
      context.input->release_hotkey(context.input->user, previous).code !=
          ANOMALY_STATUS_V1_OK) {
    static_cast<void>(
        context.input->release_hotkey(context.input->user, replacement));
    return false;
  }
  context.toggle_hotkey = replacement;
  context.toggle_key.store(key, std::memory_order_release);
  MarkSettingsDirty(context);
  return true;
}

void CaptureToggleKey(Context &context) noexcept {
  int pressed{};
  if (context.input->was_pressed(context.input->user, VK_ESCAPE, &pressed)
              .code == ANOMALY_STATUS_V1_OK &&
      pressed != 0) {
    context.capturing_toggle.store(false, std::memory_order_release);
    return;
  }
  for (std::uint32_t key = 1; key < 256U; ++key) {
    if (key == VK_ESCAPE || (key >= VK_LBUTTON && key <= VK_XBUTTON2))
      continue;
    pressed = 0;
    if (context.input->was_pressed(context.input->user, key, &pressed).code ==
            ANOMALY_STATUS_V1_OK &&
        pressed != 0 && ReplaceToggleHotkey(context, key)) {
      context.capturing_toggle.store(false, std::memory_order_release);
      return;
    }
  }
}

void ApplyViewDistance(double *location, const double *rotation,
                       const double distance) noexcept {
  if (!DistanceValid(distance) || distance == 0.0)
    return;
  const double pitch = rotation[0] * kDegreesToRadians;
  const double yaw = rotation[1] * kDegreesToRadians;
  const double pitch_cosine = std::cos(pitch);
  location[0] -= pitch_cosine * std::cos(yaw) * distance;
  location[1] -= pitch_cosine * std::sin(yaw) * distance;
  location[2] -= std::sin(pitch) * distance;
}

void ANOMALY_CALL CameraViewPointDetour(void *object, double *location,
                                        double *rotation) noexcept {
  Context *context = g_active.load(std::memory_order_acquire);
  AnomalyGenerationHandleV1 lease{};
  bool leased = false;
  CameraViewPointFn original = nullptr;
  try {
    if (context != nullptr && context->view_point_hook.id != 0 &&
        HookReady(context->hook)) {
      leased = context->hook
                   ->begin_callback(context->hook->user,
                                    context->view_point_hook, &lease)
                   .code == ANOMALY_STATUS_V1_OK;
      original =
          reinterpret_cast<CameraViewPointFn>(context->view_point_original);
    }
  } catch (...) {
    original =
        context == nullptr
            ? nullptr
            : reinterpret_cast<CameraViewPointFn>(context->view_point_original);
  }

  try {
    if (original != nullptr) {
      original(object, location, rotation);
      if (context != nullptr && location != nullptr && rotation != nullptr &&
          object == reinterpret_cast<void *>(context->camera_manager.load(
                        std::memory_order_acquire))) {
        for (std::size_t axis{}; axis != 3; ++axis) {
          context->observed_rotation[axis].store(rotation[axis],
                                                 std::memory_order_release);
        }
        const double distance =
            context->distance.load(std::memory_order_acquire);
        if (context->enabled.load(std::memory_order_acquire)) {
          if (!context->active.load(std::memory_order_acquire)) {
            ApplyViewDistance(location, rotation, distance);
            for (std::size_t axis{}; axis != 3; ++axis) {
              context->position[axis].store(location[axis],
                                            std::memory_order_relaxed);
              context->rotation[axis].store(rotation[axis],
                                            std::memory_order_relaxed);
            }
            context->active.store(true, std::memory_order_release);
          }
          for (std::size_t axis{}; axis != 3; ++axis) {
            location[axis] =
                context->position[axis].load(std::memory_order_acquire);
            rotation[axis] =
                context->rotation[axis].load(std::memory_order_acquire);
          }
        } else {
          ApplyViewDistance(location, rotation, distance);
        }
      }
    }
  } catch (...) {
  }
  if (leased && context != nullptr && HookReady(context->hook)) {
    static_cast<void>(context->hook->end_callback(context->hook->user, lease));
  }
}

bool ANOMALY_CALL PlayerInputKeyDetour(void *object,
                                       const void *parameters) noexcept {
  Context *context = g_active.load(std::memory_order_acquire);
  AnomalyGenerationHandleV1 lease{};
  bool leased = false;
  PlayerInputKeyFn original = nullptr;
  try {
    if (context != nullptr && context->input_key_hook.id != 0 &&
        HookReady(context->hook)) {
      leased = context->hook
                   ->begin_callback(context->hook->user,
                                    context->input_key_hook, &lease)
                   .code == ANOMALY_STATUS_V1_OK;
      original =
          reinterpret_cast<PlayerInputKeyFn>(context->input_key_original);
    }
  } catch (...) {
    original =
        context == nullptr
            ? nullptr
            : reinterpret_cast<PlayerInputKeyFn>(context->input_key_original);
  }

  bool handled = false;
  try {
    const bool mouse_axis = IsMouseAxisInput(parameters);
    if (context != nullptr &&
        object == reinterpret_cast<void *>(
                      context->player_input.load(std::memory_order_acquire)) &&
        context->enabled.load(std::memory_order_acquire) && !mouse_axis) {
      handled = true;
    } else if (original != nullptr) {
      handled = original(object, parameters);
    }
  } catch (...) {
  }
  if (leased && context != nullptr && HookReady(context->hook)) {
    static_cast<void>(context->hook->end_callback(context->hook->user, lease));
  }
  return handled;
}

void UpdateFreeCamera(Context &context, const double delta_seconds) noexcept {
  if (!InputReady(context.input) ||
      context.camera_manager.load(std::memory_order_acquire) == 0)
    return;
  if (!context.enabled.load(std::memory_order_acquire)) {
    context.active.store(false, std::memory_order_release);
    return;
  }
  if (!context.active.load(std::memory_order_acquire))
    return;

  AnomalyInputSnapshotV1 input{sizeof(input)};
  if (context.input->snapshot(context.input->user, &input).code !=
      ANOMALY_STATUS_V1_OK) {
    return;
  }
  std::array<double, 3> view_rotation{};
  for (std::size_t axis{}; axis != view_rotation.size(); ++axis) {
    view_rotation[axis] =
        context.observed_rotation[axis].load(std::memory_order_acquire);
    context.rotation[axis].store(view_rotation[axis],
                                 std::memory_order_release);
  }
  if ((input.capture_flags & ANOMALY_INPUT_CAPTURE_V1_KEYBOARD) != 0 ||
      !std::isfinite(delta_seconds) || delta_seconds <= 0.0) {
    return;
  }

  const double pitch = view_rotation[0] * kDegreesToRadians;
  const double yaw = view_rotation[1] * kDegreesToRadians;
  const std::array<double, 3> forward{std::cos(pitch) * std::cos(yaw),
                                      std::cos(pitch) * std::sin(yaw),
                                      std::sin(pitch)};
  const std::array<double, 3> right{-std::sin(yaw), std::cos(yaw), 0.0};
  const double forward_axis = static_cast<double>(KeyDown(input, kForwardKey)) -
                              static_cast<double>(KeyDown(input, kBackwardKey));
  const double right_axis = static_cast<double>(KeyDown(input, kRightKey)) -
                            static_cast<double>(KeyDown(input, kLeftKey));
  std::array<double, 3> movement{};
  for (std::size_t axis{}; axis != movement.size(); ++axis) {
    movement[axis] = forward[axis] * forward_axis + right[axis] * right_axis;
  }
  movement[2] += static_cast<double>(KeyDown(input, kUpKey)) -
                 static_cast<double>(KeyDown(input, kDownKey));
  const double length =
      std::sqrt(movement[0] * movement[0] + movement[1] * movement[1] +
                movement[2] * movement[2]);
  if (length == 0.0)
    return;
  const double boost = KeyDown(input, kBoostKey) ? kBoostMultiplier : 1.0;
  const double movement_distance =
      static_cast<double>(context.speed.load(std::memory_order_acquire)) *
      boost * delta_seconds;
  for (std::size_t axis{}; axis != movement.size(); ++axis) {
    context.position[axis].fetch_add(
        movement[axis] / length * movement_distance,
        std::memory_order_release);
  }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1 *host,
                                  void **plugin_context) {
  if (host == nullptr || plugin_context == nullptr)
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  *plugin_context = nullptr;
  auto *context = new (std::nothrow) Context();
  if (context == nullptr)
    return Status(ANOMALY_STATUS_V1_FAILED);
  context->localizer = anomaly::plugins::Localizer(host);
  context->core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID,
                                              ANOMALY_CORE_SERVICE_V1_VERSION);
  context->config = Query<AnomalyConfigServiceV1>(
      host, ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
  context->input = Query<AnomalyInputServiceV1>(
      host, ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION);
  context->ui = Query<AnomalyUiServiceV1>(host, ANOMALY_UI_SERVICE_V1_ID,
                                          ANOMALY_UI_SERVICE_V1_VERSION);
  context->signature =
      Query<AnomalySignatureServiceV1>(host, ANOMALY_SIGNATURE_SERVICE_V1_ID,
                                       ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
  context->hook = Query<AnomalyHookServiceV1>(host, ANOMALY_HOOK_SERVICE_V1_ID,
                                              ANOMALY_HOOK_SERVICE_V1_VERSION);
  if (!ConfigReady(context->config) || !InputReady(context->input) ||
      !UiReady(context->ui) || !SignatureReady(context->signature) ||
      !HookReady(context->hook)) {
    delete context;
    return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                  "required plugin services are unavailable");
  }
  const auto schema_status = context->config->register_schema(
      context->config->user, anomaly::sdk::StringView(kSettingsSchemaId),
      kSettingsSchemaVersion, Bytes(kSettingsSchema),
      &context->settings_schema);
  if (schema_status.code != ANOMALY_STATUS_V1_OK ||
      context->settings_schema.id == 0 || !LoadSettings(*context)) {
    delete context;
    return Status(ANOMALY_STATUS_V1_FAILED,
                  "camera tools settings are invalid");
  }
  *plugin_context = context;
  return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void *plugin_context) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr)
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  if (!ResolveRipRelative(*context, kGWorldPattern, kGWorldResolveOffset,
                          kGWorldInstructionSize, context->g_world_address) ||
      !ResolveRipRelative(*context, kFNamePoolPattern,
                          kFNamePoolResolveOffset,
                          kFNamePoolInstructionSize,
                          context->f_name_pool_address) ||
      !MouseAxisNamesValid(*context) ||
      !ResolveSignature(*context, kCameraViewPointPattern,
                        context->view_point_target) ||
      !ResolveSignature(*context, kPlayerInputKeyPattern,
                        context->input_key_target)) {
    return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                  "camera tools discovery signatures are unavailable");
  }
  RefreshCameraManager(*context);
  RefreshPlayerInput(*context);
  AnomalyHookRequestV1 view_point_request{sizeof(view_point_request)};
  view_point_request.kind = ANOMALY_HOOK_V1_FUNCTION;
  view_point_request.target = context->view_point_target;
  view_point_request.detour = reinterpret_cast<void *>(&CameraViewPointDetour);
  view_point_request.label =
      anomaly::sdk::StringView("camera-tools-get-camera-view-point");
  g_active.store(context, std::memory_order_release);
  const auto hook_status = context->hook->create(
      context->hook->user, &view_point_request, &context->view_point_original,
      &context->view_point_hook);
  if (hook_status.code != ANOMALY_STATUS_V1_OK ||
      context->view_point_hook.id == 0 || context->view_point_original == 0) {
    g_active.store(nullptr, std::memory_order_release);
    context->view_point_hook = {};
    return Status(ANOMALY_STATUS_V1_FAILED,
                  "GetCameraViewPoint hook creation failed");
  }

  AnomalyHookRequestV1 input_key_request{sizeof(input_key_request)};
  input_key_request.kind = ANOMALY_HOOK_V1_FUNCTION;
  input_key_request.target = context->input_key_target;
  input_key_request.detour = reinterpret_cast<void *>(&PlayerInputKeyDetour);
  input_key_request.label =
      anomaly::sdk::StringView("camera-tools-player-input-key");
  const auto input_hook_status = context->hook->create(
      context->hook->user, &input_key_request, &context->input_key_original,
      &context->input_key_hook);
  if (input_hook_status.code != ANOMALY_STATUS_V1_OK ||
      context->input_key_hook.id == 0 || context->input_key_original == 0) {
    static_cast<void>(
        context->hook->release(context->hook->user, context->view_point_hook));
    g_active.store(nullptr, std::memory_order_release);
    context->view_point_hook = {};
    context->view_point_original = 0;
    context->input_key_hook = {};
    return Status(ANOMALY_STATUS_V1_FAILED,
                  "PlayerInput InputKey hook creation failed");
  }
  if (!RegisterToggleHotkey(
          *context, context->toggle_key.load(std::memory_order_acquire),
          context->toggle_hotkey)) {
    static_cast<void>(
        context->hook->release(context->hook->user, context->input_key_hook));
    static_cast<void>(
        context->hook->release(context->hook->user, context->view_point_hook));
    g_active.store(nullptr, std::memory_order_release);
    context->input_key_hook = {};
    context->view_point_hook = {};
    context->input_key_original = 0;
    context->view_point_original = 0;
    return Status(ANOMALY_STATUS_V1_FAILED,
                  "free camera hotkey registration failed");
  }
  Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
      "camera tools hooks started");
  return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void *plugin_context, std::uint32_t) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr)
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  context->enabled.store(false, std::memory_order_release);
  context->active.store(false, std::memory_order_release);
  context->capturing_toggle.store(false, std::memory_order_release);
  ReleaseToggleHotkey(*context);
  AnomalyStatusV1 result = anomaly::sdk::Ok();
  if (context->input_key_hook.id != 0 && HookReady(context->hook)) {
    result =
        context->hook->release(context->hook->user, context->input_key_hook);
    if (result.code != ANOMALY_STATUS_V1_OK &&
        result.code != ANOMALY_STATUS_V1_NOT_FOUND) {
      return result;
    }
    context->input_key_hook = {};
    result = anomaly::sdk::Ok();
  }
  if (context->view_point_hook.id != 0 && HookReady(context->hook)) {
    result =
        context->hook->release(context->hook->user, context->view_point_hook);
    if (result.code != ANOMALY_STATUS_V1_OK &&
        result.code != ANOMALY_STATUS_V1_NOT_FOUND) {
      return result;
    }
    context->view_point_hook = {};
    result = anomaly::sdk::Ok();
  }
  Context *expected = context;
  static_cast<void>(g_active.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel));
  context->input_key_original = 0;
  context->view_point_original = 0;
  context->camera_manager.store(0, std::memory_order_release);
  context->player_input.store(0, std::memory_order_release);
  if (context->settings_revision.load(std::memory_order_acquire) !=
          context->persisted_settings_revision.load(
              std::memory_order_acquire) &&
      !PersistSettings(*context)) {
    result = Status(ANOMALY_STATUS_V1_FAILED, "settings save failed");
  }
  return result;
}

void ANOMALY_CALL Unload(void *plugin_context) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr)
    return;
  static_cast<void>(Stop(context, 0));
  delete context;
}

void ANOMALY_CALL Update(void *plugin_context, const double delta_seconds) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr)
    return;
  try {
    RefreshCameraManager(*context);
    RefreshPlayerInput(*context);
    UpdateFreeCamera(*context, delta_seconds);
    const auto revision =
        context->settings_revision.load(std::memory_order_acquire);
    if (revision != context->persisted_settings_revision.load(
                        std::memory_order_acquire)) {
      const auto now = GetTickCount64();
      const auto changed_at =
          context->settings_changed_at.load(std::memory_order_acquire);
      if (now - changed_at >= kSettingsSaveDelayMilliseconds) {
        context->settings_changed_at.store(now, std::memory_order_release);
        static_cast<void>(PersistSettings(*context));
      }
    }
  } catch (...) {
    context->camera_manager.store(0, std::memory_order_release);
  }
}

void ANOMALY_CALL Draw(void *plugin_context, const AnomalyUiServiceV1 *ui) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr || !UiReady(ui))
    return;
  bool window_begun = false;
  try {
    const std::string title =
        context->localizer.Text("window.title", "Camera Tools");
    int open = 1;
    ui->set_next_window_size(ui->user, 440.0F, 0.0F, 4U);
    const int visible =
        ui->begin_window(ui->user, anomaly::sdk::StringView(title), &open, 0);
    window_begun = true;
    if (visible != 0) {
      const std::string view_distance_section = context->localizer.Text(
          "section.view_distance", "View Distance");
      ui->text(ui->user, anomaly::sdk::StringView(view_distance_section));
      const std::string distance_label = context->localizer.Label(
          "setting.distance", "Extra view distance", "camera-distance");
      const std::string restore_label = context->localizer.Label(
          "action.restore_game_default", "Restore game default",
          "restore-game-distance");
      const auto draw_distance = [&]() {
        double distance = context->distance.load(std::memory_order_acquire);
        if (ui->input_double(ui->user,
                             anomaly::sdk::StringView(distance_label),
                             &distance, 100.0, 1000.0) != 0 &&
            DistanceValid(distance)) {
          context->distance.store(distance, std::memory_order_release);
          MarkSettingsDirty(*context);
        }
      };
      if (ui->begin_table(ui->user,
                          anomaly::sdk::StringView("camera-distance-row"), 2,
                          0, 0.0F, 0.0F) != 0) {
        ui->table_next_row(ui->user);
        static_cast<void>(ui->table_next_column(ui->user));
        draw_distance();
        static_cast<void>(ui->table_next_column(ui->user));
        if (ui->button(ui->user, anomaly::sdk::StringView(restore_label), 0.0F,
                       0.0F) != 0) {
          context->distance.store(kDefaultDistance,
                                  std::memory_order_release);
          MarkSettingsDirty(*context);
        }
        ui->end_table(ui->user);
      } else {
        draw_distance();
      }

      ui->separator(ui->user);
      const std::string free_section =
          context->localizer.Text("section.free_camera", "Free Camera");
      ui->text(ui->user, anomaly::sdk::StringView(free_section));
      int enabled =
          context->configured_enabled.load(std::memory_order_acquire) ? 1 : 0;
      const std::string enabled_label = context->localizer.Label(
          "setting.enabled", "Enabled", "free-camera-enabled");
      if (ui->checkbox(ui->user, anomaly::sdk::StringView(enabled_label),
                       &enabled) != 0) {
        SetFreeCameraEnabled(*context, enabled != 0);
      }

      const std::string speed_label = context->localizer.Label(
          "setting.speed", "Movement speed", "free-camera-speed");
      const std::string reset_speed_label = context->localizer.Label(
          "action.reset_speed", "Reset speed", "reset-free-camera-speed");
      const auto draw_speed = [&]() {
        std::uint32_t speed = static_cast<std::uint32_t>(
            std::lround(context->speed.load(std::memory_order_acquire)));
        if (ui->input_uint32(ui->user, anomaly::sdk::StringView(speed_label),
                             &speed, 50U, 200U) != 0) {
          context->speed.store(
              static_cast<float>(
                  (std::clamp)(speed, static_cast<std::uint32_t>(kMinimumSpeed),
                               static_cast<std::uint32_t>(kMaximumSpeed))),
              std::memory_order_release);
          MarkSettingsDirty(*context);
        }
      };
      if (ui->begin_table(ui->user,
                          anomaly::sdk::StringView("free-camera-speed-row"), 2,
                          0, 0.0F, 0.0F) != 0) {
        ui->table_next_row(ui->user);
        static_cast<void>(ui->table_next_column(ui->user));
        draw_speed();
        static_cast<void>(ui->table_next_column(ui->user));
        if (ui->button(ui->user,
                       anomaly::sdk::StringView(reset_speed_label), 0.0F,
                       0.0F) != 0) {
          context->speed.store(kDefaultSpeed, std::memory_order_release);
          MarkSettingsDirty(*context);
        }
        ui->end_table(ui->user);
      } else {
        draw_speed();
      }

      if (context->capturing_toggle.load(std::memory_order_acquire)) {
        const std::string capture_label = context->localizer.Label(
            "action.capture_key", "Press a key...", "free-camera-hotkey");
        if (ui->button(ui->user, anomaly::sdk::StringView(capture_label),
                       0.0F, 0.0F) != 0) {
          context->capturing_toggle.store(false, std::memory_order_release);
        } else {
          CaptureToggleKey(*context);
        }
      } else {
        const std::string key_name = VirtualKeyName(
            *context, context->toggle_key.load(std::memory_order_acquire));
        const std::array<std::string_view, 1> arguments{key_name};
        std::string activation_label = context->localizer.Format(
            "setting.activation_key", "Activation key: {0}", arguments);
        activation_label.append("###free-camera-hotkey");
        if (ui->button(ui->user, anomaly::sdk::StringView(activation_label),
                       0.0F, 0.0F) != 0) {
          context->capturing_toggle.store(true, std::memory_order_release);
        }
      }

      const std::string guide = context->localizer.Text(
          "controls.guide", "W/A/S/D: Forward / Backward / Left / Right\n"
                            "Space: Up\nShift: Down");
      ui->text(ui->user, anomaly::sdk::StringView(guide));
    }
  } catch (...) {
  }
  if (window_begun)
    ui->end_window(ui->user);
}

} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL
AnomalyPluginEntryV1(AnomalyPluginDescriptorV1 *descriptor) {
  if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  *descriptor = {sizeof(*descriptor),
                 ANOMALY_PLUGIN_API_V1_MAJOR,
                 ANOMALY_PLUGIN_API_V1_MINOR,
                 anomaly::sdk::StringView("anomaly.local.nte.camera-tools"),
                 anomaly::sdk::StringView("Camera Tools"),
                 anomaly::sdk::StringView("Anomaly"),
                 anomaly::sdk::StringView("1.0.0"),
                 Load,
                 Start,
                 Stop,
                 Unload,
                 Update,
                 Draw};
  return anomaly::sdk::Ok();
}
