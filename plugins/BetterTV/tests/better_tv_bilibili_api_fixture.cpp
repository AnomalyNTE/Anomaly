// Offline fixture for the native bilibili API client.
//
// The WBI reference vector is checked exactly, then view/nav/playurl resolution
// runs against an injected fetcher so every branch (unsigned view, WBI view
// fallback, page selection, error mapping, DASH/multi-segment refusal) is
// exercised without touching the network. The media URL safety predicate and
// the offline part of the WinHTTP client are covered as well.

#include "plugins/BetterTV/bilibili_api.hpp"
#include "plugins/BetterTV/http_client.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using better_tv_video::ApiFetcher;
using better_tv_video::BilibiliApiError;
using better_tv_video::BilibiliApiResult;
using better_tv_video::BilibiliPage;
using better_tv_video::BilibiliPlayInfo;
using better_tv_video::HttpTextResult;

constexpr std::string_view kWbiImgKey = "7cd084941338484aae1ad9425b84077c";
constexpr std::string_view kWbiSubKey = "4932caff0ff746eab6f01bf08b70ac45";
constexpr std::string_view kMixinKey = "ea1db124af3c7062474693fa704f4ff8";
constexpr std::string_view kBvId = "BV1GJ411x7h7";
constexpr std::string_view kMediaUrl =
    "https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/1/2/3/123.m4s.mp4";

constexpr std::string_view kViewPrefix = "https://api.bilibili.com/x/web-interface/view?";
constexpr std::string_view kWbiViewPrefix =
    "https://api.bilibili.com/x/web-interface/wbi/view?";
constexpr std::string_view kNavUrl = "https://api.bilibili.com/x/web-interface/nav";
constexpr std::string_view kWbiPlayUrlPrefix = "https://api.bilibili.com/x/player/wbi/playurl?";
constexpr std::string_view kPlayUrlPrefix = "https://api.bilibili.com/x/player/playurl?";

int g_checks = 0;
int g_failures = 0;

void Check(const bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++g_failures;
    }
}

bool StartsWith(const std::string_view text, const std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

HttpTextResult Ok(std::string body) {
    HttpTextResult result;
    result.ok = true;
    result.status = 200;
    result.body = std::move(body);
    return result;
}

HttpTextResult HttpError(const int status) {
    HttpTextResult result;
    result.status = status;
    result.detail = "http " + std::to_string(status);
    return result;
}

HttpTextResult TransportFailure(std::string detail) {
    HttpTextResult result;
    result.detail = std::move(detail);
    return result;
}

std::string ViewJson(const std::string_view bvid, const std::uint64_t aid,
                     const std::vector<std::int64_t>& cids) {
    std::string json = R"({"code":0,"message":"ok","data":{"bvid":")";
    json += bvid;
    json += R"(","aid":)";
    json += std::to_string(aid);
    json += R"(,"pages":[)";
    for (std::size_t index = 0; index < cids.size(); ++index) {
        if (index != 0) json.push_back(',');
        json += R"({"cid":)";
        json += std::to_string(cids[index]);
        json += R"(,"page":)";
        json += std::to_string(index + 1);
        json += '}';
    }
    json += "]}}";
    return json;
}

std::string ApiErrorJson(const std::int64_t code, const std::string_view message) {
    std::string json = R"({"code":)";
    json += std::to_string(code);
    json += R"(,"message":")";
    json += message;
    json += R"(","data":null})";
    return json;
}

// The anonymous nav response reports code -101 but still carries the key URLs;
// the message uses escaped code points so the JSON reader's unicode path runs.
std::string NavJson(const std::string_view img_key, const std::string_view sub_key) {
    std::string json =
        R"({"code":-101,"message":"\u672a\u767b\u5f55","data":{"wbi_img":{"img_url":)"
        R"("https://i0.hdslb.com/bfs/wbi/)";
    json += img_key;
    json += R"(.png","sub_url":"https://i0.hdslb.com/bfs/wbi/)";
    json += sub_key;
    json += R"(.png"}}})";
    return json;
}

