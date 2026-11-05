#pragma once

#include "damage_log.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nte::damage_log {

struct LogWorkerStatus {
    bool running{};
    bool ready{};
    std::size_t queue_depth{};
    std::uint64_t dropped_events{};
    std::uint64_t dropped_first_sequence{};
    std::uint64_t dropped_last_sequence{};
    std::string last_error;
    std::vector<ReportHistory> history;
};

class DamageLogWorker {
public:
    explicit DamageLogWorker(std::filesystem::path root);
    ~DamageLogWorker();
    DamageLogWorker(const DamageLogWorker&) = delete;
    DamageLogWorker& operator=(const DamageLogWorker&) = delete;

    bool Start();
    void Stop();
    bool Submit(const ReportOpen& command);
    bool Submit(const EncounterOpen& command);
    bool Submit(const LoggedCombatEvent& command);
    bool Submit(const EncounterClose& command);
    bool Submit(const ReportClose& command);
    bool FlushAndWait(std::chrono::milliseconds timeout, std::string& error);
    bool DeleteReport(account::AccountIdentity account, std::string report_id);
    [[nodiscard]] LogWorkerStatus Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::filesystem::path DefaultLogRoot();
[[nodiscard]] std::string GenerateUuidV4();

}  // namespace nte::damage_log
