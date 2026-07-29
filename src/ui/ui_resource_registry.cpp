#include "anomaly/ui_resource_registry.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace anomaly {
namespace {

bool HasText(const std::string_view value) noexcept {
    return !value.empty() && value.find('\0') == std::string_view::npos;
}

bool HasOptionalText(const std::string_view value) noexcept {
    return value.find('\0') == std::string_view::npos;
}

bool IsFiniteNonNegative(const float value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

bool IsFinitePositive(const float value) noexcept {
    return std::isfinite(value) && value > 0.0F;
}

bool ValidConstraints(const UiWindowConstraints& constraints) noexcept {
    if (!IsFiniteNonNegative(constraints.minimum_width) ||
        !IsFiniteNonNegative(constraints.minimum_height) ||
        !IsFiniteNonNegative(constraints.maximum_width) ||
        !IsFiniteNonNegative(constraints.maximum_height)) {
        return false;
    }
    return (constraints.maximum_width == 0.0F ||
            constraints.minimum_width <= constraints.maximum_width) &&
        (constraints.maximum_height == 0.0F ||
            constraints.minimum_height <= constraints.maximum_height);
}

bool ValidWindowSize(
    const float width, const float height, const UiWindowConstraints& constraints) noexcept {
    if (!IsFiniteNonNegative(width) || !IsFiniteNonNegative(height)) return false;
    if (width != 0.0F && width < constraints.minimum_width) return false;
    if (height != 0.0F && height < constraints.minimum_height) return false;
    if (constraints.maximum_width != 0.0F && width > constraints.maximum_width) return false;
    if (constraints.maximum_height != 0.0F && height > constraints.maximum_height) return false;
    return true;
}

bool ValidGlyphRange(const UiGlyphRange range) noexcept {
    return range >= UiGlyphRange::Default && range <= UiGlyphRange::ChineseFull;
}

bool ValidTextureFormat(const UiTextureFormat format) noexcept {
    return format == UiTextureFormat::Auto || format == UiTextureFormat::Rgba8;
}

bool ValidWindowRequest(const UiWindowRequest& request) noexcept {
    return HasText(request.id) && HasOptionalText(request.title) &&
        ValidConstraints(request.constraints) &&
        ValidWindowSize(request.initial_width, request.initial_height, request.constraints);
}

bool ValidPersistentWindowState(const UiWindowPersistentState& state) noexcept {
    return HasText(state.stable_id) && ValidConstraints(state.constraints) &&
        ValidWindowSize(state.width, state.height, state.constraints);
}

bool ValidFontRequest(const UiFontRequest& request) noexcept {
    return HasText(request.relative_path) && IsFinitePositive(request.size_pixels) &&
        IsFinitePositive(request.scale) && ValidGlyphRange(request.glyph_range);
}

bool ValidTextureRequest(const UiTextureRequest& request) noexcept {
    return HasOptionalText(request.relative_path) &&
        (!request.relative_path.empty() || !request.encoded_bytes.empty()) &&
        ValidTextureFormat(request.format);
}

void AppendUnsigned(std::string& key, const std::uint64_t value) {
    key.append(std::to_string(value));
    key.push_back(';');
}

void AppendFloat(std::string& key, const float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    AppendUnsigned(key, bits);
}

void AppendText(std::string& key, const std::string_view value) {
    AppendUnsigned(key, value.size());
    key.append(value.data(), value.size());
    key.push_back(';');
}

std::uint64_t FingerprintBytes(
    const std::vector<std::uint8_t>& bytes,
    const std::uint64_t seed) noexcept {
    std::uint64_t value = seed;
    for (const std::uint8_t byte : bytes) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

std::uint64_t ReverseFingerprintBytes(const std::vector<std::uint8_t>& bytes) noexcept {
    std::uint64_t value = 0x9E3779B185EBCA87ULL;
    for (auto iterator = bytes.rbegin(); iterator != bytes.rend(); ++iterator) {
        value ^= *iterator;
        value *= 0xC2B2AE3D27D4EB4FULL;
    }
    return value;
}

struct TextureIdentity final {
    // These fingerprints are cache identity hints rather than cryptographic
    // equality proofs. They avoid retaining a second full encoded payload.
    std::string relative_path;
    std::size_t encoded_byte_size{};
    std::uint64_t encoded_fingerprint{};
    std::uint64_t reverse_encoded_fingerprint{};
    std::uint32_t flags{};
    UiTextureFormat format{UiTextureFormat::Auto};
    std::uint32_t width{};
    std::uint32_t height{};
};

TextureIdentity MakeTextureIdentity(const UiTextureRequest& request) {
    return {request.relative_path, request.encoded_bytes.size(),
        FingerprintBytes(request.encoded_bytes, 1469598103934665603ULL),
        ReverseFingerprintBytes(request.encoded_bytes), request.flags,
        request.format, request.width, request.height};
}

std::string StableWindowId(const PluginScope& scope, const std::string_view local_id) {
    std::string result;
    result.reserve(scope.Owner().size() + local_id.size() + 32U);
    AppendText(result, scope.Owner());
    AppendText(result, local_id);
    return result;
}

void AppendScopeKey(std::string& key, const PluginScope& scope) {
    AppendText(key, scope.Owner());
    AppendUnsigned(key, scope.Generation());
}

std::string FontKey(const PluginScope& scope, const UiFontRequest& request) {
    std::string result{"font;"};
    AppendScopeKey(result, scope);
    AppendText(result, request.relative_path);
    AppendUnsigned(result, request.flags);
    AppendFloat(result, request.size_pixels);
    AppendFloat(result, request.scale);
    AppendUnsigned(result, static_cast<std::uint32_t>(request.glyph_range));
    return result;
}

std::string TextureKeyBase(const PluginScope& scope, const TextureIdentity& identity) {
    std::string result{"texture;"};
    AppendScopeKey(result, scope);
    AppendText(result, identity.relative_path);
    AppendUnsigned(result, identity.encoded_byte_size);
    AppendUnsigned(result, identity.encoded_fingerprint);
    AppendUnsigned(result, identity.reverse_encoded_fingerprint);
    AppendUnsigned(result, identity.flags);
    AppendUnsigned(result, static_cast<std::uint32_t>(identity.format));
    AppendUnsigned(result, identity.width);
    AppendUnsigned(result, identity.height);
    return result;
}

bool SameTextureIdentity(
    const TextureIdentity& left, const TextureIdentity& right) noexcept {
    return left.relative_path == right.relative_path &&
        left.encoded_byte_size == right.encoded_byte_size &&
        left.encoded_fingerprint == right.encoded_fingerprint &&
        left.reverse_encoded_fingerprint == right.reverse_encoded_fingerprint &&
        left.flags == right.flags && left.format == right.format &&
        left.width == right.width && left.height == right.height;
}

}  // namespace

class UiResourceRegistry::Impl final : public std::enable_shared_from_this<Impl> {
public:
    explicit Impl(const std::size_t staging_byte_budget) noexcept
        : staging_byte_budget(staging_byte_budget) {}

    struct WindowData final {
        std::string stable_id;
        std::string title;
        std::uint32_t flags{};
        bool persist_settings{true};
        bool default_open{true};
        bool open{};
        float width{};
        float height{};
        UiWindowConstraints constraints;
    };

    struct ResourceData final {
        UiResourceKind kind{UiResourceKind::Font};
        UiResourceState state{UiResourceState::Queued};
        std::uint64_t device_generation{};
        std::size_t references{};
        std::size_t staged_bytes{};
        std::size_t reserved_staging_bytes{};
        std::string key;
        UiFontRequest font;
        TextureIdentity texture_identity;
        UiTextureRequest texture;
        std::uint32_t texture_width{};
        std::uint32_t texture_height{};
    };

    struct Lease final {
        UiResourceKind kind{UiResourceKind::Window};
        std::weak_ptr<PluginScope> scope;
        std::string owner;
        std::uint64_t generation{};
        std::uint64_t ledger_token{};
        std::uint64_t resource_id{};
        WindowData window;
    };

    [[nodiscard]] bool ReserveStagingBytesLocked(const std::size_t byte_count) noexcept {
        if (byte_count == 0) return true;
        if (staging_bytes_in_use > staging_byte_budget ||
            byte_count > staging_byte_budget - staging_bytes_in_use) {
            return false;
        }
        staging_bytes_in_use += byte_count;
        return true;
    }

    void ReleaseStagingBytesLocked(const std::size_t byte_count) noexcept {
        if (byte_count >= staging_bytes_in_use) {
            staging_bytes_in_use = 0;
        } else {
            staging_bytes_in_use -= byte_count;
        }
    }

    void ReleaseResourceStagingLocked(ResourceData& resource) noexcept {
        ReleaseStagingBytesLocked(resource.staged_bytes);
        ReleaseStagingBytesLocked(resource.reserved_staging_bytes);
        resource.staged_bytes = 0;
        resource.reserved_staging_bytes = 0;
        if (resource.kind == UiResourceKind::Font) {
            std::vector<std::uint8_t>{}.swap(resource.font.encoded_bytes);
        } else if (resource.kind == UiResourceKind::Texture) {
            std::vector<std::uint8_t>{}.swap(resource.texture.encoded_bytes);
            resource.texture.format = resource.texture_identity.format;
            resource.texture.width = resource.texture_identity.width;
            resource.texture.height = resource.texture_identity.height;
        }
    }

    [[nodiscard]] bool CommitResourceStagingLocked(
        ResourceData& resource, const std::size_t byte_count) noexcept {
        if (resource.staged_bytes > staging_bytes_in_use) return false;
        const std::size_t after_staged = staging_bytes_in_use - resource.staged_bytes;
        if (resource.reserved_staging_bytes > after_staged) return false;
        const std::size_t other_bytes = after_staged - resource.reserved_staging_bytes;
        if (other_bytes > staging_byte_budget || byte_count > staging_byte_budget - other_bytes) {
            return false;
        }
        staging_bytes_in_use = other_bytes + byte_count;
        resource.staged_bytes = byte_count;
        resource.reserved_staging_bytes = 0;
        return true;
    }

    [[nodiscard]] bool ReserveResourceStagingLocked(
        ResourceData& resource, const std::size_t byte_count) noexcept {
        if (byte_count > resource.reserved_staging_bytes) {
            const std::size_t additional = byte_count - resource.reserved_staging_bytes;
            if (!ReserveStagingBytesLocked(additional)) return false;
        } else {
            ReleaseStagingBytesLocked(resource.reserved_staging_bytes - byte_count);
        }
        resource.reserved_staging_bytes = byte_count;
        return true;
    }

    [[nodiscard]] bool ConsumeUnboundStagingLocked(
        const UiResourceStagingReservation reservation,
        const std::size_t byte_count,
        ResourceData& resource) noexcept {
        if (!reservation) return false;
        const auto found = unbound_staging.find(reservation.id);
        if (found == unbound_staging.end() || byte_count > found->second) return false;
        ReleaseStagingBytesLocked(found->second);
        unbound_staging.erase(found);
        if (!ReserveStagingBytesLocked(byte_count)) return false;
        resource.staged_bytes = byte_count;
        return true;
    }

    void ReleaseUnboundStagingLocked(const UiResourceStagingReservation reservation) noexcept {
        if (!reservation) return;
        const auto found = unbound_staging.find(reservation.id);
        if (found == unbound_staging.end()) return;
        ReleaseStagingBytesLocked(found->second);
        unbound_staging.erase(found);
    }

    [[nodiscard]] bool MatchesScope(
        const Lease& lease, const std::shared_ptr<PluginScope>& scope) const noexcept {
        if (!scope || lease.owner != scope->Owner() || lease.generation != scope->Generation()) {
            return false;
        }
        const auto owner = lease.scope.lock();
        return owner != nullptr && owner == scope;
    }

    [[nodiscard]] std::optional<UiResourceSnapshot> SnapshotLocked(
        const Lease& lease, const UiResourceHandle handle) const {
        UiResourceSnapshot snapshot;
        snapshot.handle = handle;
        snapshot.resource_id = lease.resource_id;
        snapshot.kind = lease.kind;
        snapshot.owner = lease.owner;
        snapshot.generation = lease.generation;
        if (lease.kind == UiResourceKind::Window) {
            snapshot.state = UiResourceState::Ready;
            snapshot.references = 1;
            return snapshot;
        }
        const auto resource = resources.find(lease.resource_id);
        if (resource == resources.end()) return std::nullopt;
        snapshot.state = resource->second.state;
        snapshot.device_generation = resource->second.device_generation;
        snapshot.references = resource->second.references;
        snapshot.staged_bytes = resource->second.staged_bytes;
        snapshot.reserved_staging_bytes = resource->second.reserved_staging_bytes;
        if (resource->second.kind == UiResourceKind::Font) {
            const float effective_scale = resource->second.font.scale * host_font_scale;
            snapshot.effective_font_size_pixels =
                resource->second.font.size_pixels * effective_scale;
            snapshot.font_scale = effective_scale;
            snapshot.font_glyph_range = resource->second.font.glyph_range;
        } else if (resource->second.kind == UiResourceKind::Texture) {
            snapshot.texture_format = resource->second.texture.format;
            snapshot.texture_width = resource->second.texture_width != 0
                ? resource->second.texture_width : resource->second.texture.width;
            snapshot.texture_height = resource->second.texture_height != 0
                ? resource->second.texture_height : resource->second.texture.height;
        }
        return snapshot;
    }

    [[nodiscard]] std::optional<UiResourceSnapshot> ResourceState(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || !MatchesScope(lease->second, scope)) return std::nullopt;
        return SnapshotLocked(lease->second, handle);
    }

    [[nodiscard]] UiResourceStagingReservation ReserveStaging(
        const std::size_t byte_count) noexcept {
        if (byte_count == 0) return {};
        try {
            std::scoped_lock lock(mutex);
            if (!ReserveStagingBytesLocked(byte_count)) return {};
            if (next_unbound_staging_id == 0) {
                ReleaseStagingBytesLocked(byte_count);
                return {};
            }
            const std::uint64_t id = next_unbound_staging_id++;
            bool inserted{};
            try {
                inserted = unbound_staging.emplace(id, byte_count).second;
            } catch (...) {
                ReleaseStagingBytesLocked(byte_count);
                return {};
            }
            if (!inserted) {
                ReleaseStagingBytesLocked(byte_count);
                return {};
            }
            return {id, byte_count};
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] bool ReleaseStaging(
        const UiResourceStagingReservation reservation) noexcept {
        if (!reservation) return false;
        std::scoped_lock lock(mutex);
        if (!unbound_staging.contains(reservation.id)) return false;
        ReleaseUnboundStagingLocked(reservation);
        return true;
    }

    [[nodiscard]] bool ReserveResourceStaging(
        const std::shared_ptr<PluginScope>& scope,
        const UiResourceHandle handle,
        const std::size_t byte_count) noexcept {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind == UiResourceKind::Window ||
            !MatchesScope(lease->second, scope)) {
            return false;
        }
        const auto resource = resources.find(lease->second.resource_id);
        if (resource == resources.end() || resource->second.state == UiResourceState::Failed ||
            resource->second.state == UiResourceState::Revoked) {
            return false;
        }
        return ReserveResourceStagingLocked(resource->second, byte_count);
    }

    [[nodiscard]] std::size_t StagingBytesInUse() const noexcept {
        std::scoped_lock lock(mutex);
        return staging_bytes_in_use;
    }

    [[nodiscard]] std::size_t StagingByteBudget() const noexcept {
        return staging_byte_budget;
    }

    void RevokeFromLedger(const std::uint64_t handle) noexcept {
        std::scoped_lock lock(mutex);
        static_cast<void>(RemoveLeaseLocked(handle));
    }

    [[nodiscard]] bool RemoveLeaseLocked(const std::uint64_t handle) noexcept {
        const auto found = leases.find(handle);
        if (found == leases.end()) return false;
        const Lease& lease = found->second;
        if (lease.kind == UiResourceKind::Window) {
            const auto window = windows.find(lease.window.stable_id);
            if (window != windows.end() && window->second == handle) windows.erase(window);
        } else {
            const auto resource = resources.find(lease.resource_id);
            if (resource != resources.end()) {
                if (resource->second.references != 0) --resource->second.references;
                if (resource->second.references == 0) {
                    if (resource->second.kind == UiResourceKind::Font) {
                        fonts.erase(resource->second.key);
                    } else {
                        textures.erase(resource->second.key);
                    }
                    ReleaseResourceStagingLocked(resource->second);
                    resources.erase(resource);
                }
            }
        }
        leases.erase(found);
        return true;
    }

    [[nodiscard]] std::uint64_t RegisterLedgerLocked(
        const std::shared_ptr<PluginScope>& scope,
        const PluginResourceKind kind,
        std::string label,
        const std::uint64_t handle) {
        const std::weak_ptr<Impl> weak = shared_from_this();
        return scope->Register(kind, std::move(label), [weak, handle] {
            const auto state = weak.lock();
            if (state) state->RevokeFromLedger(handle);
        });
    }

    [[nodiscard]] UiResourceHandle RegisterWindow(
        const std::shared_ptr<PluginScope>& scope, UiWindowRequest request) {
        if (!scope || !ValidWindowRequest(request)) return {};
        if (request.title.empty()) request.title = request.id;
        const std::string stable_id = StableWindowId(*scope, request.id);

        std::scoped_lock lock(mutex);
        if (windows.contains(stable_id)) return {};

        UiWindowPersistentState state{
            stable_id, request.default_open, request.initial_width, request.initial_height,
            request.constraints};
        if (request.persist_settings) {
            const auto persisted = persistent_windows.find(stable_id);
            if (persisted == persistent_windows.end()) {
                persistent_windows.emplace(stable_id, state);
            } else {
                state = persisted->second;
            }
        }

        const std::uint64_t handle = next_handle++;
        Lease lease;
        lease.kind = UiResourceKind::Window;
        lease.scope = scope;
        lease.owner = scope->Owner();
        lease.generation = scope->Generation();
        lease.window = {stable_id, std::move(request.title), request.flags,
            request.persist_settings, request.default_open, state.open,
            state.width, state.height, state.constraints};
        leases.emplace(handle, std::move(lease));
        windows.emplace(stable_id, handle);

        std::uint64_t token{};
        try {
            token = RegisterLedgerLocked(
                scope, PluginResourceKind::Window, "ui.window:" + request.id, handle);
        } catch (...) {
            static_cast<void>(RemoveLeaseLocked(handle));
            throw;
        }
        const auto registered = leases.find(handle);
        if (token == 0 || registered == leases.end()) {
            static_cast<void>(RemoveLeaseLocked(handle));
            return {};
        }
        registered->second.ledger_token = token;
        return {handle};
    }

    [[nodiscard]] UiResourceHandle RequestFont(
        const std::shared_ptr<PluginScope>& scope, UiFontRequest request,
        const UiResourceStagingReservation reservation) {
        if (!scope || !ValidFontRequest(request)) {
            std::scoped_lock lock(mutex);
            ReleaseUnboundStagingLocked(reservation);
            return {};
        }
        const std::string key = FontKey(*scope, request);
        const std::size_t payload_bytes = request.encoded_bytes.size();

        std::scoped_lock lock(mutex);
        std::uint64_t resource_id{};
        if (const auto found = fonts.find(key); found != fonts.end()) {
            resource_id = found->second;
            ++resources.at(resource_id).references;
            ReleaseUnboundStagingLocked(reservation);
        } else {
            ResourceData resource;
            resource.kind = UiResourceKind::Font;
            resource.references = 1;
            resource.key = key;
            const bool staged = reservation
                ? ConsumeUnboundStagingLocked(reservation, payload_bytes, resource)
                : ReserveStagingBytesLocked(payload_bytes);
            if (!staged) {
                ReleaseUnboundStagingLocked(reservation);
                return {};
            }
            if (!reservation) resource.staged_bytes = payload_bytes;
            resource.font = std::move(request);
            resource_id = next_resource_id++;
            bool resource_inserted{};
            try {
                resource_inserted = resources.emplace(resource_id, std::move(resource)).second;
                if (!resource_inserted || !fonts.emplace(key, resource_id).second) {
                    if (resource_inserted) {
                        const auto stored = resources.find(resource_id);
                        ReleaseResourceStagingLocked(stored->second);
                        resources.erase(stored);
                    } else {
                        ReleaseStagingBytesLocked(payload_bytes);
                    }
                    return {};
                }
            } catch (...) {
                if (resource_inserted) {
                    const auto stored = resources.find(resource_id);
                    if (stored != resources.end()) {
                        ReleaseResourceStagingLocked(stored->second);
                        resources.erase(stored);
                    }
                } else {
                    ReleaseStagingBytesLocked(payload_bytes);
                }
                throw;
            }
        }

        const std::uint64_t handle = next_handle++;
        Lease lease;
        lease.kind = UiResourceKind::Font;
        lease.scope = scope;
        lease.owner = scope->Owner();
        lease.generation = scope->Generation();
        lease.resource_id = resource_id;
        leases.emplace(handle, std::move(lease));

        std::uint64_t token{};
        try {
            token = RegisterLedgerLocked(scope, PluginResourceKind::Font, "ui.font", handle);
        } catch (...) {
            static_cast<void>(RemoveLeaseLocked(handle));
            throw;
        }
        const auto registered = leases.find(handle);
        if (token == 0 || registered == leases.end()) {
            static_cast<void>(RemoveLeaseLocked(handle));
            return {};
        }
        registered->second.ledger_token = token;
        return {handle};
    }

    [[nodiscard]] UiResourceHandle RequestTexture(
        const std::shared_ptr<PluginScope>& scope, UiTextureRequest request,
        const UiResourceStagingReservation reservation) {
        if (!scope || !ValidTextureRequest(request)) {
            std::scoped_lock lock(mutex);
            ReleaseUnboundStagingLocked(reservation);
            return {};
        }
        const TextureIdentity identity = MakeTextureIdentity(request);
        const std::size_t payload_bytes = request.encoded_bytes.size();

        std::scoped_lock lock(mutex);
        std::uint64_t resource_id{};
        std::string key = TextureKeyBase(*scope, identity);
        std::size_t collision{};
        for (;;) {
            const auto found = textures.find(key);
            if (found == textures.end()) break;
            const auto resource = resources.find(found->second);
            if (resource != resources.end() &&
                SameTextureIdentity(resource->second.texture_identity, identity)) {
                resource_id = found->second;
                ++resource->second.references;
                ReleaseUnboundStagingLocked(reservation);
                break;
            }
            key = TextureKeyBase(*scope, identity) + "collision;" + std::to_string(++collision);
        }
        if (resource_id == 0) {
            ResourceData resource;
            resource.kind = UiResourceKind::Texture;
            resource.references = 1;
            resource.key = key;
            resource.texture_identity = identity;
            const bool staged = reservation
                ? ConsumeUnboundStagingLocked(reservation, payload_bytes, resource)
                : ReserveStagingBytesLocked(payload_bytes);
            if (!staged) {
                ReleaseUnboundStagingLocked(reservation);
                return {};
            }
            if (!reservation) resource.staged_bytes = payload_bytes;
            resource.texture = std::move(request);
            resource_id = next_resource_id++;
            bool resource_inserted{};
            try {
                resource_inserted = resources.emplace(resource_id, std::move(resource)).second;
                if (!resource_inserted || !textures.emplace(key, resource_id).second) {
                    if (resource_inserted) {
                        const auto stored = resources.find(resource_id);
                        ReleaseResourceStagingLocked(stored->second);
                        resources.erase(stored);
                    } else {
                        ReleaseStagingBytesLocked(payload_bytes);
                    }
                    return {};
                }
            } catch (...) {
                if (resource_inserted) {
                    const auto stored = resources.find(resource_id);
                    if (stored != resources.end()) {
                        ReleaseResourceStagingLocked(stored->second);
                        resources.erase(stored);
                    }
                } else {
                    ReleaseStagingBytesLocked(payload_bytes);
                }
                throw;
            }
        }

        const std::uint64_t handle = next_handle++;
        Lease lease;
        lease.kind = UiResourceKind::Texture;
        lease.scope = scope;
        lease.owner = scope->Owner();
        lease.generation = scope->Generation();
        lease.resource_id = resource_id;
        leases.emplace(handle, std::move(lease));

        std::uint64_t token{};
        try {
            token = RegisterLedgerLocked(scope, PluginResourceKind::Texture, "ui.texture", handle);
        } catch (...) {
            static_cast<void>(RemoveLeaseLocked(handle));
            throw;
        }
        const auto registered = leases.find(handle);
        if (token == 0 || registered == leases.end()) {
            static_cast<void>(RemoveLeaseLocked(handle));
            return {};
        }
        registered->second.ledger_token = token;
        return {handle};
    }

    [[nodiscard]] bool SetWindowOpen(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
        const bool open) noexcept {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Window ||
            !MatchesScope(lease->second, scope)) {
            return false;
        }
        lease->second.window.open = open;
        if (lease->second.window.persist_settings) {
            const auto persistent = persistent_windows.find(lease->second.window.stable_id);
            if (persistent != persistent_windows.end()) persistent->second.open = open;
        }
        return true;
    }