std::string PlayUrlJson(const int quality, const std::int64_t timelength,
                        const std::uint64_t size, const std::string_view media_url,
                        const std::size_t segment_count = 1) {
    std::string json = R"({"code":0,"message":"ok","data":{"quality":)";
    json += std::to_string(quality);
    json += R"(,"timelength":)";
    json += std::to_string(timelength);
    json += R"(,"durl":[)";
    for (std::size_t index = 0; index < segment_count; ++index) {
        if (index != 0) json.push_back(',');
        json += R"({"order":)";
        json += std::to_string(index + 1);
        json += R"(,"size":)";
        json += std::to_string(size);
        json += R"(,"url":")";
        json += media_url;
        json += R"("})";
    }
    json += "]}}";
    return json;
}

// Canned API used by the injected fetcher; unset endpoints fail loudly so a
// wrong request order cannot silently pass a test.
struct FakeApi final {
    HttpTextResult view;
    HttpTextResult wbi_view;
    HttpTextResult nav;
    HttpTextResult wbi_playurl;
    HttpTextResult playurl;
    std::vector<std::string> requests;

    FakeApi() {
        view = TransportFailure("unexpected unsigned view request");
        wbi_view = TransportFailure("unexpected wbi view request");
        nav = TransportFailure("unexpected nav request");
        wbi_playurl = TransportFailure("unexpected wbi playurl request");
        playurl = TransportFailure("unexpected unsigned playurl request");
    }

    HttpTextResult operator()(const std::string_view url) {
        requests.emplace_back(url);
        if (StartsWith(url, kWbiViewPrefix)) return wbi_view;
        if (StartsWith(url, kViewPrefix)) return view;
        if (StartsWith(url, kNavUrl)) return nav;
        if (StartsWith(url, kWbiPlayUrlPrefix)) return wbi_playurl;
        if (StartsWith(url, kPlayUrlPrefix)) return playurl;
        return TransportFailure("unknown api endpoint");
    }
};

std::size_t CountRequests(const FakeApi& api, const std::string_view prefix) {
    std::size_t count = 0;
    for (const std::string& request : api.requests) {
        if (StartsWith(request, prefix)) ++count;
    }
    return count;
}

const std::string* FindRequest(const FakeApi& api, const std::string_view prefix) {
    for (const std::string& request : api.requests) {
        if (StartsWith(request, prefix)) return &request;
    }
    return nullptr;
}

BilibiliPage BvPage(std::string bvid, const int page_number) {
    BilibiliPage page;
    page.bvid = std::move(bvid);
    page.page = page_number;
    page.canonical_url = "https://www.bilibili.com/video/" + page.bvid;
    page.cache_key = "bv:" + page.bvid + ":p" + std::to_string(page_number);
    return page;
}

BilibiliPage AvPage(const std::uint64_t aid, const int page_number) {
    BilibiliPage page;
    page.aid = aid;
    page.page = page_number;
    page.canonical_url = "https://www.bilibili.com/video/av" + std::to_string(aid);
    page.cache_key = "av:" + std::to_string(aid) + ":p" + std::to_string(page_number);
    return page;
}

BilibiliApiResult Resolve(const BilibiliPage& page, FakeApi& api) {
    return better_tv_video::ResolveMuxedPlayInfo(
        page, [&api](const std::string_view url) { return api(url); });
}

FakeApi MakePlayUrlApi(const std::string& playurl_body) {
    FakeApi api;
    api.view = Ok(ViewJson(kBvId, 170001, {137649199}));
    api.nav = Ok(NavJson(kWbiImgKey, kWbiSubKey));
    api.wbi_playurl = Ok(playurl_body);
    api.playurl = Ok(playurl_body);
    return api;
}

