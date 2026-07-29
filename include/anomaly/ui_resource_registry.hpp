#pragma once

#include "anomaly/plugin_scope.hpp"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anomaly {

// Process-local opaque token. Every operation also requires the PluginScope
// that created it, so a token cannot cross a plugin generation boundary.
struct UiResourceHandle final {
    std::uint64_t id{};

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
    auto operator<=>(const UiResourceHandle&) const = default;
};

// Host-only token used to reserve capacity before an ABI adapter copies or a
// Worker allocates a staged resource payload. Tokens are one-shot: a matching
// Request* call consumes them, otherwise ReleaseStaging returns the capacity.
struct UiResourceStagingReservation final {
    std::uint64_t id{};
    std::size_t bytes{};

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
    auto operator<=>(const UiResourceStagingReservation&) const = default;
};

inline constexpr std::size_t kDefaultUiResourceStagingByteBudget =
    256U * 1024U * 1024U;

enum class UiResourceKind : std::uint8_t {
    Window,
    Font,
    Texture,
};

// Font and texture resources are intentionally backend-neutral. A render host
// changes their state as device generations are lost and rebuilt.
enum class UiResourceState : std::uint8_t {
    Queued,
    Ready,
    Failed,
    StaleDevice,
    Revoked,
};

enum class UiGlyphRange : std::uint32_t {
    Default = 0,
    Latin = 1,
    Cyrillic = 2,
    Japanese = 3,
    ChineseFull = 4,
};

enum class UiTextureFormat : std::uint32_t {
    Auto = 0,
    Rgba8 = 1,
};

struct UiWindowConstraints final {
    float minimum_width{};
    float minimum_height{};
    // A zero maximum means unbounded.
    float maximum_width{};
    float maximum_height{};
};

// This is a C++ host descriptor rather than an ImGui type. The proxy layer can
// map the public Window service descriptor to this shape without leaking UI
// implementation details.
struct UiWindowRequest final {
    std::string id;
    std::string title;
    std::uint32_t flags{};
    bool persist_settings{true};
    float initial_width{};
    float initial_height{};
    UiWindowConstraints constraints;
    bool default_open{true};
};

struct UiWindowPersistentState final {
    std::string stable_id;
    bool open{true};
    float width{};
    float height{};
    UiWindowConstraints constraints;
};

struct UiWindowSnapshot final {
    UiResourceHandle handle;
    std::string owner;
    std::uint64_t generation{};
    std::string stable_id;
    std::string title;
    std::uint32_t flags{};
    bool open{};
    float width{};
    float height{};
    UiWindowConstraints constraints;
};

struct UiWindowGroupState final {
    std::size_t window_count{};
    std::size_t open_window_count{};
};

struct UiFontRequest final {
    // The plugin adapter records this internal source boundary. The registry
    // retains it for the Worker, which opens package files only after it has
    // admitted the asynchronous request.
    std::filesystem::path package_directory;
    std::string relative_path;
    // Worker-owned staging bytes. The render backend consumes this copy when
    // rebuilding the host font atlas and never reads the package filesystem.
    std::vector<std::uint8_t> encoded_bytes;
    std::uint32_t flags{};
    float size_pixels{};
    float scale{1.0F};
    UiGlyphRange glyph_range{UiGlyphRange::Default};
};

struct UiTextureRequest final {
    // Empty for caller-owned bytes and direct host fixtures. A non-empty value
    // requires the Worker to re-open relative_path through package confinement.
    std::filesystem::path package_directory;
    std::string relative_path;
    // The registry takes ownership of encoded bytes. It does not decode or
    // upload them; a later worker/render proxy consumes this descriptor.
    std::vector<std::uint8_t> encoded_bytes;
    std::uint32_t flags{};
    UiTextureFormat format{UiTextureFormat::Auto};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct UiResourceSnapshot final {
    UiResourceHandle handle;
    // Canonical logical resource identity shared by duplicate font/texture
    // leases. Windows do not have a shared backend resource and use zero.
    std::uint64_t resource_id{};
    UiResourceKind kind{UiResourceKind::Window};
    std::string owner;
    std::uint64_t generation{};
    UiResourceState state{UiResourceState::Revoked};
    std::uint64_t device_generation{};
    std::size_t references{};
    std::size_t staged_bytes{};
    std::size_t reserved_staging_bytes{};
    float effective_font_size_pixels{};
    float font_scale{1.0F};
    UiGlyphRange font_glyph_range{UiGlyphRange::Default};
    UiTextureFormat texture_format{UiTextureFormat::Auto};
    std::uint32_t texture_width{};
    std::uint32_t texture_height{};
};

struct UiFontSnapshot final {
    UiResourceSnapshot resource;
    UiFontRequest request;
    float effective_size_pixels{};
    float scale{1.0F};
};

struct UiTextureSnapshot final {
    UiResourceSnapshot resource;
    UiTextureRequest request;
    std::size_t byte_size{};
    std::uint32_t width{};
    std::uint32_t height{};
};

// Thread-safe process-lifetime registry for the logical UI resources owned by
// plugin scopes. It deliberately has no ImGui or D3D dependency.
class UiResourceRegistry final {
public:
    explicit UiResourceRegistry(
        std::size_t staging_byte_budget = kDefaultUiResourceStagingByteBudget);
    ~UiResourceRegistry();

    UiResourceRegistry(const UiResourceRegistry&) = delete;
    UiResourceRegistry& operator=(const UiResourceRegistry&) = delete;

