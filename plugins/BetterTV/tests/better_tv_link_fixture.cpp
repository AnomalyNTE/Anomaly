// Non-ASCII escapes below are original share-text/parser fixture data, not UI copy.
// Fixture for bilibili link parsing and short link redirect validation.
//
// The redirect fetcher is injected and every hop is a fixed rule, so the whole
// parser runs offline. Assertions use only the public result (page identity,
// canonical URL, cache key, error class and the fetched chain); they never
// re-implement the parser's own matching rules.

#include "plugins/BetterTV/bilibili_source.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using better_tv_video::BilibiliLinkError;
using better_tv_video::BilibiliLinkResult;
using better_tv_video::BilibiliPage;
using better_tv_video::HttpRedirectResult;
using better_tv_video::RedirectFetcher;

constexpr std::string_view kBv = "BV1xx411c7mD";
constexpr std::string_view kVideoOrigin = "https://www.bilibili.com/video/";
constexpr std::string_view kBvPage = "https://www.bilibili.com/video/BV1xx411c7mD";
constexpr std::string_view kAvPage = "https://www.bilibili.com/video/av170001";
constexpr std::string_view kBvCacheKey = "bv:BV1xx411c7mD:p1";
constexpr std::string_view kAvCacheKey = "av:170001:p1";

int g_checks = 0;
int g_failures = 0;

void Fail(const std::string& message) {
    ++g_failures;
    std::cout << "FAIL: " << message << '\n';
}

void Expect(const bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) Fail(message);
}

void ExpectText(const std::string_view actual, const std::string_view expected,
                const std::string& label) {
    ++g_checks;
    if (actual != expected) {
        Fail(label + ": expected \"" + std::string(expected) + "\", got \"" +
             std::string(actual) + "\"");
    }
}

void ExpectNumber(const long long actual, const long long expected, const std::string& label) {
    ++g_checks;
    if (actual != expected) {
        Fail(label + ": expected " + std::to_string(expected) + ", got " +
             std::to_string(actual));
    }
}

const char* ErrorName(const BilibiliLinkError error) {
    switch (error) {
        case BilibiliLinkError::None: return "None";
        case BilibiliLinkError::Empty: return "Empty";
        case BilibiliLinkError::UnsupportedInput: return "UnsupportedInput";
        case BilibiliLinkError::UnsupportedHost: return "UnsupportedHost";
        case BilibiliLinkError::NotHttps: return "NotHttps";
        case BilibiliLinkError::Malformed: return "Malformed";
        case BilibiliLinkError::RedirectLimit: return "RedirectLimit";
        case BilibiliLinkError::RedirectInvalid: return "RedirectInvalid";
        case BilibiliLinkError::Network: return "Network";
        case BilibiliLinkError::NotVideoPage: return "NotVideoPage";
    }
    return "Unknown";
}

void ExpectPage(const BilibiliLinkResult& result, const std::string& label,
                const std::string_view expected_bvid, const std::uint64_t expected_aid,
                const int expected_page, const std::string_view expected_canonical,
                const std::string_view expected_cache_key) {
    ++g_checks;
    if (!result.page.has_value()) {
        Fail(label + ": expected a resolved page, got " + ErrorName(result.error) + " (" +
             result.detail + ")");
        return;
    }
    const BilibiliPage& page = *result.page;
    ExpectText(page.bvid, expected_bvid, label + " bvid");
    ExpectNumber(static_cast<long long>(page.aid), static_cast<long long>(expected_aid),
                 label + " aid");
    ExpectNumber(page.page, expected_page, label + " page");
    ExpectText(page.canonical_url, expected_canonical, label + " canonical_url");
    ExpectText(page.cache_key, expected_cache_key, label + " cache_key");
}

