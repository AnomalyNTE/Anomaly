#include "sound_capture.hpp"

#include "sound_matcher.hpp"

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <propidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace anomaly::plugins::auto_dodge {
namespace {

// PKEY_Device_FriendlyName（自己定义一份，避免依赖 propsys 的定义/初始化约定）
const PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// KSDATAFORMAT_SUBTYPE_*
const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

enum class SampleFormat { Float32, Pcm16, Pcm32, Unsupported };

SampleFormat DetectFormat(const WAVEFORMATEX& format) noexcept {
    const bool extensible = format.wFormatTag == WAVE_FORMAT_EXTENSIBLE;
    const GUID* subtype = nullptr;
    if (extensible && format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        subtype = &reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&format)->SubFormat;
    }
    const bool is_float = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (subtype != nullptr && IsEqualGUID(*subtype, kSubtypeIeeeFloat));
    const bool is_pcm = format.wFormatTag == WAVE_FORMAT_PCM ||
        (subtype != nullptr && IsEqualGUID(*subtype, kSubtypePcm));
    if (is_float && format.wBitsPerSample == 32) return SampleFormat::Float32;
    if (is_pcm && format.wBitsPerSample == 16) return SampleFormat::Pcm16;
    if (is_pcm && format.wBitsPerSample == 32) return SampleFormat::Pcm32;
    return SampleFormat::Unsupported;
}

double SampleAt(const BYTE* data, const std::size_t index, const std::size_t channels,
                const SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Float32: {
        double sum{};
        for (std::size_t c = 0; c < channels; ++c) {
            sum += static_cast<double>(
                reinterpret_cast<const float*>(data)[index * channels + c]);
        }
        return sum / static_cast<double>(channels);
    }
    case SampleFormat::Pcm16: {
        double sum{};
        for (std::size_t c = 0; c < channels; ++c) {
            sum += static_cast<double>(
                       reinterpret_cast<const std::int16_t*>(data)[index * channels + c]) /
                32768.0;
        }
        return sum / static_cast<double>(channels);
    }
    case SampleFormat::Pcm32: {
        double sum{};
        for (std::size_t c = 0; c < channels; ++c) {
            sum += static_cast<double>(
                       reinterpret_cast<const std::int32_t*>(data)[index * channels + c]) /
                2147483648.0;
        }
        return sum / static_cast<double>(channels);
    }
    case SampleFormat::Unsupported:
    default:
        return 0.0;
    }
}

bool ReadDeviceName(IMMDevice* device, char* destination, const std::size_t capacity) noexcept {
    if (device == nullptr || destination == nullptr || capacity == 0) return false;
    destination[0] = '\0';
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || store == nullptr) return false;
    PROPVARIANT value{};
    PropVariantInit(&value);
    bool ok = false;
    if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR &&
        value.pwszVal != nullptr) {
        const int written =
            WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, destination,
                static_cast<int>(capacity), nullptr, nullptr);
        ok = written > 0;
    }
    PropVariantClear(&value);
    store->Release();
    return ok;
}

}  // namespace

struct LoopbackCapture::Impl {
    FrameCallback callback{};
    void* user{};
    std::size_t frame_samples{};
    std::thread thread;
    std::atomic_bool running{false};
    HANDLE stop_event{};
    HANDLE capture_event{};

    mutable std::mutex mutex;
    CaptureStatus status;

    // 重采样与组帧状态（仅采集线程访问）
    std::vector<float> pending;
    double position{};
    std::vector<float> frame;
    std::vector<float> mono;
    std::vector<float> resampled;

    void SetState(const char* text) noexcept {
        std::scoped_lock lock(mutex);
        std::snprintf(status.state, sizeof(status.state), "%s", text);
    }

    void SetError(const char* text) noexcept {
        std::scoped_lock lock(mutex);
        std::snprintf(status.error, sizeof(status.error), "%s", text);
    }

    // 线性重采样到 32kHz，并把结果攒成 frame_samples 一帧回调出去。
    void ResampleAndEmit() noexcept {
        if (mono.empty()) return;
        const double ratio = static_cast<double>(status.device_sample_rate) /
            static_cast<double>(kReferenceSampleRate);
        if (status.device_sample_rate == 0 || ratio <= 0.0) return;
        pending.insert(pending.end(), mono.begin(), mono.end());
        mono.clear();

        resampled.clear();
        if (ratio == 1.0) {
            resampled.swap(pending);
            pending.clear();
        } else {
            while (position + 1.0 < static_cast<double>(pending.size())) {
                const std::size_t index = static_cast<std::size_t>(position);
                const double fraction = position - static_cast<double>(index);
                const double value = pending[index] * (1.0 - fraction) + pending[index + 1] * fraction;
                resampled.push_back(static_cast<float>(value));
                position += ratio;
            }
            const std::size_t consumed = static_cast<std::size_t>(position);
            if (consumed > 0) {
                pending.erase(pending.begin(),
                    pending.begin() + static_cast<std::ptrdiff_t>(consumed));
                position -= static_cast<double>(consumed);
            }
        }

        for (const float value : resampled) {
            frame.push_back(value);
            if (frame.size() < frame_samples) continue;
            if (callback != nullptr) callback(user, frame.data(), frame.size());
            frame.clear();
            {
                std::scoped_lock lock(mutex);
                status.frames += 1;
            }
        }
    }

