#pragma once

#include "history_reader.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <stop_token>

namespace nte::combat_stats::overlay {

[[nodiscard]] std::string SerializeCombatExport(
    const EncounterDetail& detail,
    const std::string& producer_version,
    std::int64_t exported_utc_ms,
    std::stop_token stop = {});

[[nodiscard]] std::filesystem::path SuggestedCombatExportFilename(
    const HistoricalEncounter& encounter);

[[nodiscard]] bool WriteCombatExport(
    const std::filesystem::path& directory,
    const EncounterDetail& detail,
    const std::string& producer_version,
    std::int64_t exported_utc_ms,
    std::filesystem::path& written_path,
    std::string& error,
    std::stop_token stop = {});

}  // namespace nte::combat_stats::overlay
