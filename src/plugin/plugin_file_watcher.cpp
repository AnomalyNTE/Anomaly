#include "anomaly/plugin_file_watcher.hpp"

#include <Windows.h>

#include <algorithm>
#include <sstream>
#include <thread>
#include <utility>

namespace anomaly {
namespace {

std::string Utf8(const std::filesystem::path& path) {
    const std::u8string value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string PackageSignature(const std::filesystem::path& package) {
    std::vector<std::string> records;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(package, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error) break;
        const std::filesystem::path relative = iterator->path().lexically_relative(package);
        if (std::filesystem::is_symlink(status)) {
            records.push_back(Utf8(relative) + ":reparse");
        } else if (std::filesystem::is_regular_file(status)) {
            const auto size = iterator->file_size(error);
            if (error) break;
            const auto write_time = iterator->last_write_time(error);
            if (error) break;
            records.push_back(
                Utf8(relative) + ':' + std::to_string(size) + ':' +
                std::to_string(write_time.time_since_epoch().count()));
        }
    }
    if (error) return "error:" + std::to_string(error.value());
    std::sort(records.begin(), records.end());
    std::string result;
    for (const std::string& record : records) {
        result.append(record);
        result.push_back('\n');
    }
    return result;
}

}  // namespace

PluginFileWatcher::PluginFileWatcher(
    std::filesystem::path plugin_root, PluginFileWatcherOptions options)
    : plugin_root_(std::move(plugin_root)), options_(options) {}

PluginFileWatcher::~PluginFileWatcher() { Stop(); }

bool PluginFileWatcher::Start(Callback callback) {
    std::scoped_lock lock(mutex_);
    if (worker_.joinable() || !callback) return false;
    const HANDLE change = FindFirstChangeNotificationW(
        plugin_root_.c_str(), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_CREATION);
    if (change == INVALID_HANDLE_VALUE) return false;
    try {
        observations_.clear();
        initialized_ = false;
        static_cast<void>(PollLocked(Clock::now()));
        callback_ = std::move(callback);
        worker_ = std::jthread([this, change](std::stop_token token) {
            Run(token, change);
        });
    } catch (...) {
        static_cast<void>(FindCloseChangeNotification(change));
        callback_ = {};
        return false;
    }
    return true;
}

void PluginFileWatcher::Stop() noexcept {
    std::jthread worker;
    {
        std::scoped_lock lock(mutex_);
        if (!worker_.joinable()) return;
        worker_.request_stop();
        worker = std::move(worker_);
    }
    worker.join();
    std::scoped_lock lock(mutex_);
    callback_ = {};
}

bool PluginFileWatcher::Running() const noexcept {
    std::scoped_lock lock(mutex_);
    return worker_.joinable();
}

void PluginFileWatcher::SetDiagnosticsEnabled(const bool enabled) noexcept {
    if (diagnostics_enabled_.exchange(enabled, std::memory_order_acq_rel) == enabled ||
        !enabled) {
        return;
    }
    notifications_.store(0, std::memory_order_relaxed);
    scans_.store(0, std::memory_order_relaxed);
    changed_packages_.store(0, std::memory_order_relaxed);
    total_scan_nanoseconds_.store(0, std::memory_order_relaxed);
    maximum_scan_nanoseconds_.store(0, std::memory_order_relaxed);
}

PluginFileWatcherDiagnostics PluginFileWatcher::Diagnostics() const noexcept {
    return {
        notifications_.load(std::memory_order_relaxed),
        scans_.load(std::memory_order_relaxed),
        changed_packages_.load(std::memory_order_relaxed),
        total_scan_nanoseconds_.load(std::memory_order_relaxed),
        maximum_scan_nanoseconds_.load(std::memory_order_relaxed)};
}

void PluginFileWatcher::ResetBaseline() noexcept {
    std::scoped_lock lock(mutex_);
    observations_.clear();
    initialized_ = false;
}

std::unordered_map<std::string, std::string> PluginFileWatcher::Scan() const {
    const bool measure = diagnostics_enabled_.load(std::memory_order_relaxed);
    const auto started = measure ? Clock::now() : Clock::time_point{};
    std::unordered_map<std::string, std::string> result;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(plugin_root_, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->path().filename() == L".cache") continue;
        if (!iterator->is_directory(error) || iterator->is_symlink(error)) {
            error.clear();
            continue;
        }
        result.emplace(Utf8(iterator->path().filename()), PackageSignature(iterator->path()));
    }
    if (measure) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started).count();
        const auto value = static_cast<std::uint64_t>((std::max)(elapsed, std::int64_t{}));
        total_scan_nanoseconds_.fetch_add(value, std::memory_order_relaxed);
        std::uint64_t maximum = maximum_scan_nanoseconds_.load(std::memory_order_relaxed);
        while (maximum < value && !maximum_scan_nanoseconds_.compare_exchange_weak(
                   maximum, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
        scans_.fetch_add(1, std::memory_order_release);
    }
    return result;
}

