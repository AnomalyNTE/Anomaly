#include "anomaly/plugin_scope.hpp"
#include "anomaly/ui_resource_registry.hpp"

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures{};

bool Expect(const bool value, const std::string_view message) {
    if (value) return true;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
    return false;
}

std::shared_ptr<anomaly::PluginScope> Scope(
    const std::shared_ptr<anomaly::ResourceLedger>& ledger,
    const std::uint64_t generation) {
    return std::make_shared<anomaly::PluginScope>(ledger, "anomaly.test.resources", generation);
}

anomaly::UiWindowRequest WindowRequest() {
    anomaly::UiWindowRequest request;
    request.id = "settings";
    request.title = "Settings";
    request.initial_width = 640.0F;
    request.initial_height = 480.0F;
    request.constraints = {320.0F, 200.0F, 1280.0F, 900.0F};
    return request;
}

anomaly::UiFontRequest FontRequest() {
    anomaly::UiFontRequest request;
    request.relative_path = "fonts/inter.ttf";
    request.size_pixels = 16.0F;
    request.scale = 1.25F;
    request.glyph_range = anomaly::UiGlyphRange::Latin;
    return request;
}

anomaly::UiFontRequest FontRequestWithBytes(
    const std::initializer_list<std::uint8_t> encoded_bytes) {
    auto request = FontRequest();
    request.encoded_bytes.assign(encoded_bytes);
    return request;
}

anomaly::UiTextureRequest TextureRequest() {
    anomaly::UiTextureRequest request;
    request.relative_path = "images/status.png";
    request.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
    request.format = anomaly::UiTextureFormat::Rgba8;
    return request;
}

anomaly::UiTextureRequest FileTextureRequest() {
    anomaly::UiTextureRequest request;
    request.relative_path = "images/worker-staged.png";
    request.format = anomaly::UiTextureFormat::Auto;
    return request;
}

anomaly::UiTextureRequest TextureRequestWithBytes(
    std::string relative_path, const std::initializer_list<std::uint8_t> encoded_bytes) {
    anomaly::UiTextureRequest request;
    request.relative_path = std::move(relative_path);
    request.encoded_bytes.assign(encoded_bytes);
    request.format = anomaly::UiTextureFormat::Auto;
    return request;
}

