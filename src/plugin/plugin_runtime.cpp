#include "anomaly/plugin_runtime.hpp"

#include <future>
#include <thread>
#include <utility>

namespace anomaly {

PluginRuntime::PluginRuntime(
    std::string id, std::shared_ptr<ResourceLedger> ledger,
    std::chrono::milliseconds callback_timeout)
    : id_(std::move(id)), ledger_(std::move(ledger)), callback_timeout_(callback_timeout) {}

PluginRuntime::~PluginRuntime() { static_cast<void>(Stop()); }

void PluginRuntime::SetFailure(PluginRuntimeState state, std::string message) noexcept {
    std::scoped_lock lock(mutex_);
    state_ = state;
    last_error_ = std::move(message);
}

bool PluginRuntime::Activate(Factory factory) {
    if (!factory) return false;
    std::shared_ptr<PluginModule> module;
    try { module = factory(); } catch (...) {
        SetFailure(PluginRuntimeState::Faulted, "module factory raised an exception");
        return false;
    }
    if (!module) {
        SetFailure(PluginRuntimeState::Faulted, "module factory returned no module");
        return false;
    }
    try {
        if (!module->Prepare()) {
            SetFailure(PluginRuntimeState::Faulted, "module validation failed");
            return false;
        }
    } catch (...) {
        SetFailure(PluginRuntimeState::Faulted, "module validation raised an exception");
        return false;
    }
    std::shared_ptr<PluginScope> scope;
    {
        std::scoped_lock lock(mutex_);
        if (module_) return false;
        state_ = PluginRuntimeState::Validated;
        scope = std::make_shared<PluginScope>(ledger_, id_, ++generation_);
        state_ = PluginRuntimeState::Shadowed;
    }
    try {
        if (!module->Load(scope)) throw std::runtime_error("module load returned failure");
        {
            std::scoped_lock lock(mutex_);
            state_ = PluginRuntimeState::Loaded;
        }
        {
            std::scoped_lock lock(mutex_);
            state_ = PluginRuntimeState::Starting;
        }
        if (!module->Start()) throw std::runtime_error("module start returned failure");
    } catch (const std::exception& error) {
        static_cast<void>(scope->BeginStop(callback_timeout_));
        scope->RevokeAll();
        module->Unload();
        SetFailure(PluginRuntimeState::Faulted, error.what());
        return false;
    } catch (...) {
        static_cast<void>(scope->BeginStop(callback_timeout_));
        scope->RevokeAll();
        module->Unload();
        SetFailure(PluginRuntimeState::Faulted, "module activation raised an exception");
        return false;
    }
    {
        std::scoped_lock lock(mutex_);
        module_ = std::move(module);
        scope_ = std::move(scope);
        state_ = PluginRuntimeState::Running;
        last_error_.clear();
    }
    return true;
}

bool PluginRuntime::StopCurrent() {
    std::shared_ptr<PluginModule> module;
    std::shared_ptr<PluginScope> scope;
    std::uint64_t generation{};
    {
        std::scoped_lock lock(mutex_);
        if (!module_) {
            if (state_ != PluginRuntimeState::Quarantined) state_ = PluginRuntimeState::Unloaded;
            return state_ != PluginRuntimeState::Quarantined;
        }
        state_ = PluginRuntimeState::Stopping;
        module = module_;
        scope = scope_;
        generation = generation_;
    }
    // Freeze every ordinary callback source before revoking resources.  The
    // lifecycle pin is enabled only after the in-flight barrier has drained.
    scope->FreezeCallbackSources();
    scope->RevokeAll();
    if (!scope->BeginStop(callback_timeout_)) {
        std::scoped_lock lock(mutex_);
        quarantined_.push_back(module);
        module_.reset();
        scope_.reset();
        state_ = PluginRuntimeState::Quarantined;
        last_error_ = "callback barrier timed out";
        return false;
    }

    auto lifecycle_lease = scope->AcquireLifecycleLease(generation);
    if (!lifecycle_lease) {
        std::scoped_lock lock(mutex_);
        quarantined_.push_back(module);
        module_.reset();
        scope_.reset();
        state_ = PluginRuntimeState::Quarantined;
        last_error_ = "lifecycle lease unavailable";
        return false;
    }

    std::promise<bool> completed;
    std::future<bool> completion = completed.get_future();
    std::thread stop_thread([
        module, lease = std::move(lifecycle_lease), promise = std::move(completed)]() mutable {
        try {
            module->Stop();
            try { promise.set_value(true); } catch (...) {}
        } catch (...) {
            try { promise.set_value(false); } catch (...) {}
        }
    });
    if (completion.wait_for(callback_timeout_) != std::future_status::ready) {
        stop_thread.detach();
        std::scoped_lock lock(mutex_);
        quarantined_.push_back(module);
        module_.reset();
        scope_.reset();
        state_ = PluginRuntimeState::Quarantined;
        last_error_ = "plugin stop timed out";
        return false;
    }
    const bool stopped = completion.get();
    stop_thread.join();
    if (!stopped) {
        std::scoped_lock lock(mutex_);
        quarantined_.push_back(module);
        module_.reset();
        scope_.reset();
        state_ = PluginRuntimeState::Quarantined;
        last_error_ = "plugin stop raised an exception";
        return false;
    }
    module->Unload();
    {
        std::scoped_lock lock(mutex_);
        module_.reset();
        scope_.reset();
        state_ = PluginRuntimeState::Unloaded;
        last_error_.clear();
    }
    return true;
}

bool PluginRuntime::Reload(Factory factory) {
    if (!StopCurrent()) return false;
    return Activate(std::move(factory));
}

bool PluginRuntime::Stop() { return StopCurrent(); }

void PluginRuntime::Update(double delta_seconds) noexcept {
    std::shared_ptr<PluginModule> module;
    std::shared_ptr<PluginScope> scope;
    std::uint64_t generation{};
    {
        std::scoped_lock lock(mutex_);
        if (state_ != PluginRuntimeState::Running) return;
        module = module_;
        scope = scope_;
        generation = generation_;
    }
    auto lease = scope->AcquireCallback(generation);
    if (!lease) return;
    try { module->Update(delta_seconds); }
    catch (...) { SetFailure(PluginRuntimeState::Faulted, "update callback raised an exception"); }
}

void PluginRuntime::Draw() noexcept {
    std::shared_ptr<PluginModule> module;
    std::shared_ptr<PluginScope> scope;
    std::uint64_t generation{};
    {
        std::scoped_lock lock(mutex_);
        if (state_ != PluginRuntimeState::Running) return;
        module = module_;
        scope = scope_;
        generation = generation_;
    }
    auto lease = scope->AcquireCallback(generation);
    if (!lease) return;
    try { module->Draw(); }
    catch (...) { SetFailure(PluginRuntimeState::Faulted, "draw callback raised an exception"); }
}

PluginRuntimeSnapshot PluginRuntime::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return {id_, state_, generation_, last_error_,
        scope_ ? scope_->Resources().size() : 0, quarantined_.size()};
}

std::shared_ptr<PluginScope> PluginRuntime::Scope() const {
    std::scoped_lock lock(mutex_);
    return scope_;
}

}  // namespace anomaly
