#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace better_tv_video {

// A short share link may follow at most five redirects; the sixth is rejected.
constexpr int kBilibiliMaxRedirects = 5;

// One playable page of a normal (non-interactive) bilibili submission.
struct BilibiliPage final {
    // Exactly one identity is set: the canonical BV id or the canonical av number.
    std::string bvid;
    std::uint64_t aid{};
    int page{1};
    // The canonical bilibili web video page URL used for logging and diagnostics.
    std::string canonical_url;
    // Normalized identity and page used for in-process cache reuse:
    // "bv:<bvid>:p<N>" or "av:<aid>:p<N>" (for example "bv:BV1xx411c7mD:p2").
    std::string cache_key;
};

enum class BilibiliLinkError {
    None,
    // Nothing but whitespace was pasted.
    Empty,
    // Text contains no recognizable BV/av identifier or URL.
    UnsupportedInput,
    // The link, or a redirect target, is not b23.tv or a bilibili video host.
    UnsupportedHost,
    // The link, or a redirect target, does not use HTTPS.
    NotHttps,
    // The link, or a redirect target, is not a well-formed absolute URL.
    Malformed,
    // More than kBilibiliMaxRedirects redirects would have to be followed.
    RedirectLimit,
    // Redirect loop, missing or unusable Location, or a response that is not a
    // redirect at all.
    RedirectInvalid,
    // The short link could not be requested or answered with a failure status.
    Network,
    // The final target is not a /video/ page of a normal submission.
    NotVideoPage
};

struct BilibiliLinkResult final {
    std::optional<BilibiliPage> page;
    BilibiliLinkError error{BilibiliLinkError::None};
    // English diagnostic for the log. Never rendered as user-facing copy.
    std::string detail;
};

// Result of requesting one URL without following its redirect.
struct HttpRedirectResult final {
    bool ok{};
    int status{};
    std::string location;
};

// Fetches one absolute URL without following redirects. Production uses WinHTTP;
// fixtures inject fixed chains so redirect validation is testable offline.
using RedirectFetcher = std::function<HttpRedirectResult(std::string_view url)>;

// Accepts a pasted BV/av identifier, a bilibili video page URL, a b23.tv share
// link, or share text containing one of those. `fetch` is used only when the
// input is a short link; pass an empty function for direct identifiers.
//
// Every redirect hop must be HTTPS and use an allowlisted host, the chain ends
// within kBilibiliMaxRedirects hops, and the final URL must be a normal video
// page. `?p=N` is preserved; a missing or invalid page selects P1.
BilibiliLinkResult ParseBilibiliLink(std::string_view input, const RedirectFetcher& fetch);

// Production entry point: uses WinHTTP for short links.
BilibiliLinkResult ParseBilibiliLink(std::string_view input);

// True when the pasted input is a b23.tv share link that needs network resolution.
bool NeedsRedirectResolution(std::string_view input);

// One production redirect request (HTTPS only, no automatic redirect following).
HttpRedirectResult FetchRedirect(std::string_view url);

}  // namespace better_tv_video
