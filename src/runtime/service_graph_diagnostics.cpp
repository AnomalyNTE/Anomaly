#include "anomaly/service_graph_diagnostics.hpp"

#include "json.hpp"

#include <string_view>

namespace anomaly {
namespace {

constexpr const char* Boolean(bool value) noexcept {
    return value ? "true" : "false";
}

std::string_view LifetimeName(ServiceLifetime lifetime) noexcept {
    switch (lifetime) {
    case ServiceLifetime::Provided: return "Provided";
    case ServiceLifetime::Singleton: return "Singleton";
    case ServiceLifetime::PluginScoped: return "PluginScoped";
    }
    return "Unknown";
}

std::string_view StartupName(ServiceStartup startup) noexcept {
    switch (startup) {
    case ServiceStartup::Blocking: return "Blocking";
    case ServiceStartup::Async: return "Async";
    case ServiceStartup::Lazy: return "Lazy";
    }
    return "Unknown";
}

std::string_view AffinityName(ServiceAffinity affinity) noexcept {
    switch (affinity) {
    case ServiceAffinity::Lifecycle: return "Lifecycle";
    case ServiceAffinity::Game: return "Game";
    case ServiceAffinity::Render: return "Render";
    case ServiceAffinity::Worker: return "Worker";
    case ServiceAffinity::Any: return "Any";
    }
    return "Unknown";
}

std::string_view StateName(ServiceState state) noexcept {
    switch (state) {
    case ServiceState::Registered: return "Registered";
    case ServiceState::Starting: return "Starting";
    case ServiceState::Ready: return "Ready";
    case ServiceState::Degraded: return "Degraded";
    case ServiceState::Failed: return "Failed";
    case ServiceState::Stopping: return "Stopping";
    case ServiceState::Stopped: return "Stopped";
    }
    return "Unknown";
}

}  // namespace

std::string SerializeServiceGraphSnapshotJson(const ServiceGraphSnapshot& snapshot) {
    std::string output;
    output.reserve(256 + snapshot.failures.size() * 96 + snapshot.services.size() * 256);
    output += "{\"built\":";
    output += Boolean(snapshot.built);
    output += ",\"stop_requested\":";
    output += Boolean(snapshot.stop_requested);
    output += ",\"startup_active\":";
    output += Boolean(snapshot.startup_active);
    output += ",\"blocking_startup_complete\":";
    output += Boolean(snapshot.blocking_startup_complete);
    output += ",\"async_startup_complete\":";
    output += Boolean(snapshot.async_startup_complete);
    output += ",\"error\":" + std::to_string(snapshot.error);
    output += ",\"async_startup_error\":" + std::to_string(snapshot.async_startup_error);
    output += ",\"failures\":[";
    for (std::size_t index = 0; index < snapshot.failures.size(); ++index) {
        const ServiceFailureSnapshot& failure = snapshot.failures[index];
        if (index != 0) output.push_back(',');
        output += "{\"service_id\":" + ue5mem::json::Quote(failure.service_id) +
            ",\"error\":" + std::to_string(failure.error) +
            ",\"caused_by\":" + ue5mem::json::Quote(failure.caused_by) + '}';
    }
    output += "],\"services\":[";
    for (std::size_t service_index = 0;
         service_index < snapshot.services.size(); ++service_index) {
        const ServiceSnapshot& service = snapshot.services[service_index];
        if (service_index != 0) output.push_back(',');
        output += "{\"id\":" + ue5mem::json::Quote(service.id) +
            ",\"version\":" + std::to_string(service.version) +
            ",\"lifetime\":" + ue5mem::json::Quote(LifetimeName(service.lifetime)) +
            ",\"startup\":" + ue5mem::json::Quote(StartupName(service.startup)) +
            ",\"affinity\":" + ue5mem::json::Quote(AffinityName(service.affinity)) +
            ",\"state\":" + ue5mem::json::Quote(StateName(service.state)) +
            ",\"error\":" + std::to_string(service.error) +
            ",\"startup_us\":" + std::to_string(service.startup_duration.count()) +
            ",\"start_thread_id\":" + std::to_string(service.start_thread_id) +
            ",\"stop_thread_id\":" + std::to_string(service.stop_thread_id) +
            ",\"start_queue_us\":" + std::to_string(service.start_queue_delay.count()) +
            ",\"stop_queue_us\":" + std::to_string(service.stop_queue_delay.count()) +
            ",\"affinity_bypassed\":" + Boolean(service.affinity_bypassed) +
            ",\"dependencies\":[";
        for (std::size_t dependency_index = 0;
             dependency_index < service.dependencies.size(); ++dependency_index) {
            const ServiceDependencySnapshot& dependency =
                service.dependencies[dependency_index];
            if (dependency_index != 0) output.push_back(',');
            output += "{\"id\":" + ue5mem::json::Quote(dependency.id) +
                ",\"minimum_version\":" + std::to_string(dependency.minimum_version) +
                ",\"optional\":";
            output += Boolean(dependency.optional);
            output += ",\"resolved\":";
            output += Boolean(dependency.resolved);
            output += ",\"resolved_version\":" +
                std::to_string(dependency.resolved_version) +
                ",\"state\":" + ue5mem::json::Quote(StateName(dependency.state)) + '}';
        }
        output += "],\"failure_chain\":[";
        for (std::size_t chain_index = 0;
             chain_index < service.failure_chain.size(); ++chain_index) {
            if (chain_index != 0) output.push_back(',');
            output += ue5mem::json::Quote(service.failure_chain[chain_index]);
        }
        output += "]}";
    }
    output += "]}";
    return output;
}

}  // namespace anomaly