    [[nodiscard]] UiWindowGroupState WindowGroupState(
        const std::shared_ptr<PluginScope>& scope) const noexcept {
        UiWindowGroupState state;
        if (!scope) return state;
        std::scoped_lock lock(mutex);
        for (const auto& [handle, lease] : leases) {
            static_cast<void>(handle);
            if (lease.kind != UiResourceKind::Window || !MatchesScope(lease, scope)) continue;
            ++state.window_count;
            if (lease.window.open) ++state.open_window_count;
        }
        return state;
    }

    [[nodiscard]] bool SetWindowGroupOpen(
        const std::shared_ptr<PluginScope>& scope, const bool open) noexcept {
        if (!scope) return false;
        std::scoped_lock lock(mutex);
        Lease* first{};
        std::uint64_t first_handle{(std::numeric_limits<std::uint64_t>::max)()};
        bool found{};
        bool any_open{};
        for (auto& [handle, lease] : leases) {
            if (lease.kind != UiResourceKind::Window || !MatchesScope(lease, scope)) continue;
            found = true;
            any_open = any_open || lease.window.open;
            if (handle < first_handle) {
                first = &lease;
                first_handle = handle;
            }
        }
        if (!found) return false;

        const auto set_open = [&](WindowData& window, const bool value) {
            window.open = value;
            if (!window.persist_settings) return;
            const auto persistent = persistent_windows.find(window.stable_id);
            if (persistent != persistent_windows.end()) persistent->second.open = value;
        };
        if (!open) {
            for (auto& [handle, lease] : leases) {
                static_cast<void>(handle);
                if (lease.kind == UiResourceKind::Window && MatchesScope(lease, scope)) {
                    set_open(lease.window, false);
                }
            }
            return true;
        }
        if (any_open) return true;

        bool opened_default{};
        for (auto& [handle, lease] : leases) {
            static_cast<void>(handle);
            if (lease.kind == UiResourceKind::Window && MatchesScope(lease, scope) &&
                lease.window.default_open) {
                set_open(lease.window, true);
                opened_default = true;
            }
        }
        if (!opened_default && first != nullptr) set_open(first->window, true);
        return true;
    }

