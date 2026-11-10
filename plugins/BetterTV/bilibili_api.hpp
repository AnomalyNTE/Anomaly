#pragma once

#include "bilibili_source.hpp"
#include "http_client.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace better_tv_video {

// One page resolved to a single muxed MP4 stream (durl).
struct BilibiliPlayInfo final {
    std::string bvid;
    std::uint64_t aid{};
    int page{1};
    std::int64_t cid{};
    std::string media_url;
    std::uint64_t media_size{};
    std::int64_t duration_ms{};
    int quality{};
};

enum class BilibiliApiError {
    None,
    Network,
    HttpStatus,
    ApiRejected,
    InvalidDocument,
    PageOutOfRange,
    NoPlayableStream,
    SegmentedStream,
    UnsupportedQuality
};

struct BilibiliApiResult final {
    std::optional<BilibiliPlayInfo> info;
    BilibiliApiError error{BilibiliApiError::None};
    // English diagnostic for the log. Never rendered as user-facing copy.
    std::string detail;
};

// Injected HTTP boundary: production is HttpGetText, fixtures return canned
// documents keyed by URL.
using ApiFetcher = std::function<HttpTextResult(std::string_view url)>;

// view (unsigned, WBI fallback) -> page cid -> WBI-signed playurl(fnval=1)
// (unsigned fallback). Only a single muxed durl is accepted; DASH-only pages
// report NoPlayableStream and multi-segment pages report SegmentedStream.
BilibiliApiResult ResolveMuxedPlayInfo(const BilibiliPage& page, const ApiFetcher& fetch);

// WBI helpers exposed so the fixture can assert a fixed reference vector.
bool ComputeMixinKey(std::string_view img_key, std::string_view sub_key,
                     std::string* mixin);
std::string SignWbiQuery(std::span<const std::pair<std::string, std::string>> params,
                         std::string_view mixin_key, std::int64_t wts);

}  // namespace better_tv_video
