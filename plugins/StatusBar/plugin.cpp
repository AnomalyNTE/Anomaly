// Status Bar: a liquid glass strip with frame rate, 1% low, clock, CPU load and clock
// speed, memory load and GPU load.
//
// Threads: a sampler thread reads the system counters once a second, because a PDH
// collection can take long enough to show up as a hitch on the game or render thread.
// on_draw (Render) times frames and draws the bar. Readings are guarded by
// Context::mutex; frame timing is owned by on_draw alone.
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"

#include <Windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#define STATUS_HAS_FIELD(table, Type, field)                                   \
    ((table) != nullptr &&                                                     \
     (table)->struct_size >= offsetof(Type, field) + sizeof((table)->field) && \
     (table)->field != nullptr)

namespace {

using anomaly::sdk::StringView;
using anomaly::sdk::Succeeded;

// Material Symbols Rounded codepoints; assets/MaterialSymbolsStatusBar.ttf carries only these.
constexpr char32_t kIconFps = 0xE26B;        // bar_chart
constexpr char32_t kIconClock = 0xEFD6;      // schedule
constexpr char32_t kIconMemory = 0xF7A3;     // memory_alt
constexpr char32_t kIconCpu = 0xE322;        // memory (a chip)
constexpr char32_t kIconFrequency = 0xF190;  // monitoring
constexpr char32_t kIconGpu = 0xE30C;        // desktop_windows
constexpr char32_t kIconPower = 0xEA0B;      // bolt
constexpr char32_t kIconGpuClock = 0xE9E4;   // speed
constexpr char32_t kIconTemperature = 0xE1FF;  // device_thermostat

constexpr float kIconFontPixels = 18.0F;
constexpr std::size_t kItemCount = 10;
constexpr auto kSampleInterval = std::chrono::seconds(1);
constexpr auto kPublishInterval = std::chrono::milliseconds(500);
constexpr std::string_view kSettingsPath = "settings.txt";

enum class Position : int {
    TopLeft = 0, TopCenter = 1, TopRight = 2, BottomLeft = 3, BottomCenter = 4, BottomRight = 5
};
constexpr int kPositionCount = 6;

// What the bar can show; each one is a checkbox in the window.
enum Toggle : std::size_t {
    kFps, kLow, kClock, kMemory, kCpu, kFrequency, kCpuPower,
    kGpu, kGpuClock, kGpuTemperature, kGpuPower, kLogo, kToggleCount
};
// Keys in settings.txt, in Toggle order.
constexpr std::array<std::string_view, kToggleCount> kToggleKeys{
    "fps", "low", "clock", "memory", "cpu", "frequency", "cpu_power",
    "gpu", "gpu_clock", "gpu_temperature", "gpu_power", "logo"};

// Spacing between items, as a percentage of the default gap.
constexpr int kMinSpacing = 30;
constexpr int kMaxSpacing = 200;

struct Settings final {
    Position position{Position::TopCenter};
    std::array<bool, kToggleCount> shown{
        true, true, true, true, true, true, true, true, true, true, true, true};
    int spacing{100};

    bool operator==(const Settings&) const = default;
};

// System readings; a negative value means the counter is unavailable.
struct Readings final {
    float cpu_percent{-1.0F};
    float cpu_ghz{-1.0F};
    float memory_percent{-1.0F};
    float gpu_percent{-1.0F};
    float cpu_watts{-1.0F};
    float gpu_mhz{-1.0F};
    float gpu_celsius{-1.0F};
    float gpu_watts{-1.0F};
};

// NVIDIA's management library ships with the driver. It is loaded at run time so the
// plugin still starts on a machine without an NVIDIA GPU; those readings stay "--".
class NvidiaSensors final {
public:
    NvidiaSensors() noexcept {
        library_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library_ == nullptr) return;
        init_ = Function<Init>("nvmlInit_v2");
        shutdown_ = Function<Shutdown>("nvmlShutdown");
        handle_by_index_ = Function<HandleByIndex>("nvmlDeviceGetHandleByIndex_v2");
        clock_ = Function<Clock>("nvmlDeviceGetClockInfo");
        temperature_ = Function<Temperature>("nvmlDeviceGetTemperature");
        power_ = Function<Power>("nvmlDeviceGetPowerUsage");
        if (init_ == nullptr || shutdown_ == nullptr || handle_by_index_ == nullptr ||
            init_() != kSuccess) {
            FreeLibrary(library_);
            library_ = nullptr;
            return;
        }
        initialized_ = true;
        // The first NVIDIA GPU, which is the render GPU on a single-card machine.
        if (handle_by_index_(0, &device_) != kSuccess) device_ = nullptr;
    }

    ~NvidiaSensors() {
        if (initialized_) shutdown_();
        if (library_ != nullptr) FreeLibrary(library_);
    }

    NvidiaSensors(const NvidiaSensors&) = delete;
    NvidiaSensors& operator=(const NvidiaSensors&) = delete;

