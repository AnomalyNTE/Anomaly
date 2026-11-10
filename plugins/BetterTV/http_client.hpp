#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace better_tv_video {

// Text response for the bilibili API. `detail` is English log data only.
struct HttpTextResult final {
    bool ok{};
    int status{};
    std::string body;
    std::string detail;
};

// HTTPS GET used for API documents and short links. Every redirect stays on
// HTTPS, at most five are followed, and the bilibili UA/Referer/Origin are set.
// `cancelled` is polled between redirects and while reading the body.
HttpTextResult HttpGetText(std::string_view url, const std::function<bool()>& cancelled = {});

// Streaming HTTPS response used for media downloads. Read must return promptly
// enough that the caller can poll its cancel flag (the transport uses a short
// receive timeout); >0 is a byte count, 0 is end of stream, <0 is an error.
class HttpMediaStream {
public:
    virtual ~HttpMediaStream() = default;
    virtual bool Open(std::string_view url, std::uint64_t* total_bytes,
                      std::string* detail) = 0;
    virtual int Read(std::span<std::uint8_t> buffer, std::string* detail) = 0;
    virtual void Close() = 0;
};

using HttpMediaStreamFactory = std::function<std::unique_ptr<HttpMediaStream>()>;

// Production factory using WinHTTP with the bilibili media headers.
HttpMediaStreamFactory DefaultMediaStreamFactory();

// True for an HTTPS URL without userinfo whose literal host is not loopback or
// a private address. Media URLs come from the API, but the downloader still
// enforces transport and destination safety.
bool IsSafeMediaUrl(std::string_view url, std::string* detail);

}  // namespace better_tv_video
