#pragma once

#include "anomaly/thread_local_value.hpp"

namespace ue5mem::embedded {

// One scope covers both Present entry points and the call to the next hook.
// A proxy may forward Present -> Present1 -> native Present; none of those
// nested calls may render a second overlay or tear down the outer frame.
class EmbeddedPresentScope final {
public:
    explicit EmbeddedPresentScope(anomaly::ThreadLocalScalar<bool>& active) noexcept
        : active_(active), outermost_(!active.Get()) {
        if (outermost_) active_.Set(true);
    }
    ~EmbeddedPresentScope() {
        if (outermost_) active_.Set(false);
    }
    EmbeddedPresentScope(const EmbeddedPresentScope&) = delete;
    EmbeddedPresentScope& operator=(const EmbeddedPresentScope&) = delete;
    [[nodiscard]] bool Outermost() const noexcept { return outermost_; }

private:
    anomaly::ThreadLocalScalar<bool>& active_;
    bool outermost_{};
};

} // namespace ue5mem::embedded