    void Read(Readings& readings) const noexcept {
        if (device_ == nullptr) return;
        unsigned int value{};
        if (clock_ != nullptr && clock_(device_, kGraphicsClock, &value) == kSuccess) {
            readings.gpu_mhz = static_cast<float>(value);
        }
        if (temperature_ != nullptr && temperature_(device_, kGpuSensor, &value) == kSuccess) {
            readings.gpu_celsius = static_cast<float>(value);
        }
        if (power_ != nullptr && power_(device_, &value) == kSuccess) {
            readings.gpu_watts = static_cast<float>(value) / 1000.0F;  // milliwatts
        }
    }

private:
    // nvml.h values, declared here so the plugin needs no CUDA toolkit to build.
    using Device = void*;
    static constexpr int kSuccess = 0;
    static constexpr int kGraphicsClock = 0;  // NVML_CLOCK_GRAPHICS
    static constexpr int kGpuSensor = 0;      // NVML_TEMPERATURE_GPU
    using Init = int(__cdecl*)();
    using Shutdown = int(__cdecl*)();
    using HandleByIndex = int(__cdecl*)(unsigned int, Device*);
    using Clock = int(__cdecl*)(Device, int, unsigned int*);
    using Temperature = int(__cdecl*)(Device, int, unsigned int*);
    using Power = int(__cdecl*)(Device, unsigned int*);

    template <typename Fn>
    Fn Function(const char* name) const noexcept {
        return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(library_, name)));
    }

    HMODULE library_{};
    bool initialized_{};
    Device device_{};
    Init init_{};
    Shutdown shutdown_{};
    HandleByIndex handle_by_index_{};
    Clock clock_{};
    Temperature temperature_{};
    Power power_{};
};

// PDH counters, added by their English names so a localized Windows resolves them too.
class SystemCounters final {
public:
    SystemCounters() noexcept {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            query_ = nullptr;
            return;
        }
        // "% Processor Utility" is what Task Manager shows; older systems only have
        // "% Processor Time".
        if (!Add(L"\\Processor Information(_Total)\\% Processor Utility", cpu_)) {
            Add(L"\\Processor Information(_Total)\\% Processor Time", cpu_);
        }
        Add(L"\\Processor Information(_Total)\\% Processor Performance", performance_);
        Add(L"\\Processor Information(_Total)\\Processor Frequency", base_frequency_);
        Add(L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", gpu_);
        // The CPU package's RAPL meter, in milliwatts; present on recent AMD and Intel CPUs.
        Add(L"\\Energy Meter(RAPL_Package0_PKG)\\Power", cpu_power_);
        // Rate counters report a value only from the second collection on.
        PdhCollectQueryData(query_);
    }

    ~SystemCounters() {
        if (query_ != nullptr) PdhCloseQuery(query_);
    }

    SystemCounters(const SystemCounters&) = delete;
    SystemCounters& operator=(const SystemCounters&) = delete;

    Readings Read() {
        Readings readings;
        MEMORYSTATUSEX memory{sizeof(memory)};
        if (GlobalMemoryStatusEx(&memory) != FALSE) {
            readings.memory_percent = static_cast<float>(memory.dwMemoryLoad);
        }
        if (query_ == nullptr || PdhCollectQueryData(query_) != ERROR_SUCCESS) return readings;
        double value{};
        if (Value(cpu_, value)) readings.cpu_percent = static_cast<float>(std::clamp(value, 0.0, 100.0));
        double performance{};
        double base{};
        if (Value(performance_, performance) && Value(base_frequency_, base) && base > 0.0) {
            readings.cpu_ghz = static_cast<float>(base * performance / 100.0 / 1000.0);
        }
        if (gpu_ != nullptr) readings.gpu_percent = GpuPercent();
        if (Value(cpu_power_, value) && value >= 0.0) {
            readings.cpu_watts = static_cast<float>(value / 1000.0);
        }
        return readings;
    }

private:
    bool Add(const wchar_t* path, PDH_HCOUNTER& counter) noexcept {
        if (PdhAddEnglishCounterW(query_, path, 0, &counter) == ERROR_SUCCESS) return true;
        counter = nullptr;
        return false;
    }