    [[nodiscard]] bool ToggleWindow(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) noexcept {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Window ||
            !MatchesScope(lease->second, scope)) {
            return false;
        }
        lease->second.window.open = !lease->second.window.open;
        if (lease->second.window.persist_settings) {
            const auto persistent = persistent_windows.find(lease->second.window.stable_id);
            if (persistent != persistent_windows.end()) persistent->second.open = lease->second.window.open;
        }
        return true;
    }

    [[nodiscard]] bool ShouldDrawWindow(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const noexcept {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        return lease != leases.end() && lease->second.kind == UiResourceKind::Window &&
            MatchesScope(lease->second, scope) && lease->second.window.open;
    }

    [[nodiscard]] bool SetWindowConstraints(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
        const UiWindowConstraints constraints) noexcept {
        if (!ValidConstraints(constraints)) return false;
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Window ||
            !MatchesScope(lease->second, scope) ||
            !ValidWindowSize(lease->second.window.width, lease->second.window.height, constraints)) {
            return false;
        }
        lease->second.window.constraints = constraints;
        if (lease->second.window.persist_settings) {
            const auto persistent = persistent_windows.find(lease->second.window.stable_id);
            if (persistent != persistent_windows.end()) persistent->second.constraints = constraints;
        }
        return true;
    }

    [[nodiscard]] bool SetWindowSize(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
        const float width, const float height) noexcept {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Window ||
            !MatchesScope(lease->second, scope) ||
            !ValidWindowSize(width, height, lease->second.window.constraints)) {
            return false;
        }
        lease->second.window.width = width;
        lease->second.window.height = height;
        if (lease->second.window.persist_settings) {
            const auto persistent = persistent_windows.find(lease->second.window.stable_id);
            if (persistent != persistent_windows.end()) {
                persistent->second.width = width;
                persistent->second.height = height;
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<UiWindowSnapshot> WindowState(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Window ||
            !MatchesScope(lease->second, scope)) {
            return std::nullopt;
        }
        const WindowData& window = lease->second.window;
        return UiWindowSnapshot{handle, lease->second.owner, lease->second.generation,
            window.stable_id, window.title, window.flags, window.open, window.width, window.height,
            window.constraints};
    }

    [[nodiscard]] std::vector<UiWindowPersistentState> ExportPersistentWindowState() const {
        std::scoped_lock lock(mutex);
        std::vector<UiWindowPersistentState> result;
        result.reserve(persistent_windows.size());
        for (const auto& [id, state] : persistent_windows) result.push_back(state);
        return result;
    }

    [[nodiscard]] bool ImportPersistentWindowState(
        std::vector<UiWindowPersistentState> state) {
        std::map<std::string, UiWindowPersistentState, std::less<>> imported;
        for (auto& item : state) {
            if (!ValidPersistentWindowState(item) ||
                !imported.emplace(item.stable_id, std::move(item)).second) {
                return false;
            }
        }

        std::scoped_lock lock(mutex);
        persistent_windows = std::move(imported);
        for (auto& [handle, lease] : leases) {
            if (lease.kind != UiResourceKind::Window || !lease.window.persist_settings) continue;
            const auto found = persistent_windows.find(lease.window.stable_id);
            if (found == persistent_windows.end()) continue;
            lease.window.open = found->second.open;
            lease.window.width = found->second.width;
            lease.window.height = found->second.height;
            lease.window.constraints = found->second.constraints;
        }
        return true;
    }

    [[nodiscard]] std::optional<UiFontSnapshot> FontState(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Font ||
            !MatchesScope(lease->second, scope)) {
            return std::nullopt;
        }
        const auto resource = resources.find(lease->second.resource_id);
        if (resource == resources.end()) return std::nullopt;
        const auto snapshot = SnapshotLocked(lease->second, handle);
        if (!snapshot) return std::nullopt;
        const UiFontRequest& request = resource->second.font;
        const float effective_scale = request.scale * host_font_scale;
        return UiFontSnapshot{
            *snapshot, request, request.size_pixels * effective_scale, effective_scale};
    }

    [[nodiscard]] std::optional<UiTextureSnapshot> TextureState(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Texture ||
            !MatchesScope(lease->second, scope)) {
            return std::nullopt;
        }
        const auto resource = resources.find(lease->second.resource_id);
        if (resource == resources.end()) return std::nullopt;
        const auto snapshot = SnapshotLocked(lease->second, handle);
        if (!snapshot) return std::nullopt;
        return UiTextureSnapshot{*snapshot, resource->second.texture,
            resource->second.texture.encoded_bytes.size(), resource->second.texture_width,
            resource->second.texture_height};
    }

    [[nodiscard]] std::optional<UiFontRequest> FontRequest(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Font ||
            !MatchesScope(lease->second, scope)) {
            return std::nullopt;
        }
        const auto resource = resources.find(lease->second.resource_id);
        return resource == resources.end() ? std::nullopt
            : std::optional<UiFontRequest>{resource->second.font};
    }

    [[nodiscard]] std::optional<UiTextureRequest> TextureRequest(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Texture ||
            !MatchesScope(lease->second, scope)) {
            return std::nullopt;
        }
        const auto resource = resources.find(lease->second.resource_id);
        return resource == resources.end() ? std::nullopt
            : std::optional<UiTextureRequest>{resource->second.texture};
    }

    [[nodiscard]] bool SetFontData(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
        std::vector<std::uint8_t> encoded_bytes) noexcept {
        if (encoded_bytes.empty()) return false;
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Font ||
            !MatchesScope(lease->second, scope)) {
            return false;
        }
        const auto resource = resources.find(lease->second.resource_id);
        if (resource == resources.end() || resource->second.state == UiResourceState::Failed ||
            resource->second.state == UiResourceState::Revoked) {
            return false;
        }
        if (!CommitResourceStagingLocked(resource->second, encoded_bytes.size())) return false;
        resource->second.font.encoded_bytes = std::move(encoded_bytes);
        resource->second.state = UiResourceState::Queued;
        resource->second.device_generation = 0;
        return true;
    }

    [[nodiscard]] bool SetTextureData(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
        std::vector<std::uint8_t> encoded_bytes, const UiTextureFormat format,
        const std::uint32_t width, const std::uint32_t height) noexcept {
        if (encoded_bytes.empty() || format != UiTextureFormat::Rgba8 || width == 0 || height == 0 ||
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4U !=
                encoded_bytes.size()) {
            return false;
        }
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != UiResourceKind::Texture ||
            !MatchesScope(lease->second, scope)) {
            return false;
        }
        const auto resource = resources.find(lease->second.resource_id);
        if (resource == resources.end() || resource->second.state == UiResourceState::Failed ||
            resource->second.state == UiResourceState::Revoked) {
            return false;
        }
        if (!CommitResourceStagingLocked(resource->second, encoded_bytes.size())) return false;
        resource->second.texture.encoded_bytes = std::move(encoded_bytes);
        resource->second.texture.format = format;
        resource->second.texture.width = width;
        resource->second.texture.height = height;
        resource->second.state = UiResourceState::Queued;
        resource->second.texture_width = 0;
        resource->second.texture_height = 0;
        resource->second.device_generation = 0;
        return true;
    }

    [[nodiscard]] std::vector<UiResourceSnapshot> Resources(
        const std::shared_ptr<PluginScope>& scope) const {
        std::scoped_lock lock(mutex);
        std::vector<UiResourceSnapshot> result;
        for (const auto& [handle, lease] : leases) {
            if (!MatchesScope(lease, scope)) continue;
            const auto snapshot = SnapshotLocked(lease, {handle});
            if (snapshot) result.push_back(*snapshot);
        }
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.handle.id < right.handle.id;
        });
        return result;
    }

    [[nodiscard]] std::size_t ResourceLeaseCount() const noexcept {
        std::scoped_lock lock(mutex);
        return leases.size();
    }

    [[nodiscard]] bool IsResourceLive(const std::uint64_t resource_id) const noexcept {
        if (resource_id == 0) return false;
        std::scoped_lock lock(mutex);
        return resources.contains(resource_id);
    }

    [[nodiscard]] bool SetResourceState(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
        const UiResourceKind kind, const UiResourceState state) noexcept {
        std::scoped_lock lock(mutex);
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != kind ||
            !MatchesScope(lease->second, scope)) {
            return false;
        }
        const auto resource = resources.find(lease->second.resource_id);
        if (resource == resources.end()) return false;
        resource->second.state = state;
        if (state == UiResourceState::Queued) {
            resource->second.device_generation = 0;
        } else if (state == UiResourceState::Failed || state == UiResourceState::Revoked) {
            ReleaseResourceStagingLocked(resource->second);
            resource->second.device_generation = 0;
            resource->second.texture_width = 0;
            resource->second.texture_height = 0;
        }
        return true;
    }

    [[nodiscard]] bool MarkReady(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
        const UiResourceKind kind, const std::uint64_t generation,
        const std::uint32_t width = 0, const std::uint32_t height = 0) noexcept {
        if (generation == 0 ||
            (kind == UiResourceKind::Texture && (width == 0 || height == 0))) {
            return false;
        }
        std::scoped_lock lock(mutex);
        if (generation != device_generation) return false;
        const auto lease = leases.find(handle.id);
        if (lease == leases.end() || lease->second.kind != kind ||
            !MatchesScope(lease->second, scope)) {
            return false;
        }
        const auto resource = resources.find(lease->second.resource_id);
        if (resource == resources.end() || resource->second.state == UiResourceState::Failed) {
            return false;
        }
        resource->second.state = UiResourceState::Ready;
        resource->second.device_generation = generation;
        if (kind == UiResourceKind::Texture) {
            resource->second.texture_width = width;
            resource->second.texture_height = height;
        }
        return true;
    }

    [[nodiscard]] bool Release(
        const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) noexcept {
        std::uint64_t ledger_token{};
        {
            std::scoped_lock lock(mutex);
            const auto lease = leases.find(handle.id);
            if (lease == leases.end() || !MatchesScope(lease->second, scope)) return false;
            ledger_token = lease->second.ledger_token;
        }
        return ledger_token != 0 && scope->Release(ledger_token);
    }

    mutable std::mutex mutex;
    std::uint64_t next_handle{1};
    std::uint64_t next_resource_id{1};
    std::uint64_t next_unbound_staging_id{1};
    std::uint64_t device_generation{1};
    float host_font_scale{1.0F};
    std::size_t staging_byte_budget{};
    std::size_t staging_bytes_in_use{};
    std::unordered_map<std::uint64_t, Lease> leases;
    std::unordered_map<std::uint64_t, std::size_t> unbound_staging;
    std::unordered_map<std::string, std::uint64_t> windows;
    std::map<std::string, UiWindowPersistentState, std::less<>> persistent_windows;
    std::unordered_map<std::uint64_t, ResourceData> resources;
    std::unordered_map<std::string, std::uint64_t> fonts;
    std::unordered_map<std::string, std::uint64_t> textures;
};