bool TestWindowsAndPersistence() {
    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = Scope(ledger, 7);
    anomaly::UiResourceRegistry registry;
    const auto window = registry.RegisterWindow(scope, WindowRequest());
    bool result = true;
    result = Expect(static_cast<bool>(window), "window registration failed") && result;
    const auto initial = registry.WindowState(scope, window);
    result = Expect(initial && initial->open && initial->stable_id.find("settings") != std::string::npos &&
            initial->width == 640.0F && initial->constraints.minimum_width == 320.0F,
        "window state did not retain a stable id and initial constraints") && result;
    const auto initial_group = registry.WindowGroupState(scope);
    result = Expect(initial_group.window_count == 1 && initial_group.open_window_count == 1,
        "window group state did not report the registered open window") && result;
    result = Expect(registry.CloseWindow(scope, window) && !registry.ShouldDrawWindow(scope, window) &&
            registry.ToggleWindow(scope, window) && registry.ShouldDrawWindow(scope, window),
        "open, close, toggle, and draw condition are inconsistent") && result;
    const anomaly::UiWindowConstraints resized{400.0F, 300.0F, 1000.0F, 800.0F};
    result = Expect(registry.SetWindowConstraints(scope, window, resized) &&
            registry.SetWindowSize(scope, window, 700.0F, 500.0F) &&
            !registry.SetWindowSize(scope, window, 1100.0F, 500.0F),
        "window constraint enforcement failed") && result;

    const auto persistent = registry.ExportPersistentWindowState();
    result = Expect(persistent.size() == 1 && persistent[0].open && persistent[0].width == 700.0F &&
            persistent[0].constraints.minimum_height == 300.0F,
        "window persistent export missed current state") && result;

    auto transient_request = WindowRequest();
    transient_request.id = "transient";
    transient_request.persist_settings = false;
    transient_request.default_open = false;
    const auto transient_window = registry.RegisterWindow(scope, std::move(transient_request));
    result = Expect(
        transient_window && registry.CloseWindow(scope, transient_window) &&
            registry.SetWindowSize(scope, transient_window, 640.0F, 480.0F) &&
            registry.ExportPersistentWindowState().size() == 1,
        "a no-saved-settings window changed persistent state") && result;
    result = Expect(registry.SetWindowGroupOpen(scope, true) &&
            registry.ShouldDrawWindow(scope, window) &&
            !registry.ShouldDrawWindow(scope, transient_window),
        "opening an already visible group changed a closed auxiliary window") && result;
    result = Expect(registry.SetWindowGroupOpen(scope, false) &&
            registry.WindowGroupState(scope).open_window_count == 0 &&
            registry.SetWindowGroupOpen(scope, true) &&
            registry.ShouldDrawWindow(scope, window) &&
            !registry.ShouldDrawWindow(scope, transient_window),
        "window group hide and reopen did not restore only default windows") && result;

    auto invalid_persistent = persistent;
    invalid_persistent.push_back(persistent.front());
    const bool invalid_rejected = !registry.ImportPersistentWindowState(std::move(invalid_persistent));
    const auto after_invalid_import = registry.ExportPersistentWindowState();
    result = Expect(invalid_rejected &&
            after_invalid_import.size() == 1 && after_invalid_import.front().stable_id ==
                persistent.front().stable_id && after_invalid_import.front().width == 700.0F,
        "invalid persistent import changed the existing window state") && result;

    anomaly::UiResourceRegistry restored;
    result = Expect(restored.ImportPersistentWindowState(persistent),
        "window persistent import failed") && result;
    const auto new_scope = Scope(ledger, 8);
    auto request = WindowRequest();
    request.default_open = false;
    const auto restored_window = restored.RegisterWindow(new_scope, std::move(request));
    const auto restored_state = restored.WindowState(new_scope, restored_window);
    result = Expect(restored_state && restored_state->open && restored_state->width == 700.0F &&
            restored_state->height == 500.0F && restored_state->constraints.maximum_width == 1000.0F,
        "persistent state was not reapplied across plugin generations") && result;

    auto restored_transient_request = WindowRequest();
    restored_transient_request.id = "transient";
    restored_transient_request.persist_settings = false;
    restored_transient_request.default_open = true;
    const auto restored_transient = restored.RegisterWindow(
        new_scope, std::move(restored_transient_request));
    const auto restored_transient_state = restored.WindowState(new_scope, restored_transient);
    result = Expect(
        restored_transient_state && restored_transient_state->open &&
            restored_transient_state->width == 640.0F && restored_transient_state->height == 480.0F,
        "a no-saved-settings window reused persisted state") && result;

    const auto old_generation = Scope(ledger, 6);
    result = Expect(!registry.WindowState(old_generation, window) &&
            !registry.CloseWindow(old_generation, window) &&
            registry.WindowGroupState(old_generation).window_count == 0 &&
            !registry.SetWindowGroupOpen(old_generation, true),
        "a different scope generation accessed a window handle") && result;

    const auto fallback_scope = Scope(ledger, 9);
    auto first_fallback_request = WindowRequest();
    first_fallback_request.id = "fallback-one";
    first_fallback_request.default_open = false;
    auto second_fallback_request = WindowRequest();
    second_fallback_request.id = "fallback-two";
    second_fallback_request.default_open = false;
    const auto first_fallback = registry.RegisterWindow(
        fallback_scope, std::move(first_fallback_request));
    const auto second_fallback = registry.RegisterWindow(
        fallback_scope, std::move(second_fallback_request));
    result = Expect(first_fallback && second_fallback &&
            registry.WindowGroupState(fallback_scope).open_window_count == 0 &&
            registry.SetWindowGroupOpen(fallback_scope, true) &&
            registry.WindowGroupState(fallback_scope).open_window_count == 1,
        "a group without default windows did not reopen exactly one fallback window") && result;
    return result;
}

