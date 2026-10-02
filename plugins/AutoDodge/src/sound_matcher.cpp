#include "sound_matcher.hpp"

#include <algorithm>
#include <cmath>

namespace anomaly::plugins::auto_dodge {
namespace {

constexpr double kPi = 3.14159265358979323846;

// tools/make_reference.py 输出的 scipy butter(4, 1000, 'highpass', fs=32000, 'ba')
constexpr std::array<double, kHighPassOrder + 1> kHighPassB{
    0.7733467891606215, -3.093387156642486, 4.6400807349637292,
    -3.093387156642486, 0.7733467891606215};
constexpr std::array<double, kHighPassOrder + 1> kHighPassA{
    1.0, -3.4873077415499001, 4.5892912320784056, -2.6988843913407536,
    0.59806526160088691};

constexpr double kPi2 = 6.28318530717958647692;

}  // namespace

void FftTransform(std::vector<double>& real, std::vector<double>& imaginary,
                  const bool inverse) noexcept {
    const std::size_t n = real.size();
    if (n < 2 || (n & (n - 1U)) != 0 || imaginary.size() != n) return;

    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1U;
        for (; (j & bit) != 0; bit >>= 1U) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imaginary[i], imaginary[j]);
        }
    }

    for (std::size_t length = 2; length <= n; length <<= 1U) {
        const double angle = (inverse ? kPi2 : -kPi2) / static_cast<double>(length);
        const double step_real = std::cos(angle);
        const double step_imaginary = std::sin(angle);
        const std::size_t half = length >> 1U;
        for (std::size_t base = 0; base < n; base += length) {
            double twiddle_real = 1.0;
            double twiddle_imaginary = 0.0;
            for (std::size_t k = 0; k < half; ++k) {
                const std::size_t even = base + k;
                const std::size_t odd = even + half;
                const double product_real =
                    real[odd] * twiddle_real - imaginary[odd] * twiddle_imaginary;
                const double product_imaginary =
                    real[odd] * twiddle_imaginary + imaginary[odd] * twiddle_real;
                real[odd] = real[even] - product_real;
                imaginary[odd] = imaginary[even] - product_imaginary;
                real[even] += product_real;
                imaginary[even] += product_imaginary;
                const double next_real = twiddle_real * step_real - twiddle_imaginary * step_imaginary;
                twiddle_imaginary = twiddle_real * step_imaginary + twiddle_imaginary * step_real;
                twiddle_real = next_real;
            }
        }
    }

    if (!inverse) return;
    const double scale = 1.0 / static_cast<double>(n);
    for (std::size_t i = 0; i < n; ++i) {
        real[i] *= scale;
        imaginary[i] *= scale;
    }
}

bool SoundMatcher::Prepare(const std::vector<double>& reference,
                           const std::uint32_t sample_rate) noexcept {
    if (reference.empty() || sample_rate == 0) {
        error_ = "参考波形为空";
        return false;
    }
    b_ = kHighPassB;
    a_ = kHighPassA;
    x_.fill(0.0);
    y_.fill(0.0);
    position_ = 0;
    filled_ = 0;
    error_ = "";

    // 参考波形按 RMS 归一化，和参考项目一致（先在 32kHz 域校验采样率）。
    if (sample_rate != kReferenceSampleRate) {
        error_ = "参考波形采样率与采集管线不一致";
        return false;
    }
    reference_ = reference;
    double sum{};
    for (const double value : reference_) sum += value * value;
    const double rms = std::sqrt(sum / static_cast<double>(reference_.size()) + 1e-6);
    for (double& value : reference_) value /= rms;

    // 环形缓冲长度 = max(参考时长, 0.5s)，与参考项目一致。
    const std::size_t reference_seconds = reference_.size();
    const std::size_t minimum = static_cast<std::size_t>(sample_rate / 2U);
    buffer_.assign(std::max(reference_seconds, minimum), 0.0);
    filtered_.reserve(4096);

    correlation_samples_ = buffer_.size() + reference_.size() - 1U;
    std::size_t plan = 1;
    while (plan < correlation_samples_) plan <<= 1U;
    fft_size_ = plan;

    // 参考的共轭频谱只算一次。
    reference_fft_real_.assign(fft_size_, 0.0);
    reference_fft_imaginary_.assign(fft_size_, 0.0);
    std::copy(reference_.begin(), reference_.end(), reference_fft_real_.begin());
    FftTransform(reference_fft_real_, reference_fft_imaginary_, false);
    for (std::size_t i = 0; i < fft_size_; ++i) {
        reference_fft_imaginary_[i] = -reference_fft_imaginary_[i];
    }

    signal_real_.assign(fft_size_, 0.0);
    signal_imaginary_.assign(fft_size_, 0.0);
    segment_.assign(buffer_.size(), 0.0);
    return true;
}

