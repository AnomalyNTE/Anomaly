#include "plugins/HiFiVehicleMusic/audio_engine.hpp"

#include <Windows.h>
#include <objbase.h>

#include "miniaudio.h"
#include "RtAudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hifi_vehicle_music {
namespace {

std::string Utf8(const std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr);
    }
    return result;
}

bool IsTrackExtension(std::wstring extension) {
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return extension == L".flac" || extension == L".wav" || extension == L".mp3";
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

// cover.jpg, folder.png, ... in the library root: the usual album art names.
bool IsCoverFile(const std::filesystem::path& path) {
    const std::wstring stem = Lower(path.stem().wstring());
    const std::wstring extension = Lower(path.extension().wstring());
    return (stem == L"cover" || stem == L"folder" || stem == L"front" || stem == L"album") &&
        (extension == L".png" || extension == L".jpg" || extension == L".jpeg" ||
            extension == L".bmp");
}

const char* BackendName(const Backend backend) noexcept {
    switch (backend) {
    case Backend::WasapiExclusive: return "WASAPI exclusive";
    case Backend::WasapiShared: return "WASAPI shared";
    case Backend::DirectSound: return "DirectSound";
    case Backend::Asio: return "ASIO";
    }
    return "unknown";
}

const char* FormatName(const ma_format format) noexcept {
    switch (format) {
    case ma_format_u8: return "8-bit";
    case ma_format_s16: return "16-bit";
    case ma_format_s24: return "24-bit";
    case ma_format_s32: return "32-bit";
    case ma_format_f32: return "32-bit float";
    default: return "unknown format";
    }
}

struct Command final {
    enum class Kind { Configure, Rescan, Play, Stop, StopNow, Previous, Next, Pause, Resume, Seek, Quit } kind{};
    EngineSettings settings;
    std::string key;
    float position{};
    std::optional<std::size_t> index;
};

} // namespace

struct AudioEngine::Impl final {
    // Command queue (any thread -> worker).
    std::mutex queue_mutex;
    std::condition_variable queue_signal;
    std::deque<Command> queue;
    std::thread worker;

    // Published state (worker -> hook/render threads).
    mutable std::mutex state_mutex;
    std::unordered_map<std::string, std::wstring> tracks;
    EngineSnapshot snapshot;
    bool output_failed{};
    std::vector<std::wstring> titles;           // playlist stems, playlist order
    std::wstring cover;                         // album art in the library root
    std::atomic<std::uint64_t> library_generation{0};
    std::optional<std::size_t> playing_slot;    // album song now playing
    std::optional<std::size_t> requested_slot;  // worker-owned: album song the game last picked

    // Playlist (worker-owned): every library file in path order. Events
    // without a same-named file play from here and advance on track end.
    std::vector<std::wstring> playlist;
    std::size_t cursor{};
    bool playlist_mode{};
    std::atomic<bool> track_ended{};
    static constexpr std::chrono::milliseconds kStopGrace{1500};
    std::optional<std::chrono::steady_clock::time_point> stop_deadline;

    // Shared with the realtime callback. The worker swaps the decoder while
    // holding audio_mutex; the callback only try_locks and outputs silence
    // when it cannot get the decoder immediately.
    std::mutex audio_mutex;
    ma_decoder* decoder{};
    std::atomic<float> volume{1.0F};
    std::atomic<bool> paused{};
    // Current decoder: frames played since its start, its length and rate.
    std::atomic<std::uint64_t> position_frames{};
    std::atomic<std::uint64_t> length_frames{};
    std::atomic<std::uint32_t> decoder_rate{};

    // Worker-owned.
    EngineSettings settings;
    std::optional<ma_context> context;
    std::optional<ma_device> device;
    ma_uint32 device_channels{};
    ma_uint32 device_rate{};
    std::unique_ptr<RtAudio> asio;
    unsigned int asio_channels{};
    unsigned int asio_rate{};

    void Post(Command command) {
        {
            std::scoped_lock lock(queue_mutex);
            queue.push_back(std::move(command));
        }
        queue_signal.notify_one();
    }