UiResourceRegistry::UiResourceRegistry(const std::size_t staging_byte_budget)
    : impl_(std::make_shared<Impl>(staging_byte_budget)) {}

UiResourceRegistry::~UiResourceRegistry() = default;

UiResourceHandle UiResourceRegistry::RegisterWindow(
    const std::shared_ptr<PluginScope>& scope, UiWindowRequest request) {
    return impl_->RegisterWindow(scope, std::move(request));
}

bool UiResourceRegistry::OpenWindow(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) {
    return impl_->SetWindowOpen(scope, handle, true);
}

bool UiResourceRegistry::CloseWindow(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) {
    return impl_->SetWindowOpen(scope, handle, false);
}

bool UiResourceRegistry::ToggleWindow(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) {
    return impl_->ToggleWindow(scope, handle);
}

bool UiResourceRegistry::ShouldDrawWindow(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
    return impl_->ShouldDrawWindow(scope, handle);
}

bool UiResourceRegistry::SetWindowConstraints(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
    const UiWindowConstraints constraints) {
    return impl_->SetWindowConstraints(scope, handle, constraints);
}

bool UiResourceRegistry::SetWindowSize(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
    const float width, const float height) {
    return impl_->SetWindowSize(scope, handle, width, height);
}

std::optional<UiWindowSnapshot> UiResourceRegistry::WindowState(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
    return impl_->WindowState(scope, handle);
}

