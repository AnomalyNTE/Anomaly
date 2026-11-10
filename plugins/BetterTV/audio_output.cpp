#include "audio_output.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <xaudio2.h>
#include <x3daudio.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace better_tv_video {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

float AudioDistanceRange::Gain(float distance_meters) const noexcept {
    return std::clamp((max_distance_meters - distance_meters) /
        (max_distance_meters - full_volume_meters), 0.0f, 1.0f);
}

namespace {

constexpr std::int64_t kTicksPerSecond = 10000000;
constexpr std::int64_t kSyncTolerance = 1000000;  // 100 ms
constexpr auto kSampleLifetime = 500ms;
constexpr auto kAudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr auto kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);

std::int64_t Ticks(Clock::duration time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time).count() / 100;
}

struct AudioCommand {
    std::wstring path;
    std::int64_t ticks{};
    bool playing{};
    AudioOutput::Frame frame;
    Clock::time_point sampled;
};

class SpatialVoice {
public:
    ~SpatialVoice() {
        ClearVoice();
        if (master_) master_->DestroyVoice();
    }

    HRESULT Open(const wchar_t* path) {
        HRESULT result = MFCreateSourceReaderFromURL(path, nullptr, &reader_);
        if (FAILED(result)) return result;
        ComPtr<IMFMediaType> native, pcm;
        result = reader_->GetNativeMediaType(kAudioStream, 0, &native);
        if (result == MF_E_INVALIDSTREAMNUMBER) return S_FALSE;
        if (FAILED(result)) return result;

        UINT32 channels{};
        if (FAILED(native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
            channels < 1 || channels > 2) return MF_E_INVALIDMEDIATYPE;
        if (FAILED(result = reader_->SetStreamSelection(kAllStreams, FALSE)) ||
            FAILED(result = reader_->SetStreamSelection(kAudioStream, TRUE)) ||
            FAILED(result = MFCreateMediaType(&pcm)) ||
            FAILED(result = pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
            FAILED(result = pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) ||
            FAILED(result = reader_->SetCurrentMediaType(kAudioStream, nullptr, pcm.Get())) ||
            FAILED(result = reader_->GetCurrentMediaType(kAudioStream, &pcm))) return result;
        if (FAILED(result = ReadFormat(*pcm.Get()))) return result;
        if (FAILED(result = XAudio2Create(&engine_)) ||
            FAILED(result = engine_->CreateMasteringVoice(&master_))) return result;
        DWORD speaker_mask{};
        if (FAILED(result = master_->GetChannelMask(&speaker_mask))) return result;
        XAUDIO2_VOICE_DETAILS details{};
        master_->GetVoiceDetails(&details);
        matrix_.resize(details.InputChannels);
        return X3DAudioInitialize(speaker_mask, X3DAUDIO_SPEED_OF_SOUND, spatial_);
    }

    void Pause() {
        ClearVoice();
    }

    HRESULT Update(std::int64_t target, const AudioOutput::Frame& frame,
                   bool discontinuity, const auto& current, std::int64_t& actual) {
        XAUDIO2_VOICE_STATE state{};
        bool stalled = false;
        if (voice_) {
            voice_->GetState(&state);
            while (queued_.size() > state.BuffersQueued) queued_.pop_front();
            actual = first_frame_ + FrameTicks(state.SamplesPlayed);
            stalled = started_ && state.BuffersQueued && state.SamplesPlayed == last_samples_;
            if (!stalled) last_advance_ = Clock::now();
            last_samples_ = state.SamplesPlayed;
            if (!discontinuity && stalled && Clock::now() - last_advance_ > kSampleLifetime)
                return XAUDIO2_E_DEVICE_INVALIDATED;
        }
        const bool drifting = started_ && !stalled && std::abs(target - actual) > kSyncTolerance &&
            (!eof_ || state.BuffersQueued != 0);
        if (!voice_ || discontinuity || drifting) {
            if (const auto result = Seek(target); FAILED(result)) return result;
            actual = target;
        }

        // A local decode can block. At most 80 ms is submitted to the device,
        // so an expired sample or stalled worker cannot keep playing indefinitely.
        for (unsigned attempts = 0; attempts < 4 && queued_.size() < 4 &&
                (!eof_ || pending_offset_ < pending_.size()); ++attempts) {
            if (const auto result = Fill(target, current); result != S_OK) return result;
        }
        if (!current()) return S_FALSE;
        if (const auto result = Spatialize(frame); FAILED(result)) return result;
        if (!started_ && !queued_.empty() && target >= first_frame_) {
            if (const auto result = voice_->Start(); FAILED(result)) return result;
            started_ = true;
            last_advance_ = Clock::now();
        }
        return S_OK;
    }

private:
    HRESULT ReadFormat(IMFMediaType& pcm) {
        GUID subtype{};
        if (FAILED(pcm.GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFAudioFormat_Float ||
            FAILED(pcm.GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels_)) || channels_ < 1 || channels_ > 2 ||
            FAILED(pcm.GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate_)) ||
            sample_rate_ < XAUDIO2_MIN_SAMPLE_RATE || sample_rate_ > XAUDIO2_MAX_SAMPLE_RATE)
            return MF_E_INVALIDMEDIATYPE;
        return S_OK;
    }

    void ClearVoice() {
        if (voice_) {
            voice_->DestroyVoice();  // The PCM buffers remain alive until this returns.
            voice_ = nullptr;
        }
        queued_.clear();
        pending_.clear();
        pending_offset_ = 0;
        submitted_ = 0;
        last_samples_ = 0;
        started_ = false;
        eof_ = false;
    }

    HRESULT Seek(std::int64_t target) {
        ClearVoice();
        PROPVARIANT position;
        InitPropVariantFromInt64(target, &position);
        const auto result = reader_->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
        if (FAILED(result)) return result;
        const WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, 1, sample_rate_,
            sample_rate_ * static_cast<DWORD>(sizeof(float)), sizeof(float), 8 * sizeof(float), 0};
        return engine_->CreateSourceVoice(&voice_, &format, XAUDIO2_VOICE_NOPITCH);
    }

