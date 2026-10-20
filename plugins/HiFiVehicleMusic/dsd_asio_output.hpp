#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ASIOTime;

namespace hifi_vehicle_music {

// Native DSD output through an ASIO driver switched to DSD mode
// (ASIOFuture kAsioSetIoFormat / kASIODSDFormat). The bitstream reaches the
// driver unchanged; there is no PCM conversion, so no volume either.
//
// It shares the process-wide ASIO driver slot with RtAudio: only one of the
// two may hold a driver at a time, and every call must come from the thread
// that owns the ASIO driver (the engine worker, single-threaded apartment).
class DsdAsioOutput final {
public:
    // Fills `frames` frames into `interleaved` (one byte per channel per
    // frame, first DSD sample in the MSB). Runs on the driver's thread.
    using Fill = void (*)(void* user, std::uint8_t* interleaved, std::size_t frames);

    DsdAsioOutput() = default;
    ~DsdAsioOutput();
    DsdAsioOutput(const DsdAsioOutput&) = delete;
    DsdAsioOutput& operator=(const DsdAsioOutput&) = delete;

    // `rate` is the DSD sample rate (2822400 for DSD64). On failure returns
    // false with a reason in `error` and leaves no driver loaded.
    bool Open(const std::string& driver, unsigned channels, unsigned rate, Fill fill, void* user,
        std::string& error);
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return open_; }
    [[nodiscard]] unsigned Channels() const noexcept { return channels_; }
    [[nodiscard]] unsigned Rate() const noexcept { return rate_; }
    [[nodiscard]] bool LsbFirst() const noexcept { return lsb_first_; }

private:
    static void BufferSwitch(long index, long direct_process);
    static void SampleRateChanged(double rate);
    static long Message(long selector, long value, void* message, double* opt);
    static ::ASIOTime* BufferSwitchTimeInfo(::ASIOTime* time, long index, long direct_process);
    void Render(long index) noexcept;

    bool open_{};
    bool loaded_{};
    bool dsd_mode_{};
    bool buffers_{};
    unsigned channels_{};
    unsigned rate_{};
    bool lsb_first_{};
    std::size_t bytes_{};  // per channel per buffer half
    std::vector<void*> halves_[2];
    std::vector<std::uint8_t> scratch_;
    Fill fill_{};
    void* user_{};
};

} // namespace hifi_vehicle_music
