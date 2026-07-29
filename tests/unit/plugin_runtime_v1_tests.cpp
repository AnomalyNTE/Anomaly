#include "anomaly/sdk/anomaly_sdk.h"
#include "anomaly/plugin_runtime.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
int failures{};
void Expect(bool value, std::string_view message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

struct ModuleState {
    std::weak_ptr<anomaly::PluginScope> scope;
    std::uint64_t generation{};
    std::atomic_uint updates{};
    std::atomic_uint unloads{};
    bool throw_draw{};
    bool slow_stop{};
};

class FixtureModule final : public anomaly::PluginModule {
public:
    explicit FixtureModule(std::shared_ptr<ModuleState> state) : state_(std::move(state)) {}
    bool Prepare() override { return true; }
    bool Load(const std::shared_ptr<anomaly::PluginScope>& scope) override {
        state_->scope = scope;
        state_->generation = scope->Generation();
        static_cast<void>(scope->Register(
            anomaly::PluginResourceKind::Task, "fixture-task", [this] { revoked_ = true; }));
        return true;
    }
    bool Start() override { return true; }
    void Stop() override { if (state_->slow_stop) std::this_thread::sleep_for(200ms); }
    void Unload() noexcept override { ++state_->unloads; }
    void Update(double) override { ++state_->updates; }
    void Draw() override { if (state_->throw_draw) throw std::runtime_error("draw"); }
    bool revoked_{};
private:
    std::shared_ptr<ModuleState> state_;
};

}  // namespace

int main() {
    {
        auto scope_ledger = std::make_shared<anomaly::ResourceLedger>();
        auto scope = std::make_shared<anomaly::PluginScope>(scope_ledger, "fixture.scope", 9);
        const auto token = scope->Register(anomaly::PluginResourceKind::Command, "command");
        auto callback = scope->AcquireCallback(9);
        Expect(static_cast<bool>(callback), "scope accepts ordinary generation lease");
        Expect(scope->FreezeCallbackSources(), "scope freezes callback sources once");
        Expect(!scope->AcquireCallback(9), "frozen scope rejects ordinary lease");
        Expect(scope->Register(anomaly::PluginResourceKind::Task, "late") == 0,
            "frozen scope rejects late resource registration");
        Expect(!scope->BeginStop(1ms), "scope reports an in-flight callback deadline");
        callback = {};
        Expect(scope->BeginStop(50ms), "scope drains after callback release");
        auto lifecycle = scope->AcquireLifecycleLease(9);
        Expect(static_cast<bool>(lifecycle), "drained scope grants lifecycle lease");
        const auto snapshot = scope->Snapshot();
        Expect(!snapshot.accepting_callbacks && snapshot.lifecycle_allowed,
            "scope snapshot records frozen lifecycle state");
        lifecycle = {};
        Expect(scope->Release(token), "scope ledger releases registered resource");
    }
    {
        auto scope_ledger = std::make_shared<anomaly::ResourceLedger>();
        auto scope = std::make_shared<anomaly::PluginScope>(scope_ledger, "fixture.config", 10);
        const auto config = scope->Register(anomaly::PluginResourceKind::Config, "settings");
        const auto task = scope->Register(anomaly::PluginResourceKind::Task, "task");
        Expect(config != 0 && task != 0, "scope registers staged shutdown resources");
        Expect(scope->RevokeAllExcept(anomaly::PluginResourceKind::Config) == 1,
            "staged shutdown preserves only config resources");
        const auto resources = scope->Resources();
        Expect(resources.size() == 1 && resources.front().token == config &&
                resources.front().kind == anomaly::PluginResourceKind::Config,
            "staged shutdown retained the config resource");
        Expect(scope->RevokeAll() == 1, "final shutdown revokes preserved config resource");
    }

    auto ledger = std::make_shared<anomaly::ResourceLedger>();
    anomaly::PluginRuntime runtime("fixture.reload", ledger, 50ms);
    std::vector<std::shared_ptr<ModuleState>> generations;
    auto factory = [&] {
        auto state = std::make_shared<ModuleState>();
        generations.push_back(state);
        return std::make_shared<FixtureModule>(state);
    };
    Expect(runtime.Activate(factory), "initial activation");
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto old = generations.back();
        const auto old_scope = runtime.Scope();
        Expect(runtime.Reload(factory), "transactional reload");
        Expect(old_scope && !old_scope->AcquireCallback(old->generation),
            "old generation callback rejected");
        Expect(runtime.Snapshot().resources == 1, "one current-generation resource");
    }
    runtime.Update(0.016);
    Expect(generations.back()->updates == 1, "current generation receives update");
    Expect(runtime.Stop(), "normal runtime stop");
    Expect(ledger->Snapshot("fixture.reload").empty(), "normal unload revokes every resource");
    Expect(runtime.Snapshot().state == anomaly::PluginRuntimeState::Unloaded, "unloaded state");

    anomaly::PluginRuntime faulted("fixture.fault", ledger, 50ms);
    auto fault_state = std::make_shared<ModuleState>();
    fault_state->throw_draw = true;
    Expect(faulted.Activate([&] { return std::make_shared<FixtureModule>(fault_state); }),
        "fault fixture activation");
    faulted.Draw();
    Expect(faulted.Snapshot().state == anomaly::PluginRuntimeState::Faulted,
        "draw exception isolated as fault");
    Expect(faulted.Stop(), "faulted plugin still stops");

    anomaly::PluginRuntime quarantined("fixture.slow", ledger, 20ms);
    auto slow_state = std::make_shared<ModuleState>();
    slow_state->slow_stop = true;
    Expect(quarantined.Activate([&] { return std::make_shared<FixtureModule>(slow_state); }),
        "slow fixture activation");
    Expect(!quarantined.Stop(), "slow stop times out");
    const auto quarantine_snapshot = quarantined.Snapshot();
    Expect(quarantine_snapshot.state == anomaly::PluginRuntimeState::Quarantined &&
        quarantine_snapshot.quarantined_modules == 1, "timed out module retained mapped");

    if (failures != 0) return 1;
    std::cout << "plugin ABI, scope, reload and isolation passed\n";
    return 0;
}