void ExpectError(const BilibiliLinkResult& result, const BilibiliLinkError expected,
                 const std::string& label) {
    ++g_checks;
    if (result.page.has_value()) {
        Fail(label + ": expected error " + ErrorName(expected) + ", resolved a page instead");
        return;
    }
    if (result.error != expected) {
        Fail(label + ": expected " + ErrorName(expected) + ", got " + ErrorName(result.error) +
             " (" + result.detail + ")");
    }
}

void ExpectFailure(const BilibiliLinkResult& result, const std::string& label) {
    ++g_checks;
    if (result.page.has_value() || result.error == BilibiliLinkError::None) {
        Fail(label + ": expected a failure, resolved a page instead");
    }
}

// Fixed redirect rules keyed by the exact URL. A request outside the scripted
// chain is recorded and answered like a network failure.
class FakeRedirects final {
public:
    void Redirect(std::string url, const int status, std::string location) {
        rules_.insert_or_assign(std::move(url),
                                HttpRedirectResult{true, status, std::move(location)});
    }

    void FailNetwork(const std::string& url) {
        rules_.insert_or_assign(url, HttpRedirectResult{});
    }

    RedirectFetcher Fetcher() {
        return [this](const std::string_view url) {
            calls_.emplace_back(url);
            const auto found = rules_.find(std::string(url));
            if (found == rules_.end()) {
                missing_.emplace_back(url);
                return HttpRedirectResult{};
            }
            return found->second;
        };
    }

    const std::vector<std::string>& calls() const { return calls_; }
    const std::vector<std::string>& missing() const { return missing_; }

private:
    std::map<std::string, HttpRedirectResult> rules_;
    std::vector<std::string> calls_;
    std::vector<std::string> missing_;
};

BilibiliLinkResult ResolveWith(const std::string_view input, FakeRedirects* redirects) {
    if (redirects == nullptr) return better_tv_video::ParseBilibiliLink(input, RedirectFetcher{});
    return better_tv_video::ParseBilibiliLink(input, redirects->Fetcher());
}

void ExpectRequests(const FakeRedirects& redirects, const std::vector<std::string>& expected,
                    const std::string& label) {
    ++g_checks;
    if (!redirects.missing().empty()) {
        Fail(label + ": requested an unscripted URL \"" + redirects.missing().front() + "\"");
        return;
    }
    if (redirects.calls() != expected) {
        std::string chain;
        for (const std::string& call : redirects.calls()) {
            if (!chain.empty()) chain.append(" -> ");
            chain.append(call);
        }
        Fail(label + ": unexpected redirect chain (" + chain + ")");
        return;
    }
    for (const std::string& call : redirects.calls()) {
        Expect(call.rfind("https://b23.tv/", 0) == 0,
               label + ": redirect request is not an allowlisted HTTPS short link: " + call);
    }
}

void TestBareIdentifiers() {
    {
        FakeRedirects redirects;
        const BilibiliLinkResult result = ResolveWith(kBv, &redirects);
        ExpectPage(result, "bare BV", kBv, 0, 1, kBvPage, kBvCacheKey);
        ExpectRequests(redirects, {}, "bare BV");
    }
    {
        FakeRedirects redirects;
        const BilibiliLinkResult result = ResolveWith("av170001", &redirects);
        ExpectPage(result, "bare av", "", 170001, 1, kAvPage, kAvCacheKey);
        ExpectRequests(redirects, {}, "bare av");
    }
    {
        FakeRedirects redirects;
        const BilibiliLinkResult result = ResolveWith("BV1xx411c7mD?p=2", &redirects);
        ExpectPage(result, "bare BV with page", kBv, 0, 2, "https://www.bilibili.com/video/BV1xx411c7mD?p=2",
                   "bv:BV1xx411c7mD:p2");
        ExpectRequests(redirects, {}, "bare BV with page");
    }
    {
        const BilibiliLinkResult result = better_tv_video::ParseBilibiliLink(kBv);
        ExpectPage(result, "production overload bare BV", kBv, 0, 1, kBvPage, kBvCacheKey);
    }
}