    static bool Value(PDH_HCOUNTER counter, double& value) noexcept {
        if (counter == nullptr) return false;
        PDH_FMT_COUNTERVALUE formatted{};
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr,
                                        &formatted) != ERROR_SUCCESS ||
            (formatted.CStatus != PDH_CSTATUS_VALID_DATA &&
             formatted.CStatus != PDH_CSTATUS_NEW_DATA)) {
            return false;
        }
        value = formatted.doubleValue;
        return std::isfinite(value);
    }

    // One instance exists per process and 3D engine. Summing the processes of each engine
    // and taking the busiest engine matches Task Manager's per-GPU 3D figure; on a machine
    // with two GPUs that is the busier one.
    float GpuPercent() {
        DWORD bytes{};
        DWORD count{};
        PDH_STATUS status = PdhGetFormattedCounterArrayW(
            gpu_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
        if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bytes == 0) return -1.0F;
        buffer_.resize((bytes + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) - 1) /
                       sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
        auto* items = buffer_.data();
        status = PdhGetFormattedCounterArrayW(
            gpu_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items);
        if (status != ERROR_SUCCESS) return -1.0F;
        engines_.clear();
        for (DWORD index = 0; index < count; ++index) {
            const auto& item = items[index];
            if (item.szName == nullptr || (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                                           item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)) {
                continue;
            }
            // "pid_1234_luid_0x..._0x..._phys_0_eng_0_engtype_3D": the engine is the part
            // from "luid_" on.
            const std::wstring_view name(item.szName);
            const auto luid = name.find(L"luid_");
            if (luid == std::wstring_view::npos) continue;
            engines_[std::wstring(name.substr(luid))] += item.FmtValue.doubleValue;
        }
        double busiest{};
        for (const auto& [engine, load] : engines_) busiest = (std::max)(busiest, load);
        return static_cast<float>(std::clamp(busiest, 0.0, 100.0));
    }

    PDH_HQUERY query_{};
    PDH_HCOUNTER cpu_{};
    PDH_HCOUNTER performance_{};
    PDH_HCOUNTER base_frequency_{};
    PDH_HCOUNTER gpu_{};
    PDH_HCOUNTER cpu_power_{};
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buffer_;
    std::unordered_map<std::wstring, double> engines_;
};

// Frame timing from successive on_draw calls. The average and the 1% low are published
// twice a second so the figures stay readable instead of flickering every frame.
class FrameStats final {
public:
    void Tick(const std::chrono::steady_clock::time_point now) noexcept {
        if (last_frame_.time_since_epoch().count() != 0) {
            const float seconds = std::chrono::duration<float>(now - last_frame_).count();
            // A pause (alt-tab, a loading screen with the overlay idle) is not a frame.
            if (seconds > 0.0F && seconds < 1.0F) {
                times_[next_] = seconds;
                next_ = (next_ + 1) % times_.size();
                count_ = (std::min)(count_ + 1, times_.size());
                window_seconds_ += seconds;
                ++window_frames_;
            }
        } else {
            last_publish_ = now;
        }
        last_frame_ = now;
        if (now - last_publish_ < kPublishInterval) return;
        last_publish_ = now;
        if (window_frames_ != 0 && window_seconds_ > 0.0) {
            fps_ = static_cast<float>(window_frames_ / window_seconds_);
        }
        window_seconds_ = 0.0;
        window_frames_ = 0;
        low_ = OnePercentLow();
    }

    [[nodiscard]] float fps() const noexcept { return fps_; }
    [[nodiscard]] float low() const noexcept { return low_; }

private:
    // The average rate of the slowest 1% of the recent frames.
    float OnePercentLow() noexcept {
        if (count_ < 100) return -1.0F;
        std::copy_n(times_.begin(), count_, sorted_.begin());
        const std::size_t worst = count_ / 100;
        std::nth_element(sorted_.begin(), sorted_.begin() + static_cast<std::ptrdiff_t>(worst),
                         sorted_.begin() + static_cast<std::ptrdiff_t>(count_),
                         std::greater<float>());
        double total{};
        for (std::size_t index = 0; index < worst; ++index) total += sorted_[index];
        return total > 0.0 ? static_cast<float>(static_cast<double>(worst) / total) : -1.0F;
    }

    std::array<float, 3000> times_{};  // about 20 s at 144 FPS, 50 s at 60 FPS
    std::array<float, 3000> sorted_{};
    std::size_t next_{};
    std::size_t count_{};
    std::chrono::steady_clock::time_point last_frame_{};
    std::chrono::steady_clock::time_point last_publish_{};
    double window_seconds_{};
    std::uint32_t window_frames_{};
    float fps_{-1.0F};
    float low_{-1.0F};
};

// Localized strings, resolved once on load and immutable afterwards.
struct Texts final {
    std::string window_title;
    std::string help;
    std::string position;
    std::array<std::string, kPositionCount> positions;
    std::string items;
    std::string spacing;
    std::array<std::string, kToggleCount> toggles;
    std::string overlay_missing;
    std::string icons_missing;
};

struct Context final {
    const AnomalyUiServiceV1* ui{};
    const AnomalyFontServiceV1* font{};
    const AnomalyTextureServiceV1* texture{};
    anomaly::sdk::Localizer localizer;
    const AnomalyStorageServiceV1* storage{};
    Texts texts;

    // Resources requested in Start and released in Stop.
    AnomalyGenerationHandleV1 icon_font{};
    AnomalyGenerationHandleV1 logo{};

    // Owned by on_draw (Render thread) only.
    FrameStats frames;
    Settings settings;
    int window_open{1};

    // Shared with the sampler thread.
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping{};
    Readings readings;
    bool save_pending{};
    Settings pending_save;
    std::thread sampler;
};

