// WASAPI 默认渲染设备的回路（loopback）采集。
//
// 采集线程流程：
//   事件驱动读包 → 混单声道 → 线性重采样到 32kHz → 攒够一帧回调一次
// 回调发生在采集线程上，回调里做的事必须短且自包含。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace anomaly::plugins::auto_dodge {

struct CaptureStatus {
    std::uint32_t device_sample_rate{};
    std::uint32_t device_channels{};
    std::uint32_t device_bits{};
    std::uint64_t packets{};
    std::uint64_t frames{};
    char device[128]{"未打开"};
    char state[64]{"停止"};
    char error[192]{};
};

class LoopbackCapture final {
public:
    // mono 是 32kHz 单声道采样，长度固定为 frame_samples。
    using FrameCallback = void (*)(void* user, const float* mono, std::size_t count);

    LoopbackCapture();
    ~LoopbackCapture();
    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;

    bool Start(void* user, FrameCallback callback, std::size_t frame_samples) noexcept;
    void Stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] CaptureStatus Status() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly::plugins::auto_dodge
