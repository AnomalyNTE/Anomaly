#include "plugins/NteTvVideo/audio_output.hpp"

#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    const bool no_audio = argc == 3 && std::wstring_view(argv[2]) == L"--expect-no-audio";
    const bool failure = argc == 3 && std::wstring_view(argv[2]) == L"--expect-error";
    if (argc != 2 && !no_audio && !failure) {
        std::cerr << "usage: anomaly_nte_tv_audio_fixture <local-video> [--expect-no-audio|--expect-error]\n";
        return 2;
    }
    using Clock = std::chrono::steady_clock;
    using Audio = nte_tv_video::AudioOutput;
    Audio audio;
    const auto start = Clock::now();
    Audio::Snapshot sample;
    do {
        const auto elapsed = Clock::now() - start;
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
        audio.Transport(argv[1], ticks, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sample = audio.Read();
        if (sample.state == Audio::State::Failed || sample.state == Audio::State::NoAudio) break;
    } while (Clock::now() - start < std::chrono::seconds(4));
    if (no_audio || failure) {
        const bool expected = sample.state == (no_audio ? Audio::State::NoAudio : Audio::State::Failed);
        const auto stopping = Clock::now();
        audio.Shutdown();
        const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stopping).count();
        std::cout << "expected_state=" << expected << " state=" << static_cast<int>(sample.state)
                  << " stop_ms=" << stop_ms << " error=0x" << std::hex << sample.error << '\n';
        return expected && stop_ms < 1000 ? 0 : 1;
    }
    const bool advanced = sample.state == Audio::State::Playing && sample.position_ticks > 10000000;
    const auto seek_start = Clock::now();
    do {
        const auto elapsed = Clock::now() - seek_start;
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
        audio.Transport(argv[1], 5000000 + ticks, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (Clock::now() - seek_start < std::chrono::milliseconds(600));
    sample = audio.Read();
    const bool sought = sample.position_ticks > 8000000 && sample.position_ticks < 14000000;
    audio.Transport(argv[1], sample.position_ticks, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool paused = audio.Read().state == Audio::State::Paused;
    const auto resumed_at = Clock::now();
    do {
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - resumed_at).count() / 100;
        audio.Transport(argv[1], 20000000 + ticks, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        sample = audio.Read();
    } while (Clock::now() - resumed_at < std::chrono::milliseconds(400));
    const bool resumed = sample.state == Audio::State::Playing && sample.position_ticks > 20000000;
    audio.Transport({}, 0, false);
    const auto clearing = Clock::now();
    while (audio.Read().state != Audio::State::Idle && Clock::now() - clearing < std::chrono::seconds(1))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool cleared = audio.Read().state == Audio::State::Idle;
    const auto stopping = Clock::now();
    audio.Shutdown();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stopping).count();
    std::cout << "advanced=" << advanced << " sought=" << sought << " paused=" << paused
              << " resumed=" << resumed << " cleared=" << cleared
              << " stop_ms=" << stop_ms << " error=0x" << std::hex << sample.error
              << " position_ticks=" << std::dec << sample.position_ticks << '\n';
    return advanced && sought && paused && resumed && cleared && stop_ms < 1000 ? 0 : 1;
}