bool TestResourceStateAndDeviceRebuild() {
    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = Scope(ledger, 9);
    anomaly::UiResourceRegistry registry;
    const auto font_one = registry.RequestFont(scope, FontRequest());
    const auto font_two = registry.RequestFont(scope, FontRequest());
    const auto texture = registry.RequestTexture(scope, TextureRequest());
    const auto texture_two = registry.RequestTexture(scope, TextureRequest());
    const auto other_scope = Scope(ledger, 10);
    const auto isolated_font = registry.RequestFont(other_scope, FontRequest());
    const auto isolated_texture = registry.RequestTexture(other_scope, TextureRequest());
    bool result = true;
    const auto font_state = registry.FontState(scope, font_one);
    const auto font_two_state = registry.FontState(scope, font_two);
    const auto texture_state = registry.TextureState(scope, texture);
    const auto texture_two_state = registry.TextureState(scope, texture_two);
    result = Expect(font_state && font_two_state && texture_state && texture_two_state &&
            font_state->resource.state == anomaly::UiResourceState::Queued &&
            font_state->resource.references == 2 &&
            font_state->resource.resource_id != 0 &&
            font_state->resource.resource_id == font_two_state->resource.resource_id &&
            texture_state->resource.references == 2 &&
            texture_state->resource.resource_id != 0 &&
            texture_state->resource.resource_id == texture_two_state->resource.resource_id &&
            texture_state->byte_size == 4 &&
            font_state->effective_size_pixels == 20.0F &&
            font_state->resource.font_glyph_range == anomaly::UiGlyphRange::Latin &&
            texture_state->resource.texture_format == anomaly::UiTextureFormat::Rgba8 &&
            texture_state->resource.staged_bytes == 4,
        "duplicate resource leases did not retain a shared canonical cache identity") && result;
    result = Expect(
        isolated_font && isolated_texture &&
            registry.FontState(other_scope, isolated_font)->resource.references == 1 &&
            registry.TextureState(other_scope, isolated_texture)->resource.references == 1,
        "UI resource cache crossed an owner or generation boundary") && result;
    const auto old_generation = Scope(ledger, 8);
    result = Expect(!registry.FontState(old_generation, font_one) &&
            !registry.TextureState(scope, font_one) &&
            !registry.MarkFontFailed(old_generation, font_one) &&
            !registry.Release(old_generation, font_one),
        "stale or wrong-kind resource handles were accepted") && result;
    result = Expect(registry.RebuildDeviceResources(registry.DeviceGeneration()) &&
            registry.FontState(scope, font_one)->resource.state == anomaly::UiResourceState::Queued &&
            registry.MarkFontReady(scope, font_one, 1) &&
            registry.MarkTextureReady(scope, texture, 1, 64, 32) &&
            registry.TextureState(scope, texture)->resource.device_generation == 1 &&
            registry.TextureState(scope, texture)->width == 64,
        "resource readiness was not tied to an explicit backend completion") && result;
    result = Expect(
        registry.FontState(other_scope, isolated_font)->resource.state == anomaly::UiResourceState::Queued &&
            registry.TextureState(other_scope, isolated_texture)->resource.state == anomaly::UiResourceState::Queued,
        "a resource transition crossed an owner or generation boundary") && result;
    const auto replacement_generation = registry.InvalidateDeviceResources();
    result = Expect(replacement_generation == 2 &&
            registry.FontState(scope, font_one)->resource.state == anomaly::UiResourceState::StaleDevice &&
            !registry.RebuildDeviceResources(1) && registry.RebuildDeviceResources(2) &&
            registry.TextureState(scope, texture)->resource.state == anomaly::UiResourceState::StaleDevice &&
            registry.MarkFontReady(scope, font_one, 2) &&
            registry.MarkTextureReady(scope, texture, 2, 64, 32) &&
            registry.TextureState(scope, texture)->resource.device_generation == 2,
        "device generation invalidation accepted a stale rebuild") && result;
    result = Expect(registry.MarkTextureFailed(scope, texture) &&
            registry.TextureState(scope, texture)->resource.state == anomaly::UiResourceState::Failed &&
            registry.RetryTexture(scope, texture) && registry.RebuildDeviceResources(2) &&
            registry.TextureState(scope, texture)->resource.state == anomaly::UiResourceState::Queued &&
            registry.MarkTextureReady(scope, texture, 2, 64, 32) &&
            registry.TextureState(scope, texture)->resource.state == anomaly::UiResourceState::Ready,
        "resource failure and retry state were not recoverable") && result;
    result = Expect(registry.Release(scope, font_one) &&
            registry.FontState(scope, font_two)->resource.references == 1 &&
            !registry.FontState(scope, font_one),
        "releasing one font lease did not preserve the shared resource") && result;
    return result;
}

