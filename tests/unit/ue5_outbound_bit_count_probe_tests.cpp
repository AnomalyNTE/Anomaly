#include "anomaly/ue5_outbound_bit_count_probe.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

namespace {

class FakeBackend final : public anomaly::HookBackend {
public:
    bool Initialize() noexcept override { return true; }
    void Uninitialize() noexcept override {}
    bool Create(void* target, void* detour_value, void** original) noexcept override {
        detour = detour_value;
        *original = target;
        return true;
    }
    bool Enable(void*) noexcept override {
        enabled = true;
        return true;
    }
    bool Disable(void*) noexcept override {
        if (fail_disable) return false;
        enabled = false;
        return true;
    }
    bool Remove(void*) noexcept override {
        removed = true;
        return true;
    }

    void* detour{};
    bool enabled{};
    bool removed{};
    bool fail_disable{};
};

using Result = anomaly::Ue5PacketHandlerOutgoingTransformResult;
using TargetFunction = anomaly::Ue5OutboundBitCountProbe::TargetFunction;

std::atomic_uint32_t g_original_calls{};
std::atomic_bool g_original_before_metadata{};
std::atomic<std::uintptr_t> g_last_handler{};
std::atomic<std::uintptr_t> g_last_result{};
std::atomic<std::uintptr_t> g_last_input{};
std::atomic<std::int32_t> g_last_input_bit_count{};
std::atomic<std::uintptr_t> g_last_opaque_5{};
std::atomic_uint32_t g_last_opaque_6{};
std::atomic<std::uintptr_t> g_last_opaque_7{};
anomaly::Ue5OutboundBitCountProbe* g_probe{};
Result g_alternate_result{};

Result* __fastcall Target(
    void* handler,
    Result* result,
    const void* input_data,
    const std::int32_t input_bit_count,
    const void* opaque_argument_5,
    const std::uint8_t opaque_argument_6,
    const void* opaque_argument_7) {
    const std::uint32_t prior_calls = g_original_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_probe != nullptr && g_probe->Snapshot().call_count == prior_calls) {
        g_original_before_metadata.store(true, std::memory_order_release);
    }
    g_last_handler.store(reinterpret_cast<std::uintptr_t>(handler), std::memory_order_release);
    g_last_result.store(reinterpret_cast<std::uintptr_t>(result), std::memory_order_release);
    g_last_input.store(reinterpret_cast<std::uintptr_t>(input_data), std::memory_order_release);
    g_last_input_bit_count.store(input_bit_count, std::memory_order_release);
    g_last_opaque_5.store(
        reinterpret_cast<std::uintptr_t>(opaque_argument_5), std::memory_order_release);
    g_last_opaque_6.store(opaque_argument_6, std::memory_order_release);
    g_last_opaque_7.store(
        reinterpret_cast<std::uintptr_t>(opaque_argument_7), std::memory_order_release);
    if (result == nullptr) return nullptr;

    if (opaque_argument_6 == 1U) {
        *result = {nullptr, 0, 1, {}};
        return result;
    }
    if (opaque_argument_6 == 2U) {
        *result = {input_data, 3, 0, {}};
        g_alternate_result = {input_data, 3, 0, {}};
        return &g_alternate_result;
    }
    if (opaque_argument_6 == 3U) {
        *result = {nullptr, 17, 0, {}};
        return result;
    }
    *result = {input_data, 5, 0, {}};
    return result;
}

std::atomic_uint32_t g_blocking_target_calls{};
std::atomic_bool g_blocking_target_entered{};
std::atomic_bool g_release_blocking_target{};

Result* __fastcall BlockingTarget(
    void*,
    Result* result,
    const void* input_data,
    std::int32_t,
    const void*,
    std::uint8_t,
    const void*) {
    g_blocking_target_calls.fetch_add(1, std::memory_order_relaxed);
    g_blocking_target_entered.store(true, std::memory_order_release);
    while (!g_release_blocking_target.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    if (result != nullptr) *result = {input_data, 1, 0, {}};
    return result;
}

}  // namespace