void TestVideoUrls() {
    {
        FakeRedirects redirects;
        const BilibiliLinkResult result = ResolveWith(
            "https://www.bilibili.com/video/BV1xx411c7mD?p=2&vd_source=deadbeef", &redirects);
        ExpectPage(result, "video URL with page", kBv, 0, 2,
                   "https://www.bilibili.com/video/BV1xx411c7mD?p=2", "bv:BV1xx411c7mD:p2");
        ExpectRequests(redirects, {}, "video URL with page");
    }
    {
        FakeRedirects redirects;
        const BilibiliLinkResult result = ResolveWith(
            "https://m.bilibili.com/video/av170001/?spm_id_from=333.788", &redirects);
        ExpectPage(result, "mobile video URL", "", 170001, 1, kAvPage, kAvCacheKey);
        ExpectRequests(redirects, {}, "mobile video URL");
    }
    {
        FakeRedirects redirects;
        const BilibiliLinkResult result =
            ResolveWith("https://www.bilibili.com/video/av170001?p=3", &redirects);
        ExpectPage(result, "av URL with page", "", 170001, 3,
                   "https://www.bilibili.com/video/av170001?p=3", "av:170001:p3");
        ExpectRequests(redirects, {}, "av URL with page");
    }
    {
        FakeRedirects redirects;
        const BilibiliLinkResult result = ResolveWith(
            "\u30104K \u4fee\u590d\u3011\u5206\u4eab\u89c6\u9891 https://www.bilibili.com/video/BV1xx411c7mD?p=2 \u590d\u5236\u6253\u5f00\u54d4\u54e9\u54d4\u54e9",
            &redirects);
        ExpectPage(result, "share text video URL", kBv, 0, 2,
                   "https://www.bilibili.com/video/BV1xx411c7mD?p=2", "bv:BV1xx411c7mD:p2");
        ExpectRequests(redirects, {}, "share text video URL");
    }
}

void TestInvalidPageNumbers() {
    const char* const invalid_pages[] = {"0", "-1", "abc", "", "2.5", "99999999999999999999"};
    for (const char* const invalid : invalid_pages) {
        FakeRedirects redirects;
        const std::string input =
            "https://www.bilibili.com/video/BV1xx411c7mD?p=" + std::string(invalid);
        const BilibiliLinkResult result = ResolveWith(input, &redirects);
        ExpectPage(result, std::string("invalid page \"") + invalid + "\"", kBv, 0, 1, kBvPage,
                   kBvCacheKey);
        ExpectRequests(redirects, {}, "invalid page");
    }
}

void TestInputErrors() {
    ExpectError(ResolveWith("", nullptr), BilibiliLinkError::Empty, "empty input");
    ExpectError(ResolveWith(" \t\r\n ", nullptr), BilibiliLinkError::Empty,
                "whitespace input");

    ExpectFailure(ResolveWith("hello world", nullptr), "plain text input");
    ExpectFailure(ResolveWith("https://", nullptr), "scheme only input");
    ExpectFailure(ResolveWith("ftp://www.bilibili.com/video/BV1xx411c7mD", nullptr),
                  "unsupported scheme input");

    ExpectError(ResolveWith("http://www.bilibili.com/video/BV1xx411c7mD", nullptr),
                BilibiliLinkError::NotHttps, "plain HTTP video URL");
    {
        FakeRedirects redirects;
        const BilibiliLinkResult result = ResolveWith("http://b23.tv/raw", &redirects);
        ExpectError(result, BilibiliLinkError::NotHttps, "plain HTTP short link");
        ExpectRequests(redirects, {}, "plain HTTP short link");
    }
    ExpectError(ResolveWith("https://www.youtube.com/watch?v=dQw4w9WgXcQ", nullptr),
                BilibiliLinkError::UnsupportedHost, "foreign host video URL");
    ExpectError(ResolveWith("https://www.bilibili.com/read/cv12345", nullptr),
                BilibiliLinkError::NotVideoPage, "article URL");
    ExpectError(ResolveWith("https://www.bilibili.com/bangumi/play/ep123456", nullptr),
                BilibiliLinkError::NotVideoPage, "bangumi URL");
}