    [[nodiscard]] UiResourceHandle RegisterWindow(
        const std::shared_ptr<PluginScope>& scope, UiWindowRequest request);
    [[nodiscard]] bool OpenWindow(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle);
    [[nodiscard]] bool CloseWindow(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle);
    [[nodiscard]] bool ToggleWindow(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle);
    [[nodiscard]] bool ShouldDrawWindow(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) const;
    [[nodiscard]] bool SetWindowConstraints(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle,
        UiWindowConstraints constraints);
    [[nodiscard]] bool SetWindowSize(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle,
        float width, float height);
    [[nodiscard]] std::optional<UiWindowSnapshot> WindowState(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) const;

    // Host visibility controls operate on a plugin's complete managed-window
    // group. Opening an entirely closed group restores its default-open windows,
    // or its first registered window when no default exists.
    [[nodiscard]] UiWindowGroupState WindowGroupState(
        const std::shared_ptr<PluginScope>& scope) const noexcept;
    [[nodiscard]] bool SetWindowGroupOpen(
        const std::shared_ptr<PluginScope>& scope, bool open) noexcept;

    // Exported records are sorted by stable ID. Import replaces the persistent
    // store atomically and applies matching records to currently live windows.
    [[nodiscard]] std::vector<UiWindowPersistentState> ExportPersistentWindowState() const;
    [[nodiscard]] bool ImportPersistentWindowState(
        std::vector<UiWindowPersistentState> state);

    // Reserve unbound capacity before copying an ABI byte span into a request.
    // A token is consumed by the matching Request* call or released explicitly.
    [[nodiscard]] UiResourceStagingReservation ReserveStaging(std::size_t byte_count) noexcept;
    [[nodiscard]] bool ReleaseStaging(UiResourceStagingReservation reservation) noexcept;
    // Reserve a prospective replacement payload before a Worker decodes it.
    // The reservation is canonical-resource scoped, so duplicate leases do not
    // multiply the host-wide capacity charge.
    [[nodiscard]] bool ReserveResourceStaging(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle,
        std::size_t byte_count) noexcept;
    [[nodiscard]] std::size_t StagingBytesInUse() const noexcept;
    [[nodiscard]] std::size_t StagingByteBudget() const noexcept;

    [[nodiscard]] UiResourceHandle RequestFont(
        const std::shared_ptr<PluginScope>& scope, UiFontRequest request,
        UiResourceStagingReservation reservation = {});
    [[nodiscard]] UiResourceHandle RequestTexture(
        const std::shared_ptr<PluginScope>& scope, UiTextureRequest request,
        UiResourceStagingReservation reservation = {});
    [[nodiscard]] std::optional<UiFontSnapshot> FontState(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) const;
    [[nodiscard]] std::optional<UiTextureSnapshot> TextureState(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) const;
    // These return the logical request for a live lease. They are intended for
    // a worker/render bridge and do not expose backend objects.
    [[nodiscard]] std::optional<UiFontRequest> FontRequest(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) const;
    [[nodiscard]] std::optional<UiTextureRequest> TextureRequest(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) const;
    // Worker staging updates replace only the data payload. A failed or
    // revoked resource cannot be revived by a stale worker completion.
    [[nodiscard]] bool SetFontData(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle,
        std::vector<std::uint8_t> encoded_bytes) noexcept;
    [[nodiscard]] bool SetTextureData(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle,
        std::vector<std::uint8_t> encoded_bytes, UiTextureFormat format,
        std::uint32_t width, std::uint32_t height) noexcept;
    [[nodiscard]] std::optional<UiResourceSnapshot> ResourceState(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) const;
    [[nodiscard]] std::vector<UiResourceSnapshot> Resources(
        const std::shared_ptr<PluginScope>& scope) const;
    // Host diagnostics use this to verify that scope revocation left no live
    // Window/Font/Texture leases behind after a plugin generation stops.
    [[nodiscard]] std::size_t ResourceLeaseCount() const noexcept;

    // Host-only backend coordination. A cache entry can outlive an individual
    // lease while another duplicate lease has not reached Prepare* yet.
    [[nodiscard]] bool IsResourceLive(std::uint64_t resource_id) const noexcept;

    // These transitions are host-only coordination points for decode/upload
    // workers. They still require the owning scope, which rejects stale leases.
    [[nodiscard]] bool MarkFontFailed(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) noexcept;
    [[nodiscard]] bool MarkTextureFailed(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) noexcept;
    [[nodiscard]] bool MarkFontReady(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle,
        std::uint64_t device_generation) noexcept;
    [[nodiscard]] bool MarkTextureReady(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle,
        std::uint64_t device_generation, std::uint32_t width, std::uint32_t height) noexcept;
    [[nodiscard]] bool RetryFont(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) noexcept;
    [[nodiscard]] bool RetryTexture(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) noexcept;

    // Render-host only. This applies the current host DPI multiplier without
    // changing a plugin request's canonical identity. Ready fonts return to
    // Queued so the backend rebuilds the atlas at the new effective size.
    [[nodiscard]] bool SetHostFontScale(float scale) noexcept;

    // Initial device generation is one. Invalidate returns a new generation
    // and marks ready resources stale. Rebuild only acknowledges the current
    // generation; individual resources become Ready only after their backend
    // upload or atlas build calls Mark*Ready.
    [[nodiscard]] std::uint64_t DeviceGeneration() const noexcept;
    [[nodiscard]] std::uint64_t InvalidateDeviceResources() noexcept;
    [[nodiscard]] bool RebuildDeviceResources(std::uint64_t generation) noexcept;

    // Releases one resource lease. Scope teardown instead calls the ledger
    // revoker registered at creation time, which reaches the same path.
    [[nodiscard]] bool Release(
        const std::shared_ptr<PluginScope>& scope, UiResourceHandle handle) noexcept;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace anomaly