void TestWbiReferenceVector() {
    std::string mixin;
    Check(better_tv_video::ComputeMixinKey(kWbiImgKey, kWbiSubKey, &mixin),
          "mixin key is computed from the wbi keys");
    Check(mixin == kMixinKey, "mixin key matches the reference vector");

    std::string unusable;
    Check(!better_tv_video::ComputeMixinKey("short", "keys", &unusable),
          "mixin key rejects a source that is not 64 characters");

    const std::array<std::pair<std::string, std::string>, 9> params{{
        {"bvid", std::string(kBvId)},
        {"cid", "137649199"},
        {"fnval", "1"},
        {"fnver", "0"},
        {"fourk", "1"},
        {"high_quality", "1"},
        {"otype", "json"},
        {"platform", "pc"},
        {"qn", "64"},
    }};
    const std::string signed_query = better_tv_video::SignWbiQuery(params, kMixinKey, 1700000000);
    Check(signed_query ==
              "bvid=BV1GJ411x7h7&cid=137649199&fnval=1&fnver=0&fourk=1&high_quality=1&"
              "otype=json&platform=pc&qn=64&wts=1700000000&"
              "w_rid=2c368bade759a83d42de59ba592ddc19",
          "signed query matches the reference vector");

    const std::array<std::pair<std::string, std::string>, 2> unsafe_params{{
        {"bvid", "BV1!GJ'41(1x)7*h7"},
        {"cid", "137649199"},
    }};
    const std::string filtered_query =
        better_tv_video::SignWbiQuery(unsafe_params, kMixinKey, 1700000000);
    Check(StartsWith(filtered_query, "bvid=BV1GJ411x7h7&cid=137649199&wts=1700000000"),
          "wbi signing removes !'()* characters from parameter values");
    const std::size_t digest_marker = filtered_query.find("&w_rid=");
    Check(digest_marker != std::string::npos &&
              filtered_query.size() - digest_marker == std::string("&w_rid=").size() + 32,
          "wbi signing appends a 32 character digest");
}

void TestBvPageResolvesMuxedStream() {
    FakeApi api;
    api.view = Ok(ViewJson(kBvId, 170001, {137649199, 137649200}));
    api.nav = Ok(NavJson(kWbiImgKey, kWbiSubKey));
    api.wbi_playurl = Ok(PlayUrlJson(64, 3600000, 12345678, kMediaUrl));
    api.playurl = Ok(PlayUrlJson(16, 1000, 1, "https://example.com/unsigned.mp4"));

    const BilibiliApiResult result = Resolve(BvPage(std::string(kBvId), 1), api);
    Check(result.error == BilibiliApiError::None,
          "a BV page resolves without an error: " + result.detail);
    if (!result.info.has_value()) {
        Check(false, "a BV page returns play info");
        return;
    }
    const BilibiliPlayInfo& info = *result.info;
    Check(info.bvid == kBvId, "play info keeps the BV id");
    Check(info.aid == 170001, "play info keeps the aid from the view document");
    Check(info.page == 1, "play info keeps the requested page");
    Check(info.cid == 137649199, "P1 uses the first page cid");
    Check(info.media_url == kMediaUrl, "play info keeps the muxed media url");
    Check(info.media_size == 12345678, "play info keeps the durl byte size");
    Check(info.duration_ms == 3600000, "play info keeps the playurl timelength");
    Check(info.quality == 64, "play info keeps the 720p quality");

    Check(CountRequests(api, kWbiPlayUrlPrefix) == 1,
          "the signed playurl endpoint is requested once");
    Check(CountRequests(api, kPlayUrlPrefix) == 0,
          "the unsigned playurl fallback is unused when signing succeeds");
    Check(CountRequests(api, kNavUrl) == 1, "the wbi keys are fetched once");

    const std::string* view_request = FindRequest(api, kViewPrefix);
    Check(view_request != nullptr &&
              view_request->find("bvid=BV1GJ411x7h7") != std::string::npos,
          "the unsigned view request carries the BV id");
    const std::string* signed_request = FindRequest(api, kWbiPlayUrlPrefix);
    Check(signed_request != nullptr && signed_request->find("w_rid=") != std::string::npos,
          "the signed playurl request carries w_rid");
    Check(signed_request != nullptr &&
              signed_request->find("cid=137649199") != std::string::npos,
          "the signed playurl request carries the resolved cid");
    Check(signed_request != nullptr && signed_request->find("fnval=1") != std::string::npos &&
              signed_request->find("qn=64") != std::string::npos,
          "the signed playurl request asks for a muxed 720p stream");
}

