#include "plugins/BetterTV/audio_output.hpp"

#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    const std::wstring_view mode = argc == 3 ? argv[2] : L"";
    const bool no_audio = mode == L"--expect-no-audio";
    const bool failure = mode == L"--expect-error";
    const bool spatial = mode == L"--spatial";
    const bool fallback = mode == L"--spatial-fallback";
    if (argc != 2 && !no_audio && !failure && !spatial && !fallback) {
        std::cerr << "usage: anomaly_better_tv_audio_fixture <local-video> "
                     "[--spatial|--spatial-fallback|--expect-no-audio|--expect-error]\n";
        return 2;
    }
    using Clock = std::chrono::steady_clock;
    using Audio = better_tv_video::AudioOutput;
    Audio audio;
    Audio::Frame frame{1};
    if (spatial || fallback || no_audio) frame.source = better_tv_video::AudioPosition{0.4f, 0.0f, 2.0f};
    const auto transport = [&](std::int64_t ticks, bool playing) {
        audio.Transport(argv[1], ticks, playing, frame);
    };
    const auto start = Clock::now();
    Audio::Snapshot sample;
    do {
        const auto elapsed = Clock::now() - start;
        // Move the mute boundary inside the listener distance without reopening the stream.
        if (spatial && elapsed > std::chrono::seconds(2)) frame.distance_range = {0.5f, 1.5f};
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
        transport(ticks, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sample = audio.Read();
        if (sample.state == Audio::State::Failed || sample.state == Audio::State::NoAudio) break;
    } while (Clock::now() - start < std::chrono::seconds(4));
    if (no_audio || failure) {
        const bool expected = sample.state == (no_audio ? Audio::State::NoAudio : Audio::State::Failed);
        const auto stopping = Clock::now();
        const bool stopped = audio.Shutdown(std::chrono::seconds(1));
        const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stopping).count();
        std::cout << "expected_state=" << expected << " state=" << static_cast<int>(sample.state)
                  << " stop_ms=" << stop_ms << " error=0x" << std::hex << sample.error << '\n';
        return expected && stopped && stop_ms < 1000 ? 0 : 1;
    }
    const auto expected_route = spatial ? Audio::Route::Spatial : Audio::Route::External;
    const bool advanced = sample.state == Audio::State::Playing && sample.position_ticks > 30000000 &&
        sample.route == expected_route && (!fallback || sample.spatial_error != 0);
    const auto seek_start = Clock::now();
    frame.distance_range = {};
    ++frame.timeline;
    do {
        const auto elapsed = Clock::now() - seek_start;
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
        transport(5000000 + ticks, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (Clock::now() - seek_start < std::chrono::milliseconds(600));
    sample = audio.Read();
    const bool sought = sample.position_ticks > 8000000 && sample.position_ticks < 14000000;
    transport(sample.position_ticks, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool paused = audio.Read().state == Audio::State::Paused;
    const auto resumed_at = Clock::now();
    ++frame.timeline;
    do {
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - resumed_at).count() / 100;
        transport(20000000 + ticks, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        sample = audio.Read();
    } while (Clock::now() - resumed_at < std::chrono::milliseconds(400));
    const bool resumed = sample.state == Audio::State::Playing && sample.position_ticks > 20000000 &&
        sample.route == expected_route;
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    const bool expired = audio.Read().state == Audio::State::Paused;
    audio.Transport({}, 0, false);
    const auto clearing = Clock::now();
    while (audio.Read().state != Audio::State::Idle && Clock::now() - clearing < std::chrono::seconds(1))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool cleared = audio.Read().state == Audio::State::Idle;
    const auto stopping = Clock::now();
    const bool stopped = audio.Shutdown(std::chrono::seconds(1));
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stopping).count();
    std::cout << "advanced=" << advanced << " sought=" << sought << " paused=" << paused
              << " resumed=" << resumed << " expired=" << expired << " cleared=" << cleared
              << " route=" << static_cast<unsigned>(sample.route) << " stop_ms=" << stop_ms
              << " error=0x" << std::hex << sample.error << " spatial_error=0x" << sample.spatial_error
              << " position_ticks=" << std::dec << sample.position_ticks << '\n';
    return advanced && sought && paused && resumed && expired && cleared && stopped && stop_ms < 1000 ? 0 : 1;
}
