#include "anomaly/hook_manager.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

namespace {
int failures{};
void Expect(bool value, std::string_view message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

struct BackendState {
    bool initialized{};
    bool fail_create{};
    bool fail_remove{};
    std::unordered_set<void*> created;
    std::unordered_set<void*> enabled;
    std::vector<void*> enable_calls;
    std::vector<void*> remove_calls;
    std::vector<std::string> events;
};

class FakeBackend final : public anomaly::HookBackend {
public:
    explicit FakeBackend(std::shared_ptr<BackendState> state) : state_(std::move(state)) {}
    bool Initialize() noexcept override { state_->initialized = true; state_->events.push_back("init"); return true; }
    void Uninitialize() noexcept override { state_->initialized = false; state_->events.push_back("uninit"); }
    bool Create(void* target, void*, void** original) noexcept override {
        if (state_->fail_create) {
            state_->events.push_back("create-failed");
            return false;
        }
        state_->created.insert(target); *original = target; state_->events.push_back("create"); return true;
    }
    bool Enable(void* target) noexcept override {
        state_->enabled.insert(target); state_->enable_calls.push_back(target);
        state_->events.push_back("enable"); return true;
    }
    bool Disable(void* target) noexcept override {
        state_->enabled.erase(target); state_->events.push_back("disable"); return true;
    }
    bool Remove(void* target) noexcept override {
        state_->remove_calls.push_back(target);
        if (state_->fail_remove) {
            state_->events.push_back("remove-failed");
            return false;
        }
        state_->created.erase(target); state_->events.push_back("remove"); return true;
    }
private:
    std::shared_ptr<BackendState> state_;
};
}  // namespace

int main() {
    {
        auto failure_state = std::make_shared<BackendState>();
        auto failure_ledger = std::make_shared<anomaly::ResourceLedger>();
        anomaly::HookManager hooks(
            std::make_unique<FakeBackend>(failure_state), failure_ledger);
        int target{};
        int detour{};
        void* original{};
        failure_state->fail_create = true;
        Expect(!hooks.Create(
                   "plugin.rollback", 1, "failed", &target, &detour, &original),
            "backend create failure is reported");
        Expect(hooks.Snapshot().empty(),
            "backend create failure rolls back the owner snapshot");
        Expect(failure_ledger->Snapshot("plugin.rollback").empty(),
            "backend create failure rolls back the hook ledger");
        Expect(!failure_state->initialized && failure_state->created.empty(),
            "backend create failure rolls back backend initialization");
        failure_state->fail_create = false;
        Expect(hooks.Create(
                   "plugin.rollback", 1, "retry", &target, &detour, &original),
            "hook creation succeeds after rollback");
    }

    {
        auto retry_state = std::make_shared<BackendState>();
        auto retry_ledger = std::make_shared<anomaly::ResourceLedger>();
        anomaly::HookManager hooks(std::make_unique<FakeBackend>(retry_state), retry_ledger);
        int target{};
        int detour{};
        void* original{};
        Expect(hooks.Create(
                   "plugin.remove-retry", 3, "target", &target, &detour, &original),
            "create hook for target remove retry");
        Expect(hooks.Enable("plugin.remove-retry", 3, &target),
            "enable hook for target remove retry");
        retry_state->fail_remove = true;
        Expect(!hooks.Remove("plugin.remove-retry", 3, &target, 50ms),
            "backend target remove failure is reported");
        const auto retained_target = hooks.Snapshot();
        Expect(retained_target.size() == 1 &&
                   retained_target.front().owner == "plugin.remove-retry" &&
                   retained_target.front().generation == 3 &&
                   retained_target.front().target == &target,
            "failed target removal retains the hook ownership record");
        Expect(retry_state->created.contains(&target) &&
                   retry_ledger->Snapshot("plugin.remove-retry").size() == 1,
            "failed target removal retains backend and ledger ownership for retry");
        Expect(!hooks.Create(
                   "plugin.remove-retry", 3, "duplicate", &target, &detour, &original),
            "failed target removal retains target conflict ownership");
        retry_state->fail_remove = false;
        Expect(hooks.Remove("plugin.remove-retry", 3, &target, 50ms),
            "target removal succeeds on retry");
        Expect(hooks.Snapshot().empty() && retry_ledger->Snapshot("plugin.remove-retry").empty() &&
                   !retry_state->created.contains(&target),
            "successful target retry releases ownership state");
    }

    {
        auto owner_retry_state = std::make_shared<BackendState>();
        auto owner_retry_ledger = std::make_shared<anomaly::ResourceLedger>();
        anomaly::HookManager hooks(
            std::make_unique<FakeBackend>(owner_retry_state), owner_retry_ledger);
        int targets[2]{};
        int detours[2]{};
        void* originals[2]{};
        Expect(hooks.Create(
                   "plugin.owner-remove-retry", 4, "first", &targets[0], &detours[0], &originals[0]) &&
                   hooks.Create(
                       "plugin.owner-remove-retry", 4, "second", &targets[1], &detours[1], &originals[1]),
            "create hooks for owner remove retry");
        Expect(hooks.Enable("plugin.owner-remove-retry", 4, &targets[0]) &&
                   hooks.Enable("plugin.owner-remove-retry", 4, &targets[1]),
            "enable hooks for owner remove retry");
        owner_retry_state->fail_remove = true;
        Expect(!hooks.RemoveOwner("plugin.owner-remove-retry", 4, 50ms),
            "backend owner remove failure is reported");
        const auto retained_owner = hooks.Snapshot();
        Expect(retained_owner.size() == 2 &&
                   owner_retry_state->created.contains(&targets[0]) &&
                   owner_retry_state->created.contains(&targets[1]) &&
                   owner_retry_ledger->Snapshot("plugin.owner-remove-retry").size() == 2,
            "failed owner removal retains target and ledger state for retry");
        owner_retry_state->fail_remove = false;
        Expect(hooks.RemoveOwner("plugin.owner-remove-retry", 4, 50ms),
            "owner removal succeeds on retry");
        Expect(hooks.Snapshot().empty() && owner_retry_ledger->Snapshot("plugin.owner-remove-retry").empty() &&
                   owner_retry_state->created.empty(),
            "successful owner retry releases all ownership state");
    }

    auto state = std::make_shared<BackendState>();
    auto ledger = std::make_shared<anomaly::ResourceLedger>();
    {
        anomaly::HookManager hooks(std::make_unique<FakeBackend>(state), ledger);
        int targets[4]{};
        int detours[4]{};
        void* originals[4]{};
        Expect(hooks.Create("plugin.a", 7, "present", &targets[0], &detours[0], &originals[0]),
            "create owner A hook");
        Expect(hooks.Create("plugin.a", 7, "resize", &targets[1], &detours[1], &originals[1]),
            "create second owner A hook");
        Expect(hooks.Create("plugin.b", 2, "present", &targets[2], &detours[2], &originals[2]),
            "create owner B hook");
        Expect(!hooks.Create("plugin.b", 2, "duplicate", &targets[0], &detours[3], &originals[3]),
            "target ownership is unique");
        Expect(state->enabled.empty(), "creating hooks does not enable unpublished targets");
        Expect(hooks.Enable("plugin.a", 7, &targets[1]), "enable second owner A hook by target");
        Expect(state->enable_calls.size() == 1 && state->enable_calls.back() == &targets[1] &&
                   !state->enabled.contains(&targets[0]) && state->enabled.contains(&targets[1]),
            "target-level enable does not enable an unpublished sibling");
        Expect(hooks.Enable("plugin.a", 7, &targets[0]), "enable first owner A hook by target");
        Expect(state->enabled.contains(&targets[0]) && state->enabled.contains(&targets[1]) &&
                   !state->enabled.contains(&targets[2]),
            "target-level enable includes only explicitly published owner hooks");
        auto callback = hooks.AcquireCallback("plugin.a", 7, &targets[0]);
        Expect(static_cast<bool>(callback), "hook callback lease acquired");
        Expect(!hooks.Remove("plugin.a", 7, &targets[0], 1ms),
            "in-flight callback blocks its hook removal");
        callback = {};
        Expect(hooks.Remove("plugin.a", 7, &targets[0], 50ms),
            "first owner hook removed after callback drains");
        Expect(!state->created.contains(&targets[0]) && state->created.contains(&targets[1]) &&
                   state->enabled.contains(&targets[1]),
            "independent owner hook remains installed after sibling release");
        auto owner_callback = hooks.AcquireCallback("plugin.a", 7);
        Expect(static_cast<bool>(owner_callback), "owner callback lease acquired");
        Expect(!hooks.RemoveOwner("plugin.a", 7, 1ms), "in-flight owner callback blocks owner removal");
        owner_callback = {};
        Expect(hooks.RemoveOwner("plugin.a", 7, 50ms), "remaining owner hooks removed after callback drains");
        Expect(ledger->Snapshot("plugin.a").empty(), "hook ledger entries released");
        Expect(!state->created.contains(&targets[1]) && state->created.contains(&targets[2]),
            "unrelated owner remains installed");
        Expect(hooks.Enable("plugin.b", 2, &targets[2]), "enable owner B hook by target");
    }
    Expect(state->created.empty() && state->enabled.empty() && !state->initialized,
        "manager destructor removes hooks and backend");
    if (failures != 0) return 1;
    std::cout << "owner-scoped HookManager passed\n";
    return 0;
}