void TestAvPageUsesRequestedPage() {
    FakeApi api;
    api.view = Ok(ViewJson("", 170001, {111, 222, 333}));
    api.nav = Ok(NavJson(kWbiImgKey, kWbiSubKey));
    api.wbi_playurl = Ok(PlayUrlJson(32, 240000, 5000000, kMediaUrl));

    const BilibiliApiResult result = Resolve(AvPage(170001, 2), api);
    Check(result.error == BilibiliApiError::None,
          "an av page resolves without an error: " + result.detail);
    if (!result.info.has_value()) {
        Check(false, "an av page returns play info");
        return;
    }
    Check(result.info->bvid.empty(), "an av page keeps an empty BV id");
    Check(result.info->aid == 170001, "an av page keeps the av number");
    Check(result.info->page == 2, "an av page keeps the requested page number");
    Check(result.info->cid == 222, "P2 uses the second page cid");

    const std::string* view_request = FindRequest(api, kViewPrefix);
    Check(view_request != nullptr && view_request->find("aid=170001") != std::string::npos &&
              view_request->find("bvid=") == std::string::npos,
          "the view request identifies the video by av number");
    const std::string* signed_request = FindRequest(api, kWbiPlayUrlPrefix);
    Check(signed_request != nullptr && signed_request->find("avid=170001") != std::string::npos,
          "the signed playurl request identifies the video by av number");
}

void TestPageOutOfRange() {
    FakeApi api;
    api.view = Ok(ViewJson(kBvId, 170001, {111, 222}));

    const BilibiliApiResult result = Resolve(BvPage(std::string(kBvId), 3), api);
    Check(result.error == BilibiliApiError::PageOutOfRange,
          "a page index outside the view page list is rejected");
    Check(!result.info.has_value(), "an out-of-range page returns no play info");
    Check(CountRequests(api, kNavUrl) == 0 && CountRequests(api, kWbiPlayUrlPrefix) == 0 &&
              CountRequests(api, kPlayUrlPrefix) == 0,
          "an out-of-range page stops before nav and playurl");
}

void TestViewRejectedFallsBackToWbiView() {
    FakeApi api;
    api.view = Ok(ApiErrorJson(-404, "not found"));
    api.nav = Ok(NavJson(kWbiImgKey, kWbiSubKey));
    api.wbi_view = Ok(ViewJson(kBvId, 170001, {137649199}));
    api.wbi_playurl = Ok(PlayUrlJson(64, 1000, 100, kMediaUrl));

    const BilibiliApiResult result = Resolve(BvPage(std::string(kBvId), 1), api);
    Check(result.error == BilibiliApiError::None,
          "a rejected unsigned view falls back to the signed view: " + result.detail);
    if (result.info.has_value()) {
        Check(result.info->cid == 137649199, "the signed view resolves the page cid");
    } else {
        Check(false, "the signed view fallback returns play info");
    }
    Check(CountRequests(api, kWbiViewPrefix) == 1, "the signed view endpoint is requested once");
    const std::string* signed_view = FindRequest(api, kWbiViewPrefix);
    Check(signed_view != nullptr && signed_view->find("w_rid=") != std::string::npos &&
              signed_view->find("bvid=BV1GJ411x7h7") != std::string::npos,
          "the signed view request is signed and carries the BV id");
}

void TestViewRejectedWithoutWbiFallback() {
    FakeApi api;
    api.view = Ok(ApiErrorJson(-404, "not found"));
    api.nav = TransportFailure("nav unavailable");

    const BilibiliApiResult result = Resolve(BvPage(std::string(kBvId), 1), api);
    Check(result.error == BilibiliApiError::ApiRejected,
          "a rejected view stays rejected when the wbi keys are unavailable");
    Check(!result.detail.empty(), "the rejection carries a diagnostic");
    Check(CountRequests(api, kWbiViewPrefix) == 0,
          "no signed view is attempted without wbi keys");
}

void TestViewRejectedEverywhere() {
    FakeApi api;
    api.view = Ok(ApiErrorJson(-404, "not found"));
    api.nav = Ok(NavJson(kWbiImgKey, kWbiSubKey));
    api.wbi_view = Ok(ApiErrorJson(-404, "not found"));

    const BilibiliApiResult result = Resolve(BvPage(std::string(kBvId), 1), api);
    Check(result.error == BilibiliApiError::ApiRejected,
          "a video rejected by both view endpoints reports ApiRejected");
    Check(result.detail.find("-404") != std::string::npos,
          "the rejection diagnostic keeps the api code");
}