UiWindowGroupState UiResourceRegistry::WindowGroupState(
    const std::shared_ptr<PluginScope>& scope) const noexcept {
    return impl_->WindowGroupState(scope);
}

bool UiResourceRegistry::SetWindowGroupOpen(
    const std::shared_ptr<PluginScope>& scope, const bool open) noexcept {
    return impl_->SetWindowGroupOpen(scope, open);
}

std::vector<UiWindowPersistentState> UiResourceRegistry::ExportPersistentWindowState() const {
    return impl_->ExportPersistentWindowState();
}

bool UiResourceRegistry::ImportPersistentWindowState(
    std::vector<UiWindowPersistentState> state) {
    return impl_->ImportPersistentWindowState(std::move(state));
}

UiResourceStagingReservation UiResourceRegistry::ReserveStaging(
    const std::size_t byte_count) noexcept {
    return impl_->ReserveStaging(byte_count);
}

bool UiResourceRegistry::ReleaseStaging(
    const UiResourceStagingReservation reservation) noexcept {
    return impl_->ReleaseStaging(reservation);
}

bool UiResourceRegistry::ReserveResourceStaging(
    const std::shared_ptr<PluginScope>& scope,
    const UiResourceHandle handle,
    const std::size_t byte_count) noexcept {
    return impl_->ReserveResourceStaging(scope, handle, byte_count);
}

