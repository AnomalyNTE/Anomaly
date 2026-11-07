#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace nte_tv_video {

class AudioOutput final {
public:
    enum class State { Idle, Opening, Playing, Paused, NoAudio, Failed };
    struct Snapshot {
        State state{};
        std::uint32_t error{};
        std::int64_t position_ticks{};
    };

    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Transport time uses the same 100 ns units as UE FTimespan. File and device
    // operations stay on the owned worker; no UE object enters that thread.
    void Transport(std::wstring path, std::int64_t position_ticks, bool playing);
    void Shutdown() noexcept;
    [[nodiscard]] Snapshot Read() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nte_tv_video