bool TestHostFontScaleRebuildsReadyFonts() {
    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = Scope(ledger, 15);
    anomaly::UiResourceRegistry registry;
    const auto first = registry.RequestFont(scope, FontRequest());
    const auto duplicate = registry.RequestFont(scope, FontRequest());
    const std::uint64_t generation = registry.DeviceGeneration();
    bool result = true;
    result = Expect(
        first && duplicate && registry.MarkFontReady(scope, first, generation),
        "font was not ready before the host DPI transition") && result;

    const auto before = registry.ResourceState(scope, first);
    result = Expect(
        before && before->state == anomaly::UiResourceState::Ready &&
            before->effective_font_size_pixels == 20.0F && before->font_scale == 1.25F,
        "base font metadata did not preserve the request scale") && result;

    const bool scale_changed = registry.SetHostFontScale(1.5F);
    const auto scaled = registry.ResourceState(scope, first);
    const auto duplicate_scaled = registry.ResourceState(scope, duplicate);
    result = Expect(
        scale_changed && !registry.SetHostFontScale(1.5F) && scaled && duplicate_scaled &&
            scaled->state == anomaly::UiResourceState::Queued && scaled->device_generation == 0 &&
            scaled->effective_font_size_pixels == 30.0F && scaled->font_scale == 1.875F &&
            scaled->resource_id == duplicate_scaled->resource_id &&
            duplicate_scaled->state == anomaly::UiResourceState::Queued,
        "a host DPI change did not requeue shared ready fonts at the new effective size") && result;

    result = Expect(
        registry.MarkFontReady(scope, duplicate, generation) && registry.SetHostFontScale(2.0F) &&
            registry.ResourceState(scope, first)->state == anomaly::UiResourceState::Queued &&
            registry.ResourceState(scope, first)->effective_font_size_pixels == 40.0F,
        "a later host DPI change did not invalidate the rebuilt atlas entry") && result;
    return result;
}

bool TestTextureIdentitySurvivesWorkerStaging() {
    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = Scope(ledger, 11);
    anomaly::UiResourceRegistry registry;
    const auto first = registry.RequestTexture(scope, FileTextureRequest());
    bool result = true;
    result = Expect(first && registry.SetTextureData(
            scope, first, {0x10, 0x20, 0x30, 0x40}, anomaly::UiTextureFormat::Rgba8, 1, 1),
        "worker staging did not accept a file texture") && result;

    const auto second = registry.RequestTexture(scope, FileTextureRequest());
    const auto first_state = registry.TextureState(scope, first);
    const auto second_state = registry.TextureState(scope, second);
    result = Expect(second && first_state && second_state &&
            first_state->resource.resource_id != 0 &&
            first_state->resource.resource_id == second_state->resource.resource_id &&
            first_state->resource.references == 2 &&
            second_state->request.format == anomaly::UiTextureFormat::Rgba8 &&
            second_state->request.encoded_bytes.size() == 4,
        "worker staging changed the texture cache identity for a duplicate request") && result;

    const std::uint64_t resource_id = first_state ? first_state->resource.resource_id : 0;
    result = Expect(registry.IsResourceLive(resource_id) && registry.Release(scope, first) &&
            registry.IsResourceLive(resource_id) && registry.Release(scope, second) &&
            !registry.IsResourceLive(resource_id),
        "canonical cache liveness did not follow the last duplicate lease") && result;
    return result;
}