std::size_t UiResourceRegistry::StagingBytesInUse() const noexcept {
    return impl_->StagingBytesInUse();
}

std::size_t UiResourceRegistry::StagingByteBudget() const noexcept {
    return impl_->StagingByteBudget();
}

UiResourceHandle UiResourceRegistry::RequestFont(
    const std::shared_ptr<PluginScope>& scope, UiFontRequest request,
    const UiResourceStagingReservation reservation) {
    return impl_->RequestFont(scope, std::move(request), reservation);
}

UiResourceHandle UiResourceRegistry::RequestTexture(
    const std::shared_ptr<PluginScope>& scope, UiTextureRequest request,
    const UiResourceStagingReservation reservation) {
    return impl_->RequestTexture(scope, std::move(request), reservation);
}

std::optional<UiFontSnapshot> UiResourceRegistry::FontState(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
    return impl_->FontState(scope, handle);
}

std::optional<UiTextureSnapshot> UiResourceRegistry::TextureState(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
    return impl_->TextureState(scope, handle);
}

std::optional<UiFontRequest> UiResourceRegistry::FontRequest(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
    return impl_->FontRequest(scope, handle);
}

std::optional<UiTextureRequest> UiResourceRegistry::TextureRequest(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
    return impl_->TextureRequest(scope, handle);
}