void SoundMatcher::Push(const float* samples, const std::size_t count) noexcept {
    if (samples == nullptr || count == 0 || buffer_.empty()) return;
    const std::size_t size = buffer_.size();
    if (count >= size) {
        for (std::size_t i = 0; i < size; ++i) {
            buffer_[i] = static_cast<double>(samples[count - size + i]);
        }
        position_ = 0;
        filled_ = size;
        return;
    }
    const std::size_t end = position_ + count;
    if (end <= size) {
        for (std::size_t i = 0; i < count; ++i) {
            buffer_[position_ + i] = static_cast<double>(samples[i]);
        }
    } else {
        const std::size_t first = size - position_;
        for (std::size_t i = 0; i < first; ++i) buffer_[position_ + i] = samples[i];
        for (std::size_t i = 0; i < count - first; ++i) buffer_[i] = samples[first + i];
    }
    position_ = end % size;
    filled_ = std::min(filled_ + count, size);
}

void SoundMatcher::ExtractSegment() noexcept {
    const std::size_t size = buffer_.size();
    if (filled_ < size) {
        std::copy(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(filled_),
            segment_.begin());
        segment_.resize(filled_);
        return;
    }
    segment_.resize(size);
    if (position_ == 0) {
        segment_ = buffer_;
        return;
    }
    const std::size_t first = size - position_;
    std::copy(buffer_.begin() + static_cast<std::ptrdiff_t>(position_), buffer_.end(),
        segment_.begin());
    std::copy(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(position_),
        segment_.begin() + static_cast<std::ptrdiff_t>(first));
}

void SoundMatcher::UpdateFftPlan() noexcept {
    std::size_t plan = 1;
    while (plan < correlation_samples_) plan <<= 1U;
    if (plan == fft_size_) return;
    fft_size_ = plan;
    reference_fft_real_.assign(fft_size_, 0.0);
    reference_fft_imaginary_.assign(fft_size_, 0.0);
    std::copy(reference_.begin(), reference_.end(), reference_fft_real_.begin());
    FftTransform(reference_fft_real_, reference_fft_imaginary_, false);
    for (std::size_t i = 0; i < fft_size_; ++i) {
        reference_fft_imaginary_[i] = -reference_fft_imaginary_[i];
    }
    signal_real_.assign(fft_size_, 0.0);
    signal_imaginary_.assign(fft_size_, 0.0);
}

double SoundMatcher::Feed(const float* samples, const std::size_t count) noexcept {
    if (samples == nullptr || count == 0 || !ready()) return 0.0;

    // 1) 高通滤波（lfilter 等价形式，状态跨帧保留）
    filtered_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double input = static_cast<double>(samples[i]);
        double output = b_[0] * input;
        for (std::size_t k = 1; k <= kHighPassOrder; ++k) {
            output += b_[k] * x_[k - 1U] - a_[k] * y_[k - 1U];
        }
        for (std::size_t k = kHighPassOrder; k > 1U; --k) {
            x_[k - 1U] = x_[k - 2U];
            y_[k - 1U] = y_[k - 2U];
        }
        x_[0] = input;
        y_[0] = output;
        filtered_[i] = static_cast<float>(output);
    }

    // 2) 环形缓冲
    Push(filtered_.data(), filtered_.size());

    // 3) 取段 + RMS 归一化
    ExtractSegment();
    const std::size_t segment = segment_.size();
    if (segment == 0 || reference_.empty()) return 0.0;
    double energy{};
    for (const double value : segment_) energy += value * value;
    const double rms = std::sqrt(energy / static_cast<double>(segment) + 1e-6);

    correlation_samples_ = segment + reference_.size() - 1U;
    UpdateFftPlan();

    std::fill(signal_real_.begin(), signal_real_.end(), 0.0);
    std::fill(signal_imaginary_.begin(), signal_imaginary_.end(), 0.0);
    for (std::size_t i = 0; i < segment; ++i) {
        signal_real_[i] = segment_[i] / rms;
    }
    FftTransform(signal_real_, signal_imaginary_, false);

    // 4) 频域相乘（参考取共轭）后逆变换 = 互相关
    for (std::size_t i = 0; i < fft_size_; ++i) {
        const double real = signal_real_[i];
        const double imaginary = signal_imaginary_[i];
        signal_real_[i] = real * reference_fft_real_[i] - imaginary * reference_fft_imaginary_[i];
        signal_imaginary_[i] =
            real * reference_fft_imaginary_[i] + imaginary * reference_fft_real_[i];
    }
    FftTransform(signal_real_, signal_imaginary_, true);

    // 5) 得分：与参考项目一致，除以 max(len(seg), len(ref))
    double peak = signal_real_[0];
    for (std::size_t i = 1; i < correlation_samples_; ++i) {
        if (signal_real_[i] > peak) peak = signal_real_[i];
    }
    const double divisor = static_cast<double>(std::max(segment, reference_.size()));
    return divisor > 0.0 ? peak / divisor : 0.0;
}

}  // namespace anomaly::plugins::auto_dodge