void BuildTexts(Texts& texts, const anomaly::sdk::Localizer& localizer) {
    texts.window_title = localizer.Label("window.title", "Status Bar", "status-bar");
    texts.help = localizer.Text("help",
        "Closing this window keeps the status bar on screen; reopen it from the plugin "
        "list to change the settings.");
    texts.position = localizer.Text("settings.position", "Position");
    texts.positions[0] = localizer.Label("position.left", "Top left", "position-0");
    texts.positions[1] = localizer.Label("position.center", "Top center", "position-1");
    texts.positions[2] = localizer.Label("position.right", "Top right", "position-2");
    texts.positions[3] = localizer.Label("position.bottom_left", "Bottom left", "position-3");
    texts.positions[4] = localizer.Label("position.bottom_center", "Bottom center", "position-4");
    texts.positions[5] = localizer.Label("position.bottom_right", "Bottom right", "position-5");
    texts.items = localizer.Text("settings.items", "Show");
    texts.spacing = localizer.Label("settings.spacing", "Spacing (%)", "spacing");
    texts.toggles[kFps] = localizer.Label("item.fps", "Frame rate", "fps");
    texts.toggles[kLow] = localizer.Label("item.low", "1% low", "low");
    texts.toggles[kClock] = localizer.Label("item.clock", "Clock", "clock");
    texts.toggles[kMemory] = localizer.Label("item.memory", "Memory load", "memory");
    texts.toggles[kCpu] = localizer.Label("item.cpu", "CPU load", "cpu");
    texts.toggles[kFrequency] = localizer.Label("item.frequency", "CPU clock speed", "frequency");
    texts.toggles[kCpuPower] = localizer.Label("item.cpu_power", "CPU power", "cpu_power");
    texts.toggles[kGpu] = localizer.Label("item.gpu", "GPU load", "gpu");
    texts.toggles[kGpuClock] = localizer.Label("item.gpu_clock", "GPU clock speed", "gpu_clock");
    texts.toggles[kGpuTemperature] = localizer.Label("item.gpu_temperature", "GPU temperature", "gpu_temperature");
    texts.toggles[kGpuPower] = localizer.Label("item.gpu_power", "GPU power", "gpu_power");
    texts.toggles[kLogo] = localizer.Label("item.logo", "Logo", "logo");
    texts.overlay_missing = localizer.Text("status.overlay_missing",
        "This Anomaly runtime has no overlay drawing API: the status bar cannot be shown.");
    texts.icons_missing = localizer.Text("status.icons_missing",
        "The icon font did not load: this runtime has no icon glyph range, so only text "
        "is shown.");
}

// settings.txt holds "key=value" lines: "position=0..5" and one "<toggle>=0|1" per item.
// Unknown keys and bad values are ignored, so a damaged file falls back to the defaults.
Settings ParseSettings(const std::string_view document) {
    Settings settings;
    std::size_t start = 0;
    while (start < document.size()) {
        std::size_t end = document.find('\n', start);
        if (end == std::string_view::npos) end = document.size();
        std::string_view line = document.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, equals);
        const std::string_view text = line.substr(equals + 1);
        if (key == "spacing") {
            int spacing = 0;
            bool digits = !text.empty() && text.size() <= 3;
            for (const char digit : text) {
                if (digit < '0' || digit > '9') digits = false;
                spacing = spacing * 10 + (digit - '0');
            }
            if (digits && spacing >= kMinSpacing && spacing <= kMaxSpacing) {
                settings.spacing = spacing;
            }
            continue;
        }
        if (text.size() != 1) continue;
        const char value = text[0];
        if (key == "position") {
            if (value >= '0' && value < '0' + kPositionCount) {
                settings.position = static_cast<Position>(value - '0');
            }
            continue;
        }
        if (value != '0' && value != '1') continue;
        for (std::size_t index = 0; index < kToggleCount; ++index) {
            if (key == kToggleKeys[index]) settings.shown[index] = value == '1';
        }
    }
    return settings;
}

std::string FormatSettings(const Settings& settings) {
    std::string document = "position=" + std::to_string(static_cast<int>(settings.position)) + "\n";
    document += "spacing=" + std::to_string(settings.spacing) + "\n";
    for (std::size_t index = 0; index < kToggleCount; ++index) {
        document += std::string(kToggleKeys[index]) + (settings.shown[index] ? "=1\n" : "=0\n");
    }
    return document;
}

Settings LoadSettings(const AnomalyStorageServiceV1* storage) {
    if (storage == nullptr) return {};
    std::array<std::uint8_t, 1024> buffer{};
    std::size_t size = buffer.size();
    const AnomalyStatusV1 status = storage->read(
        storage->user, StringView(kSettingsPath), {buffer.data(), buffer.size()}, &size);
    if (!Succeeded(status) || size > buffer.size()) return {};
    return ParseSettings(std::string_view(reinterpret_cast<const char*>(buffer.data()), size));
}

