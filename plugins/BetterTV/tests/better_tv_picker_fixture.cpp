#include "plugins/BetterTV/file_picker.hpp"
#include <chrono>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
int main() {
    for (int round = 0; round < 3; ++round) {
        better_tv_video::FilePicker picker;
        if (!picker.Start(L"Fixture videos", L"Fixture all files")) return 1;
        picker.Request();
        // Exercise queued cancellation and cancellation inside the real shell modal loop.
        if (round) std::this_thread::sleep_for(500ms);
        const auto started = std::chrono::steady_clock::now();
        if (!picker.Stop(3s) || picker.TakeResult().busy ||
            std::chrono::steady_clock::now() - started > 3s) return 2;
        if (!picker.Stop(0ms)) return 3;
    }
    std::cout << "PASS: queued and modal STA dialogs cancelled and joined before destruction\n";
}
