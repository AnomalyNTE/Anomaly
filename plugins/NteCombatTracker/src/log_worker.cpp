#include "log_worker.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <array>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>

namespace nte::damage_log {
namespace {

constexpr std::size_t kMaximumQueuedEvents = 8192;

struct DeleteCommand {
    account::AccountIdentity account;
    std::string report_id;
};
struct FlushState {
    std::mutex mutex;
    std::condition_variable completed;
    bool done{};
    bool succeeded{};
    std::string error;
};
struct FlushCommand { std::shared_ptr<FlushState> state; };
using Command = std::variant<ReportOpen, EncounterOpen, LoggedCombatEvent,
    EncounterClose, ReportClose, DeleteCommand, FlushCommand>;

}  // namespace

struct DamageLogWorker::Impl {
    explicit Impl(std::filesystem::path value) : root(std::move(value)) {}

    std::filesystem::path root;
    mutable std::mutex mutex;
    std::condition_variable available;
    std::deque<Command> commands;
    std::thread thread;
    LogWorkerStatus status;
    bool stopping{};
    std::size_t queued_events{};
    std::string accepting_encounter_id;

    bool SubmitControl(Command command) {
        std::scoped_lock lock(mutex);
        if (!status.running || stopping) return false;
        commands.push_back(std::move(command));
        status.queue_depth = commands.size();
        available.notify_one();
        return true;
    }

    bool SubmitEvent(LoggedCombatEvent command) {
        std::scoped_lock lock(mutex);
        if (!status.running || stopping) return false;
        if (command.encounter_id.empty() ||
            command.encounter_id != accepting_encounter_id) return false;
        if (queued_events >= kMaximumQueuedEvents) {
            ++status.dropped_events;
            if (status.dropped_first_sequence == 0) {
                status.dropped_first_sequence = command.event.sequence;
            }
            status.dropped_last_sequence = command.event.sequence;
            return false;
        }
        commands.emplace_back(std::move(command));
        ++queued_events;
        status.queue_depth = commands.size();
        available.notify_one();
        return true;
    }

    bool SubmitEncounterOpen(EncounterOpen command) {
        std::scoped_lock lock(mutex);
        if (!status.running || stopping || !accepting_encounter_id.empty()) {
            return false;
        }
        accepting_encounter_id = command.encounter_id;
        commands.emplace_back(std::move(command));
        status.queue_depth = commands.size();
        available.notify_one();
        return true;
    }

    bool SubmitEncounterClose(EncounterClose command) {
        std::scoped_lock lock(mutex);
        if (!status.running || stopping || accepting_encounter_id.empty() ||
            accepting_encounter_id != command.encounter_id) return false;
        accepting_encounter_id.clear();
        commands.emplace_back(std::move(command));
        status.queue_depth = commands.size();
        available.notify_one();
        return true;
    }