int main() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fixture = backend.get();
    anomaly::Ue5OutboundBitCountProbe probe(std::move(backend));
    g_probe = &probe;
    if (!probe.Start(reinterpret_cast<void*>(&Target)) || !probe.Started() || !fixture->enabled ||
        fixture->detour == nullptr || probe.Start(reinterpret_cast<void*>(&Target))) {
        std::cerr << "outbound transform probe did not start exactly once\n";
        return 1;
    }

    const auto target = reinterpret_cast<TargetFunction>(fixture->detour);
    std::uint8_t input_one{};
    std::uint8_t input_two{};
    std::uint8_t opaque_five{};
    std::uint8_t opaque_seven{};
    Result normal{};
    Result error{};
    Result invalid{};
    Result mismatched{};
    const auto* const normal_return = target(
        reinterpret_cast<void*>(0x1010U), &normal, &input_one, 9,
        &opaque_five, 0U, &opaque_seven);
    const auto* const error_return = target(
        reinterpret_cast<void*>(0x2020U), &error, nullptr, 0,
        &opaque_five, 1U, &opaque_seven);
    const auto* const invalid_return = target(
        reinterpret_cast<void*>(0x3030U), &invalid, nullptr, 17,
        &opaque_five, 3U, &opaque_seven);
    const auto* const mismatched_return = target(
        reinterpret_cast<void*>(0x4040U), &mismatched, &input_two, 11,
        &opaque_five, 2U, &opaque_seven);

    const auto snapshot = probe.Snapshot();
    if (normal_return != &normal || error_return != &error || invalid_return != &invalid ||
        mismatched_return != &g_alternate_result || g_original_calls.load(std::memory_order_acquire) != 4U ||
        !g_original_before_metadata.load(std::memory_order_acquire) || snapshot.call_count != 4U ||
        snapshot.successful_result_count != 3U || snapshot.error_result_count != 1U ||
        snapshot.input_nonzero_bit_count_call_count != 3U ||
        snapshot.output_nonzero_bit_count_call_count != 3U ||
        snapshot.null_input_data_count != 2U || snapshot.null_output_data_count != 2U ||
        snapshot.same_data_pointer_result_count != 2U || snapshot.invalid_input_argument_count != 1U ||
        snapshot.invalid_output_result_count != 1U || snapshot.result_pointer_mismatch_count != 1U ||
        snapshot.unexpected_error_value_count != 0U || snapshot.aggregate_overflow_count != 0U ||
        snapshot.maximum_input_bit_count != 17U || snapshot.maximum_output_bit_count != 17U ||
        snapshot.input_ceil_byte_total != 7U || snapshot.output_ceil_byte_total != 5U ||
        g_last_handler.load(std::memory_order_acquire) != 0x4040U ||
        g_last_result.load(std::memory_order_acquire) != reinterpret_cast<std::uintptr_t>(&mismatched) ||
        g_last_input.load(std::memory_order_acquire) != reinterpret_cast<std::uintptr_t>(&input_two) ||
        g_last_input_bit_count.load(std::memory_order_acquire) != 11 ||
        g_last_opaque_5.load(std::memory_order_acquire) != reinterpret_cast<std::uintptr_t>(&opaque_five) ||
        g_last_opaque_6.load(std::memory_order_acquire) != 2U ||
        g_last_opaque_7.load(std::memory_order_acquire) != reinterpret_cast<std::uintptr_t>(&opaque_seven)) {
        std::cerr << "outgoing transform ABI forwarding or metadata changed\n";
        return 2;
    }
    if (!probe.Stop() || probe.Started() || fixture->enabled || !fixture->removed) {
        std::cerr << "outbound transform probe did not stop cleanly\n";
        return 3;
    }
    g_probe = nullptr;

    auto disable_failure_backend = std::make_unique<FakeBackend>();
    auto* disable_failure_fixture = disable_failure_backend.get();
    anomaly::Ue5OutboundBitCountProbe disable_failure_probe(std::move(disable_failure_backend));
    if (!disable_failure_probe.Start(reinterpret_cast<void*>(&Target))) {
        std::cerr << "disable-failure outgoing transform probe did not start\n";
        return 4;
    }
    disable_failure_fixture->fail_disable = true;
    if (disable_failure_probe.Stop(std::chrono::milliseconds(10)) ||
        !disable_failure_probe.Started() || !disable_failure_fixture->enabled ||
        disable_failure_fixture->removed) {
        std::cerr << "disable failure falsely stopped the outgoing transform probe\n";
        return 5;
    }
    const auto disable_failure_target =
        reinterpret_cast<TargetFunction>(disable_failure_fixture->detour);
    const auto original_calls_before_disable_retry =
        g_original_calls.load(std::memory_order_acquire);
    Result disable_failure_result{};
    std::uint8_t disable_failure_input{};
    if (disable_failure_target(
            nullptr, &disable_failure_result, &disable_failure_input, 1, nullptr, 0U, nullptr) !=
            &disable_failure_result ||
        disable_failure_result.data != &disable_failure_input ||
        g_original_calls.load(std::memory_order_acquire) != original_calls_before_disable_retry + 1U ||
        disable_failure_probe.Snapshot().call_count != 1U) {
        std::cerr << "disable-failure outgoing transform probe lost passthrough behavior\n";
        return 6;
    }
    disable_failure_fixture->fail_disable = false;
    if (!disable_failure_probe.Stop(std::chrono::milliseconds(100)) ||
        disable_failure_probe.Started() || !disable_failure_fixture->removed) {
        std::cerr << "disable-failure outgoing transform probe did not recover\n";
        return 7;
    }

    auto blocking_backend = std::make_unique<FakeBackend>();
    auto* blocking_fixture = blocking_backend.get();
    anomaly::Ue5OutboundBitCountProbe blocking_probe(std::move(blocking_backend));
    if (!blocking_probe.Start(reinterpret_cast<void*>(&BlockingTarget))) {
        std::cerr << "blocking outbound transform probe did not start\n";
        return 4;
    }
    const auto blocking_target = reinterpret_cast<TargetFunction>(blocking_fixture->detour);
    Result blocking_result{};
    std::uint8_t blocking_input{};
    std::thread blocked_call([&] {
        blocking_target(nullptr, &blocking_result, &blocking_input, 1, nullptr, 0U, nullptr);
    });
    const auto entry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!g_blocking_target_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entry_deadline) {
        std::this_thread::yield();
    }
    if (!g_blocking_target_entered.load(std::memory_order_acquire)) {
        g_release_blocking_target.store(true, std::memory_order_release);
        blocked_call.join();
        std::cerr << "blocking outgoing transform target did not enter\n";
        return 8;
    }
    const auto stop_started = std::chrono::steady_clock::now();
    if (blocking_probe.Stop(std::chrono::milliseconds(5)) ||
        std::chrono::steady_clock::now() - stop_started > std::chrono::seconds(1) ||
        blocking_fixture->enabled || blocking_fixture->removed || blocking_probe.Started()) {
        g_release_blocking_target.store(true, std::memory_order_release);
        blocked_call.join();
        std::cerr << "in-flight outgoing transform generation did not remain quarantinable\n";
        return 9;
    }
    g_release_blocking_target.store(true, std::memory_order_release);
    blocked_call.join();

    Result stopped_result{};
    const auto* const stopped_return = blocking_target(
        nullptr, &stopped_result, &blocking_input, 1, nullptr, 0U, nullptr);
    const auto stopped_snapshot = blocking_probe.Snapshot();
    if (stopped_return != &stopped_result || stopped_result.data != nullptr ||
        g_blocking_target_calls.load(std::memory_order_acquire) != 1U ||
        stopped_snapshot.call_count != 1U ||
        !blocking_probe.Stop(std::chrono::milliseconds(100)) || !blocking_fixture->removed) {
        std::cerr << "stopping outgoing transform probe invoked an unleased trampoline\n";
        return 10;
    }
    return 0;
}
