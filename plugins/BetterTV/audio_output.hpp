#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace better_tv_video {

// TV position in listener space, in metres, using X3DAudio's axis order.
struct AudioPosition {
    float right{}, up{}, forward{};
};

struct AudioDistanceRange {
    static constexpr float kMinFullVolumeMeters = 0.5f;
    static constexpr float kMaxFullVolumeMeters = 20.0f;
    static constexpr float kMaxDistanceMeters = 60.0f;
    static constexpr float kMinimumFadeMeters = 1.0f;

    float full_volume_meters{4.0f};
    float max_distance_meters{16.0f};

    [[nodiscard]] float Gain(float distance_meters) const noexcept;
};

class AudioOutput final {
public:
    enum class State { Idle, Opening, Playing, Paused, NoAudio, Failed };
    enum class Route { None, External, Spatial };
    struct Snapshot {
        State state{};
        std::uint32_t error{};
        std::int64_t position_ticks{};
        Route route{};
        std::uint32_t spatial_error{};
    };
    struct Frame {
        std::uint64_t session{};
        std::uint64_t timeline{};
        std::optional<AudioPosition> source;
        AudioDistanceRange distance_range;
    };

    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Transport time uses the same 100 ns units as UE FTimespan. File and device
    // operations stay on the owned worker; no UE object enters that thread.
    void Transport(std::wstring path, std::int64_t position_ticks, bool playing);
    void Transport(std::wstring path, std::int64_t position_ticks, bool playing, Frame frame);
    [[nodiscard]] bool Shutdown(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] Snapshot Read() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace better_tv_video
