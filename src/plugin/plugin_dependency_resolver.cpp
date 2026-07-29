#include "anomaly/plugin_dependency_resolver.hpp"

#include <algorithm>
#include <functional>
#include <queue>
#include <set>
#include <unordered_set>
#include <utility>

namespace anomaly {

CompatibilityIndex::CompatibilityIndex(
    const PluginCatalogSnapshot& catalog,
    std::vector<AvailableServiceVersion> services)
    : plugins_(catalog.AvailablePlugins()), services_(std::move(services)) {
    for (std::size_t index = 0; index < plugins_.size(); ++index) {
        if (!plugin_index_.emplace(plugins_[index].id, index).second) {
            duplicate_ids_.push_back(plugins_[index].id);
        }
    }
    for (std::size_t index = 0; index < services_.size(); ++index) {
        if (!service_index_.emplace(services_[index].id, index).second) {
            duplicate_ids_.push_back(services_[index].id);
        }
    }
    std::sort(duplicate_ids_.begin(), duplicate_ids_.end());
    duplicate_ids_.erase(std::unique(duplicate_ids_.begin(), duplicate_ids_.end()), duplicate_ids_.end());
}

const AvailablePluginVersion* CompatibilityIndex::Plugin(std::string_view id) const noexcept {
    const auto found = plugin_index_.find(std::string(id));
    return found == plugin_index_.end() ? nullptr : &plugins_[found->second];
}

const AvailableServiceVersion* CompatibilityIndex::Service(std::string_view id) const noexcept {
    const auto found = service_index_.find(std::string(id));
    return found == service_index_.end() ? nullptr : &services_[found->second];
}

const PluginDependencyNode* PluginDependencyPlan::Find(std::string_view id) const noexcept {
    const auto found = std::find_if(nodes.begin(), nodes.end(),
        [&](const PluginDependencyNode& node) { return node.id == id; });
    return found == nodes.end() ? nullptr : &*found;
}

PluginDependencyPlan ResolvePluginDependencies(const PluginCatalogSnapshot& catalog) {
    PluginDependencyPlan plan;
    std::unordered_map<std::string, std::size_t> node_index;
    std::unordered_map<std::string, const PluginCatalogEntry*> entries;
    for (const PluginCatalogEntry& entry : catalog.Entries()) {
        if (!entry.manifest) continue;
        PluginDependencyNode node{entry.manifest->id};
        if (!entry.LoadCandidate()) {
            node.state = PluginDependencyState::InvalidCatalog;
            node.diagnostics.push_back("catalog entry is not a load candidate");
        }
        node_index.emplace(node.id, plan.nodes.size());
        entries.emplace(node.id, &entry);
        plan.nodes.push_back(std::move(node));
    }
    std::sort(plan.nodes.begin(), plan.nodes.end(),
        [](const PluginDependencyNode& left, const PluginDependencyNode& right) {
            return left.id < right.id;
        });
    node_index.clear();
    for (std::size_t index = 0; index < plan.nodes.size(); ++index) {
        node_index.emplace(plan.nodes[index].id, index);
    }

    std::vector<std::vector<std::size_t>> outgoing(plan.nodes.size());
    std::vector<std::vector<std::size_t>> required_dependencies(plan.nodes.size());
    for (std::size_t consumer = 0; consumer < plan.nodes.size(); ++consumer) {
        PluginDependencyNode& node = plan.nodes[consumer];
        if (node.state != PluginDependencyState::Ready) continue;
        const PluginManifest& manifest = *entries.at(node.id)->manifest;
        for (const PluginDependencyManifest& dependency : manifest.dependencies) {
            const auto dependency_node = node_index.find(dependency.id);
            if (dependency_node == node_index.end() ||
                plan.nodes[dependency_node->second].state == PluginDependencyState::InvalidCatalog) {
                if (!dependency.optional) {
                    node.state = PluginDependencyState::MissingDependency;
                    node.diagnostics.push_back("missing dependency: " + dependency.id);
                }
                continue;
            }
            const PluginManifest& available = *entries.at(dependency.id)->manifest;
            if (!dependency.version_range.Matches(available.version)) {
                if (!dependency.optional) {
                    node.state = PluginDependencyState::VersionConflict;
                    node.diagnostics.push_back(
                        "dependency version conflict: " + dependency.id + " expected " +
                        dependency.version_expression + " actual " + available.version.ToString());
                }
                continue;
            }
            if (!dependency.optional) required_dependencies[consumer].push_back(dependency_node->second);
            outgoing[dependency_node->second].push_back(consumer);
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t consumer = 0; consumer < plan.nodes.size(); ++consumer) {
            PluginDependencyNode& node = plan.nodes[consumer];
            if (node.state != PluginDependencyState::Ready) continue;
            for (const std::size_t dependency : required_dependencies[consumer]) {
                const PluginDependencyState state = plan.nodes[dependency].state;
                if (state != PluginDependencyState::Ready) {
                    node.state = PluginDependencyState::BlockedTransitively;
                    node.diagnostics.push_back(
                        "blocked by dependency: " + plan.nodes[dependency].id);
                    changed = true;
                    break;
                }
            }
        }
    }

    std::vector<int> tarjan_index(plan.nodes.size(), -1);
    std::vector<int> low_link(plan.nodes.size(), -1);
    std::vector<bool> on_stack(plan.nodes.size());
    std::vector<std::size_t> stack;
    int next_index{};
    std::function<void(std::size_t)> strong_connect = [&](std::size_t node) {
        tarjan_index[node] = low_link[node] = next_index++;
        stack.push_back(node);
        on_stack[node] = true;
        for (const std::size_t dependency : required_dependencies[node]) {
            if (plan.nodes[dependency].state != PluginDependencyState::Ready) continue;
            if (tarjan_index[dependency] < 0) {
                strong_connect(dependency);
                low_link[node] = (std::min)(low_link[node], low_link[dependency]);
            } else if (on_stack[dependency]) {
                low_link[node] = (std::min)(low_link[node], tarjan_index[dependency]);
            }
        }
        if (low_link[node] != tarjan_index[node]) return;
        std::vector<std::size_t> component;
        for (;;) {
            const std::size_t current = stack.back();
            stack.pop_back();
            on_stack[current] = false;
            component.push_back(current);
            if (current == node) break;
        }
        const bool self_cycle = component.size() == 1 &&
            std::find(required_dependencies[node].begin(), required_dependencies[node].end(), node) !=
                required_dependencies[node].end();
        if (component.size() > 1 || self_cycle) {
            for (const std::size_t member : component) {
                plan.nodes[member].state = PluginDependencyState::DependencyCycle;
                plan.nodes[member].diagnostics.push_back("dependency cycle");
            }
        }
    };
    for (std::size_t index = 0; index < plan.nodes.size(); ++index) {
        if (plan.nodes[index].state == PluginDependencyState::Ready && tarjan_index[index] < 0) {
            strong_connect(index);
        }
    }

    changed = true;
    while (changed) {
        changed = false;
        for (std::size_t consumer = 0; consumer < plan.nodes.size(); ++consumer) {
            PluginDependencyNode& node = plan.nodes[consumer];
            if (node.state != PluginDependencyState::Ready) continue;
            for (const std::size_t dependency : required_dependencies[consumer]) {
                if (plan.nodes[dependency].state != PluginDependencyState::Ready) {
                    node.state = PluginDependencyState::BlockedTransitively;
                    node.diagnostics.push_back(
                        "blocked by dependency: " + plan.nodes[dependency].id);
                    changed = true;
                    break;
                }
            }
        }
    }

    std::vector<std::size_t> indegree(plan.nodes.size());
    std::set<std::string> ready;
    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        if (plan.nodes[node].state != PluginDependencyState::Ready) continue;
        for (const std::size_t dependency : required_dependencies[node]) {
            if (plan.nodes[dependency].state == PluginDependencyState::Ready) ++indegree[node];
        }
        if (indegree[node] == 0) ready.insert(plan.nodes[node].id);
    }
    while (!ready.empty()) {
        const std::string id = *ready.begin();
        ready.erase(ready.begin());
        const std::size_t node = node_index.at(id);
        plan.load_order.push_back(id);
        for (const std::size_t consumer : outgoing[node]) {
            if (plan.nodes[consumer].state != PluginDependencyState::Ready || indegree[consumer] == 0) {
                continue;
            }
            if (--indegree[consumer] == 0) ready.insert(plan.nodes[consumer].id);
        }
    }
    plan.stop_order.assign(plan.load_order.rbegin(), plan.load_order.rend());
    return plan;
}

}  // namespace anomaly
