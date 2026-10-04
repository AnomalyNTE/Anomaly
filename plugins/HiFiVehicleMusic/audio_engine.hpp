#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hifi_vehicle_music {

enum class Backend : std::uint32_t {
    WasapiExclusive = 0,
    WasapiShared = 1,
    DirectSound = 2,
    Asio = 3,
};

// How DSF/DFF reach the DAC. Native (ASIO DSD mode) and DoP pass the
// bitstream unchanged and need the ASIO backend; PCM (dsd2pcm) is the last
// resort and the only way on the other backends.
enum class DsdMode : std::uint32_t {
    PcmOnly = 0,
    NativeThenDop = 1,  // Native > DoP > PCM
    DopThenNative = 2,  // DoP > Native > PCM
};

struct EngineSettings final {
    Backend backend{Backend::WasapiExclusive};
    DsdMode dsd_mode{DsdMode::NativeThenDop};
    std::string device;          // UTF-8 device name; empty selects the default device
    std::uint32_t buffer_ms{20};
    float volume{1.0F};
    std::wstring folder;         // absolute music library directory
};

enum class EngineStatus : std::uint32_t {
    Starting,
    Ready,               // detail: backend name
    BackendUnavailable,  // detail: backend name
    FolderUnavailable,
    DecodeFailed,        // detail: file path
    OpenFailed,          // detail: backend name
    StartFailed,         // detail: backend name
    Playing,
    AsioError,           // detail: RtAudio message
    EngineError,         // detail: exception message
};

struct EngineSnapshot final {
    EngineStatus status{EngineStatus::Starting};
    std::string detail;
    std::string track;
    std::string output;   // "<backend> <rate> Hz, <format>, <channels> ch"
    bool resampled{};
    std::vector<std::string> devices;
    std::size_t track_count{};
    bool playing{};
    bool paused{};
};

// Owns one worker thread. Every device, decoder and directory operation runs on
// that thread; the public methods only queue commands or read published state,
// so they are safe to call from the game hook and render callbacks.
class AudioEngine final {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Start(EngineSettings settings);
    void Shutdown();

    void Configure(EngineSettings settings);
    void Rescan();
    void SetVolume(float volume) noexcept;

    // True when the library is non-empty and the configured output has not
    // failed to open. Play uses a file named after the lower-cased event if
    // one exists (looped). Otherwise `index` (the album song) selects
    // that playlist song; re-posting the same song keeps it playing.
    // Without an index it starts or keeps the playlist.
    [[nodiscard]] bool CanPlay(std::string_view key) const;
    void Play(std::string key, float position, std::optional<std::size_t> index = {});
    // Playlist songs stop only if no Play arrives within a short grace period.
    void Stop();
    // Stops immediately, e.g. when the player leaves the vehicle.
    void StopNow();
    void Previous();
    void Next();
    void Pause();
    void Resume();
    // Jumps within the current song; `fraction` is 0..1 of its length.
    void Seek(float fraction);

    // Position in the current song, both 0 when nothing is loaded. Safe from
    // any thread; reads only atomics.
    struct Progress final {
        float seconds{};
        float duration{};
    };
    [[nodiscard]] Progress Position() const noexcept;

    [[nodiscard]] EngineSnapshot Snapshot() const;
    // Playlist index of the song now playing, also after the engine moved on
    // by itself; empty when nothing from the playlist plays.
    [[nodiscard]] std::optional<std::size_t> PlayingSlot() const;
    // Length in seconds of a playlist song, measured at scan time; 0 = unknown.
    [[nodiscard]] float Duration(std::size_t index) const;
    // Playlist song titles (file stems) in playlist order, and a counter that
    // changes whenever a scan replaces them.
    [[nodiscard]] std::vector<std::wstring> Titles() const;
    // cover/folder/front/album .png|.jpg|.jpeg|.bmp in the library root, or empty.
    [[nodiscard]] std::wstring Cover() const;
    // Name of the scanned library folder (its last path component), or empty.
    [[nodiscard]] std::wstring LibraryName() const;
    [[nodiscard]] std::uint64_t LibraryGeneration() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace hifi_vehicle_music