void TestShortLinks() {
    {
        FakeRedirects redirects;
        redirects.Redirect("https://b23.tv/one", 302,
                           "https://www.bilibili.com/video/BV1xx411c7mD?p=2");
        const BilibiliLinkResult result = ResolveWith("https://b23.tv/one", &redirects);
        ExpectPage(result, "short link one hop", kBv, 0, 2,
                   "https://www.bilibili.com/video/BV1xx411c7mD?p=2", "bv:BV1xx411c7mD:p2");
        ExpectRequests(redirects, {"https://b23.tv/one"}, "short link one hop");
    }
    {
        FakeRedirects redirects;
        redirects.Redirect("https://b23.tv/share", 301, "https://www.bilibili.com/video/av170001");
        const BilibiliLinkResult result = ResolveWith("\u770b\u8fd9\u4e2a\u89c6\u9891 https://b23.tv/share \u8d85\u597d\u770b", &redirects);
        ExpectPage(result, "share text short link", "", 170001, 1, kAvPage, kAvCacheKey);
        ExpectRequests(redirects, {"https://b23.tv/share"}, "share text short link");
    }
    {
        // Two b23.tv hops before the video page.
        FakeRedirects redirects;
        redirects.Redirect("https://b23.tv/two-a", 302, "https://b23.tv/two-b");
        redirects.Redirect("https://b23.tv/two-b", 307, "https://www.bilibili.com/video/BV1xx411c7mD");
        const BilibiliLinkResult result = ResolveWith("https://b23.tv/two-a", &redirects);
        ExpectPage(result, "short link two hops", kBv, 0, 1, kBvPage, kBvCacheKey);
        ExpectRequests(redirects, {"https://b23.tv/two-a", "https://b23.tv/two-b"},
                       "short link two hops");
    }

    const int statuses[] = {301, 302, 303, 307, 308};
    for (int hops = 1; hops <= 5; ++hops) {
        FakeRedirects redirects;
        const std::string prefix = "https://b23.tv/chain-" + std::to_string(hops) + "-";
        std::vector<std::string> expected;
        for (int index = 1; index <= hops; ++index) {
            expected.push_back(prefix + std::to_string(index));
        }
        for (int index = 1; index <= hops; ++index) {
            const std::string location = index < hops
                                             ? prefix + std::to_string(index + 1)
                                             : std::string(kBvPage);
            redirects.Redirect(expected[static_cast<std::size_t>(index - 1)],
                               statuses[(index - 1) % 5], location);
        }
        const BilibiliLinkResult result = ResolveWith(expected.front(), &redirects);
        ExpectPage(result, "short link chain of " + std::to_string(hops), kBv, 0, 1, kBvPage,
                   kBvCacheKey);
        ExpectRequests(redirects, expected, "short link chain of " + std::to_string(hops));
    }
    {
        // Five redirects are allowed; needing a sixth one is rejected.
        FakeRedirects redirects;
        const std::string prefix = "https://b23.tv/limit-";
        std::vector<std::string> expected;
        for (int index = 1; index <= 5; ++index) {
            expected.push_back(prefix + std::to_string(index));
        }
        for (int index = 1; index <= 5; ++index) {
            redirects.Redirect(expected[static_cast<std::size_t>(index - 1)], 302,
                               prefix + std::to_string(index + 1));
        }
        redirects.Redirect(prefix + "6", 302, std::string(kBvPage));
        const BilibiliLinkResult result = ResolveWith(prefix + "1", &redirects);
        ExpectError(result, BilibiliLinkError::RedirectLimit, "short link sixth redirect");
        ExpectRequests(redirects, expected, "short link sixth redirect");
    }
    {
        FakeRedirects redirects;
        redirects.Redirect("https://b23.tv/to-http", 302,
                           "http://www.bilibili.com/video/BV1xx411c7mD");
        const BilibiliLinkResult result = ResolveWith("https://b23.tv/to-http", &redirects);
        ExpectError(result, BilibiliLinkError::NotHttps, "redirect to plain HTTP");
        ExpectRequests(redirects, {"https://b23.tv/to-http"}, "redirect to plain HTTP");
    }
    {
        FakeRedirects redirects;
        redirects.Redirect("https://b23.tv/to-foreign", 302,
                           "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
        const BilibiliLinkResult result = ResolveWith("https://b23.tv/to-foreign", &redirects);
        ExpectError(result, BilibiliLinkError::UnsupportedHost,
                    "redirect to foreign host");
        ExpectRequests(redirects, {"https://b23.tv/to-foreign"}, "redirect to foreign host");
    }
    {
        FakeRedirects redirects;
        redirects.Redirect("https://b23.tv/to-article", 302,
                           "https://www.bilibili.com/read/cv12345");
        const BilibiliLinkResult result = ResolveWith("https://b23.tv/to-article", &redirects);
        ExpectError(result, BilibiliLinkError::NotVideoPage, "redirect to article");
        ExpectRequests(redirects, {"https://b23.tv/to-article"}, "redirect to article");
    }
    {
        FakeRedirects redirects;
        redirects.Redirect("https://b23.tv/loop-a", 302, "https://b23.tv/loop-b");
        redirects.Redirect("https://b23.tv/loop-b", 302, "https://b23.tv/loop-a");
        const BilibiliLinkResult result = ResolveWith("https://b23.tv/loop-a", &redirects);
        ExpectError(result, BilibiliLinkError::RedirectInvalid, "redirect loop");
        ExpectRequests(redirects, {"https://b23.tv/loop-a", "https://b23.tv/loop-b"},
                       "redirect loop");
    }
    {
        FakeRedirects redirects;
        redirects.FailNetwork("https://b23.tv/down");
        const BilibiliLinkResult result = ResolveWith("https://b23.tv/down", &redirects);
        ExpectError(result, BilibiliLinkError::Network, "network failure");
        ExpectRequests(redirects, {"https://b23.tv/down"}, "network failure");
    }
    {
        // A 200 without a Location header cannot be resolved.
        FakeRedirects redirects;
        redirects.Redirect("https://b23.tv/plain", 200, "");
        const BilibiliLinkResult result = ResolveWith("https://b23.tv/plain", &redirects);
        ExpectError(result, BilibiliLinkError::RedirectInvalid, "short link without redirect");
        ExpectRequests(redirects, {"https://b23.tv/plain"}, "short link without redirect");
    }
}

void TestNeedsRedirectResolution() {
    Expect(better_tv_video::NeedsRedirectResolution("https://b23.tv/abc"),
           "b23 link needs resolution");
    Expect(better_tv_video::NeedsRedirectResolution("\u770b\u8fd9\u4e2a https://b23.tv/abc \u597d\u770b"),
           "share text b23 link needs resolution");
    Expect(!better_tv_video::NeedsRedirectResolution(kBvPage),
           "video page does not need resolution");
    Expect(!better_tv_video::NeedsRedirectResolution(kBv), "bare BV does not need resolution");
    Expect(!better_tv_video::NeedsRedirectResolution(""), "empty input does not need resolution");
    Expect(!better_tv_video::NeedsRedirectResolution("hello"), "plain text does not need resolution");
}

}  // namespace

int main() {
    TestBareIdentifiers();
    TestVideoUrls();
    TestInvalidPageNumbers();
    TestInputErrors();
    TestShortLinks();
    TestNeedsRedirectResolution();

    if (g_failures != 0) {
        std::cout << "FAIL: " << g_failures << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "PASS: " << g_checks << " checks\n";
    return 0;
}
