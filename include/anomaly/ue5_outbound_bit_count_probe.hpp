#pragma once

#include "anomaly/hook_manager.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace anomaly {

// This is the exact, transient ABI result storage used by the verified
// PacketHandler outgoing transform. The probe never retains or dereferences
// data; it only reads the scalar fields while the caller-owned result is live.
struct Ue5PacketHandlerOutgoingTransformResult final {
    const void* data{};
    std::int32_t bit_count{};
    std::uint8_t error{};
    std::uint8_t padding[3]{};
};

static_assert(sizeof(Ue5PacketHandlerOutgoingTransformResult) == 16U);
static_assert(offsetof(Ue5PacketHandlerOutgoingTransformResult, data) == 0U);
static_assert(offsetof(Ue5PacketHandlerOutgoingTransformResult, bit_count) == 8U);
static_assert(offsetof(Ue5PacketHandlerOutgoingTransformResult, error) == 12U);

// Metadata only. This deliberately contains neither packet bytes nor any
// process address captured from a hooked invocation.
struct Ue5OutboundBitCountProbeSnapshot final {
    bool started{};
    std::uint64_t call_count{};
    std::uint64_t successful_result_count{};
    std::uint64_t error_result_count{};
    std::uint64_t input_nonzero_bit_count_call_count{};
    std::uint64_t output_nonzero_bit_count_call_count{};
    std::uint64_t null_input_data_count{};
    std::uint64_t null_output_data_count{};
    std::uint64_t same_data_pointer_result_count{};
    std::uint64_t invalid_input_argument_count{};
    std::uint64_t invalid_output_result_count{};
    std::uint64_t result_pointer_mismatch_count{};
    std::uint64_t unexpected_error_value_count{};
    // Number of invocations for which one or more aggregate counters had to
    // saturate. The overflow counter itself also saturates silently.
    std::uint64_t aggregate_overflow_count{};
    std::uint32_t maximum_input_bit_count{};
    std::uint32_t maximum_output_bit_count{};
    std::uint64_t input_ceil_byte_total{};
    std::uint64_t output_ceil_byte_total{};
};

class Ue5OutboundBitCountProbe final {
public:
    // This raw declaration intentionally mirrors the machine ABI rather than
    // relying on compiler-specific aggregate-return lowering. It preserves
    // RCX/RDX/R8/R9 and the three verified stack arguments exactly.
    using TargetFunction = Ue5PacketHandlerOutgoingTransformResult* (__fastcall*)(
        void* handler,
        Ue5PacketHandlerOutgoingTransformResult* result,
        const void* input_data,
        std::int32_t input_bit_count,
        const void* opaque_argument_5,
        std::uint8_t opaque_argument_6,
        const void* opaque_argument_7);

    explicit Ue5OutboundBitCountProbe(std::unique_ptr<HookBackend> backend);
    ~Ue5OutboundBitCountProbe();

    Ue5OutboundBitCountProbe(const Ue5OutboundBitCountProbe&) = delete;
    Ue5OutboundBitCountProbe& operator=(const Ue5OutboundBitCountProbe&) = delete;

    [[nodiscard]] bool Start(void* target);
    // Disables new detour entries, then drains callbacks within timeout. A
    // false result leaves the disabled generation intact for quarantine and
    // may be retried after its in-flight target invocation returns.
    bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] Ue5OutboundBitCountProbeSnapshot Snapshot() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
