#include "anomaly/memory_files.hpp"
#include "anomaly/hook_manager.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace anomaly {
namespace {
struct OpenFile {
    std::shared_ptr<const MemoryFile> file;
    std::mutex mutex;
    std::uint64_t position{};
    HANDLE port{};
    ULONG_PTR key{};
    UCHAR notifications{};
};
struct State {
    std::mutex mutex;
    std::map<std::wstring, std::shared_ptr<const MemoryFile>> files;
    std::map<HANDLE, std::shared_ptr<OpenFile>> handles;
    std::vector<std::shared_ptr<const MemoryFileSet>> retained;
    std::unique_ptr<HookBackend> hooks{CreateMinHookBackend()};
    bool installed{};
    bool failed{};
};
// Process-owned, like the engine's outstanding I/O. Never destroyed on session stop.
std::atomic<State*> state{};
std::mutex install_mutex;
std::atomic<std::uint64_t> generation{};

#define ORIGINAL(name) decltype(&::name) original_##name = &::name
ORIGINAL(CreateFileW); ORIGINAL(CloseHandle); ORIGINAL(ReadFile);
ORIGINAL(GetFileAttributesW); ORIGINAL(GetFileAttributesExW);
ORIGINAL(GetFileSizeEx); ORIGINAL(GetFileSize); ORIGINAL(SetFilePointerEx); ORIGINAL(SetFilePointer);
ORIGINAL(GetOverlappedResult); ORIGINAL(ReOpenFile); ORIGINAL(CreateIoCompletionPort);
ORIGINAL(SetFileCompletionNotificationModes); ORIGINAL(CreateFileMappingW);
#undef ORIGINAL

std::wstring Key(const std::filesystem::path& path) {
    auto text = path.wstring();
    if (text.starts_with(L"\\\\?\\UNC\\")) text = L"\\\\" + text.substr(8);
    else if (text.starts_with(L"\\\\?\\")) text.erase(0, 4);
    text = std::filesystem::absolute(text).lexically_normal().generic_wstring();
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}
std::shared_ptr<const MemoryFile> Find(LPCWSTR path) {
    auto* s = state.load();
    if (!s || !path) return {};
    const auto key = Key(path);
    std::scoped_lock lock(s->mutex);
    const auto i = s->files.find(key);
    return i == s->files.end() ? nullptr : i->second;
}
std::shared_ptr<OpenFile> Find(HANDLE handle) {
    auto* s = state.load();
    if (!s) return {};
    std::scoped_lock lock(s->mutex);
    const auto i = s->handles.find(handle);
    return i == s->handles.end() ? nullptr : i->second;
}
BOOL Fail(DWORD error) { SetLastError(error); return FALSE; }
HANDLE Open(std::shared_ptr<const MemoryFile> file) {
    auto open = std::make_shared<OpenFile>();
    open->file = std::move(file);
    // A real, unnamed kernel handle supplies identity and synchronous wait semantics.
    const HANDLE handle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!handle) return INVALID_HANDLE_VALUE;
    try {
        auto* s = state.load();
        std::scoped_lock lock(s->mutex);
        s->handles.emplace(handle, std::move(open));
    } catch (...) { original_CloseHandle(handle); throw; }
    return handle;
}
HANDLE WINAPI Create(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
    DWORD creation, DWORD flags, HANDLE template_file) noexcept {
    try {
        if (auto file = Find(path)) {
            if (creation != OPEN_EXISTING || (access & (GENERIC_WRITE | GENERIC_ALL | DELETE | FILE_WRITE_DATA | FILE_APPEND_DATA))) {
                SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
            }
            return Open(std::move(file));
        }
        return original_CreateFileW(path, access, share, security, creation, flags, template_file);
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
}
BOOL WINAPI Close(HANDLE handle) noexcept {
    try {
        std::shared_ptr<OpenFile> file;
        if (auto* s = state.load()) {
            std::scoped_lock lock(s->mutex);
            const auto i = s->handles.find(handle);
            if (i != s->handles.end()) { file = std::move(i->second); s->handles.erase(i); }
        }
        return original_CloseHandle(handle);
    } catch (...) { return Fail(ERROR_INVALID_HANDLE); }
}
BOOL WINAPI Read(HANDLE handle, LPVOID buffer, DWORD count, LPDWORD read, LPOVERLAPPED overlapped) noexcept {
    try {
        const auto open = Find(handle);
        if (!open) return original_ReadFile(handle, buffer, count, read, overlapped);
        if (read) *read = 0;
        if ((!buffer && count) || (!read && !overlapped)) return Fail(ERROR_INVALID_PARAMETER);
        std::scoped_lock lock(open->mutex);
        const auto bytes = open->file->bytes;
        const std::uint64_t at = overlapped ? (std::uint64_t(overlapped->OffsetHigh) << 32) | overlapped->Offset : open->position;
        const DWORD n = at < bytes.size() ? static_cast<DWORD>((std::min)(std::uint64_t(count), bytes.size() - at)) : 0;
        if (n) std::memcpy(buffer, bytes.data() + at, n);
        if (read) *read = n;
        if (!overlapped) { open->position += n; return TRUE; }
        const bool eof = count && !n;
        overlapped->InternalHigh = n;
        // STATUS_END_OF_FILE or STATUS_SUCCESS, consumed by GetOverlappedResult and IOCP.
        overlapped->Internal = eof ? 0xC0000011u : 0;
        const auto event = reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(overlapped->hEvent) & ~ULONG_PTR{1});
        if (event) SetEvent(event);
        if (!(open->notifications & FILE_SKIP_SET_EVENT_ON_HANDLE)) SetEvent(handle);
        if (!eof && open->port && !(reinterpret_cast<ULONG_PTR>(overlapped->hEvent) & 1) &&
            !(open->notifications & FILE_SKIP_COMPLETION_PORT_ON_SUCCESS))
            PostQueuedCompletionStatus(open->port, n, open->key, overlapped);
        return eof ? Fail(ERROR_HANDLE_EOF) : TRUE;
    } catch (...) { return Fail(ERROR_READ_FAULT); }
}
BOOL WINAPI SizeEx(HANDLE handle, PLARGE_INTEGER size) noexcept {
    try {
        if (const auto open = Find(handle)) {
            if (!size) return Fail(ERROR_INVALID_PARAMETER);
            size->QuadPart = static_cast<LONGLONG>(open->file->bytes.size()); return TRUE;
        }
        return original_GetFileSizeEx(handle, size);
    } catch (...) { return Fail(ERROR_INVALID_HANDLE); }
}
DWORD WINAPI Size(HANDLE handle, LPDWORD high) noexcept {
    try {
        if (const auto open = Find(handle)) {
            const auto n = open->file->bytes.size();
            if (high) *high = static_cast<DWORD>(n >> 32);
            SetLastError(ERROR_SUCCESS); return static_cast<DWORD>(n);
        }
        return original_GetFileSize(handle, high);
    } catch (...) { SetLastError(ERROR_INVALID_HANDLE); return INVALID_FILE_SIZE; }
}
BOOL Seek(const std::shared_ptr<OpenFile>& open, LARGE_INTEGER distance, PLARGE_INTEGER result, DWORD method) {
    std::scoped_lock lock(open->mutex);
    const auto base = method == FILE_BEGIN ? 0ull : method == FILE_CURRENT ? open->position : open->file->bytes.size();
    if (method > FILE_END) return Fail(ERROR_INVALID_PARAMETER);
    if ((distance.QuadPart < 0 && std::uint64_t(-(distance.QuadPart + 1)) + 1 > base) ||
        (distance.QuadPart >= 0 && std::uint64_t(distance.QuadPart) > std::uint64_t(INT64_MAX) - base))
        return Fail(ERROR_NEGATIVE_SEEK);
    open->position = base + distance.QuadPart;
    if (result) result->QuadPart = static_cast<LONGLONG>(open->position);
    return TRUE;
}
BOOL WINAPI SeekEx(HANDLE handle, LARGE_INTEGER distance, PLARGE_INTEGER result, DWORD method) noexcept {
    try { if (const auto open = Find(handle)) return Seek(open, distance, result, method);
        return original_SetFilePointerEx(handle, distance, result, method);
    } catch (...) { return Fail(ERROR_INVALID_HANDLE); }
}
DWORD WINAPI Seek32(HANDLE handle, LONG low, PLONG high, DWORD method) noexcept {
    try {
        if (const auto open = Find(handle)) {
            LARGE_INTEGER distance{}, result{};
            if (high) { distance.LowPart = static_cast<DWORD>(low); distance.HighPart = *high; }
            else distance.QuadPart = low;
            if (!Seek(open, distance, &result, method)) return INVALID_SET_FILE_POINTER;
            if (high) *high = result.HighPart;
            SetLastError(ERROR_SUCCESS); return result.LowPart;
        }
        return original_SetFilePointer(handle, low, high, method);
    } catch (...) { SetLastError(ERROR_INVALID_HANDLE); return INVALID_SET_FILE_POINTER; }
}
DWORD WINAPI Attributes(LPCWSTR path) noexcept {
    try { if (Find(path)) return FILE_ATTRIBUTE_READONLY;
        return original_GetFileAttributesW(path);
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_FILE_ATTRIBUTES; }
}
BOOL WINAPI AttributesEx(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID output) noexcept {
    try {
        if (const auto file = Find(path)) {
            if (level != GetFileExInfoStandard || !output) return Fail(ERROR_INVALID_PARAMETER);
            auto& info = *static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(output);
            info = {}; info.dwFileAttributes = FILE_ATTRIBUTE_READONLY;
            info.nFileSizeHigh = static_cast<DWORD>(file->bytes.size() >> 32);
            info.nFileSizeLow = static_cast<DWORD>(file->bytes.size()); return TRUE;
        }
        return original_GetFileAttributesExW(path, level, output);
    } catch (...) { return Fail(ERROR_NOT_ENOUGH_MEMORY); }
}
BOOL WINAPI Result(HANDLE handle, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL wait) noexcept {
    try {
        if (Find(handle)) {
            if (!overlapped || !transferred) return Fail(ERROR_INVALID_PARAMETER);
            *transferred = static_cast<DWORD>(overlapped->InternalHigh);
            return overlapped->Internal == 0 ? TRUE : Fail(ERROR_HANDLE_EOF);
        }
        return original_GetOverlappedResult(handle, overlapped, transferred, wait);
    } catch (...) { return Fail(ERROR_INVALID_HANDLE); }
}
HANDLE WINAPI Reopen(HANDLE handle, DWORD access, DWORD share, DWORD flags) noexcept {
    try {
        if (const auto open = Find(handle)) {
            if (access & (GENERIC_WRITE | GENERIC_ALL | DELETE | FILE_WRITE_DATA | FILE_APPEND_DATA)) {
                SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
            }
            return Open(open->file);
        }
        return original_ReOpenFile(handle, access, share, flags);
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
}
HANDLE WINAPI CompletionPort(HANDLE handle, HANDLE port, ULONG_PTR key, DWORD threads) noexcept {
    try {
        if (const auto open = Find(handle)) {
            std::scoped_lock lock(open->mutex);
            if (open->port) {
                if (port == open->port) return port;
                SetLastError(ERROR_INVALID_PARAMETER); return nullptr;
            }
            if (!port) port = original_CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, threads);
            if (port) { open->port = port; open->key = key; }
            return port;
        }
        return original_CreateIoCompletionPort(handle, port, key, threads);
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
}
BOOL WINAPI Notifications(HANDLE handle, UCHAR modes) noexcept {
    try {
        if (const auto open = Find(handle)) {
            if (modes & ~(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE))
                return Fail(ERROR_INVALID_PARAMETER);
            std::scoped_lock lock(open->mutex); open->notifications |= modes; return TRUE;
        }
        return original_SetFileCompletionNotificationModes(handle, modes);
    } catch (...) { return Fail(ERROR_INVALID_HANDLE); }
}
HANDLE WINAPI Mapping(HANDLE handle, LPSECURITY_ATTRIBUTES security, DWORD protect,
    DWORD high, DWORD low, LPCWSTR name) noexcept {
    try {
        if (const auto open = Find(handle)) {
            const auto bytes = open->file->bytes;
            const std::uint64_t size = (std::uint64_t(high) << 32) | low;
            if (protect != PAGE_READONLY || name || (size && size != bytes.size())) {
                SetLastError(ERROR_NOT_SUPPORTED); return nullptr;
            }
            // UE's optional mapped bulk reads use a paging-file section, never a temporary file.
            const HANDLE section = original_CreateFileMappingW(INVALID_HANDLE_VALUE, security, PAGE_READWRITE,
                static_cast<DWORD>(bytes.size() >> 32), static_cast<DWORD>(bytes.size()), nullptr);
            if (!section) return nullptr;
            void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0);
            if (!view) { original_CloseHandle(section); return nullptr; }
            std::memcpy(view, bytes.data(), bytes.size()); UnmapViewOfFile(view);
            HANDLE read_only{};
            const bool duplicated = DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(),
                &read_only, SECTION_MAP_READ | SECTION_QUERY, FALSE, 0) != FALSE;
            original_CloseHandle(section);
            return duplicated ? read_only : nullptr;
        }
        return original_CreateFileMappingW(handle, security, protect, high, low, name);
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
}