std::vector<std::string> PluginFileWatcher::PollLocked(Clock::time_point now) {
    const auto current = Scan();
    if (!initialized_) {
        initialized_ = true;
        for (const auto& [id, signature] : current) {
            observations_.emplace(id, Observation{signature, now, false});
        }
        return {};
    }
    for (const auto& [id, signature] : current) {
        auto [position, inserted] = observations_.try_emplace(
            id, Observation{signature, now, true});
        if (!inserted && position->second.signature != signature) {
            position->second.signature = signature;
            position->second.changed_at = now;
            position->second.pending = true;
        }
    }
    for (auto& [id, observation] : observations_) {
        if (!current.contains(id) && observation.signature != "<removed>") {
            observation.signature = "<removed>";
            observation.changed_at = now;
            observation.pending = true;
        }
    }
    std::vector<std::string> changed;
    for (auto iterator = observations_.begin(); iterator != observations_.end();) {
        Observation& observation = iterator->second;
        if (observation.pending && now - observation.changed_at >= options_.debounce) {
            observation.pending = false;
            changed.push_back(iterator->first);
            if (observation.signature == "<removed>") {
                iterator = observations_.erase(iterator);
                continue;
            }
        }
        ++iterator;
    }
    std::sort(changed.begin(), changed.end());
    if (diagnostics_enabled_.load(std::memory_order_relaxed)) {
        changed_packages_.fetch_add(changed.size(), std::memory_order_relaxed);
    }
    return changed;
}

std::vector<std::string> PluginFileWatcher::PollForTests(Clock::time_point now) {
    std::scoped_lock lock(mutex_);
    return PollLocked(now);
}

void PluginFileWatcher::Run(std::stop_token stop_token, void* change_handle) {
    const HANDLE change = static_cast<HANDLE>(change_handle);
    const HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
        static_cast<void>(FindCloseChangeNotification(change));
        return;
    }
    std::stop_callback stop_callback(stop_token, [stop_event] {
        static_cast<void>(SetEvent(stop_event));
    });
    const HANDLE events[]{stop_event, change};
    const auto publish_scan = [this] {
        std::vector<std::string> changed;
        Callback callback;
        {
            std::scoped_lock lock(mutex_);
            changed = PollLocked(Clock::now());
            callback = callback_;
        }
        if (!changed.empty() && callback) callback(std::move(changed));
    };
    const auto wait_timeout = [](const Clock::duration remaining) {
        if (remaining <= Clock::duration::zero()) return DWORD{};
        const auto rounded = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        return static_cast<DWORD>((std::min)(
            rounded, static_cast<std::int64_t>(INFINITE - 1)));
    };

    bool running = true;
    while (running) {
        const DWORD signaled = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (signaled == WAIT_OBJECT_0) break;
        if (signaled != WAIT_OBJECT_0 + 1) break;
        if (diagnostics_enabled_.load(std::memory_order_relaxed)) {
            notifications_.fetch_add(1, std::memory_order_relaxed);
        }
        publish_scan();
        if (FindNextChangeNotification(change) == FALSE) break;

        auto deadline = Clock::now() +
            (std::max)(options_.debounce, std::chrono::milliseconds::zero());
        while (running) {
            const DWORD settled = WaitForMultipleObjects(
                2, events, FALSE, wait_timeout(deadline - Clock::now()));
            if (settled == WAIT_OBJECT_0) {
                running = false;
                break;
            }
            if (settled == WAIT_OBJECT_0 + 1) {
                if (diagnostics_enabled_.load(std::memory_order_relaxed)) {
                    notifications_.fetch_add(1, std::memory_order_relaxed);
                }
                publish_scan();
                if (FindNextChangeNotification(change) == FALSE) {
                    running = false;
                    break;
                }
                deadline = Clock::now() +
                    (std::max)(options_.debounce, std::chrono::milliseconds::zero());
                continue;
            }
            if (settled == WAIT_TIMEOUT) {
                publish_scan();
            }
            break;
        }
    }
    static_cast<void>(FindCloseChangeNotification(change));
    CloseHandle(stop_event);
}

}  // namespace anomaly