    std::int64_t FrameTicks(std::uint64_t frames) const {
        return static_cast<std::int64_t>(frames * kTicksPerSecond / sample_rate_);
    }

    std::size_t FramesFor(std::int64_t ticks) const {
        if (ticks <= 0) return 0;
        return static_cast<std::size_t>((ticks * sample_rate_ + kTicksPerSecond - 1) / kTicksPerSecond);
    }

    HRESULT Decode() {
        DWORD flags{};
        LONGLONG pts{};
        ComPtr<IMFSample> sample;
        const auto result = reader_->ReadSample(kAudioStream, 0,
            nullptr, &flags, &pts, &sample);
        if (FAILED(result)) return result;
        if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))
            return MF_E_INVALIDMEDIATYPE;
        eof_ = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
        if (!sample) return S_OK;
        ComPtr<IMFMediaBuffer> buffer;
        if (const auto hr = sample->ConvertToContiguousBuffer(&buffer); FAILED(hr)) return hr;
        DWORD bytes{};
        if (const auto hr = buffer->GetCurrentLength(&bytes); FAILED(hr)) return hr;
        const auto stride = sizeof(float) * channels_;
        if (bytes % stride) return MF_E_INVALIDMEDIATYPE;
        pending_.resize(bytes / stride);
        BYTE* data{};
        if (const auto hr = buffer->Lock(&data, nullptr, nullptr); FAILED(hr)) return hr;
        const auto* samples = reinterpret_cast<const float*>(data);
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            const float left = samples[i * channels_];
            pending_[i] = channels_ == 2 ? (left + samples[i * channels_ + 1]) * 0.5f : left;
        }
        static_cast<void>(buffer->Unlock());
        pending_pts_ = pts;
        pending_offset_ = 0;
        return S_OK;
    }

    HRESULT Fill(std::int64_t target, const auto& current) {
        if (pending_offset_ == pending_.size()) {
            pending_.clear();
            pending_offset_ = 0;
            if (const auto result = Decode(); FAILED(result)) return result;
        }
        if (!current()) return S_FALSE;
        if (pending_.empty()) return S_OK;
        const auto max_frames = static_cast<std::size_t>(sample_rate_ / 50);  // 20 ms
        const auto wanted = submitted_ ? first_frame_ + FrameTicks(submitted_) : target;
        const auto available = pending_pts_ + FrameTicks(pending_offset_);
        const auto tolerance = FrameTicks(2);
        if (wanted > available + tolerance)
            pending_offset_ += std::min(FramesFor(wanted - available), pending_.size() - pending_offset_);
        if (pending_offset_ == pending_.size()) return S_OK;

        if (!submitted_) first_frame_ = pending_pts_ + FrameTicks(pending_offset_);
        if (submitted_ && available > wanted + tolerance) {
            queued_.emplace_back(std::min(max_frames, FramesFor(available - wanted)), 0.0f);
        } else {
            const auto count = std::min(max_frames, pending_.size() - pending_offset_);
            const auto start = pending_.begin() + static_cast<std::ptrdiff_t>(pending_offset_);
            queued_.emplace_back(start, start + static_cast<std::ptrdiff_t>(count));
            pending_offset_ += count;
        }
        const auto& frames = queued_.back();
        XAUDIO2_BUFFER audio{};
        audio.AudioBytes = static_cast<UINT32>(frames.size() * sizeof(float));
        audio.pAudioData = reinterpret_cast<const BYTE*>(frames.data());
        const auto result = voice_->SubmitSourceBuffer(&audio);
        if (SUCCEEDED(result)) submitted_ += frames.size();
        return result;
    }

    HRESULT Spatialize(const AudioOutput::Frame& frame) {
        float gain = 0.0f;
        if (frame.source) {
            const auto [right, up, forward] = *frame.source;
            gain = frame.distance_range.Gain(std::hypot(right, up, forward));
            X3DAUDIO_LISTENER listener{};
            listener.OrientFront = {0, 0, 1};
            listener.OrientTop = {0, 1, 0};
            X3DAUDIO_DISTANCE_CURVE_POINT points[]{{0, 1}, {1, 1}};
            X3DAUDIO_DISTANCE_CURVE flat{points, 2};
            X3DAUDIO_EMITTER emitter{};
            emitter.Position = {right, up, forward};
            emitter.OrientFront = listener.OrientFront;
            emitter.OrientTop = listener.OrientTop;
            emitter.ChannelCount = 1;
            emitter.CurveDistanceScaler = frame.distance_range.max_distance_meters;
            emitter.pVolumeCurve = &flat;  // The explicit gain below owns distance attenuation.
            X3DAUDIO_DSP_SETTINGS dsp{};
            dsp.SrcChannelCount = 1;
            dsp.DstChannelCount = static_cast<UINT32>(matrix_.size());
            dsp.pMatrixCoefficients = matrix_.data();
            X3DAudioCalculate(spatial_, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dsp);
            if (const auto result = voice_->SetOutputMatrix(master_, 1, dsp.DstChannelCount, matrix_.data());
                FAILED(result)) return result;
        }
        if (started_) gain_ += (gain - gain_) * 0.35f;
        else gain_ = gain;
        if (gain_ < 0.001f) gain_ = 0.0f;
        return voice_->SetVolume(gain_);
    }

    ComPtr<IMFSourceReader> reader_;
    ComPtr<IXAudio2> engine_;
    IXAudio2MasteringVoice* master_{};
    IXAudio2SourceVoice* voice_{};
    X3DAUDIO_HANDLE spatial_{};
    std::vector<float> matrix_;
    std::deque<std::vector<float>> queued_;
    std::vector<float> pending_;
    std::size_t pending_offset_{};
    std::int64_t pending_pts_{}, first_frame_{};
    std::uint64_t submitted_{}, last_samples_{};
    UINT32 sample_rate_{}, channels_{};
    float gain_{};
    Clock::time_point last_advance_{};
    bool eof_{}, started_{};
};