bool TestStagingBudgetAndReservations() {
    bool result = true;

    {
        const auto ledger = std::make_shared<anomaly::ResourceLedger>();
        const auto scope = Scope(ledger, 12);
        anomaly::UiResourceRegistry registry(8);
        const auto first = registry.RequestFont(scope, FontRequestWithBytes({1, 2, 3, 4}));
        const auto duplicate = registry.RequestFont(scope, FontRequestWithBytes({1, 2, 3, 4}));
        const auto state = registry.ResourceState(scope, first);
        result = Expect(first && duplicate && state && state->references == 2 &&
                state->staged_bytes == 4 && registry.StagingBytesInUse() == 4,
            "duplicate canonical font leases charged the staging budget twice") && result;

        const auto rejected = registry.RequestTexture(
            scope, TextureRequestWithBytes("images/too-large.png", {5, 6, 7, 8, 9}));
        result = Expect(!rejected && registry.StagingBytesInUse() == 4,
            "a second unique payload exceeded the global staging budget") && result;

        result = Expect(registry.ReserveResourceStaging(scope, first, 4) &&
                registry.StagingBytesInUse() == 8 &&
                registry.ResourceState(scope, duplicate)->reserved_staging_bytes == 4 &&
                registry.SetFontData(scope, duplicate, {10, 11, 12}) &&
                registry.StagingBytesInUse() == 3 &&
                registry.ResourceState(scope, first)->staged_bytes == 3 &&
                registry.ResourceState(scope, first)->reserved_staging_bytes == 0,
            "resource-scoped staging reservation did not replace the prior payload") && result;

        result = Expect(registry.ReserveResourceStaging(scope, first, 5) &&
                registry.StagingBytesInUse() == 8 &&
                registry.SetFontData(scope, first, {20, 21, 22, 23, 24}) &&
                registry.StagingBytesInUse() == 5 &&
                registry.MarkFontFailed(scope, duplicate) && registry.StagingBytesInUse() == 0,
            "failed resource staging did not release the shared payload charge") && result;
    }

    {
        const auto ledger = std::make_shared<anomaly::ResourceLedger>();
        const auto scope = Scope(ledger, 13);
        anomaly::UiResourceRegistry registry(8);
        const auto first = registry.RequestTexture(
            scope, TextureRequestWithBytes("images/revoke.png", {1, 2, 3, 4}));
        const auto duplicate = registry.RequestTexture(
            scope, TextureRequestWithBytes("images/revoke.png", {1, 2, 3, 4}));
        result = Expect(first && duplicate && registry.StagingBytesInUse() == 4 &&
                registry.Release(scope, first) && registry.StagingBytesInUse() == 4 &&
                scope->RevokeAll() == 1 && registry.StagingBytesInUse() == 0,
            "last resource lease revocation did not release the staging payload") && result;
    }

    {
        const auto ledger = std::make_shared<anomaly::ResourceLedger>();
        const auto scope = Scope(ledger, 14);
        anomaly::UiResourceRegistry registry(8);
        const auto first_reservation = registry.ReserveStaging(4);
        result = Expect(first_reservation && registry.StagingBytesInUse() == 4,
            "unbound staging reservation did not charge capacity before a request copy") && result;

        const auto first = registry.RequestFont(
            scope, FontRequestWithBytes({1, 2, 3, 4}), first_reservation);
        const auto duplicate_reservation = registry.ReserveStaging(4);
        const auto duplicate = registry.RequestFont(
            scope, FontRequestWithBytes({1, 2, 3, 4}), duplicate_reservation);
        result = Expect(first && duplicate && registry.StagingBytesInUse() == 4 &&
                registry.ResourceState(scope, first)->staged_bytes == 4,
            "request did not consume or discard unbound staging reservations correctly") && result;

        const auto released_reservation = registry.ReserveStaging(4);
        result = Expect(released_reservation && registry.StagingBytesInUse() == 8 &&
                registry.ReleaseStaging(released_reservation) && registry.StagingBytesInUse() == 4 &&
                !registry.ReleaseStaging(released_reservation) && !registry.ReserveStaging(5),
            "unbound staging reservations did not return capacity deterministically") && result;
    }

    return result;
}

bool TestScopeRevocationAndConcurrentWindowAccess() {
    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = Scope(ledger, 10);
    anomaly::UiResourceRegistry registry;
    const auto window = registry.RegisterWindow(scope, WindowRequest());
    const auto font = registry.RequestFont(scope, FontRequest());
    const auto texture = registry.RequestTexture(scope, TextureRequest());
    bool result = true;
    result = Expect(window && font && texture && ledger->Snapshot(scope->Owner()).size() == 3,
        "UI resources were not recorded in the plugin scope ledger") && result;

    std::vector<std::thread> workers;
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&] {
            for (int iteration = 0; iteration < 250; ++iteration) {
                static_cast<void>(registry.ToggleWindow(scope, window));
            }
        });
    }
    for (auto& worker : workers) worker.join();
    result = Expect(registry.ShouldDrawWindow(scope, window),
        "concurrent even toggles changed the window state") && result;

    result = Expect(scope->RevokeAll() == 3 && ledger->Snapshot(scope->Owner()).empty() &&
            !registry.WindowState(scope, window) && !registry.FontState(scope, font) &&
            !registry.TextureState(scope, texture) && !registry.Release(scope, texture),
        "scope cleanup did not deterministically revoke UI resource handles") && result;
    return result;
}

}  // namespace

int main() {
    bool result = TestWindowsAndPersistence();
    result = TestResourceStateAndDeviceRebuild() && result;
    result = TestHostFontScaleRebuildsReadyFonts() && result;
    result = TestTextureIdentitySurvivesWorkerStaging() && result;
    result = TestStagingBudgetAndReservations() && result;
    result = TestScopeRevocationAndConcurrentWindowAccess() && result;
    if (!result || failures != 0) return 1;
    std::cout << "UI resource registry scope, persistence, and device generation tests passed\n";
    return 0;
}
