#include "anomaly/hook_manager.hpp"

#include <MinHook.h>

#include <memory>

namespace anomaly {
namespace {

class MinHookBackend final : public HookBackend {
public:
    bool Initialize() noexcept override {
        const MH_STATUS status = MH_Initialize();
        initialized_ = status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
        owns_initialization_ = status == MH_OK;
        return initialized_;
    }
    void Uninitialize() noexcept override {
        if (owns_initialization_) static_cast<void>(MH_Uninitialize());
        initialized_ = false;
        owns_initialization_ = false;
    }
    bool Create(void* target, void* detour, void** original) noexcept override {
        return initialized_ && MH_CreateHook(target, detour, original) == MH_OK;
    }
    bool Enable(void* target) noexcept override { return MH_EnableHook(target) == MH_OK; }
    bool Disable(void* target) noexcept override {
        const MH_STATUS status = MH_DisableHook(target);
        return status == MH_OK || status == MH_ERROR_DISABLED;
    }
    bool Remove(void* target) noexcept override { return MH_RemoveHook(target) == MH_OK; }

private:
    bool initialized_{};
    bool owns_initialization_{};
};

}  // namespace

std::unique_ptr<HookBackend> CreateMinHookBackend() {
    return std::make_unique<MinHookBackend>();
}

}  // namespace anomaly
