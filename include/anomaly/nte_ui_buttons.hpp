#pragma once

#include "anomaly/sdk/anomaly_sdk.h"
#include "anomaly/symbol_resolver.hpp"
#include "anomaly/ue5_process_event.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

inline constexpr std::string_view kNteUiButtonsFeature = "nte.ui-buttons";
inline constexpr std::string_view kNteUiButtonsLayoutValidator = "nte-ui-buttons-layout-v1";

// Layout keys the engine reads, and the bounds check behind nte-ui-buttons-layout-v1.
[[nodiscard]] const std::vector<std::string_view>& NteUiButtonsLayoutKeys();
[[nodiscard]] bool ValidateNteUiButtonsLayout(const BuildProfile& profile, std::string& error);

// GObjects view supplied each tick by the owner of the validated object registry.
struct NteUiButtonsRegistryView {
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t num_chunks{};
    std::uint32_t chunk_size{};
    std::uint32_t item_stride{};
    std::uint32_t object_offset{};
    std::uint32_t serial_offset{};
};

struct NteUiButtonsTickInput {
    bool available{};
    NteUiButtonsRegistryView registry;
    // Changes whenever the registry is reallocated; button handles carry it.
    std::uint64_t object_generation{};
};

// Game-thread callbacks into the engine's owner. Each is called on the Game thread,
// outside any lock the owner holds while ticking the engine.
struct NteUiButtonsBindings {
    std::function<std::string(std::uint32_t name_id)> resolve_name;
    // Empty when FText decoding is unavailable; button text is then omitted.
    std::function<std::string(std::uintptr_t ftext_address)> read_ftext;
    std::function<std::uintptr_t(const wchar_t* path)> find_object;
    Ue5ProcessEventInvoker invoke;
};

struct NteUiButtonsBudget {
    std::uint32_t slice_microseconds{3000};
    std::uint32_t max_calls_per_tick{48};
    std::uint32_t phase_limit_milliseconds{10000};
    std::uint32_t max_buttons{20000};
    std::uint32_t max_containers{512};
};

// Reads the current process directly with SEH protection. Only valid on the thread that
// owns the objects being read (the Game thread for UObjects).
[[nodiscard]] std::shared_ptr<const SymbolMemory> CreateInProcessSymbolMemory();

// Host-side UI button engine behind anomaly.nte.ui-buttons. Scans, hover picks and clicks
// run in bounded slices from Tick on the Game thread; the ABI entry points only queue work
// and read the immutable published catalog, so they may be called from any thread.
class NteUiButtons final {
public:
    NteUiButtons(
        const BuildProfile& profile,
        std::shared_ptr<const SymbolMemory> memory,
        NteUiButtonsBindings bindings,
        NteUiButtonsBudget budget = {});
    ~NteUiButtons();

    NteUiButtons(const NteUiButtons&) = delete;
    NteUiButtons& operator=(const NteUiButtons&) = delete;

    // Game thread only. Cheap when no request is open.
    void Tick(const NteUiButtonsTickInput& input) noexcept;
    // Drops the catalog and completes every open request with `status`. Any thread.
    void Invalidate(std::uint32_t status) noexcept;
    AnomalyStatusV1 Cancel(AnomalyGenerationHandleV1 request) noexcept;

    AnomalyStatusV1 Status(AnomalyNteUiButtonsStatusV1* status) const noexcept;
    AnomalyStatusV1 ButtonAt(
        std::uint64_t catalog_sequence, std::uint32_t index,
        AnomalyNteUiButtonSnapshotV1* snapshot) const noexcept;
    AnomalyStatusV1 WindowAt(
        std::uint64_t catalog_sequence, std::uint32_t index,
        AnomalyNteUiWindowSnapshotV1* snapshot) const noexcept;
    AnomalyStatusV1 Find(
        const AnomalyNteUiButtonQueryV1* query, AnomalyNteUiButtonSnapshotV1* snapshot,
        std::uint32_t* match_count) const noexcept;
    AnomalyStatusV1 RequestScan(AnomalyGenerationHandleV1* request) noexcept;
    AnomalyStatusV1 RequestPick(AnomalyGenerationHandleV1* request) noexcept;
    AnomalyStatusV1 RequestClick(
        const AnomalyNteUiButtonClickRequestV1* click,
        AnomalyGenerationHandleV1* request) noexcept;
    AnomalyStatusV1 RequestSnapshot(
        AnomalyGenerationHandleV1 request,
        AnomalyNteUiButtonRequestSnapshotV1* snapshot) const noexcept;
    AnomalyStatusV1 PickHitAt(
        AnomalyGenerationHandleV1 request, std::uint32_t index,
        AnomalyNteUiButtonSnapshotV1* snapshot) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