    void Run() {
        DamageLogRepository repository(root);
        std::string error;
        const bool recovered = repository.Recover(error);
        {
            std::scoped_lock lock(mutex);
            status.ready = recovered;
            status.last_error = error;
            status.history = repository.History();
        }
        auto last_flush = std::chrono::steady_clock::now();
        bool pending_write_failure{};
        std::string pending_write_error;
        std::uint64_t encounter_failed_events{};
        std::uint64_t encounter_failed_first_sequence{};
        std::uint64_t encounter_failed_last_sequence{};
        std::vector<std::pair<std::uint64_t, std::uint64_t>> encounter_failed_ranges;
        std::string encounter_write_error;
        for (;;) {
            Command command;
            bool has_command{};
            {
                std::unique_lock lock(mutex);
                available.wait_for(lock, std::chrono::seconds(1), [&] {
                    return stopping || !commands.empty();
                });
                if (!commands.empty()) {
                    command = std::move(commands.front());
                    if (std::holds_alternative<LoggedCombatEvent>(command)) --queued_events;
                    commands.pop_front();
                    status.queue_depth = commands.size();
                    has_command = true;
                } else if (stopping) {
                    break;
                }
            }
            bool succeeded = true;
            error.clear();
            bool refresh_history{};
            bool reset_drop_status{};
            std::shared_ptr<FlushState> flush_state;
            try {
            if (has_command) {
                std::visit([&](auto& value) {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, ReportOpen>) {
                        succeeded = repository.BeginReport(value, error);
                        if (succeeded) {
                            pending_write_failure = false;
                            pending_write_error.clear();
                        }
                        reset_drop_status = succeeded;
                        refresh_history = true;
                    } else if constexpr (std::is_same_v<Value, EncounterOpen>) {
                        succeeded = repository.BeginEncounter(value, error);
                        if (succeeded) {
                            encounter_failed_events = 0;
                            encounter_failed_first_sequence = 0;
                            encounter_failed_last_sequence = 0;
                            encounter_failed_ranges.clear();
                            encounter_write_error.clear();
                        }
                    } else if constexpr (std::is_same_v<Value, LoggedCombatEvent>) {
                        succeeded = repository.Append(value, error);
                        if (!succeeded) {
                            ++encounter_failed_events;
                            if (encounter_failed_first_sequence == 0) {
                                encounter_failed_first_sequence = value.event.sequence;
                            }
                            encounter_failed_last_sequence = value.event.sequence;
                            if (!encounter_failed_ranges.empty() &&
                                encounter_failed_ranges.back().second + 1 == value.event.sequence) {
                                encounter_failed_ranges.back().second = value.event.sequence;
                            } else if (encounter_failed_ranges.size() < 64) {
                                encounter_failed_ranges.emplace_back(
                                    value.event.sequence, value.event.sequence);
                            }
                            encounter_write_error = error.empty() ? "event write failed" : error;
                        }
                    } else if constexpr (std::is_same_v<Value, EncounterClose>) {
                        if (pending_write_failure || encounter_failed_events != 0) {
                            value.quality.partial = true;
                            value.dropped_events += encounter_failed_events;
                            if (value.dropped_first_sequence == 0) {
                                value.dropped_first_sequence = encounter_failed_first_sequence;
                            }
                            value.dropped_last_sequence = (std::max)(
                                value.dropped_last_sequence, encounter_failed_last_sequence);
                            value.missing_sequence_ranges.insert(
                                value.missing_sequence_ranges.end(),
                                encounter_failed_ranges.begin(), encounter_failed_ranges.end());
                            if (value.missing_sequence_ranges.size() > 64) {
                                value.missing_sequence_ranges.resize(64);
                                value.missing_ranges_truncated = true;
                            }
                            if (value.write_error.empty()) {
                                value.write_error = encounter_write_error.empty()
                                    ? pending_write_error : encounter_write_error;
                            }
                        }
                        succeeded = repository.EndEncounter(value, error);
                        if (succeeded) {
                            pending_write_failure = false;
                        } else {
                            const std::string encounter_error = error.empty()
                                ? "encounter close failed" : error;
                            std::string abort_error;
                            static_cast<void>(repository.CloseReport(ReportClose{
                                value.reason, value.closed_utc_ms, true, true,
                                encounter_error}, abort_error));
                            if (!abort_error.empty()) {
                                error = encounter_error + "; abort failed: " + abort_error;
                            } else {
                                error = encounter_error;
                            }
                        }
                        refresh_history = true;
                    } else if constexpr (std::is_same_v<Value, ReportClose>) {
                        if (pending_write_failure) {
                            value.incomplete = true;
                            if (value.write_error.empty()) value.write_error = pending_write_error;
                        }
                        succeeded = repository.CloseReport(value, error);
                        refresh_history = true;
                    } else if constexpr (std::is_same_v<Value, DeleteCommand>) {
                        succeeded = repository.DeleteReport(
                            value.account, value.report_id, error);
                        refresh_history = true;
                    } else if constexpr (std::is_same_v<Value, FlushCommand>) {
                        flush_state = value.state;
                        succeeded = repository.Flush(error);
                        refresh_history = true;
                    }
                }, command);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= std::chrono::seconds(1)) {
                std::string flush_error;
                if (!repository.Flush(flush_error)) {
                    succeeded = false;
                    if (error.empty()) error = std::move(flush_error);
                }
                refresh_history = true;
                last_flush = now;
            }
            } catch (const std::exception& exception) {
                succeeded = false;
                error = std::string("log worker exception: ") + exception.what();
            } catch (...) {
                succeeded = false;
                error = "unknown log worker exception";
            }
            std::scoped_lock lock(mutex);
            if (reset_drop_status) {
                status.dropped_events = 0;
                status.dropped_first_sequence = 0;
                status.dropped_last_sequence = 0;
                status.last_error.clear();
            }
            if (!succeeded) {
                pending_write_failure = true;
                pending_write_error = error.empty() ? "log write failed" : error;
                status.last_error = pending_write_error;
            }
            if (refresh_history) status.history = repository.History();
            if (flush_state) {
                std::scoped_lock completion_lock(flush_state->mutex);
                flush_state->succeeded = succeeded && !pending_write_failure;
                flush_state->error = flush_state->succeeded ? std::string{} :
                    (status.last_error.empty() ? "log flush failed" : status.last_error);
                flush_state->done = true;
                flush_state->completed.notify_all();
            }
        }
        std::string flush_error;
        if (!repository.Flush(flush_error)) {
            std::scoped_lock lock(mutex);
            status.last_error = std::move(flush_error);
        }
        std::scoped_lock lock(mutex);
        status.history = repository.History();
        status.ready = false;
    }
};

