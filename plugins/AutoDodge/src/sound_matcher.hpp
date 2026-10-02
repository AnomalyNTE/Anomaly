// 音频匹配器：复刻参考项目 NTESoundTrigger 的信号链
//
//   soundcard 32kHz 回路 → to_mono → 4 阶 Butterworth 高通(1kHz, lfilter)
//   → 环形缓冲(max(参考时长, 0.5s)) → RMS 归一化 → FFT 互相关
//   → score = max(corr) / max(len(seg), len(ref))
//
// 高通系数由 tools/make_reference.py 用 numpy 复刻 scipy.signal.butter 得到，
// 频响已校验：直流 ≈ 0、1kHz = -3.010dB、奈奎斯特 = 0dB。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace anomaly::plugins::auto_dodge {

// 参考项目的采样率，参考波形也是这个采样率。
constexpr std::uint32_t kReferenceSampleRate = 32000;
constexpr std::size_t kHighPassOrder = 4;

// 原地 radix-2 复数 FFT（N 必须是 2 的幂）。inverse 时含 1/N 缩放。
void FftTransform(std::vector<double>& real, std::vector<double>& imaginary,
                  bool inverse) noexcept;

class SoundMatcher final {
public:
    // reference 为已经过零相位高通滤波的参考波形（tools/make_reference.py 导出）。
    bool Prepare(const std::vector<double>& reference, std::uint32_t sample_rate) noexcept;

    // 追加一帧单声道样本（采样率与 Prepare 一致），返回本帧互相关得分。
    double Feed(const float* samples, std::size_t count) noexcept;

    [[nodiscard]] bool ready() const noexcept { return !reference_.empty(); }
    [[nodiscard]] std::size_t reference_samples() const noexcept { return reference_.size(); }
    [[nodiscard]] std::size_t buffer_samples() const noexcept { return buffer_.size(); }
    [[nodiscard]] std::size_t fft_size() const noexcept { return fft_size_; }
    [[nodiscard]] std::size_t segment_samples() const noexcept { return segment_.size(); }
    [[nodiscard]] const char* error() const noexcept { return error_; }

private:
    void Push(const float* samples, std::size_t count) noexcept;
    void ExtractSegment() noexcept;
    void UpdateFftPlan() noexcept;

    // 高通滤波器状态（直接 I 型）
    std::array<double, kHighPassOrder + 1> b_{};
    std::array<double, kHighPassOrder + 1> a_{};
    std::array<double, kHighPassOrder> x_{};
    std::array<double, kHighPassOrder> y_{};

    std::vector<double> reference_;
    std::vector<double> reference_fft_real_;
    std::vector<double> reference_fft_imaginary_;

    std::vector<double> buffer_;   // 环形缓冲
    std::size_t position_{};
    std::size_t filled_{};

    std::vector<double> segment_;
    std::vector<double> signal_real_;
    std::vector<double> signal_imaginary_;
    std::size_t fft_size_{};
    std::size_t correlation_samples_{};

    std::vector<float> filtered_;
    const char* error_{""};
};

}  // namespace anomaly::plugins::auto_dodge
