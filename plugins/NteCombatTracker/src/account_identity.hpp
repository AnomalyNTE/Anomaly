#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace nte::account {

enum class Source { Unavailable, UiRoleId };

struct AccountIdentity {
    std::string uid;
    Source source{Source::Unavailable};

    [[nodiscard]] bool known() const noexcept { return !uid.empty(); }
    friend bool operator==(const AccountIdentity&, const AccountIdentity&) = default;
};

[[nodiscard]] AccountIdentity Unknown() noexcept;
[[nodiscard]] std::optional<AccountIdentity> FromUidText(std::string_view text);
[[nodiscard]] bool IsValid(const AccountIdentity& account) noexcept;
[[nodiscard]] std::string DirectoryName(const AccountIdentity& account);
[[nodiscard]] std::optional<AccountIdentity> FromDirectoryName(std::string_view name);
[[nodiscard]] std::string_view SourceName(Source source) noexcept;
[[nodiscard]] bool ShouldTransition(
    const AccountIdentity& current,
    const std::optional<AccountIdentity>& detected) noexcept;

}  // namespace nte::account