// Runs on the sampler thread or in Stop: the host refuses storage I/O from on_draw.
void WriteSettings(const AnomalyStorageServiceV1* storage, const Settings& settings) {
    if (storage == nullptr) return;
    try {
        const std::string document = FormatSettings(settings);
        static_cast<void>(storage->write_atomic(
            storage->user, StringView(kSettingsPath),
            {reinterpret_cast<const std::uint8_t*>(document.data()), document.size()}));
    } catch (...) {
        // Not saved; the change still applies for this session.
    }
}

// Called from on_draw: hands the settings to the sampler thread, which writes them.
void QueueSave(Context& context) {
    {
        std::scoped_lock lock(context.mutex);
        context.pending_save = context.settings;
        context.save_pending = true;
    }
    context.wake.notify_all();
}

void SamplerLoop(Context* context) {
    SystemCounters counters;
    const NvidiaSensors nvidia;
    std::unique_lock lock(context->mutex);
    auto next_sample = std::chrono::steady_clock::now();
    while (!context->stopping) {
        if (context->save_pending) {
            const Settings settings = context->pending_save;
            context->save_pending = false;
            lock.unlock();
            WriteSettings(context->storage, settings);
            lock.lock();
            continue;
        }
        if (std::chrono::steady_clock::now() >= next_sample) {
            lock.unlock();
            Readings readings = counters.Read();
            nvidia.Read(readings);
            lock.lock();
            context->readings = readings;
            next_sample = std::chrono::steady_clock::now() + kSampleInterval;
        }
        context->wake.wait_until(lock, next_sample, [context] {
            return context->stopping || context->save_pending;
        });
    }
}

bool OverlayUsable(const AnomalyUiServiceV1* ui) noexcept {
    return STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, overlay_viewport_size) &&
           STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, overlay_glass) &&
           STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, overlay_measure_text) &&
           STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, overlay_text);
}

