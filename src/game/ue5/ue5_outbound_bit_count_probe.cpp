#include "anomaly/ue5_outbound_bit_count_probe.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace anomaly {
namespace {

inline constexpr std::string_view kOwner = "anomaly.ue5.outbound-bit-count-probe";
inline constexpr std::uint64_t kGeneration = 1;

}  // namespace

class Ue5OutboundBitCountProbe::Impl final {
public:
    explicit Impl(std::unique_ptr<HookBackend> backend) : hooks_(std::move(backend)) {}

    bool Start(void* target) {
        std::scoped_lock stop_lock(stop_mutex_);
        if (target == nullptr || started_.load(std::memory_order_acquire)) return false;
        if (owner_registered_) return false;
        std::scoped_lock process_lock(process_mutex_);
        if (active_.load(std::memory_order_acquire) != nullptr) return false;

        original_ = nullptr;
        if (!hooks_.Create(
                std::string(kOwner), kGeneration, "ue5-outbound-bit-count", target,
                reinterpret_cast<void*>(&TargetThunk), reinterpret_cast<void**>(&original_))) {
            return false;
        }
        owner_registered_ = true;
        ResetMetadata();
        active_.store(this, std::memory_order_release);
        if (!hooks_.EnableOwner(kOwner, kGeneration)) {
            active_.store(nullptr, std::memory_order_release);
            if (hooks_.RemoveOwner(kOwner, kGeneration)) owner_registered_ = false;
            original_ = nullptr;
            return false;
        }
        started_.store(true, std::memory_order_release);
        return true;
    }

    bool Stop(std::chrono::milliseconds timeout) noexcept {
        std::scoped_lock stop_lock(stop_mutex_);
        if (!owner_registered_) return true;
        const bool was_started = started_.load(std::memory_order_acquire);
        if (was_started) {
            if (!hooks_.DisableOwner(kOwner, kGeneration)) return false;
            started_.store(false, std::memory_order_release);
        }
        const auto bounded_timeout =
            (std::max)(timeout, std::chrono::milliseconds::zero());
        if (!hooks_.RemoveOwner(kOwner, kGeneration, bounded_timeout)) return false;
        {
            std::scoped_lock process_lock(process_mutex_);
            Impl* expected = this;
            static_cast<void>(active_.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel));
        }
        owner_registered_ = false;
        original_ = nullptr;
        return true;
    }

    [[nodiscard]] bool Started() const noexcept {
        return started_.load(std::memory_order_acquire);
    }

    [[nodiscard]] Ue5OutboundBitCountProbeSnapshot Snapshot() const noexcept {
        return {
            Started(),
            call_count_.load(std::memory_order_relaxed),
            successful_result_count_.load(std::memory_order_relaxed),
            error_result_count_.load(std::memory_order_relaxed),
            input_nonzero_bit_count_call_count_.load(std::memory_order_relaxed),
            output_nonzero_bit_count_call_count_.load(std::memory_order_relaxed),
            null_input_data_count_.load(std::memory_order_relaxed),
            null_output_data_count_.load(std::memory_order_relaxed),
            same_data_pointer_result_count_.load(std::memory_order_relaxed),
            invalid_input_argument_count_.load(std::memory_order_relaxed),
            invalid_output_result_count_.load(std::memory_order_relaxed),
            result_pointer_mismatch_count_.load(std::memory_order_relaxed),
            unexpected_error_value_count_.load(std::memory_order_relaxed),
            aggregate_overflow_count_.load(std::memory_order_relaxed),
            maximum_input_bit_count_.load(std::memory_order_relaxed),
            maximum_output_bit_count_.load(std::memory_order_relaxed),
            input_ceil_byte_total_.load(std::memory_order_relaxed),
            output_ceil_byte_total_.load(std::memory_order_relaxed),
        };
    }