class ExternalPlayer {
public:
    ~ExternalPlayer() {
        if (player_) static_cast<void>(player_->Shutdown());
    }

    HRESULT Open(const wchar_t* path) {
        HRESULT result = MFPCreateMediaPlayer(nullptr, FALSE, MFP_OPTION_FREE_THREADED_CALLBACK,
            nullptr, nullptr, &player_);
        if (FAILED(result)) return result;
        static_cast<void>(player_->SetVolume(1.0f));
        static_cast<void>(player_->SetMute(FALSE));
        ComPtr<IMFPMediaItem> item;
        result = player_->CreateMediaItemFromURL(path, TRUE, 0, &item);
        if (FAILED(result)) return result;
        BOOL audio{}, selected{};
        if (FAILED(result = item->HasAudio(&audio, &selected))) return result;
        if (!audio) return S_FALSE;
        DWORD streams{};
        result = item->GetNumberOfStreams(&streams);
        for (DWORD index = 0; SUCCEEDED(result) && index < streams; ++index) {
            PROPVARIANT type;
            PropVariantInit(&type);
            result = item->GetStreamAttribute(index, MF_MT_MAJOR_TYPE, &type);
            if (SUCCEEDED(result) && type.vt == VT_CLSID && type.puuid && *type.puuid == MFMediaType_Video)
                result = item->SetStreamSelection(index, FALSE);
            PropVariantClear(&type);
        }
        if (FAILED(result)) return result;
        return player_->SetMediaItem(item.Get());
    }

