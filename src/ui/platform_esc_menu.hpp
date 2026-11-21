#pragma once

#include "anomaly/nte_esc_menu_bridge.hpp"
#include "anomaly/structured_logger.hpp"
#include "anomaly/ue5_nte_adapter.hpp"
#include "plugin_manager.hpp"

#include <filesystem>
#include <fstream>

namespace ue5mem {

inline std::shared_ptr<anomaly::NteEscMenuBridge> StartPlatformEscMenuBridge(
    const std::filesystem::path& root,
    anomaly::CoreMemoryServices memory_services,
    anomaly::Ue5NteAdapter& adapter,
    const std::shared_ptr<PluginManager>& plugin_owner,
    const std::shared_ptr<anomaly::StructuredLogger>& bridge_logger) {
    auto bridge = std::make_shared<anomaly::NteEscMenuBridge>(
        memory_services, adapter.Resolution(),
        [weak = std::weak_ptr<PluginManager>(plugin_owner)] {
            const auto plugins = weak.lock();
            return plugins == nullptr
                ? std::vector<anomaly::NteEscMenuButtonSnapshot>{}
                : plugins->NteEscMenuButtons();
        },
        [weak = std::weak_ptr<PluginManager>(plugin_owner)](
            const AnomalyGenerationHandleV1 handle) {
            if (const auto plugins = weak.lock()) {
                static_cast<void>(plugins->InvokeNteEscMenuButton(handle));
            }
        },
        [bridge_logger](const std::uint32_t level, std::string message) {
            if (bridge_logger == nullptr) return;
            anomaly::LogLevel mapped = anomaly::LogLevel::Info;
            if (level >= ANOMALY_CORE_LOG_LEVEL_V1_ERROR) {
                mapped = anomaly::LogLevel::Error;
            } else if (level >= ANOMALY_CORE_LOG_LEVEL_V1_WARNING) {
                mapped = anomaly::LogLevel::Warning;
            } else if (level == ANOMALY_CORE_LOG_LEVEL_V1_TRACE) {
                mapped = anomaly::LogLevel::Trace;
            }
            anomaly::LogDetails details;
            details.thread_domain = anomaly::LogThreadDomain::Game;
            details.event_id = "nte.esc-menu-bridge";
            static_cast<void>(bridge_logger->Log(
                mapped, "nte.esc-menu", std::move(message), std::move(details)));
        });
    if (bridge->Start()) return bridge;
    std::ofstream(root / L"anomaly-platform.log", std::ios::app)
        << "NTE ESC menu bridge unavailable for active Profile\n";
    return {};
}

}  // namespace ue5mem