private:
    static bool SaturatingAdd(
        std::atomic<std::uint64_t>& counter, const std::uint64_t amount) noexcept {
        if (amount == 0U) return false;
        constexpr std::uint64_t kMaximum = (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t current = counter.load(std::memory_order_relaxed);
        for (;;) {
            if (amount > kMaximum - current) {
                if (current == kMaximum) return true;
                if (counter.compare_exchange_weak(
                        current, kMaximum, std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    return true;
                }
                continue;
            }
            if (counter.compare_exchange_weak(
                    current, current + amount, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return false;
            }
        }
    }

    void ResetMetadata() noexcept {
        call_count_.store(0U, std::memory_order_relaxed);
        successful_result_count_.store(0U, std::memory_order_relaxed);
        error_result_count_.store(0U, std::memory_order_relaxed);
        input_nonzero_bit_count_call_count_.store(0U, std::memory_order_relaxed);
        output_nonzero_bit_count_call_count_.store(0U, std::memory_order_relaxed);
        null_input_data_count_.store(0U, std::memory_order_relaxed);
        null_output_data_count_.store(0U, std::memory_order_relaxed);
        same_data_pointer_result_count_.store(0U, std::memory_order_relaxed);
        invalid_input_argument_count_.store(0U, std::memory_order_relaxed);
        invalid_output_result_count_.store(0U, std::memory_order_relaxed);
        result_pointer_mismatch_count_.store(0U, std::memory_order_relaxed);
        unexpected_error_value_count_.store(0U, std::memory_order_relaxed);
        aggregate_overflow_count_.store(0U, std::memory_order_relaxed);
        maximum_input_bit_count_.store(0U, std::memory_order_relaxed);
        maximum_output_bit_count_.store(0U, std::memory_order_relaxed);
        input_ceil_byte_total_.store(0U, std::memory_order_relaxed);
        output_ceil_byte_total_.store(0U, std::memory_order_relaxed);
    }

    static void UpdateMaximum(
        std::atomic<std::uint32_t>& maximum,
        const std::uint32_t value) noexcept {
        std::uint32_t observed = maximum.load(std::memory_order_relaxed);
        while (observed < value &&
               !maximum.compare_exchange_weak(
                   observed, value, std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    bool RecordBitCount(
        const std::int32_t bit_count,
        std::atomic<std::uint64_t>& nonzero_count,
        std::atomic<std::uint32_t>& maximum,
        std::atomic<std::uint64_t>& ceil_byte_total) noexcept {
        if (bit_count <= 0) return false;
        const auto positive_bit_count = static_cast<std::uint32_t>(bit_count);
        bool overflowed = SaturatingAdd(nonzero_count, 1U);
        const std::uint64_t ceil_byte_count =
            (static_cast<std::uint64_t>(positive_bit_count) + 7U) / 8U;
        overflowed = SaturatingAdd(ceil_byte_total, ceil_byte_count) || overflowed;
        UpdateMaximum(maximum, positive_bit_count);
        return overflowed;
    }

    void Record(
        const void* const input_data,
        const std::int32_t input_bit_count,
        const Ue5PacketHandlerOutgoingTransformResult* const result,
        const Ue5PacketHandlerOutgoingTransformResult* const returned_result) noexcept {
        bool overflowed = SaturatingAdd(call_count_, 1U);
        overflowed = RecordBitCount(
            input_bit_count, input_nonzero_bit_count_call_count_, maximum_input_bit_count_,
            input_ceil_byte_total_) || overflowed;
        if (input_data == nullptr) {
            overflowed = SaturatingAdd(null_input_data_count_, 1U) || overflowed;
        }
        if (input_bit_count < 0 || (input_bit_count > 0 && input_data == nullptr)) {
            overflowed = SaturatingAdd(invalid_input_argument_count_, 1U) || overflowed;
        }
        if (returned_result != result) {
            overflowed = SaturatingAdd(result_pointer_mismatch_count_, 1U) || overflowed;
        }

        if (result == nullptr) {
            overflowed = SaturatingAdd(null_output_data_count_, 1U) || overflowed;
            overflowed = SaturatingAdd(invalid_output_result_count_, 1U) || overflowed;
        } else {
            const void* const output_data = result->data;
            const std::int32_t output_bit_count = result->bit_count;
            const std::uint8_t error = result->error;
            overflowed = RecordBitCount(
                output_bit_count, output_nonzero_bit_count_call_count_, maximum_output_bit_count_,
                output_ceil_byte_total_) || overflowed;
            if (output_data == nullptr) {
                overflowed = SaturatingAdd(null_output_data_count_, 1U) || overflowed;
            }
            if (input_data != nullptr && output_data == input_data) {
                overflowed = SaturatingAdd(same_data_pointer_result_count_, 1U) || overflowed;
            }
            if (error == 0U) {
                overflowed = SaturatingAdd(successful_result_count_, 1U) || overflowed;
                if (output_bit_count < 0 || (output_bit_count > 0 && output_data == nullptr)) {
                    overflowed = SaturatingAdd(invalid_output_result_count_, 1U) || overflowed;
                }
            } else {
                overflowed = SaturatingAdd(error_result_count_, 1U) || overflowed;
                if (error != 1U) {
                    overflowed = SaturatingAdd(unexpected_error_value_count_, 1U) || overflowed;
                }
                if (output_data != nullptr || output_bit_count != 0) {
                    overflowed = SaturatingAdd(invalid_output_result_count_, 1U) || overflowed;
                }
            }
        }
        if (overflowed) {
            static_cast<void>(SaturatingAdd(aggregate_overflow_count_, 1U));
        }
    }

    static Ue5PacketHandlerOutgoingTransformResult* __fastcall TargetThunk(
        void* handler,
        Ue5PacketHandlerOutgoingTransformResult* result,
        const void* input_data,
        std::int32_t input_bit_count,
        const void* opaque_argument_5,
        std::uint8_t opaque_argument_6,
        const void* opaque_argument_7) {
        Impl* self{};
        TargetFunction original{};
        PluginScope::CallbackLease lease;
        {
            std::scoped_lock process_lock(process_mutex_);
            self = active_.load(std::memory_order_acquire);
            if (self == nullptr) return result;
            lease = self->hooks_.AcquireCallback(kOwner, kGeneration);
            // A rejected lease means removal can reclaim MinHook's trampoline
            // immediately. Do not jump through a copied original in that state.
            if (!lease || self->original_ == nullptr) return result;
            original = self->original_;
        }

        auto* const returned_result = original(
            handler, result, input_data, input_bit_count, opaque_argument_5,
            opaque_argument_6, opaque_argument_7);
        // The caller-owned sret object is only read before this detour returns.
        // Packet bytes are never dereferenced, copied, hashed, or emitted.
        if (lease) self->Record(input_data, input_bit_count, result, returned_result);
        return returned_result;
    }

    HookManager hooks_;
    TargetFunction original_{};
    std::atomic_bool started_{};
    bool owner_registered_{};
    std::atomic<std::uint64_t> call_count_{};
    std::atomic<std::uint64_t> successful_result_count_{};
    std::atomic<std::uint64_t> error_result_count_{};
    std::atomic<std::uint64_t> input_nonzero_bit_count_call_count_{};
    std::atomic<std::uint64_t> output_nonzero_bit_count_call_count_{};
    std::atomic<std::uint64_t> null_input_data_count_{};
    std::atomic<std::uint64_t> null_output_data_count_{};
    std::atomic<std::uint64_t> same_data_pointer_result_count_{};
    std::atomic<std::uint64_t> invalid_input_argument_count_{};
    std::atomic<std::uint64_t> invalid_output_result_count_{};
    std::atomic<std::uint64_t> result_pointer_mismatch_count_{};
    std::atomic<std::uint64_t> unexpected_error_value_count_{};
    std::atomic<std::uint64_t> aggregate_overflow_count_{};
    std::atomic<std::uint32_t> maximum_input_bit_count_{};
    std::atomic<std::uint32_t> maximum_output_bit_count_{};
    std::atomic<std::uint64_t> input_ceil_byte_total_{};
    std::atomic<std::uint64_t> output_ceil_byte_total_{};
    std::mutex stop_mutex_;
    static std::atomic<Impl*> active_;
    static std::mutex process_mutex_;
};

std::atomic<Ue5OutboundBitCountProbe::Impl*> Ue5OutboundBitCountProbe::Impl::active_{};
std::mutex Ue5OutboundBitCountProbe::Impl::process_mutex_;

Ue5OutboundBitCountProbe::Ue5OutboundBitCountProbe(std::unique_ptr<HookBackend> backend)
    : impl_(std::make_unique<Impl>(std::move(backend))) {}

Ue5OutboundBitCountProbe::~Ue5OutboundBitCountProbe() {
    if (impl_ != nullptr && !impl_->Stop(std::chrono::milliseconds::zero())) {
        // The detour is disabled and detached from active_, but an in-flight
        // target still owns the HookManager lease. Keep this generation mapped.
        static_cast<void>(impl_.release());
    }
}

bool Ue5OutboundBitCountProbe::Start(void* target) { return impl_->Start(target); }

bool Ue5OutboundBitCountProbe::Stop(std::chrono::milliseconds timeout) noexcept {
    return impl_->Stop(timeout);
}

bool Ue5OutboundBitCountProbe::Started() const noexcept { return impl_->Started(); }

Ue5OutboundBitCountProbeSnapshot Ue5OutboundBitCountProbe::Snapshot() const noexcept {
    return impl_->Snapshot();
}

}  // namespace anomaly