    HRESULT Update(std::int64_t target, bool playing, bool discontinuity,
                   std::int64_t& actual, bool& ready) {
        MFP_MEDIAPLAYER_STATE state{};
        HRESULT result = player_->GetState(&state);
        if (FAILED(result)) return result;
        ready = state != MFP_MEDIAPLAYER_STATE_EMPTY;
        if (!ready) return S_OK;
        if (!playing) return state == MFP_MEDIAPLAYER_STATE_PLAYING ? player_->Pause() : S_OK;

        PROPVARIANT position;
        PropVariantInit(&position);
        result = player_->GetPosition(MFP_POSITIONTYPE_100NS, &position);
        actual = SUCCEEDED(result) && position.vt == VT_I8 ? position.hVal.QuadPart : 0;
        PropVariantClear(&position);
        if (discontinuity || std::abs(target - actual) > 1500000) {
            InitPropVariantFromInt64(target, &position);
            result = player_->SetPosition(MFP_POSITIONTYPE_100NS, &position);
            PropVariantClear(&position);
            if (FAILED(result)) return result;
        }
        return state == MFP_MEDIAPLAYER_STATE_PLAYING ? S_OK : player_->Play();
    }

private:
    ComPtr<IMFPMediaPlayer> player_;
};

}  // namespace