    void SetStatus(const EngineStatus status, std::string detail = {}) {
        std::scoped_lock lock(state_mutex);
        snapshot.status = status;
        snapshot.detail = std::move(detail);
    }

    void Run() {
        // RtAudio's ASIO host requires a single-threaded apartment on the
        // thread that owns it. miniaudio tolerates an already-initialized
        // apartment (RPC_E_CHANGED_MODE) and runs its device threads itself.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            Command command;
            {
                // The realtime callback only sets track_ended (it never locks
                // here), so the timeout bounds a missed wakeup.
                std::unique_lock lock(queue_mutex);
                queue_signal.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !queue.empty() || track_ended.load(std::memory_order_relaxed);
                });
                if (queue.empty()) {
                    lock.unlock();
                    try {
                        if (stop_deadline && std::chrono::steady_clock::now() >= *stop_deadline) {
                            StopOutput();
                        } else if (track_ended.exchange(false, std::memory_order_relaxed) && playlist_mode) {
                            PlayNext();
                        }
                    } catch (...) {
                        SetStatus(EngineStatus::EngineError);
                    }
                    continue;
                }
                command = std::move(queue.front());
                queue.pop_front();
            }
            try {
                if (command.kind == Command::Kind::Quit) break;
                Handle(command);
            } catch (const std::exception& error) {
                SetStatus(EngineStatus::EngineError, error.what());
            } catch (...) {
                SetStatus(EngineStatus::EngineError);
            }
        }
        CloseOutput();
        ReleaseDecoder();
        CloseContext();
        if (SUCCEEDED(com)) CoUninitialize();
    }

    void Handle(const Command& command) {
        switch (command.kind) {
        case Command::Kind::Configure: {
            const bool folder_changed = command.settings.folder != settings.folder;
            const bool output_changed = command.settings.backend != settings.backend ||
                command.settings.device != settings.device ||
                command.settings.buffer_ms != settings.buffer_ms || !context && !asio;
            settings = command.settings;
            volume.store(settings.volume, std::memory_order_relaxed);
            if (output_changed) {
                CloseOutput();
                ReleaseDecoder();
                CloseContext();
                OpenContext();
                {
                    std::scoped_lock lock(state_mutex);
                    output_failed = false;
                    snapshot.playing = false;
                    snapshot.track.clear();
                }
            }
            if (folder_changed) Scan();
            break;
        }
        case Command::Kind::Rescan:
            Scan();
            EnumerateDevices();
            break;
        case Command::Kind::Play:
            PlayTrack(command.key, command.position, command.index);
            break;
        case Command::Kind::Stop:
            // The game may stop its music right before posting the next
            // track; a playlist song only stops if no Play follows quickly.
            if (playlist_mode && decoder != nullptr) {
                if (!stop_deadline) stop_deadline = std::chrono::steady_clock::now() + kStopGrace;
            } else {
                StopOutput();
            }
            break;
        case Command::Kind::StopNow:
            StopOutput();
            break;
        case Command::Kind::Previous:
            // cursor already points past the current song.
            if (decoder != nullptr && playlist_mode && !playlist.empty()) {
                stop_deadline.reset();
                cursor = (cursor % playlist.size() + playlist.size() * 2 - 2) % playlist.size();
                PlayNext();
            }
            break;
        case Command::Kind::Next:
            if (decoder != nullptr) {
                stop_deadline.reset();
                PlayNext();
            }
            break;
        case Command::Kind::Pause:
            paused.store(true, std::memory_order_relaxed);
            break;
        case Command::Kind::Resume:
            paused.store(false, std::memory_order_relaxed);
            break;
        case Command::Kind::Seek: {
            std::scoped_lock lock(audio_mutex);
            const std::uint64_t length = length_frames.load(std::memory_order_relaxed);
            if (decoder != nullptr && length > 0) {
                const float fraction = std::clamp(command.position, 0.0F, 1.0F);
                const auto frame = std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(static_cast<double>(length) * fraction), length - 1);
                if (ma_decoder_seek_to_pcm_frame(decoder, frame) == MA_SUCCESS) {
                    position_frames.store(frame, std::memory_order_relaxed);
                    track_ended.store(false, std::memory_order_relaxed);
                }
            }
            break;
        }
        case Command::Kind::Quit:
            break;
        }
    }

    void Scan() {
        std::unordered_map<std::string, std::wstring> found;
        std::vector<std::wstring> list;
        std::wstring art;
        std::error_code error;
        if (!settings.folder.empty()) {
            std::error_code art_error;
            for (std::filesystem::directory_iterator it(settings.folder, art_error), end;
                 !art_error && it != end; it.increment(art_error)) {
                if (it->is_regular_file(art_error) && IsCoverFile(it->path())) {
                    art = it->path().wstring();
                    break;
                }
            }
        }
        if (!settings.folder.empty()) {
            for (std::filesystem::recursive_directory_iterator it(
                     settings.folder, std::filesystem::directory_options::skip_permission_denied, error),
                 end;
                 !error && it != end; it.increment(error)) {
                if (!it->is_regular_file(error) || !IsTrackExtension(it->path().extension().wstring())) {
                    continue;
                }
                std::string key = Utf8(it->path().stem().wstring());
                std::transform(key.begin(), key.end(), key.begin(),
                    [](const char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
                found.emplace(std::move(key), it->path().wstring());
                list.push_back(it->path().wstring());
            }
        }
        std::sort(list.begin(), list.end());
        std::vector<std::wstring> stems;
        stems.reserve(list.size());
        for (const std::wstring& path : list) stems.push_back(std::filesystem::path(path).stem().wstring());
        playlist = std::move(list);
        cursor = 0;
        requested_slot.reset();
        std::scoped_lock lock(state_mutex);
        titles = std::move(stems);
        cover = std::move(art);
        tracks = std::move(found);
        snapshot.track_count = titles.size();
        library_generation.fetch_add(1, std::memory_order_acq_rel);
        if (error) {
            snapshot.status = EngineStatus::FolderUnavailable;
            snapshot.detail.clear();
        }
    }

    // ---- output lifetime -------------------------------------------------

    void OpenContext() {
        if (settings.backend == Backend::Asio) {
            asio = std::make_unique<RtAudio>(RtAudio::WINDOWS_ASIO,
                [this](RtAudioErrorType, const std::string& message) { SetStatus(EngineStatus::AsioError, message); });
        } else {
            const ma_backend backend =
                settings.backend == Backend::DirectSound ? ma_backend_dsound : ma_backend_wasapi;
            ma_context_config config = ma_context_config_init();
            context.emplace();
            if (ma_context_init(&backend, 1, &config, &*context) != MA_SUCCESS) {
                context.reset();
                std::scoped_lock lock(state_mutex);
                output_failed = true;
                snapshot.status = EngineStatus::BackendUnavailable;
                snapshot.detail = BackendName(settings.backend);
                return;
            }
        }
        EnumerateDevices();
        SetStatus(EngineStatus::Ready, BackendName(settings.backend));
    }

    void CloseContext() {
        asio.reset();
        if (context) {
            ma_context_uninit(&*context);
            context.reset();
        }
    }

    void EnumerateDevices() {
        std::vector<std::string> names;
        if (asio) {
            for (const unsigned int id : asio->getDeviceIds()) {
                const RtAudio::DeviceInfo info = asio->getDeviceInfo(id);
                if (info.outputChannels > 0) names.push_back(info.name);
            }
        } else if (context) {
            ma_device_info* playback{};
            ma_uint32 count{};
            if (ma_context_get_devices(&*context, &playback, &count, nullptr, nullptr) == MA_SUCCESS) {
                for (ma_uint32 i = 0; i < count; ++i) names.emplace_back(playback[i].name);
            }
        }
        std::scoped_lock lock(state_mutex);
        snapshot.devices = std::move(names);
    }

    void CloseOutput() {
        if (device) {
            ma_device_uninit(&*device);
            device.reset();
        }
        if (asio && asio->isStreamOpen()) asio->closeStream();
        device_channels = device_rate = 0;
        asio_channels = asio_rate = 0;
    }

    void ReleaseDecoder() {
        ma_decoder* old{};
        {
            std::scoped_lock lock(audio_mutex);
            old = std::exchange(decoder, nullptr);
            position_frames.store(0, std::memory_order_relaxed);
            length_frames.store(0, std::memory_order_relaxed);
            decoder_rate.store(0, std::memory_order_relaxed);
        }
        if (old != nullptr) {
            ma_decoder_uninit(old);
            delete old;
        }
    }

    void FailOutput(const EngineStatus status) {
        CloseOutput();
        ReleaseDecoder();
        std::scoped_lock lock(state_mutex);
        output_failed = true;
        snapshot.playing = false;
        snapshot.status = status;
        snapshot.detail = BackendName(settings.backend);
    }

    // ---- realtime callbacks ----------------------------------------------

    void Render(float* output, const ma_uint32 frames, const ma_uint32 channels) noexcept {
        std::fill_n(output, static_cast<std::size_t>(frames) * channels, 0.0F);
        if (paused.load(std::memory_order_relaxed)) return;
        std::unique_lock lock(audio_mutex, std::try_to_lock);
        if (!lock.owns_lock() || decoder == nullptr) return;
        ma_uint64 read{};
        ma_data_source_read_pcm_frames(decoder, output, frames, &read);
        position_frames.fetch_add(read, std::memory_order_relaxed);
        if (read < frames) track_ended.store(true, std::memory_order_relaxed);
        const float gain = volume.load(std::memory_order_relaxed);
        if (gain != 1.0F) {
            const std::size_t samples = static_cast<std::size_t>(read) * channels;
            for (std::size_t i = 0; i < samples; ++i) output[i] *= gain;
        }
    }

    static void MiniaudioCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto* const self = static_cast<Impl*>(device->pUserData);
        self->Render(static_cast<float*>(output), frames, device->playback.channels);
    }

    static int AsioCallback(void* output, void*, unsigned int frames, double, RtAudioStreamStatus, void* user) {
        auto* const self = static_cast<Impl*>(user);
        self->Render(static_cast<float*>(output), frames, self->asio_channels);
        return 0;
    }

    // ---- playback ---------------------------------------------------------

    std::unique_ptr<ma_decoder> OpenDecoder(
        const std::wstring& path, const ma_uint32 channels, const ma_uint32 rate) {
        auto result = std::make_unique<ma_decoder>();
        const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, channels, rate);
        if (ma_decoder_init_file_w(path.c_str(), &config, result.get()) != MA_SUCCESS) return {};
        return result;
    }

    bool EnsureMiniaudioDevice(const ma_uint32 channels, const ma_uint32 rate) {
        if (device && device_channels == channels && device_rate == rate) return true;
        CloseOutput();
        if (!context) return false;

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        std::optional<ma_device_id> id;
        if (!settings.device.empty()) {
            ma_device_info* playback{};
            ma_uint32 count{};
            if (ma_context_get_devices(&*context, &playback, &count, nullptr, nullptr) == MA_SUCCESS) {
                for (ma_uint32 i = 0; i < count; ++i) {
                    if (settings.device == playback[i].name) id = playback[i].id;
                }
            }
            if (!id) return false;
        }
        config.playback.pDeviceID = id ? &*id : nullptr;
        config.playback.format = ma_format_f32;
        config.playback.channels = channels;
        config.sampleRate = rate;
        config.playback.shareMode = settings.backend == Backend::WasapiExclusive
            ? ma_share_mode_exclusive : ma_share_mode_shared;
        config.periodSizeInMilliseconds = settings.buffer_ms;
        config.dataCallback = &Impl::MiniaudioCallback;
        config.pUserData = this;
        config.wasapi.noAutoConvertSRC = MA_TRUE;
        config.wasapi.noDefaultQualitySRC = MA_TRUE;

        device.emplace();
        if (ma_device_init(&*context, &config, &*device) != MA_SUCCESS) {
            device.reset();
            return false;
        }
        device_channels = channels;
        device_rate = rate;
        return true;
    }

    std::optional<std::pair<unsigned int, RtAudio::DeviceInfo>> AsioDevice() {
        for (const unsigned int id : asio->getDeviceIds()) {
            RtAudio::DeviceInfo info = asio->getDeviceInfo(id);
            if (info.outputChannels == 0) continue;
            if (settings.device.empty() || settings.device == info.name) {
                return std::make_pair(id, std::move(info));
            }
        }
        return std::nullopt;
    }

    bool EnsureAsioStream(const ma_uint32 file_channels, const ma_uint32 file_rate) {
        if (!asio) return false;
        const auto selected = AsioDevice();
        if (!selected) return false;
        const RtAudio::DeviceInfo& info = selected->second;
        const unsigned int channels = (std::min)(file_channels, info.outputChannels);
        // Prefer the file rate so the driver receives the samples unchanged.
        const bool native = std::find(info.sampleRates.begin(), info.sampleRates.end(), file_rate) !=
            info.sampleRates.end();
        const unsigned int rate = native ? file_rate
            : info.preferredSampleRate != 0 ? info.preferredSampleRate : info.currentSampleRate;
        if (asio->isStreamOpen() && asio_channels == channels && asio_rate == rate) return true;
        CloseOutput();

        RtAudio::StreamParameters parameters;
        parameters.deviceId = selected->first;
        parameters.nChannels = channels;
        unsigned int frames = (std::max)(16U, rate * settings.buffer_ms / 1000U);
        RtAudio::StreamOptions options;
        options.flags = RTAUDIO_SCHEDULE_REALTIME;
        asio_channels = channels;
        asio_rate = rate;
        if (asio->openStream(&parameters, nullptr, RTAUDIO_FLOAT32, rate, &frames,
                &Impl::AsioCallback, this, &options) != RTAUDIO_NO_ERROR) {
            asio_channels = asio_rate = 0;
            return false;
        }
        return true;
    }

    // A file named after the event loops and honours the game's seek;
    // otherwise the event starts the next playlist track.
    void PlayTrack(const std::string& key, const float position, const std::optional<std::size_t> index) {
        std::wstring path;
        std::optional<std::size_t> current_slot;
        {
            std::scoped_lock lock(state_mutex);
            if (output_failed) return;
            const auto found = tracks.find(key);
            if (found != tracks.end()) path = found->second;
            current_slot = playing_slot;
        }
        stop_deadline.reset();
        if (path.empty() && index && !playlist.empty()) {
            // The album song picked in the game's music list. Re-posting the
            // same song (station/track changes, re-entering the vehicle
            // quickly) keeps it playing.
            // After the engine moved on by itself the game still names the song
            // it last asked for; that re-post must not restart it either.
            if (playlist_mode && decoder != nullptr &&
                (current_slot == index || requested_slot == index)) {
                paused.store(false, std::memory_order_relaxed);
                return;
            }
            requested_slot = index;
            cursor = *index % playlist.size();
            PlayNext();
            return;
        }
        if (path.empty()) {
            // The playlist replaces the game's radio: a station or track change
            // in the game keeps the current song playing.
            if (playlist_mode && decoder != nullptr) {
                paused.store(false, std::memory_order_relaxed);
                return;
            }
            PlayNext();
            return;
        }
        playlist_mode = false;
        PlayFile(path, position, true);
        std::scoped_lock lock(state_mutex);
        playing_slot.reset();
    }

    void StopOutput() {
        // Closing the output releases an exclusive device back to the
        // system while the vehicle music is not playing.
        stop_deadline.reset();
        CloseOutput();
        ReleaseDecoder();
        playlist_mode = false;
        requested_slot.reset();
        track_ended.store(false, std::memory_order_relaxed);
        std::scoped_lock lock(state_mutex);
        snapshot.playing = false;
        snapshot.track.clear();
        snapshot.output.clear();
        playing_slot.reset();
    }

    void PlayNext() {
        playlist_mode = true;
        // Skip files that fail to decode, at most one pass over the list.
        for (std::size_t attempt = 0; attempt < playlist.size(); ++attempt) {
            const std::size_t slot = cursor++ % playlist.size();
            if (PlayFile(playlist[slot], 0.0F, false)) {
                // The album song now playing, so the game re-posting it (or
                // picking it) keeps it instead of restarting it.
                std::scoped_lock lock(state_mutex);
                if (snapshot.playing) playing_slot = slot;
                return;
            }
        }
    }

    // Returns false only when the file cannot be decoded; output failures are
    // terminal for this play request.
    bool PlayFile(const std::wstring& path, const float position, const bool loop) {
        auto next = OpenDecoder(path, 0, 0);
        if (!next) {
            SetStatus(EngineStatus::DecodeFailed, Utf8(path));
            return false;
        }
        ma_format format{};
        ma_uint32 channels{};
        ma_uint32 rate{};
        ma_decoder_get_data_format(next.get(), &format, &channels, &rate, nullptr, 0);

        std::string output;
        bool resampled{};
        if (settings.backend == Backend::Asio) {
            if (!EnsureAsioStream(channels, rate)) {
                FailOutput(EngineStatus::OpenFailed);
                return true;
            }
            if (asio_channels != channels || asio_rate != rate) {
                ma_decoder_uninit(next.get());
                next = OpenDecoder(path, asio_channels, asio_rate);
                if (!next) {
                    SetStatus(EngineStatus::DecodeFailed, Utf8(path));
                    return false;
                }
            }
            output = "ASIO " + std::to_string(asio_rate) + " Hz, " + std::to_string(asio_channels) + " ch";
            resampled = asio_rate != rate;
        } else {
            if (!EnsureMiniaudioDevice(channels, rate)) {
                FailOutput(EngineStatus::OpenFailed);
                return true;
            }
            const auto& playback = device->playback;
            output = std::string(BackendName(settings.backend)) + " " +
                std::to_string(playback.internalSampleRate) + " Hz, " + FormatName(playback.internalFormat) +
                ", " + std::to_string(playback.internalChannels) + " ch";
            resampled = playback.internalSampleRate != rate;
        }

        // The game seeks Wwise with a percentage; map it onto the file length.
        ma_data_source_set_looping(next.get(), loop ? MA_TRUE : MA_FALSE);
        ma_uint64 length{};
        if (ma_decoder_get_length_in_pcm_frames(next.get(), &length) != MA_SUCCESS) length = 0;
        ma_uint64 start{};
        if (position > 0.0F && position < 1.0F && length > 0) {
            start = static_cast<ma_uint64>(static_cast<double>(length) * position);
            ma_decoder_seek_to_pcm_frame(next.get(), start);
        }
        ma_uint32 decoded_rate{};
        ma_decoder_get_data_format(next.get(), nullptr, nullptr, &decoded_rate, nullptr, 0);

        ma_decoder* old{};
        {
            std::scoped_lock lock(audio_mutex);
            position_frames.store(start, std::memory_order_relaxed);
            length_frames.store(length, std::memory_order_relaxed);
            decoder_rate.store(decoded_rate, std::memory_order_relaxed);
            old = std::exchange(decoder, next.release());
            // Cleared under the lock so the previous track cannot re-raise it.
            track_ended.store(false, std::memory_order_relaxed);
        }
        if (old != nullptr) {
            ma_decoder_uninit(old);
            delete old;
        }
        paused.store(false, std::memory_order_relaxed);

        const bool started = settings.backend == Backend::Asio
            ? asio->isStreamRunning() || asio->startStream() == RTAUDIO_NO_ERROR
            : ma_device_get_state(&*device) == ma_device_state_started || ma_device_start(&*device) == MA_SUCCESS;
        if (!started) {
            FailOutput(EngineStatus::StartFailed);
            return true;
        }
        std::scoped_lock lock(state_mutex);
        snapshot.playing = true;
        snapshot.track = Utf8(std::filesystem::path(path).filename().wstring());
        snapshot.output = std::move(output);
        snapshot.resampled = resampled;
        snapshot.status = EngineStatus::Playing;
        snapshot.detail.clear();
        return true;
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() { Shutdown(); }

bool AudioEngine::Start(EngineSettings settings) {
    if (impl_->worker.joinable()) return true;
    impl_->worker = std::thread([impl = impl_.get()] { impl->Run(); });
    Command command{Command::Kind::Configure};
    command.settings = std::move(settings);
    impl_->Post(std::move(command));
    return true;
}

void AudioEngine::Shutdown() {
    if (!impl_ || !impl_->worker.joinable()) return;
    impl_->Post(Command{Command::Kind::Quit});
    impl_->worker.join();
}

void AudioEngine::Configure(EngineSettings settings) {
    Command command{Command::Kind::Configure};
    command.settings = std::move(settings);
    impl_->Post(std::move(command));
}

void AudioEngine::Rescan() { impl_->Post(Command{Command::Kind::Rescan}); }

void AudioEngine::SetVolume(const float volume) noexcept {
    impl_->volume.store(volume, std::memory_order_relaxed);
}

bool AudioEngine::CanPlay(const std::string_view key) const {
    std::scoped_lock lock(impl_->state_mutex);
    // Any non-empty library can serve an event through the playlist.
    static_cast<void>(key);
    return !impl_->output_failed && !impl_->tracks.empty();
}

void AudioEngine::Play(std::string key, const float position, const std::optional<std::size_t> index) {
    Command command{Command::Kind::Play};
    command.key = std::move(key);
    command.position = position;
    command.index = index;
    impl_->Post(std::move(command));
}

void AudioEngine::Stop() { impl_->Post(Command{Command::Kind::Stop}); }
void AudioEngine::StopNow() { impl_->Post(Command{Command::Kind::StopNow}); }
void AudioEngine::Previous() { impl_->Post(Command{Command::Kind::Previous}); }
void AudioEngine::Next() { impl_->Post(Command{Command::Kind::Next}); }
void AudioEngine::Pause() { impl_->Post(Command{Command::Kind::Pause}); }
void AudioEngine::Resume() { impl_->Post(Command{Command::Kind::Resume}); }

void AudioEngine::Seek(const float fraction) {
    Command command{Command::Kind::Seek};
    command.position = fraction;
    impl_->Post(std::move(command));
}

AudioEngine::Progress AudioEngine::Position() const noexcept {
    Progress progress;
    const std::uint32_t rate = impl_->decoder_rate.load(std::memory_order_relaxed);
    const std::uint64_t length = impl_->length_frames.load(std::memory_order_relaxed);
    if (rate == 0 || length == 0) return progress;
    // A looping file wraps around; the counter keeps growing.
    const std::uint64_t played = impl_->position_frames.load(std::memory_order_relaxed) % length;
    progress.seconds = static_cast<float>(static_cast<double>(played) / rate);
    progress.duration = static_cast<float>(static_cast<double>(length) / rate);
    return progress;
}

EngineSnapshot AudioEngine::Snapshot() const {
    std::scoped_lock lock(impl_->state_mutex);
    EngineSnapshot snapshot = impl_->snapshot;
    snapshot.paused = impl_->paused.load(std::memory_order_relaxed);
    return snapshot;
}

std::optional<std::size_t> AudioEngine::PlayingSlot() const {
    std::scoped_lock lock(impl_->state_mutex);
    return impl_->snapshot.playing ? impl_->playing_slot : std::nullopt;
}

std::vector<std::wstring> AudioEngine::Titles() const {
    std::scoped_lock lock(impl_->state_mutex);
    return impl_->titles;
}

std::wstring AudioEngine::Cover() const {
    std::scoped_lock lock(impl_->state_mutex);
    return impl_->cover;
}

std::uint64_t AudioEngine::LibraryGeneration() const noexcept {
    return impl_->library_generation.load(std::memory_order_acquire);
}

} // namespace hifi_vehicle_music
