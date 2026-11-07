#include "audio_output.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mfplay.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace nte_tv_video {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

struct AudioOutput::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping{};
    std::wstring path;
    std::int64_t target_ticks{};
    bool playing{};
    Clock::time_point sampled{};
    std::uint64_t revision{};
    std::atomic<State> state{State::Idle};
    std::atomic<std::uint32_t> error{};
    std::atomic<std::int64_t> position{};
    std::thread worker;

    void Fail(HRESULT result) noexcept {
        error.store(static_cast<std::uint32_t>(result));
        state.store(State::Failed);
    }

    void Run() noexcept {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment)) { Fail(apartment); return; }
        const HRESULT startup = MFStartup(MF_VERSION);
        if (FAILED(startup)) { Fail(startup); CoUninitialize(); return; }
        try {
            ComPtr<IMFPMediaPlayer> player;
            std::wstring loaded;
            std::uint64_t consumed{};
            bool failed_file{};
            for (;;) {
                std::wstring requested;
                std::int64_t target{};
                bool should_play{};
                Clock::time_point timestamp;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(20), [&] {
                        return stopping || revision != consumed;
                    });
                    if (stopping) break;
                    requested = path;
                    target = target_ticks;
                    should_play = playing;
                    timestamp = sampled;
                    consumed = revision;
                }
                if (requested.empty()) {
                    if (player && !loaded.empty()) static_cast<void>(player->ClearMediaItem());
                    loaded.clear();
                    failed_file = false;
                    position.store(0);
                    state.store(State::Idle);
                    continue;
                }
                if (!player) {
                    const HRESULT result = MFPCreateMediaPlayer(nullptr, FALSE,
                        MFP_OPTION_FREE_THREADED_CALLBACK, nullptr, nullptr, &player);
                    if (FAILED(result)) { Fail(result); break; }
                    static_cast<void>(player->SetVolume(1.0f));
                    static_cast<void>(player->SetMute(FALSE));
                }
                if (loaded != requested) {
                    loaded = requested;
                    failed_file = false;
                    error.store(0);
                    state.store(State::Opening);
                    ComPtr<IMFPMediaItem> item;
                    HRESULT result = player->CreateMediaItemFromURL(loaded.c_str(), TRUE, 0, &item);
                    if (FAILED(result)) { failed_file = true; Fail(result); continue; }
                    BOOL audio{}, selected{};
                    result = item->HasAudio(&audio, &selected);
                    if (FAILED(result)) { failed_file = true; Fail(result); continue; }
                    if (!audio) { failed_file = true; state.store(State::NoAudio); continue; }
                    DWORD streams{};
                    result = item->GetNumberOfStreams(&streams);
                    for (DWORD index = 0; SUCCEEDED(result) && index < streams; ++index) {
                        PROPVARIANT type;
                        PropVariantInit(&type);
                        result = item->GetStreamAttribute(index, MF_MT_MAJOR_TYPE, &type);
                        if (SUCCEEDED(result) && type.vt == VT_CLSID && type.puuid &&
                            *type.puuid == MFMediaType_Video)
                            result = item->SetStreamSelection(index, FALSE);
                        PropVariantClear(&type);
                    }
                    if (SUCCEEDED(result)) result = player->SetMediaItem(item.Get());
                    if (FAILED(result)) { failed_file = true; Fail(result); continue; }
                }
                if (failed_file) continue;
                MFP_MEDIAPLAYER_STATE transport{};
                HRESULT result = player->GetState(&transport);
                if (FAILED(result)) { Fail(result); continue; }
                // An unpaused video renews this sample on Game ticks. A stalled
                // game must not leave the independently rendered audio running.
                const auto age = Clock::now() - timestamp;
                should_play = should_play && age < std::chrono::milliseconds(500);
                if (!should_play) {
                    if (transport == MFP_MEDIAPLAYER_STATE_PLAYING) static_cast<void>(player->Pause());
                    state.store(State::Paused);
                    continue;
                }
                if (transport == MFP_MEDIAPLAYER_STATE_EMPTY) continue;
                target += std::chrono::duration_cast<std::chrono::nanoseconds>(age).count() / 100;
                PROPVARIANT actual;
                PropVariantInit(&actual);
                result = player->GetPosition(MFP_POSITIONTYPE_100NS, &actual);
                const std::int64_t current = SUCCEEDED(result) && actual.vt == VT_I8 ? actual.hVal.QuadPart : 0;
                PropVariantClear(&actual);
                position.store(current);
                if (std::abs(target - current) > 1500000) {
                    PROPVARIANT seek;
                    InitPropVariantFromInt64(target, &seek);
                    result = player->SetPosition(MFP_POSITIONTYPE_100NS, &seek);
                    PropVariantClear(&seek);
                    if (FAILED(result)) { Fail(result); continue; }
                }
                if (transport != MFP_MEDIAPLAYER_STATE_PLAYING) {
                    result = player->Play();
                    if (FAILED(result)) { Fail(result); continue; }
                }
                state.store(State::Playing);
            }
            if (player) {
                static_cast<void>(player->Shutdown());
                player.Reset();
            }
        } catch (...) {
            Fail(E_FAIL);
        }
        MFShutdown();
        CoUninitialize();
    }
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() { Shutdown(); }
void AudioOutput::Transport(std::wstring path, std::int64_t position_ticks, bool playing) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->stopping) return;
    if (!impl_->worker.joinable()) {
        if (path.empty() || !playing) return;
        impl_->worker = std::thread([this] { impl_->Run(); });
    }
    impl_->path = std::move(path);
    impl_->target_ticks = position_ticks;
    impl_->playing = playing;
    impl_->sampled = Clock::now();
    ++impl_->revision;
    impl_->wake.notify_one();
}
void AudioOutput::Shutdown() noexcept {
    if (!impl_ || !impl_->worker.joinable()) return;
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->stopping = true;
        impl_->wake.notify_one();
    }
    impl_->worker.join();
}
AudioOutput::Snapshot AudioOutput::Read() const noexcept {
    return {impl_->state.load(), impl_->error.load(), impl_->position.load()};
}
}  // namespace nte_tv_video