struct AudioOutput::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping{}, finished{};
    AudioCommand command;
    std::uint64_t revision{};
    Snapshot snapshot;
    std::thread worker;

    void Publish(const Snapshot& value) noexcept {
        std::scoped_lock lock(mutex);
        snapshot = value;
    }

    bool Matches(const AudioCommand& next) const {
        return !stopping && command.playing && command.path == next.path &&
            command.frame.session == next.frame.session && command.frame.timeline == next.frame.timeline;
    }

    bool MayPlay(const AudioCommand& next) {
        std::scoped_lock lock(mutex);
        return Matches(next) && Clock::now() - command.sampled < kSampleLifetime;
    }

    bool Refresh(AudioCommand& next) {
        std::scoped_lock lock(mutex);
        if (!Matches(next) || Clock::now() - command.sampled >= kSampleLifetime) return false;
        next = command;
        return true;
    }

    void Play() {
        std::unique_ptr<SpatialVoice> spatial;
        std::unique_ptr<ExternalPlayer> external;
        std::wstring loaded;
        std::uint64_t session{}, timeline{}, consumed{};
        Clock::time_point last_pose{};
        Snapshot status;
        for (;;) {
            AudioCommand next;
            {
                std::unique_lock lock(mutex);
                const auto pending = [&] { return stopping || revision != consumed; };
                if (loaded.empty()) wake.wait(lock, pending);
                else wake.wait_for(lock, 10ms, pending);
                if (stopping) break;
                next = command;
                consumed = revision;
            }
            const bool changed = loaded != next.path || session != next.frame.session;
            if (changed || next.path.empty()) {
                spatial.reset();
                external.reset();
                loaded.clear();
                status = {};
                Publish(status);
            }
            const auto expires = next.sampled + kSampleLifetime;
            const auto now = Clock::now();
            const bool playing = next.playing && now < expires;
            if (next.path.empty()) continue;

            if (loaded.empty()) {
                if (!playing) continue;
                loaded = next.path;
                session = next.frame.session;
                status.state = State::Opening;
                Publish(status);
                if (next.frame.source) {
                    spatial = std::make_unique<SpatialVoice>();
                    const auto result = spatial->Open(loaded.c_str());
                    if (result == S_OK) {
                        status.route = Route::Spatial;
                        last_pose = now;
                    } else {
                        spatial.reset();
                        if (result == S_FALSE) status.state = State::NoAudio;
                        else status.spatial_error = static_cast<std::uint32_t>(result);
                    }
                }
            }
            if (status.state == State::NoAudio || status.state == State::Failed) {
                Publish(status);
                continue;
            }
            const bool discontinuity = timeline != next.frame.timeline;
            if (spatial) {
                if (next.frame.source) last_pose = next.sampled;
                HRESULT result = S_OK;
                if (!playing) {
                    spatial->Pause();
                } else if (now - last_pose > kSampleLifetime) {
                    result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
                } else if (Refresh(next)) {
                    const auto target = next.ticks + Ticks(Clock::now() - next.sampled);
                    result = spatial->Update(target, next.frame, discontinuity,
                        [&] { return MayPlay(next); }, status.position_ticks);
                    if (result == S_FALSE) {
                        spatial->Pause();
                        continue;
                    }
                } else {
                    spatial->Pause();
                    continue;
                }
                if (SUCCEEDED(result)) {
                    status.state = playing ? State::Playing : State::Paused;
                    timeline = next.frame.timeline;
                    Publish(status);
                    continue;
                }
                spatial.reset();  // Destroy the source voice before MFPlay can start.
                status.spatial_error = static_cast<std::uint32_t>(result);
                status.route = Route::None;
            }
            if (!external) {
                if (!playing) continue;
                external = std::make_unique<ExternalPlayer>();
                const auto result = external->Open(loaded.c_str());
                if (result != S_OK) {
                    external.reset();
                    status.state = result == S_FALSE ? State::NoAudio : State::Failed;
                    status.error = FAILED(result) ? static_cast<std::uint32_t>(result) : 0;
                    Publish(status);
                    continue;
                }
                status.route = Route::External;
                status.position_ticks = 0;
            }
            bool ready{};
            const bool current = playing && Refresh(next);
            const auto target = next.ticks + (current ? Ticks(Clock::now() - next.sampled) : 0);
            const auto result = external->Update(target, current, discontinuity, status.position_ticks, ready);
            if (FAILED(result)) {
                external.reset();
                status.state = State::Failed;
                status.error = static_cast<std::uint32_t>(result);
            } else if (ready) {
                status.state = current ? State::Playing : State::Paused;
                if (current) timeline = next.frame.timeline;
            } else {
                status.state = State::Opening;
            }
            Publish(status);
        }
        external.reset();
        spatial.reset();
        Publish({});
    }

    void Run() noexcept {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT startup = SUCCEEDED(apartment) ? MFStartup(MF_VERSION) : apartment;
        if (SUCCEEDED(startup)) {
            try {
                Play();
            } catch (...) {
                Publish({State::Failed, static_cast<std::uint32_t>(E_FAIL)});
            }
            MFShutdown();
        } else {
            Publish({State::Failed, static_cast<std::uint32_t>(startup)});
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
        {
            std::scoped_lock lock(mutex);
            finished = true;
        }
        wake.notify_all();
    }
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() {
    if (impl_->worker.joinable()) {
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->stopping = true;
        }
        impl_->wake.notify_all();
        impl_->worker.join();
    }
}

void AudioOutput::Transport(std::wstring path, std::int64_t position_ticks, bool playing) {
    Transport(std::move(path), position_ticks, playing, {});
}

void AudioOutput::Transport(std::wstring path, std::int64_t position_ticks, bool playing, Frame frame) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->stopping || impl_->finished) return;
    if (!impl_->worker.joinable()) {
        if (path.empty() || !playing) return;
        impl_->worker = std::thread([this] { impl_->Run(); });
    }
    impl_->command = {std::move(path), position_ticks, playing, frame, Clock::now()};
    ++impl_->revision;
    impl_->wake.notify_one();
}

bool AudioOutput::Shutdown(std::chrono::milliseconds timeout) noexcept {
    {
        std::unique_lock lock(impl_->mutex);
        impl_->stopping = true;
        if (!impl_->worker.joinable()) return true;
        impl_->wake.notify_all();
        if (!impl_->wake.wait_for(lock, timeout, [this] { return impl_->finished; })) return false;
    }
    impl_->worker.join();
    return true;
}

AudioOutput::Snapshot AudioOutput::Read() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->snapshot;
}
}  // namespace better_tv_video