std::string Utf8(const char32_t codepoint) {
    std::string text;
    if (codepoint < 0x80) {
        text.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return text;
}

// Format only the number here; the surrounding units and labels belong to the
// localization catalog, which can reorder them without changing sensor values.
std::string Number(const anomaly::sdk::Localizer& localizer, const float value, const char* format) {
    char number[24]{};
    if (value < 0.0F) {
        return localizer.Text("reading.unavailable", "--");
    } else {
        std::snprintf(number, sizeof(number), format, value);
    }
    return number;
}

// Green below 70 C, orange to 85 C, red above.
std::uint32_t TemperatureColor(const float celsius, const std::uint32_t normal) noexcept {
    if (celsius < 70.0F) return normal;
    if (celsius < 85.0F) return ANOMALY_RGBA_V1(255u, 176u, 90u, 255u);
    return ANOMALY_RGBA_V1(255u, 105u, 105u, 255u);
}

std::string FrameText(const anomaly::sdk::Localizer& localizer, const float fps,
                      const float low, const bool show_fps, const bool show_low) {
    const auto fps_value = Number(localizer, fps, "%.0f");
    const auto low_value = Number(localizer, low, "%.0f");
    const auto fps_text = localizer.Format("bar.fps", "{0} FPS",
        std::array<std::string_view, 1>{fps_value});
    const auto low_text = localizer.Format("bar.low", "{0} (1% low)",
        std::array<std::string_view, 1>{low_value});
    if (!show_low) return fps_text;
    if (!show_fps) return low_text;
    // The 1% low needs 100 frames; until then only the average is shown.
    if (low < 0.0F) return fps_text;
    return localizer.Format("bar.frames", "{0}  {1}",
        std::array<std::string_view, 2>{fps_text, low_text});
}

std::uint32_t FrameColor(const float fps) noexcept {
    if (fps < 0.0F) return ANOMALY_RGBA_V1(235u, 238u, 242u, 235u);
    if (fps >= 60.0F) return ANOMALY_RGBA_V1(120u, 230u, 150u, 255u);
    if (fps >= 30.0F) return ANOMALY_RGBA_V1(255u, 176u, 90u, 255u);
    return ANOMALY_RGBA_V1(255u, 105u, 105u, 255u);
}

struct Size final {
    float width{};
    float height{};
};

Size Measure(const AnomalyUiServiceV1& ui, const std::string& text) {
    Size size;
    ui.overlay_measure_text(ui.user, StringView(text), 0.0F, &size.width, &size.height);
    return size;
}

struct Item final {
    std::string icon;
    std::string text;
    std::uint32_t color{};
    Size icon_size{};
    Size text_size{};
};

bool FontReady(const Context& context) {
    if (context.font == nullptr || context.icon_font.id == 0) return false;
    AnomalyFontStateV1 state{sizeof(state)};
    return Succeeded(context.font->state(context.font->user, context.icon_font, &state)) &&
           (state.flags & ANOMALY_FONT_STATE_V1_READY) != 0;
}

bool LogoReady(const Context& context) {
    if (!STATUS_HAS_FIELD(context.texture, AnomalyTextureServiceV1, draw_overlay) ||
        context.logo.id == 0) {
        return false;
    }
    AnomalyTextureStateV1 state{sizeof(state)};
    return Succeeded(context.texture->state(context.texture->user, context.logo, &state)) &&
           (state.flags & ANOMALY_TEXTURE_STATE_V1_READY) != 0;
}

// Runs `body` with the icon font pushed; returns false when it could not be pushed.
template <typename Body>
bool WithIconFont(const Context& context, Body body) {
    if (!Succeeded(context.font->push(context.font->user, context.icon_font))) return false;
    body();
    static_cast<void>(context.font->pop(context.font->user));
    return true;
}

void DrawBar(Context& context, const AnomalyUiServiceV1& ui, const Readings& readings) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    const auto& localizer = context.localizer;
    char hour[8]{}, minute[8]{};
    std::snprintf(hour, sizeof(hour), "%02u", local.wHour);
    std::snprintf(minute, sizeof(minute), "%02u", local.wMinute);
    const auto& shown = context.settings.shown;
    const std::uint32_t text = ANOMALY_RGBA_V1(235u, 238u, 242u, 235u);
    std::array<Item, kItemCount> items{};
    std::size_t count = 0;
    const auto add = [&](const char32_t icon, std::string label, const std::uint32_t color) {
        items[count++] = {Utf8(icon), std::move(label), color};
    };
    if (shown[kFps] || shown[kLow]) {
        const float fps = context.frames.fps();
        const float low = context.frames.low();
        add(kIconFps, FrameText(localizer, fps, low, shown[kFps], shown[kLow]),
            FrameColor(shown[kFps] ? fps : low));
    }
    if (shown[kClock]) add(kIconClock,
        localizer.Format("bar.clock", "{0}:{1}",
            std::array<std::string_view, 2>{hour, minute}), text);
    if (shown[kMemory]) add(kIconMemory,
        localizer.Format("bar.memory", "{0}% MEM",
            std::array<std::string_view, 1>{Number(localizer, readings.memory_percent, "%.0f")}), text);
    if (shown[kCpu]) add(kIconCpu,
        localizer.Format("bar.cpu", "{0}% CPU",
            std::array<std::string_view, 1>{Number(localizer, readings.cpu_percent, "%.0f")}), text);
    if (shown[kFrequency]) add(kIconFrequency,
        localizer.Format("bar.cpu_clock", "{0} GHz",
            std::array<std::string_view, 1>{Number(localizer, readings.cpu_ghz, "%.2f")}), text);
    if (shown[kCpuPower]) add(kIconPower,
        localizer.Format("bar.cpu_power", "{0} W CPU",
            std::array<std::string_view, 1>{Number(localizer, readings.cpu_watts, "%.0f")}), text);
    if (shown[kGpu]) add(kIconGpu,
        localizer.Format("bar.gpu", "{0}% GPU",
            std::array<std::string_view, 1>{Number(localizer, readings.gpu_percent, "%.0f")}), text);
    if (shown[kGpuClock]) {
        add(kIconGpuClock, localizer.Format("bar.gpu_clock", "{0} MHz GPU",
            std::array<std::string_view, 1>{Number(localizer, readings.gpu_mhz, "%.0f")}), text);
    }
    if (shown[kGpuTemperature]) {
        add(kIconTemperature, localizer.Format("bar.gpu_temperature", "{0}\u00B0C GPU",
                std::array<std::string_view, 1>{Number(localizer, readings.gpu_celsius, "%.0f")}),
            TemperatureColor(readings.gpu_celsius, text));
    }
    if (shown[kGpuPower]) add(kIconPower,
        localizer.Format("bar.gpu_power", "{0} W GPU",
            std::array<std::string_view, 1>{Number(localizer, readings.gpu_watts, "%.0f")}), text);
    const bool logo = shown[kLogo] && LogoReady(context);
    if (count == 0 && !logo) return;

    // The line height comes from the text font, measured on a fixed sample, so a bar with
    // only the logo still has a size. The icon font's line box is taller than its glyphs,
    // so it does not set the bar height.
    const float line = Measure(ui, "0").height;
    if (line <= 0.0F) return;
    for (std::size_t index = 0; index < count; ++index) {
        auto& item = items[index];
        item.text_size = Measure(ui, item.text);
    }
    const bool icons = count != 0 && FontReady(context) && WithIconFont(context, [&] {
        for (std::size_t index = 0; index < count; ++index) {
            auto& item = items[index];
            item.icon_size = Measure(ui, item.icon);
        }
    });

    // Layout follows the text size, so the bar scales with the Anomaly UI font. A
    // Material Symbols glyph sits in a square with about 2/24 of side bearing, so the
    // icon-to-text gap is small: the bearing already separates them.
    const float scale = static_cast<float>(context.settings.spacing) / 100.0F;
    const float padding = std::round(line * 0.7F);
    const float height = std::round(line * 1.55F);
    const float gap = std::round(line * 0.75F * scale);
    const float icon_gap = std::round(line * 0.15F);
    const float logo_size = std::round(line * 1.05F);
    float width = padding * 2.0F + (logo ? logo_size : 0.0F) + (logo && count != 0 ? gap : 0.0F);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& item = items[index];
        if (index != 0) width += gap;
        if (icons) width += item.icon_size.width + icon_gap;
        width += item.text_size.width;
    }

    float viewport_width{};
    float viewport_height{};
    ui.overlay_viewport_size(ui.user, &viewport_width, &viewport_height);
    if (viewport_width <= 0.0F || viewport_height <= 0.0F) return;
    const float margin = 10.0F;
    const int position = static_cast<int>(context.settings.position);
    const int column = position % 3;  // 0 left, 1 center, 2 right
    float x = margin;
    if (column == 1) x = std::round((viewport_width - width) * 0.5F);
    if (column == 2) x = viewport_width - width - margin;
    const float y = position < 3 ? margin : viewport_height - height - margin;
    const float middle = y + height * 0.5F;

    // Dark smoky glass so light text stays readable over a bright sky; no rim line.
    AnomalyUiGlassStyleV1 style{sizeof(style)};
    style.flags = ANOMALY_UI_GLASS_V1_NO_RIM;
    style.variant = ANOMALY_UI_GLASS_V1_REGULAR;
    style.tint_rgba = ANOMALY_RGBA_V1(18u, 20u, 26u, 120u);
    style.rounding = height * 0.5F;
    style.scale = 1.0F;
    style.opacity = 1.0F;
    style.refraction_scale = 1.3F;
    ui.overlay_glass(ui.user, x, y, width, height, &style);

    float cursor = x + padding;
    if (logo) {
        static_cast<void>(context.texture->draw_overlay(
            context.texture->user, context.logo, cursor, middle - logo_size * 0.5F, logo_size,
            logo_size, ANOMALY_RGBA_V1(255u, 255u, 255u, 255u)));
        cursor += logo_size + gap;
    }
    std::array<float, kItemCount> icon_x{};
    for (std::size_t index = 0; index < count; ++index) {
        const auto& item = items[index];
        if (index != 0) cursor += gap;
        if (icons) {
            icon_x[index] = cursor;
            cursor += item.icon_size.width + icon_gap;
        }
        ui.overlay_text(ui.user, cursor, std::round(middle - item.text_size.height * 0.5F),
                        StringView(item.text), item.color, 0.0F);
        cursor += item.text_size.width;
    }
    if (icons) {
        WithIconFont(context, [&] {
            for (std::size_t index = 0; index < count; ++index) {
                const auto& item = items[index];
                ui.overlay_text(ui.user, icon_x[index],
                                std::round(middle - item.icon_size.height * 0.5F),
                                StringView(item.icon), item.color, 0.0F);
            }
        });
    }
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    if (ui == nullptr) ui = context->ui;
    context->frames.Tick(std::chrono::steady_clock::now());
    const bool overlay = OverlayUsable(ui);
    if (overlay) {
        Readings readings;
        {
            std::scoped_lock lock(context->mutex);
            readings = context->readings;
        }
        DrawBar(*context, *ui, readings);
    }

    // With the window closed the host still calls on_draw (the ui-overlay capability) and
    // begin_window returns 0, so the bar above keeps drawing and the body below is skipped.
    anomaly::sdk::UiWindow window(ui, context->texts.window_title, &context->window_open);
    if (!window) return;
    ui->text(ui->user, StringView(context->texts.help));
    if (!overlay) ui->text(ui->user, StringView(context->texts.overlay_missing));
    if (context->font == nullptr || context->icon_font.id == 0) {
        ui->text(ui->user, StringView(context->texts.icons_missing));
    }
    ui->separator(ui->user);
    bool changed = false;
    ui->text(ui->user, StringView(context->texts.position));
    // Two rows of three, laid out like the screen corners they stand for.
    for (int index = 0; index < kPositionCount; ++index) {
        if (index % 3 != 0) ui->same_line(ui->user, 0.0F, 6.0F);
        const bool current = static_cast<int>(context->settings.position) == index;
        // The current choice is shown disabled, so the rows read as a segmented control.
        if (ui->button_enabled(ui->user,
                               StringView(context->texts.positions[static_cast<std::size_t>(index)]),
                               0.0F, 0.0F, current ? 0 : 1) != 0) {
            context->settings.position = static_cast<Position>(index);
            changed = true;
        }
    }
    ui->separator(ui->user);
    ui->text(ui->user, StringView(context->texts.items));
    for (std::size_t index = 0; index < kToggleCount; ++index) {
        if (index % 2 != 0) ui->same_line(ui->user, 180.0F, -1.0F);
        int value = context->settings.shown[index] ? 1 : 0;
        if (ui->checkbox(ui->user, StringView(context->texts.toggles[index]), &value) != 0) {
            context->settings.shown[index] = value != 0;
            changed = true;
        }
    }
    ui->separator(ui->user);
    float spacing = static_cast<float>(context->settings.spacing);
    if (ui->slider_float(ui->user, StringView(context->texts.spacing), &spacing,
                         static_cast<float>(kMinSpacing), static_cast<float>(kMaxSpacing)) != 0) {
        context->settings.spacing = static_cast<int>(std::lround(spacing));
        changed = true;
    }
    if (changed) QueueSave(*context);
}

