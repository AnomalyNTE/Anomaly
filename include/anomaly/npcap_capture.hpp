#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

struct NpcapDevice final {
    std::string name;
    std::string description;
    std::vector<std::string> addresses;
};

struct NpcapTimestamp final {
    std::int64_t seconds{};
    std::int32_t microseconds{};
};

struct NpcapPacket final {
    NpcapTimestamp timestamp;
    std::uint32_t captured_length{};
    std::uint32_t original_length{};
    // The bytes are owned by Npcap and remain valid until the next Next call,
    // Close call, or destruction of the capture session.
    std::span<const std::uint8_t> bytes;
};

struct NpcapStats final {
    std::uint32_t received{};
    std::uint32_t dropped{};
    std::uint32_t interface_dropped{};
};

enum class NpcapNextResult : std::uint8_t {
    Packet,
    Timeout,
    Error,
};

// A synchronous, non-promiscuous Npcap capture session. Instances are not
// thread-safe; one caller owns each session and its packet views.
class NpcapCapture final {
public:
    ~NpcapCapture();

    NpcapCapture(const NpcapCapture&) = delete;
    NpcapCapture& operator=(const NpcapCapture&) = delete;
    NpcapCapture(NpcapCapture&&) = delete;
    NpcapCapture& operator=(NpcapCapture&&) = delete;

    // Loads wpcap.dll for the duration of enumeration. On failure this returns
    // an empty list and fills error.
    [[nodiscard]] static std::vector<NpcapDevice> EnumerateDevices(
        std::string& error) noexcept;

    // Opens device_name with a 65535-byte snap length, non-promiscuous mode,
    // immediate delivery when supported, and the optional BPF expression.
    [[nodiscard]] static std::unique_ptr<NpcapCapture> Open(
        std::string_view device_name, std::string_view bpf_expression,
        std::string& error) noexcept;
    [[nodiscard]] static std::unique_ptr<NpcapCapture> Open(
        const NpcapDevice& device, std::string_view bpf_expression,
        std::string& error) noexcept;

    // A zero or negative timeout performs one non-blocking poll.
    [[nodiscard]] NpcapNextResult Next(
        NpcapPacket& packet, std::chrono::milliseconds timeout,
        std::string& error) noexcept;
    [[nodiscard]] bool Stats(NpcapStats& stats, std::string& error) const noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;
    void Close() noexcept;

private:
    class Impl;

    explicit NpcapCapture(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
