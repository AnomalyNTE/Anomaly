#pragma once
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace better_tv_video {
// Lifecycle owns this STA and must drain Stop before unloading its generation.
class FilePicker final {
public:
    struct Result { bool busy{}; std::optional<std::wstring> path; };
    FilePicker();
    ~FilePicker();
    FilePicker(const FilePicker&) = delete;
    FilePicker& operator=(const FilePicker&) = delete;
    bool Start(std::wstring video_label, std::wstring all_label);
    void Request() noexcept;
    Result TakeResult();
    bool Stop(std::chrono::milliseconds timeout) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