void Install() {
    std::scoped_lock lock(install_mutex);
    auto* s = state.load();
    if (!s) { s = new State; state.store(s); }
    if (s->installed) return;
    if (s->failed) throw std::runtime_error("memory file hook installation previously failed; restart required");
    s->failed = true;
    if (!s->hooks->Initialize()) throw std::runtime_error("memory file hooks unavailable");
    struct Hook { void* target; void* detour; void** original; };
#define HOOK(api, fn) Hook{reinterpret_cast<void*>(&::api), reinterpret_cast<void*>(&fn), reinterpret_cast<void**>(&original_##api)}
    const Hook hooks[]{HOOK(CreateFileW, Create), HOOK(CloseHandle, Close), HOOK(ReadFile, Read),
        HOOK(GetFileAttributesW, Attributes), HOOK(GetFileAttributesExW, AttributesEx),
        HOOK(GetFileSizeEx, SizeEx), HOOK(GetFileSize, Size), HOOK(SetFilePointerEx, SeekEx), HOOK(SetFilePointer, Seek32),
        HOOK(GetOverlappedResult, Result), HOOK(ReOpenFile, Reopen), HOOK(CreateIoCompletionPort, CompletionPort),
        HOOK(SetFileCompletionNotificationModes, Notifications), HOOK(CreateFileMappingW, Mapping)};
#undef HOOK
    std::vector<void*> created;
    for (const auto& hook : hooks) {
        if (!s->hooks->Create(hook.target, hook.detour, hook.original)) {
            // Keep created trampolines resident, including a partially enabled set. No names
            // have been published yet, so these hooks only pass through to Windows.
            throw std::runtime_error("cannot install memory file read contract");
        }
        created.push_back(hook.target);
    }
    for (const auto target : created) if (!s->hooks->QueueEnable(target))
        throw std::runtime_error("cannot enable memory file read contract");
    if (!s->hooks->ApplyQueued()) throw std::runtime_error("cannot enable memory file read contract");
    s->installed = true;
    s->failed = false;
}
} // namespace