void TestPlayUrlWbiFallback() {
    FakeApi api;
    api.view = Ok(ViewJson(kBvId, 170001, {137649199}));
    api.nav = Ok(NavJson(kWbiImgKey, kWbiSubKey));
    api.wbi_playurl = Ok(ApiErrorJson(-400, "bad request"));
    api.playurl = Ok(PlayUrlJson(16, 120000, 987654, kMediaUrl));

    const BilibiliApiResult result = Resolve(BvPage(std::string(kBvId), 1), api);
    Check(result.error == BilibiliApiError::None,
          "a rejected signed playurl falls back to the unsigned endpoint: " + result.detail);
    if (result.info.has_value()) {
        Check(result.info->quality == 16, "the fallback keeps the lower 360p quality");
        Check(result.info->media_size == 987654, "the fallback keeps the unsigned stream size");
    } else {
        Check(false, "the unsigned playurl fallback returns play info");
    }
    Check(CountRequests(api, kWbiPlayUrlPrefix) == 1,
          "the signed playurl endpoint is tried first");
    Check(CountRequests(api, kPlayUrlPrefix) == 1,
          "the unsigned playurl endpoint is used as the fallback");
}

void TestEmptyDurlReportsNoPlayableStream() {
    FakeApi api = MakePlayUrlApi(PlayUrlJson(64, 1000, 100, kMediaUrl, 0));
    const BilibiliApiResult result = Resolve(BvPage(std::string(kBvId), 1), api);
    Check(result.error == BilibiliApiError::NoPlayableStream,
          "a DASH-only page reports NoPlayableStream");
}

void TestSegmentedDurlIsRejected() {
    FakeApi api = MakePlayUrlApi(PlayUrlJson(64, 1000, 100, kMediaUrl, 2));
    const BilibiliApiResult result = Resolve(BvPage(std::string(kBvId), 1), api);
    Check(result.error == BilibiliApiError::SegmentedStream,
          "a two segment durl reports SegmentedStream");
}

void TestQualityAbove720pIsRejected() {
    FakeApi api = MakePlayUrlApi(PlayUrlJson(80, 1000, 100, kMediaUrl));
    const BilibiliApiResult result = Resolve(BvPage(std::string(kBvId), 1), api);
    Check(result.error == BilibiliApiError::UnsupportedQuality,
          "a quality above 64 reports UnsupportedQuality");
}

void TestMalformedDurlFields() {
    {
        FakeApi api = MakePlayUrlApi(PlayUrlJson(64, 1000, 0, kMediaUrl));
        Check(Resolve(BvPage(std::string(kBvId), 1), api).error ==
                  BilibiliApiError::InvalidDocument,
              "a durl without a byte size reports InvalidDocument");
    }
    {
        FakeApi api =
            MakePlayUrlApi(PlayUrlJson(64, 1000, 100, "http://plain.example.com/x.mp4"));
        Check(Resolve(BvPage(std::string(kBvId), 1), api).error ==
                  BilibiliApiError::InvalidDocument,
              "a non-HTTPS media url reports InvalidDocument");
    }
    {
        FakeApi api = MakePlayUrlApi(PlayUrlJson(64, 0, 100, kMediaUrl));
        Check(Resolve(BvPage(std::string(kBvId), 1), api).error ==
                  BilibiliApiError::InvalidDocument,
              "a playurl without a timelength reports InvalidDocument");
    }
}

void TestTransportAndDocumentFailures() {
    {
        FakeApi api;
        api.view = TransportFailure("connection reset");
        Check(Resolve(BvPage(std::string(kBvId), 1), api).error == BilibiliApiError::Network,
              "a transport failure reports Network");
    }
    {
        FakeApi api;
        api.view = HttpError(503);
        Check(Resolve(BvPage(std::string(kBvId), 1), api).error == BilibiliApiError::HttpStatus,
              "a non-2xx response reports HttpStatus");
    }
    {
        FakeApi api;
        api.view = Ok("{this is not json");
        Check(Resolve(BvPage(std::string(kBvId), 1), api).error ==
                  BilibiliApiError::InvalidDocument,
              "a malformed api document reports InvalidDocument");
    }
    {
        FakeApi api;
        api.view = Ok(R"({"code":"zero","data":null})");
        Check(Resolve(BvPage(std::string(kBvId), 1), api).error ==
                  BilibiliApiError::InvalidDocument,
              "a view document without a numeric code reports InvalidDocument");
    }
}