template <typename Service>
const Service* Query(const AnomalyHostApiV1* host, const std::string_view id,
                     const std::uint32_t version) noexcept {
    return anomaly::sdk::Host(host).Query<Service>(id, version).get();
}

bool UiUsable(const AnomalyUiServiceV1* ui) noexcept {
    return STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, begin_window) &&
           STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, end_window) &&
           STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, text) &&
           STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, separator) &&
           STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, button_enabled) &&
           STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, checkbox) &&
           STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, slider_float) &&
           STATUS_HAS_FIELD(ui, AnomalyUiServiceV1, same_line);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (plugin_context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *plugin_context = nullptr;
    const auto* ui = Query<AnomalyUiServiceV1>(
        host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!UiUsable(ui)) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    auto* context = new (std::nothrow) Context();
    if (context == nullptr) return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    context->ui = ui;
    const auto* font = Query<AnomalyFontServiceV1>(
        host, ANOMALY_FONT_SERVICE_V1_ID, ANOMALY_FONT_SERVICE_V1_VERSION);
    context->font = STATUS_HAS_FIELD(font, AnomalyFontServiceV1, pop) &&
                    font->request != nullptr && font->release != nullptr &&
                    font->state != nullptr && font->push != nullptr
        ? font : nullptr;
    const auto* texture = Query<AnomalyTextureServiceV1>(
        host, ANOMALY_TEXTURE_SERVICE_V1_ID, ANOMALY_TEXTURE_SERVICE_V1_VERSION);
    context->texture = STATUS_HAS_FIELD(texture, AnomalyTextureServiceV1, draw) &&
                       texture->request != nullptr && texture->release != nullptr &&
                       texture->state != nullptr
        ? texture : nullptr;
    context->localizer = anomaly::sdk::Localizer(host);
    const auto* storage = Query<AnomalyStorageServiceV1>(
        host, ANOMALY_STORAGE_SERVICE_V1_ID, ANOMALY_STORAGE_SERVICE_V1_VERSION);
    context->storage = STATUS_HAS_FIELD(storage, AnomalyStorageServiceV1, write_atomic) &&
                       storage->read != nullptr
        ? storage : nullptr;
    try {
        BuildTexts(context->texts, context->localizer);
        context->settings = LoadSettings(context->storage);
    } catch (...) {
        delete context;
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

void ReleaseResources(Context& context) noexcept {
    if (context.icon_font.id != 0) {
        static_cast<void>(context.font->release(context.font->user, context.icon_font));
        context.icon_font = {};
    }
    if (context.logo.id != 0) {
        static_cast<void>(context.texture->release(context.texture->user, context.logo));
        context.logo = {};
    }
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    context->window_open = 1;
    // Both resources are optional: without them the bar shows text only.
    if (context->font != nullptr) {
        AnomalyFontRequestV1 request{sizeof(request)};
        request.relative_path = StringView("assets/MaterialSymbolsStatusBar.ttf");
        request.size_pixels = kIconFontPixels;
        request.glyph_range = ANOMALY_GLYPH_RANGE_V1_PRIVATE_USE;
        if (!Succeeded(context->font->request(context->font->user, &request, &context->icon_font))) {
            context->icon_font = {};
        }
    }
    if (context->texture != nullptr) {
        AnomalyTextureRequestV1 request{sizeof(request)};
        request.relative_path = StringView("assets/logo.png");
        request.format = ANOMALY_TEXTURE_FORMAT_V1_AUTO;
        if (!Succeeded(context->texture->request(context->texture->user, &request, &context->logo))) {
            context->logo = {};
        }
    }
    try {
        {
            std::scoped_lock lock(context->mutex);
            context->stopping = false;
            context->readings = {};
            context->save_pending = false;
        }
        context->sampler = std::thread(SamplerLoop, context);
    } catch (...) {
        ReleaseResources(*context);
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return anomaly::sdk::Ok();
    {
        std::scoped_lock lock(context->mutex);
        context->stopping = true;
    }
    context->wake.notify_all();
    if (context->sampler.joinable()) context->sampler.join();
    // A change made in the last moments before stopping is written here, off the
    // render thread.
    if (context->save_pending) {
        context->save_pending = false;
        WriteSettings(context->storage, context->pending_save);
    }
    ReleaseResources(*context);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    delete static_cast<Context*>(plugin_context);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("b1ank.status-bar"), StringView("Status Bar"),
        StringView("Local"), StringView("0.3.2"),
        Load, Start, Stop, Unload, nullptr, Draw};
    return anomaly::sdk::Ok();
}
