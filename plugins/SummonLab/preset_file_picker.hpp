#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace summon_lab {

inline std::string PathUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// This thread only owns Shell UI and a value result. It never calls the host or
// touches game objects. Lifecycle closes the dialog and joins before DLL unload.
class PresetFilePicker final {
public:
    enum class Kind { Folder, ImportFile };
    struct Result {
        Kind kind{};
        HRESULT status{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
        std::string path;
    };

    ~PresetFilePicker() { Stop(); }
    bool Busy() const noexcept { return busy_.load(std::memory_order_acquire); }

    bool Start(Kind kind, std::string initial, std::string title) {
        if (Busy()) return false;
        if (thread_.joinable()) thread_.join(); // Previous result has already been published.
        stopping_.store(false, std::memory_order_release);
        busy_.store(true, std::memory_order_release);
        HWND owner = GetForegroundWindow();
        DWORD process{};
        if (owner) GetWindowThreadProcessId(owner, &process);
        if (process != GetCurrentProcessId()) owner = nullptr;
        try {
            thread_ = std::thread([this, kind, owner, initial = std::move(initial), title = std::move(title)] {
                Result result{kind};
                try { Choose(result, initial, title, owner); }
                catch (...) { result.status = E_FAIL; }
                {
                    std::scoped_lock lock(mutex_);
                    result_ = std::move(result);
                }
                busy_.store(false, std::memory_order_release);
            });
            return true;
        } catch (...) {
            busy_.store(false, std::memory_order_release);
            return false;
        }
    }

    std::optional<Result> TakeResult() {
        std::scoped_lock lock(mutex_);
        return std::exchange(result_, std::nullopt);
    }

    void Stop() {
        stopping_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

private:
    struct Apartment {
        HRESULT status{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)};
        ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
    };
    struct DialogWatch {
        std::atomic<bool>& stopping;
        IFileDialog* dialog;
        HWND window{CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, nullptr, nullptr)};
        ~DialogWatch() {
            if (window) { KillTimer(window, 1); DestroyWindow(window); }
        }
        static void CALLBACK Tick(HWND window, UINT, UINT_PTR, DWORD) noexcept {
            auto* watch = reinterpret_cast<DialogWatch*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (watch && watch->stopping.load(std::memory_order_acquire))
                static_cast<void>(watch->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED)));
        }
        bool Start() {
            if (!window) return false;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            // The modal Shell loop dispatches this timer on the same STA as Close.
            return SetTimer(window, 1, 50, Tick) != 0;
        }
    };

    void Choose(Result& result, const std::string& initial, const std::string& title, HWND owner) {
        const Apartment apartment;
        if (FAILED(apartment.status)) { result.status = apartment.status; return; }
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        result.status = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
        if (FAILED(result.status)) return;
        DWORD options{};
        result.status = dialog->GetOptions(&options);
        if (FAILED(result.status)) return;
        result.status = dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
            (result.kind == Kind::Folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        if (FAILED(result.status)) return;
        if (result.kind == Kind::ImportFile) {
            const COMDLG_FILTERSPEC filter{L"JSON", L"*.json"};
            result.status = dialog->SetFileTypes(1, &filter);
            if (FAILED(result.status)) return;
        }
        const auto wide_title = std::filesystem::path(std::u8string(title.begin(), title.end())).native();
        static_cast<void>(dialog->SetTitle(wide_title.c_str()));
        if (!initial.empty()) {
            auto folder_path = std::filesystem::path(std::u8string(initial.begin(), initial.end()));
            if (result.kind == Kind::ImportFile) folder_path = folder_path.parent_path();
            Microsoft::WRL::ComPtr<IShellItem> folder;
            if (!folder_path.empty() && SUCCEEDED(SHCreateItemFromParsingName(
                    folder_path.c_str(), nullptr, IID_PPV_ARGS(&folder))))
                static_cast<void>(dialog->SetFolder(folder.Get()));
        }
        DialogWatch watch{stopping_, dialog.Get()};
        if (!watch.Start()) { result.status = E_FAIL; return; }
        if (stopping_.load(std::memory_order_acquire)) {
            result.status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return;
        }
        result.status = dialog->Show(owner);
        if (FAILED(result.status)) return;
        Microsoft::WRL::ComPtr<IShellItem> selected;
        result.status = dialog->GetResult(&selected);
        if (FAILED(result.status)) return;
        PWSTR raw{};
        result.status = selected->GetDisplayName(SIGDN_FILESYSPATH, &raw);
        if (FAILED(result.status)) return;
        try { result.path = PathUtf8(std::filesystem::path(raw)); }
        catch (...) { CoTaskMemFree(raw); throw; }
        CoTaskMemFree(raw);
    }

    std::thread thread_;
    std::atomic<bool> stopping_{}, busy_{};
    std::mutex mutex_;
    std::optional<Result> result_;
};

} // namespace summon_lab