bool UiResourceRegistry::SetFontData(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
    std::vector<std::uint8_t> encoded_bytes) noexcept {
    return impl_->SetFontData(scope, handle, std::move(encoded_bytes));
}

bool UiResourceRegistry::SetTextureData(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
    std::vector<std::uint8_t> encoded_bytes, const UiTextureFormat format,
    const std::uint32_t width, const std::uint32_t height) noexcept {
    return impl_->SetTextureData(
        scope, handle, std::move(encoded_bytes), format, width, height);
}

std::optional<UiResourceSnapshot> UiResourceRegistry::ResourceState(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) const {
    return impl_->ResourceState(scope, handle);
}

std::vector<UiResourceSnapshot> UiResourceRegistry::Resources(
    const std::shared_ptr<PluginScope>& scope) const {
    return impl_->Resources(scope);
}

std::size_t UiResourceRegistry::ResourceLeaseCount() const noexcept {
    return impl_->ResourceLeaseCount();
}

bool UiResourceRegistry::IsResourceLive(const std::uint64_t resource_id) const noexcept {
    return impl_->IsResourceLive(resource_id);
}

bool UiResourceRegistry::MarkFontFailed(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) noexcept {
    return impl_->SetResourceState(scope, handle, UiResourceKind::Font, UiResourceState::Failed);
}