MemoryFileSet::MemoryFileSet(std::vector<MemoryFile> files) {
    Install();
    for (auto& file : files) {
        if (!file.owner || file.bytes.empty()) throw std::runtime_error("memory file requires owned, nonempty bytes");
        file.path = Key(file.path);
        files_.push_back(std::make_shared<const MemoryFile>(std::move(file)));
    }
    auto* s = state.load();
    std::scoped_lock lock(s->mutex);
    // Copy then swap keeps registration atomic even when allocation fails.
    auto next = s->files;
    for (const auto& file : files_)
        if (!next.emplace(file->path.generic_wstring(), file).second) throw std::runtime_error("memory filename already registered");
    s->files.swap(next);
}
MemoryFileSet::~MemoryFileSet() {
    auto* s = state.load();
    std::scoped_lock lock(s->mutex);
    for (const auto& file : files_) s->files.erase(file->path.generic_wstring());
}
std::optional<MemoryFile> FindMemoryFile(const std::filesystem::path& path) {
    if (const auto file = Find(path.c_str())) return *file;
    return std::nullopt;
}
std::filesystem::path MemoryFileDirectory(const std::filesystem::path& parent) {
    return parent / L".anomaly-memory" / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++generation));
}
void RetainMemoryFiles(std::shared_ptr<const MemoryFileSet> files) {
    if (!files) return;
    auto* s = state.load();
    std::scoped_lock lock(s->mutex); s->retained.push_back(std::move(files));
}
} // namespace anomaly