void TestMediaUrlSafety() {
    const std::array<std::pair<const char*, bool>, 18> cases{{
        {"https://upos-sz-mirrorcos.bilivideo.com/x.mp4", true},
        {"https://api.bilibili.com/x/web-interface/nav", true},
        {"https://8.8.8.8/x.mp4", true},
        {"https://172.32.0.1/x.mp4", true},
        // Bilibili media CDNs expose explicit non-default TLS ports.
        {"https://upos-sz-mirrorcos.bilivideo.com:8443/x.mp4", true},
        {"https://b-baaa.edge.example.cn:4483/upgcxcode/x.mp4", true},
        {"http://upos-sz-mirrorcos.bilivideo.com/x.mp4", false},
        {"https://user:pass@upos-sz-mirrorcos.bilivideo.com/x.mp4", false},
        {"https://upos-sz-mirrorcos.bilivideo.com:0/x.mp4", false},
        {"https://upos-sz-mirrorcos.bilivideo.com:99999/x.mp4", false},
        {"https://upos-sz-mirrorcos.bilivideo.com:abc/x.mp4", false},
        {"https://127.0.0.1/x.mp4", false},
        {"https://10.1.2.3/x.mp4", false},
        {"https://172.16.5.5/x.mp4", false},
        {"https://192.168.1.10/x.mp4", false},
        {"https://169.254.1.1/x.mp4", false},
        {"https://localhost/x.mp4", false},
        {"https://[::1]/x.mp4", false},
    }};
    int index = 0;
    for (const auto& [url, expected_safe] : cases) {
        std::string detail;
        const bool safe = better_tv_video::IsSafeMediaUrl(url, &detail);
        Check(safe == expected_safe,
              "media URL safety case " + std::to_string(index) + ": " + url);
        if (!safe) {
            Check(!detail.empty(),
                  "media URL rejection " + std::to_string(index) + " carries a detail");
        }
        ++index;
    }
}

void TestHttpGetTextStopsBeforeNetwork() {
    const HttpTextResult plain =
        better_tv_video::HttpGetText("http://api.bilibili.com/x/web-interface/nav");
    Check(!plain.ok, "a plain HTTP api request is rejected without a network call");
    Check(!plain.detail.empty(), "the plain HTTP rejection carries a diagnostic");

    bool cancel_polled = false;
    const HttpTextResult cancelled = better_tv_video::HttpGetText(
        "https://api.bilibili.com/x/web-interface/nav", [&cancel_polled] {
            cancel_polled = true;
            return true;
        });
    Check(!cancelled.ok, "a cancelled request does not report success");
    Check(cancel_polled, "the cancel flag is polled before connecting");
    Check(!cancelled.detail.empty(), "the cancelled request carries a diagnostic");

    const HttpTextResult empty = better_tv_video::HttpGetText("");
    Check(!empty.ok, "an empty api URL is rejected");
}

}  // namespace

int main() {
    TestWbiReferenceVector();
    TestBvPageResolvesMuxedStream();
    TestAvPageUsesRequestedPage();
    TestPageOutOfRange();
    TestViewRejectedFallsBackToWbiView();
    TestViewRejectedWithoutWbiFallback();
    TestViewRejectedEverywhere();
    TestPlayUrlWbiFallback();
    TestEmptyDurlReportsNoPlayableStream();
    TestSegmentedDurlIsRejected();
    TestQualityAbove720pIsRejected();
    TestMalformedDurlFields();
    TestTransportAndDocumentFailures();
    TestMediaUrlSafety();
    TestHttpGetTextStopsBeforeNetwork();

    if (g_failures != 0) {
        std::cerr << g_failures << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "PASS: bilibili api fixture (" << g_checks << " checks)\n";
    return 0;
}
