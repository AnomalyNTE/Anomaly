#pragma once

#include <Windows.h>

#include <filesystem>

namespace anomaly {

// Caller-serialized invalidation for a directory snapshot. Watch its parent as well so deleting,
// replacing or creating the directory invalidates the snapshot, including an initially absent root.
// Notification failure deliberately falls back to scanning instead of retaining an unobserved cache.
class DirectoryChangeMonitor final {
public:
    DirectoryChangeMonitor() = default;
    ~DirectoryChangeMonitor() { Close(contents_); Close(parent_); Close(parent_attributes_); }
    DirectoryChangeMonitor(const DirectoryChangeMonitor&) = delete;
    DirectoryChangeMonitor& operator=(const DirectoryChangeMonitor&) = delete;

    bool Changed(const std::filesystem::path& directory) {
        if (directory_ != directory || parent_ == INVALID_HANDLE_VALUE || parent_attributes_ == INVALID_HANDLE_VALUE) {
            Close(contents_);
            Close(parent_);
            Close(parent_attributes_);
            directory_ = directory;
            parent_ = FindFirstChangeNotificationW(directory_.parent_path().c_str(), FALSE,
                FILE_NOTIFY_CHANGE_DIR_NAME);
            parent_attributes_ = FindFirstChangeNotificationW(directory_.parent_path().c_str(), FALSE,
                FILE_NOTIFY_CHANGE_ATTRIBUTES);
            WatchContents();
            return true;
        }
        bool changed = Consume(parent_);
        // A sibling's attributes (for example an ImGui settings save) do not invalidate this
        // directory. Keep root attribute/reparse changes observable without rescanning siblings.
        const bool attributes_changed = Consume(parent_attributes_);
        if (attributes_changed && !changed) {
            const auto attributes = GetFileAttributesW(directory_.c_str());
            changed = attributes != attributes_ || (attributes == INVALID_FILE_ATTRIBUTES && !missing_);
        }
        if (changed) WatchContents();
        if (contents_ == INVALID_HANDLE_VALUE) {
            if (!missing_) {
                WatchContents();
                changed = true;
            }
            return changed;
        }
        return Consume(contents_) || changed;
    }

private:
    static void Close(HANDLE& handle) noexcept {
        if (handle != INVALID_HANDLE_VALUE) FindCloseChangeNotification(handle);
        handle = INVALID_HANDLE_VALUE;
    }

    static bool Consume(HANDLE& handle) noexcept {
        const auto result = WaitForSingleObject(handle, 0);
        if (result == WAIT_TIMEOUT) return false;
        // Rearm before the caller scans. Edits made during that scan remain pending for the next read.
        if (result != WAIT_OBJECT_0 || !FindNextChangeNotification(handle)) Close(handle);
        return true;
    }

    void WatchContents() noexcept {
        Close(contents_);
        contents_ = FindFirstChangeNotificationW(directory_.c_str(), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_ATTRIBUTES);
        const auto error = contents_ == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        missing_ = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        attributes_ = GetFileAttributesW(directory_.c_str());
    }

    std::filesystem::path directory_;
    HANDLE contents_{INVALID_HANDLE_VALUE};
    HANDLE parent_{INVALID_HANDLE_VALUE};
    HANDLE parent_attributes_{INVALID_HANDLE_VALUE};
    DWORD attributes_{INVALID_FILE_ATTRIBUTES};
    bool missing_{};
};

} // namespace anomaly