DamageLogWorker::DamageLogWorker(std::filesystem::path root)
    : impl_(std::make_unique<Impl>(std::move(root))) {}

DamageLogWorker::~DamageLogWorker() {
    Stop();
}

bool DamageLogWorker::Start() {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->status.running) return true;
    impl_->stopping = false;
    impl_->status = {};
    impl_->accepting_encounter_id.clear();
    impl_->status.running = true;
    try {
        impl_->thread = std::thread([this] {
            try {
                impl_->Run();
            } catch (const std::exception& exception) {
                std::scoped_lock lock(impl_->mutex);
                impl_->status.ready = false;
                impl_->stopping = true;
                impl_->status.last_error =
                    std::string("log worker terminated safely: ") + exception.what();
            } catch (...) {
                std::scoped_lock lock(impl_->mutex);
                impl_->status.ready = false;
                impl_->stopping = true;
                impl_->status.last_error = "log worker terminated safely: unknown exception";
            }
        });
    } catch (...) {
        impl_->status.running = false;
        impl_->status.last_error = "failed to create log writer thread";
        return false;
    }
    return true;
}

void DamageLogWorker::Stop() {
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->status.running) return;
        impl_->stopping = true;
        impl_->available.notify_one();
    }
    if (impl_->thread.joinable()) impl_->thread.join();
    std::scoped_lock lock(impl_->mutex);
    impl_->status.running = false;
    impl_->status.queue_depth = 0;
    impl_->commands.clear();
    impl_->queued_events = 0;
    impl_->accepting_encounter_id.clear();
}

bool DamageLogWorker::Submit(const ReportOpen& command) {
    return impl_->SubmitControl(command);
}
bool DamageLogWorker::Submit(const EncounterOpen& command) {
    return impl_->SubmitEncounterOpen(command);
}
bool DamageLogWorker::Submit(const LoggedCombatEvent& command) {
    return impl_->SubmitEvent(command);
}
bool DamageLogWorker::Submit(const EncounterClose& command) {
    return impl_->SubmitEncounterClose(command);
}
bool DamageLogWorker::Submit(const ReportClose& command) {
    return impl_->SubmitControl(command);
}
bool DamageLogWorker::FlushAndWait(
    const std::chrono::milliseconds timeout,
    std::string& error) {
    error.clear();
    auto state = std::make_shared<FlushState>();
    if (!impl_->SubmitControl(FlushCommand{state})) {
        error = "log worker is not accepting commands";
        return false;
    }
    std::unique_lock lock(state->mutex);
    if (!state->completed.wait_for(lock, timeout, [&] { return state->done; })) {
        error = "timed out waiting for log flush";
        return false;
    }
    error = state->error;
    return state->succeeded;
}
bool DamageLogWorker::DeleteReport(
    account::AccountIdentity account, std::string report_id) {
    return impl_->SubmitControl(DeleteCommand{
        std::move(account), std::move(report_id)});
}

LogWorkerStatus DamageLogWorker::Status() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->status;
}

std::filesystem::path DefaultLogRoot() {
    PWSTR local_app_data{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,
            KF_FLAG_CREATE, nullptr, &local_app_data))) return {};
    const std::filesystem::path root = std::filesystem::path(local_app_data) /
        "NTECombatStats" / "logs" / "v2";
    CoTaskMemFree(local_app_data);
    return root;
}

std::string GenerateUuidV4() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream value;
    value << std::hex << std::setfill('0');
    for (std::size_t index{}; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) value << '-';
        value << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return value.str();
}

}  // namespace nte::damage_log
