#include "account_identity.hpp"

#include <algorithm>

namespace nte::account {

AccountIdentity Unknown() noexcept {
    return {};
}

std::optional<AccountIdentity> FromUidText(const std::string_view text) {
    if (text.empty() || text.size() > 20 ||
        !std::ranges::all_of(text, [](const char value) {
            return value >= '0' && value <= '9';
        })) {
        return std::nullopt;
    }
    const auto first = text.find_first_not_of('0');
    if (first == std::string_view::npos) return std::nullopt;
    return AccountIdentity{std::string{text.substr(first)}, Source::UiRoleId};
}

bool IsValid(const AccountIdentity& account) noexcept {
    if (!account.known()) return account.source == Source::Unavailable;
    const auto parsed = FromUidText(account.uid);
    return parsed && *parsed == account;
}

std::string DirectoryName(const AccountIdentity& account) {
    return account.known() ? account.uid : "_unknown";
}

std::optional<AccountIdentity> FromDirectoryName(const std::string_view name) {
    if (name == "_unknown") return Unknown();
    const auto account = FromUidText(name);
    if (!account || account->uid != name) return std::nullopt;
    return account;
}

std::string_view SourceName(const Source source) noexcept {
    return source == Source::UiRoleId ? "ui-role-id" : "unavailable";
}

bool ShouldTransition(const AccountIdentity& current,
    const std::optional<AccountIdentity>& detected) noexcept {
    return detected && IsValid(*detected) && *detected != current;
}

}  // namespace nte::account