bool UiResourceRegistry::MarkTextureFailed(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) noexcept {
    return impl_->SetResourceState(scope, handle, UiResourceKind::Texture, UiResourceState::Failed);
}

bool UiResourceRegistry::MarkFontReady(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
    const std::uint64_t device_generation) noexcept {
    return impl_->MarkReady(scope, handle, UiResourceKind::Font, device_generation);
}

bool UiResourceRegistry::MarkTextureReady(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle,
    const std::uint64_t device_generation, const std::uint32_t width,
    const std::uint32_t height) noexcept {
    return impl_->MarkReady(
        scope, handle, UiResourceKind::Texture, device_generation, width, height);
}

bool UiResourceRegistry::RetryFont(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) noexcept {
    return impl_->SetResourceState(scope, handle, UiResourceKind::Font, UiResourceState::Queued);
}

bool UiResourceRegistry::RetryTexture(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) noexcept {
    return impl_->SetResourceState(scope, handle, UiResourceKind::Texture, UiResourceState::Queued);
}

bool UiResourceRegistry::SetHostFontScale(const float scale) noexcept {
    if (!IsFinitePositive(scale)) return false;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->host_font_scale == scale) return false;
    for (const auto& [id, resource] : impl_->resources) {
        static_cast<void>(id);
        if (resource.kind == UiResourceKind::Font && resource.state != UiResourceState::Revoked &&
            !IsFinitePositive(resource.font.scale * scale)) {
            return false;
        }
    }
    impl_->host_font_scale = scale;
    for (auto& [id, resource] : impl_->resources) {
        static_cast<void>(id);
        if (resource.kind == UiResourceKind::Font && resource.state == UiResourceState::Ready) {
            resource.state = UiResourceState::Queued;
            resource.device_generation = 0;
        }
    }
    return true;
}

std::uint64_t UiResourceRegistry::DeviceGeneration() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->device_generation;
}

std::uint64_t UiResourceRegistry::InvalidateDeviceResources() noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->device_generation != (std::numeric_limits<std::uint64_t>::max)()) {
        ++impl_->device_generation;
    }
    for (auto& [id, resource] : impl_->resources) {
        if (resource.state == UiResourceState::Ready) resource.state = UiResourceState::StaleDevice;
    }
    return impl_->device_generation;
}

bool UiResourceRegistry::RebuildDeviceResources(const std::uint64_t generation) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (generation == 0 || generation != impl_->device_generation) return false;
    return true;
}

bool UiResourceRegistry::Release(
    const std::shared_ptr<PluginScope>& scope, const UiResourceHandle handle) noexcept {
    return impl_->Release(scope, handle);
}

}  // namespace anomaly
