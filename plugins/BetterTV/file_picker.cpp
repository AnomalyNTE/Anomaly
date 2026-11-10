#include "file_picker.hpp"
#include <Windows.h>
#include <ShlObj.h>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>

namespace better_tv_video {
struct FilePicker::Impl {
    static constexpr UINT kOpen = WM_APP + 1, kStop = WM_APP + 2;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    HWND window{};
    IFileOpenDialog* dialog{}; // STA only; never called through a borrowed pointer on another thread.
    std::wstring video_label, all_label;
    Result result;
    bool ready{}, done{}, stopping{};

    void Open() {
        {
            std::scoped_lock lock(mutex);
            if (stopping) { result.busy = false; return; }
        }
        std::optional<std::wstring> picked;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog)))) {
            // Patterns are file-format data, not display text.
            const COMDLG_FILTERSPEC filters[] = {
                {video_label.c_str(), L"*.mp4;*.m4v;*.mov;*.webm;*.mkv;*.usm"},
                {all_label.c_str(), L"*.*"}};
            dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
            FILEOPENDIALOGOPTIONS options{};
            if (SUCCEEDED(dialog->GetOptions(&options)))
                dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
            if (SUCCEEDED(dialog->Show(nullptr))) {
                IShellItem* item{};
                if (SUCCEEDED(dialog->GetResult(&item))) {
                    PWSTR path{};
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        picked = path;
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
            dialog = nullptr;
        }
        std::scoped_lock lock(mutex);
        if (!stopping) result.path = std::move(picked);
        result.busy = false;
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        try {
            if (self && message == kOpen) { self->Open(); return 0; }
            if (self && message == kStop) {
                // Dispatch on the dialog's own apartment, including its modal message loop.
                if (self->dialog) self->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
                PostQuitMessage(0);
                return 0;
            }
        } catch (...) {
            if (self) { std::scoped_lock lock(self->mutex); self->result.busy = false; }
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }

    void Run() noexcept {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const auto class_name = L"AnomalyTvPicker_" + std::to_wstring(reinterpret_cast<std::uintptr_t>(this));
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = class_name.c_str();
        const ATOM atom = SUCCEEDED(initialized) ? RegisterClassW(&wc) : 0;
        const HWND hwnd = atom ? CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, wc.hInstance, this) : nullptr;
        {
            std::scoped_lock lock(mutex);
            window = hwnd;
            ready = true;
        }
        changed.notify_all();
        if (hwnd) {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            DestroyWindow(hwnd);
        }
        if (atom) UnregisterClassW(wc.lpszClassName, wc.hInstance);
        if (SUCCEEDED(initialized)) CoUninitialize();
        {
            std::scoped_lock lock(mutex);
            window = nullptr;
            result.busy = false;
            done = true;
        }
        changed.notify_all();
    }
};

FilePicker::FilePicker() : impl_(std::make_unique<Impl>()) {}
FilePicker::~FilePicker() {
    // A host must quarantine on a failed bounded Stop. Normal destruction follows a successful drain.
    if (impl_->worker.joinable()) static_cast<void>(Stop(std::chrono::milliseconds::max()));
}
bool FilePicker::Start(std::wstring video_label, std::wstring all_label) {
    std::unique_lock lock(impl_->mutex);
    if (impl_->worker.joinable()) return impl_->window != nullptr;
    impl_->video_label = std::move(video_label);
    impl_->all_label = std::move(all_label);
    impl_->ready = impl_->done = impl_->stopping = false;
    impl_->worker = std::thread([this] { impl_->Run(); });
    impl_->changed.wait(lock, [this] { return impl_->ready; });
    const bool started = impl_->window != nullptr;
    lock.unlock();
    if (!started) impl_->worker.join();
    return started;
}
void FilePicker::Request() noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->window || impl_->stopping || impl_->result.busy) return;
    impl_->result.busy = true;
    if (!PostMessageW(impl_->window, Impl::kOpen, 0, 0)) impl_->result.busy = false;
}
FilePicker::Result FilePicker::TakeResult() {
    std::scoped_lock lock(impl_->mutex);
    return {impl_->result.busy, std::exchange(impl_->result.path, {})};
}
bool FilePicker::Stop(std::chrono::milliseconds timeout) noexcept {
    if (!impl_->worker.joinable()) return true;
    std::unique_lock lock(impl_->mutex);
    impl_->stopping = true;
    if (impl_->window) PostMessageW(impl_->window, Impl::kStop, 0, 0);
    bool done{};
    if (timeout == std::chrono::milliseconds::max()) {
        impl_->changed.wait(lock, [this] { return impl_->done; });
        done = true;
    } else done = impl_->changed.wait_for(lock, timeout, [this] { return impl_->done; });
    lock.unlock();
    if (done) impl_->worker.join();
    return done;
}
}