    void Run() noexcept {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_owned = SUCCEEDED(com);

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* format = nullptr;

        const auto cleanup = [&]() {
            if (client != nullptr) client->Stop();
            if (format != nullptr) CoTaskMemFree(format);
            if (capture != nullptr) capture->Release();
            if (client != nullptr) client->Release();
            if (device != nullptr) device->Release();
            if (enumerator != nullptr) enumerator->Release();
            if (com_owned) CoUninitialize();
        };

        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(result) || enumerator == nullptr) {
            SetError("创建音频设备枚举器失败");
            cleanup();
            running.store(false);
            return;
        }
        result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(result) || device == nullptr) {
            SetError("没有可用的默认播放设备");
            cleanup();
            running.store(false);
            return;
        }
        char device_name[128]{};
        if (ReadDeviceName(device, device_name, sizeof(device_name))) {
            std::scoped_lock lock(mutex);
            std::snprintf(status.device, sizeof(status.device), "%s", device_name);
        }
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&client));
        if (FAILED(result) || client == nullptr) {
            SetError("打开音频客户端失败");
            cleanup();
            running.store(false);
            return;
        }
        result = client->GetMixFormat(&format);
        if (FAILED(result) || format == nullptr) {
            SetError("读取混音格式失败");
            cleanup();
            running.store(false);
            return;
        }
        const SampleFormat sample_format = DetectFormat(*format);
        if (sample_format == SampleFormat::Unsupported) {
            char text[192]{};
            std::snprintf(text, sizeof(text), "不支持的混音格式 tag=%u bits=%u",
                format->wFormatTag, format->wBitsPerSample);
            SetError(text);
            cleanup();
            running.store(false);
            return;
        }
        {
            std::scoped_lock lock(mutex);
            status.device_sample_rate = format->nSamplesPerSec;
            status.device_channels = format->nChannels;
            status.device_bits = format->wBitsPerSample;
        }

        // 回路采集：100ms 缓冲 + 事件回调，实际投递间隔约等于引擎周期（~10ms）。
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0,
            format, nullptr);
        if (FAILED(result)) {
            SetError("初始化回路采集失败（共享模式不可用）");
            cleanup();
            running.store(false);
            return;
        }
        result = client->SetEventHandle(capture_event);
        if (FAILED(result)) {
            SetError("设置采集事件失败");
            cleanup();
            running.store(false);
            return;
        }
        result = client->GetService(__uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&capture));
        if (FAILED(result) || capture == nullptr) {
            SetError("获取采集接口失败");
            cleanup();
            running.store(false);
            return;
        }
        result = client->Start();
        if (FAILED(result)) {
            SetError("启动采集失败");
            cleanup();
            running.store(false);
            return;
        }

        SetState("运行中");
        const HANDLE events[2] = {capture_event, stop_event};
        while (running.load(std::memory_order_relaxed)) {
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 100);
            if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_FAILED) break;
            if (!running.load(std::memory_order_relaxed)) break;
            for (;;) {
                UINT32 packet{};
                if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0) break;
                BYTE* data = nullptr;
                UINT32 frames{};
                DWORD flags{};
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                const std::size_t channels = format->nChannels;
                mono.reserve(mono.size() + frames);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                    mono.insert(mono.end(), frames, 0.0F);
                } else {
                    for (UINT32 i = 0; i < frames; ++i) {
                        mono.push_back(static_cast<float>(
                            SampleAt(data, i, channels, sample_format)));
                    }
                }
                capture->ReleaseBuffer(frames);
                {
                    std::scoped_lock lock(mutex);
                    status.packets += 1;
                }
            }
            ResampleAndEmit();
        }

        SetState("已停止");
        cleanup();
        running.store(false);
    }
};

LoopbackCapture::LoopbackCapture() : impl_(std::make_unique<Impl>()) {}

LoopbackCapture::~LoopbackCapture() { Stop(); }

bool LoopbackCapture::Start(void* user, const FrameCallback callback,
                            const std::size_t frame_samples) noexcept {
    if (impl_ == nullptr || callback == nullptr || frame_samples == 0) return false;
    Stop();
    impl_->user = user;
    impl_->callback = callback;
    impl_->frame_samples = frame_samples;
    impl_->pending.clear();
    impl_->position = 0.0;
    impl_->frame.clear();
    impl_->frame.reserve(frame_samples * 2);
    impl_->mono.clear();
    impl_->resampled.clear();
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->status = CaptureStatus{};
        std::snprintf(impl_->status.state, sizeof(impl_->status.state), "%s", "启动中");
    }

    impl_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->capture_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->stop_event == nullptr || impl_->capture_event == nullptr) {
        if (impl_->stop_event != nullptr) CloseHandle(impl_->stop_event);
        if (impl_->capture_event != nullptr) CloseHandle(impl_->capture_event);
        impl_->stop_event = nullptr;
        impl_->capture_event = nullptr;
        return false;
    }
    impl_->running.store(true);
    impl_->thread = std::thread([this] { impl_->Run(); });
    return true;
}

void LoopbackCapture::Stop() noexcept {
    if (impl_ == nullptr) return;
    if (impl_->stop_event != nullptr) SetEvent(impl_->stop_event);
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->stop_event != nullptr) {
        CloseHandle(impl_->stop_event);
        impl_->stop_event = nullptr;
    }
    if (impl_->capture_event != nullptr) {
        CloseHandle(impl_->capture_event);
        impl_->capture_event = nullptr;
    }
    impl_->running.store(false);
}

bool LoopbackCapture::running() const noexcept {
    return impl_ != nullptr && impl_->running.load(std::memory_order_relaxed);
}

CaptureStatus LoopbackCapture::Status() const noexcept {
    if (impl_ == nullptr) return {};
    std::scoped_lock lock(impl_->mutex);
    return impl_->status;
}

}  // namespace anomaly::plugins::auto_dodge
