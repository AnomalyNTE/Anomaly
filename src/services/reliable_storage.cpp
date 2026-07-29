#include "anomaly/reliable_storage.hpp"

#include <Windows.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

constexpr ULONG kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD kInspectFlags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
constexpr ULONG kNtFileOpen = 1;
constexpr ULONG kNtFileCreate = 2;
constexpr ULONG kNtFileDirectory = 0x00000001;
constexpr ULONG kNtFileSequentialOnly = 0x00000004;
constexpr ULONG kNtFileSynchronousIoNonAlert = 0x00000020;
constexpr ULONG kNtFileOpenReparsePoint = 0x00200000;
constexpr std::size_t kIoChunkBytes = 16U * 1024U * 1024U;
// The public winternl.h enum omits the native FileRenameInformationEx value.
constexpr FILE_INFORMATION_CLASS kNtFileRenameInformationEx =
    static_cast<FILE_INFORMATION_CLASS>(65);
constexpr ULONG kNtFileRenameReplaceIfExists = 0x00000001;
constexpr ULONG kNtFileRenamePosixSemantics = 0x00000002;

StorageResult Success(std::size_t bytes = 0) noexcept {
    return {StorageError::None, ERROR_SUCCESS, bytes};
}

StorageResult Failure(
    StorageError error, DWORD win32_error, std::size_t bytes = 0) noexcept {
    return {error, win32_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : win32_error, bytes};
}

StorageResult ExceptionFailure() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return Failure(StorageError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY);
    } catch (...) {
        return Failure(StorageError::InternalFailure, ERROR_UNHANDLED_EXCEPTION);
    }
}

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.Release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) Reset(other.Release());
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] HANDLE Release() noexcept {
        return std::exchange(value_, INVALID_HANDLE_VALUE);
    }
    void Reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) static_cast<void>(CloseHandle(value_));
        value_ = value;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

using NtCreateFileFunction = NTSTATUS(NTAPI*)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
    ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtSetInformationFileFunction = NTSTATUS(NTAPI*)(
    HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
using RtlNtStatusToDosErrorFunction = ULONG(NTAPI*)(NTSTATUS);

template <typename Function>
Function ResolveProcedure(HMODULE module, const char* name) noexcept {
    const FARPROC procedure = GetProcAddress(module, name);
    Function resolved{};
    static_assert(sizeof(resolved) == sizeof(procedure));
    std::memcpy(&resolved, &procedure, sizeof(resolved));
    return resolved;
}

struct NativeApi final {
    NtCreateFileFunction create_file{};
    NtSetInformationFileFunction set_information{};
    RtlNtStatusToDosErrorFunction status_to_error{};
};

const NativeApi& GetNativeApi() noexcept {
    static const NativeApi api = [] {
        NativeApi loaded;
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr) {
            loaded.create_file = ResolveProcedure<NtCreateFileFunction>(ntdll, "NtCreateFile");
            loaded.set_information = ResolveProcedure<NtSetInformationFileFunction>(
                ntdll, "NtSetInformationFile");
            loaded.status_to_error = ResolveProcedure<RtlNtStatusToDosErrorFunction>(
                ntdll, "RtlNtStatusToDosError");
        }
        return loaded;
    }();
    return api;
}

DWORD NativeError(NTSTATUS status) noexcept {
    const auto convert = GetNativeApi().status_to_error;
    if (convert == nullptr) return ERROR_GEN_FAILURE;
    return static_cast<DWORD>(convert(status));
}

StorageResult OpenRelative(
    HANDLE parent, std::wstring_view name, ACCESS_MASK desired_access,
    ULONG share_access, ULONG disposition, ULONG options, UniqueHandle& output) noexcept {
    const NativeApi& api = GetNativeApi();
    if (api.create_file == nullptr) {
        return Failure(StorageError::InternalFailure, ERROR_PROC_NOT_FOUND);
    }
    if (parent == nullptr || parent == INVALID_HANDLE_VALUE || name.empty() ||
        name.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t)) {
        return Failure(StorageError::InvalidPath, ERROR_INVALID_NAME);
    }

    UNICODE_STRING object_name{};
    object_name.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    object_name.MaximumLength = object_name.Length;
    object_name.Buffer = const_cast<PWSTR>(name.data());

    OBJECT_ATTRIBUTES object_attributes{};
    object_attributes.Length = sizeof(object_attributes);
    object_attributes.RootDirectory = parent;
    object_attributes.ObjectName = &object_name;
    object_attributes.Attributes = OBJ_CASE_INSENSITIVE;

    IO_STATUS_BLOCK io_status{};
    HANDLE opened = INVALID_HANDLE_VALUE;
    const NTSTATUS status = api.create_file(
        &opened, desired_access | SYNCHRONIZE, &object_attributes, &io_status, nullptr,
        FILE_ATTRIBUTE_NORMAL, share_access, disposition,
        options | kNtFileOpenReparsePoint | kNtFileSynchronousIoNonAlert, nullptr, 0);
    if (status < 0) return Failure(StorageError::IoFailure, NativeError(status));
    output.Reset(opened);
    if (!output.valid()) return Failure(StorageError::IoFailure, ERROR_INVALID_HANDLE);
    return Success();
}

bool HasEmbeddedNull(std::wstring_view value) noexcept {
    return value.find(L'\0') != std::wstring_view::npos;
}

bool IsForbiddenNameCharacter(wchar_t value) noexcept {
    return value < 32 || value == L'<' || value == L'>' || value == L':' ||
        value == L'"' || value == L'|' || value == L'?' || value == L'*';
}

wchar_t UpperAscii(wchar_t value) noexcept {
    return value >= L'a' && value <= L'z'
        ? static_cast<wchar_t>(value - (L'a' - L'A'))
        : value;
}

bool IsReservedDeviceName(std::wstring_view component) noexcept {
    const auto dot = component.find(L'.');
    const std::wstring_view stem = component.substr(0, dot);
    if (stem.empty() || stem.size() > 7) return false;

    std::array<wchar_t, 8> upper{};
    for (std::size_t index = 0; index < stem.size(); ++index) {
        upper[index] = UpperAscii(stem[index]);
    }
    const std::wstring_view name(upper.data(), stem.size());
    if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL" ||
        name == L"CLOCK$" || name == L"CONIN$" || name == L"CONOUT$") {
        return true;
    }
    const bool reserved_suffix = name.size() == 4 &&
        ((name[3] >= L'1' && name[3] <= L'9') || name[3] == L'\u00B9' ||
         name[3] == L'\u00B2' || name[3] == L'\u00B3');
    return reserved_suffix &&
        (name.substr(0, 3) == L"COM" || name.substr(0, 3) == L"LPT");
}

struct NormalizedPath final {
    std::wstring relative;
    std::size_t parent_length{};

    [[nodiscard]] std::wstring_view Leaf() const noexcept {
        return parent_length == 0
            ? std::wstring_view(relative)
            : std::wstring_view(relative).substr(parent_length + 1);
    }
};

StorageResult NormalizeRelativePath(
    std::wstring_view input, NormalizedPath& output) {
    if (input.empty() || HasEmbeddedNull(input)) {
        return Failure(StorageError::InvalidPath, ERROR_INVALID_NAME);
    }
    if (input.front() == L'\\' || input.front() == L'/') {
        return Failure(StorageError::PathOutsideRoot, ERROR_ACCESS_DENIED);
    }

    std::wstring normalized;
    normalized.reserve(input.size());
    std::size_t component_start{};
    while (component_start < input.size()) {
        while (component_start < input.size() &&
               (input[component_start] == L'\\' || input[component_start] == L'/')) {
            ++component_start;
        }
        if (component_start == input.size()) break;

        std::size_t component_end = component_start;
        while (component_end < input.size() && input[component_end] != L'\\' &&
               input[component_end] != L'/') {
            ++component_end;
        }
        const std::wstring_view component = input.substr(
            component_start, component_end - component_start);
        if (component == L"..") {
            return Failure(StorageError::PathOutsideRoot, ERROR_ACCESS_DENIED);
        }
        if (component != L".") {
            if (component.size() > 255 || component.back() == L'.' ||
                component.back() == L' ' || IsReservedDeviceName(component) ||
                std::ranges::any_of(component, IsForbiddenNameCharacter)) {
                return Failure(StorageError::InvalidPath, ERROR_INVALID_NAME);
            }
            if (!normalized.empty()) normalized.push_back(L'\\');
            normalized.append(component);
        }
        component_start = component_end;
    }

    if (normalized.empty()) {
        return Failure(StorageError::InvalidPath, ERROR_INVALID_NAME);
    }
    output.parent_length = normalized.find_last_of(L'\\');
    if (output.parent_length == std::wstring::npos) output.parent_length = 0;
    output.relative = std::move(normalized);
    return Success();
}

StorageResult QueryAttributes(
    HANDLE handle, FILE_ATTRIBUTE_TAG_INFO& attributes) noexcept {
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        return Failure(StorageError::IoFailure, GetLastError());
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Failure(StorageError::ReparsePoint, ERROR_ACCESS_DENIED);
    }
    return Success();
}

StorageResult RequireDirectory(HANDLE handle) noexcept {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    StorageResult result = QueryAttributes(handle, attributes);
    if (!result) return result;
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return Failure(StorageError::InvalidPath, ERROR_PATH_NOT_FOUND);
    }
    return Success();
}

StorageResult RequireRegularFile(HANDLE handle) noexcept {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    StorageResult result = QueryAttributes(handle, attributes);
    if (!result) return result;
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return Failure(StorageError::InvalidPath, ERROR_ACCESS_DENIED);
    }
    return Success();
}

void MarkDelete(HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    static_cast<void>(SetFileInformationByHandle(
        handle, FileDispositionInfo, &disposition, sizeof(disposition)));
}

StorageResult RenameRelative(
    HANDLE file, HANDLE parent, std::wstring_view target_name) {
    const NativeApi& api = GetNativeApi();
    if (api.set_information == nullptr) {
        return Failure(StorageError::InternalFailure, ERROR_PROC_NOT_FOUND);
    }
    struct NativeRenameInformationEx final {
        ULONG flags{};
        HANDLE root_directory{};
        ULONG file_name_length{};
        std::array<wchar_t, 255> file_name{};
    };
    static_assert(
        offsetof(NativeRenameInformationEx, root_directory) ==
        offsetof(FILE_RENAME_INFO, RootDirectory));
    static_assert(
        offsetof(NativeRenameInformationEx, file_name) ==
        offsetof(FILE_RENAME_INFO, FileName));

    NativeRenameInformationEx rename;
    if (target_name.size() > rename.file_name.size()) {
        return Failure(StorageError::InvalidPath, ERROR_FILENAME_EXCED_RANGE);
    }
    const std::size_t name_bytes = target_name.size() * sizeof(wchar_t);
    const std::size_t buffer_size =
        offsetof(NativeRenameInformationEx, file_name) + name_bytes;
    rename.flags = kNtFileRenameReplaceIfExists | kNtFileRenamePosixSemantics;
    rename.root_directory = parent;
    rename.file_name_length = static_cast<ULONG>(name_bytes);
    std::memcpy(rename.file_name.data(), target_name.data(), name_bytes);
    IO_STATUS_BLOCK io_status{};
    const NTSTATUS status = api.set_information(
        file, &io_status, &rename, static_cast<ULONG>(buffer_size),
        kNtFileRenameInformationEx);
    if (status < 0) return Failure(StorageError::IoFailure, NativeError(status));
    return Success();
}

}  // namespace

class ReliableStorage::Impl final {
public:
    explicit Impl(std::wstring_view root_directory) noexcept {
        try {
            initialization_result_ = Initialize(root_directory);
        } catch (...) {
            initialization_result_ = ExceptionFailure();
        }
    }

    [[nodiscard]] StorageResult InitializationResult() const noexcept {
        return initialization_result_;
    }

    StorageReadResult Read(std::wstring_view relative_path, std::size_t max_bytes) {
        std::scoped_lock lock(operation_mutex_);
        if (!initialization_result_) return {initialization_result_, {}};

        NormalizedPath normalized;
        StorageResult result = NormalizeRelativePath(relative_path, normalized);
        if (!result) return {result, {}};

        UniqueHandle owned_parent;
        HANDLE parent{};
        result = ResolveParent(normalized, owned_parent, parent);
        if (!result) return {result, {}};

        UniqueHandle file;
        result = OpenRelative(
            parent, normalized.Leaf(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
            kShareAll, kNtFileOpen, kNtFileSequentialOnly, file);
        if (!result) return {result, {}};
        result = RequireRegularFile(file.get());
        if (!result) return {result, {}};

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size)) {
            return {Failure(StorageError::IoFailure, GetLastError()), {}};
        }
        if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > max_bytes ||
            static_cast<unsigned long long>(size.QuadPart) >
                (std::numeric_limits<std::size_t>::max)()) {
            return {Failure(StorageError::SizeLimitExceeded, ERROR_FILE_TOO_LARGE), {}};
        }

        StorageReadResult read;
        read.bytes.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset{};
        while (offset < read.bytes.size()) {
            const std::size_t remaining = read.bytes.size() - offset;
            const DWORD requested = static_cast<DWORD>((std::min)(
                remaining, kIoChunkBytes));
            DWORD count{};
            if (!ReadFile(file.get(), read.bytes.data() + offset, requested, &count, nullptr)) {
                return {Failure(StorageError::IoFailure, GetLastError(), offset), {}};
            }
            if (count == 0) break;
            offset += count;
        }
        read.bytes.resize(offset);
        read.result = Success(offset);
        return read;
    }

    StorageResult WriteAtomic(
        std::wstring_view relative_path, std::span<const std::byte> bytes) {
        std::scoped_lock lock(operation_mutex_);
        if (!initialization_result_) return initialization_result_;

        NormalizedPath normalized;
        StorageResult result = NormalizeRelativePath(relative_path, normalized);
        if (!result) return result;

        UniqueHandle owned_parent;
        HANDLE parent{};
        result = ResolveParent(normalized, owned_parent, parent);
        if (!result) return result;
        result = InspectTarget(parent, normalized.Leaf());
        if (!result) return result;

        UniqueHandle temp;
        result = CreateTemp(parent, temp);
        if (!result) return result;

        std::size_t offset{};
        while (offset < bytes.size()) {
            const std::size_t remaining = bytes.size() - offset;
            const DWORD requested = static_cast<DWORD>((std::min)(
                remaining, kIoChunkBytes));
            DWORD count{};
            if (!WriteFile(temp.get(), bytes.data() + offset, requested, &count, nullptr)) {
                const DWORD error = GetLastError();
                MarkDelete(temp.get());
                return Failure(StorageError::IoFailure, error, offset);
            }
            if (count == 0) {
                MarkDelete(temp.get());
                return Failure(StorageError::IoFailure, ERROR_WRITE_FAULT, offset);
            }
            offset += count;
        }

        if (!FlushFileBuffers(temp.get())) {
            const DWORD error = GetLastError();
            MarkDelete(temp.get());
            return Failure(StorageError::IoFailure, error, offset);
        }
        result = RenameRelative(temp.get(), parent, normalized.Leaf());
        if (!result) {
            MarkDelete(temp.get());
            result.bytes_processed = offset;
            return result;
        }
        if (!FlushFileBuffers(temp.get())) {
            return Failure(StorageError::IoFailure, GetLastError(), offset);
        }
        return Success(offset);
    }

    StorageResult Delete(std::wstring_view relative_path) {
        std::scoped_lock lock(operation_mutex_);
        if (!initialization_result_) return initialization_result_;

        NormalizedPath normalized;
        StorageResult result = NormalizeRelativePath(relative_path, normalized);
        if (!result) return result;

        UniqueHandle owned_parent;
        HANDLE parent{};
        result = ResolveParent(normalized, owned_parent, parent);
        if (!result) return result;

        UniqueHandle target;
        result = OpenRelative(
            parent, normalized.Leaf(), DELETE | FILE_READ_ATTRIBUTES, kShareAll,
            kNtFileOpen, 0, target);
        if (!result) return result;
        result = RequireRegularFile(target.get());
        if (!result) return result;

        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        if (!SetFileInformationByHandle(
                target.get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
            return Failure(StorageError::IoFailure, GetLastError());
        }
        return Success();
    }

private:
    StorageResult Initialize(std::wstring_view root_directory) {
        if (root_directory.empty() || HasEmbeddedNull(root_directory)) {
            return Failure(StorageError::InvalidRoot, ERROR_INVALID_PARAMETER);
        }

        const std::wstring supplied(root_directory);
        UniqueHandle root(CreateFileW(
            supplied.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
            kShareAll, nullptr, OPEN_EXISTING, kInspectFlags, nullptr));
        if (!root.valid()) return Failure(StorageError::InvalidRoot, GetLastError());

        StorageResult result = RequireDirectory(root.get());
        if (!result) {
            return Failure(
                result.error == StorageError::ReparsePoint
                    ? StorageError::ReparsePoint
                    : StorageError::InvalidRoot,
                result.win32_error);
        }
        root_handle_ = std::move(root);
        return Success();
    }

    StorageResult ResolveParent(
        const NormalizedPath& normalized, UniqueHandle& owned_parent,
        HANDLE& parent) const noexcept {
        parent = root_handle_.get();
        if (normalized.parent_length == 0) return Success();

        const std::wstring_view parents(
            normalized.relative.data(), normalized.parent_length);
        std::size_t start{};
        while (start < parents.size()) {
            const std::size_t end = parents.find(L'\\', start);
            const std::size_t length = end == std::wstring_view::npos
                ? parents.size() - start
                : end - start;

            UniqueHandle directory;
            StorageResult result = OpenRelative(
                parent, parents.substr(start, length),
                FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                kShareAll, kNtFileOpen, kNtFileDirectory, directory);
            if (!result) return result;
            result = RequireDirectory(directory.get());
            if (!result) return result;

            owned_parent = std::move(directory);
            parent = owned_parent.get();
            if (end == std::wstring_view::npos) break;
            start = end + 1;
        }
        return Success();
    }

    StorageResult InspectTarget(
        HANDLE parent, std::wstring_view target_name) const noexcept {
        UniqueHandle target;
        StorageResult result = OpenRelative(
            parent, target_name, FILE_READ_ATTRIBUTES, kShareAll, kNtFileOpen, 0, target);
        if (!result) {
            if (result.win32_error == ERROR_FILE_NOT_FOUND) return Success();
            return result;
        }
        return RequireRegularFile(target.get());
    }

    StorageResult CreateTemp(HANDLE parent, UniqueHandle& temp) const noexcept {
        static std::atomic<std::uint64_t> sequence{};
        for (std::uint32_t attempt = 0; attempt < 128; ++attempt) {
            const std::uint64_t token =
                sequence.fetch_add(1, std::memory_order_relaxed) ^ GetTickCount64() ^
                (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32U);
            std::array<wchar_t, 64> name{};
            const int count = swprintf_s(
                name.data(), name.size(), L".anomaly-tmp-%08lX-%016llX.tmp",
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned long long>(token));
            if (count <= 0) {
                return Failure(StorageError::InternalFailure, ERROR_INVALID_DATA);
            }

            StorageResult result = OpenRelative(
                parent, std::wstring_view(name.data(), static_cast<std::size_t>(count)),
                FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE, kShareAll,
                kNtFileCreate, 0, temp);
            if (result) return Success();
            if (result.win32_error != ERROR_FILE_EXISTS &&
                result.win32_error != ERROR_ALREADY_EXISTS) {
                return result;
            }
        }
        return Failure(StorageError::IoFailure, ERROR_FILE_EXISTS);
    }

    StorageResult initialization_result_{
        StorageError::InternalFailure, ERROR_GEN_FAILURE, 0};
    UniqueHandle root_handle_;
    std::mutex operation_mutex_;
};

ReliableStorage::ReliableStorage(std::wstring_view root_directory) noexcept {
    try {
        impl_ = std::make_unique<Impl>(root_directory);
    } catch (...) {
        impl_.reset();
    }
}

ReliableStorage::~ReliableStorage() = default;

StorageResult ReliableStorage::InitializationResult() const noexcept {
    if (!impl_) return Failure(StorageError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY);
    return impl_->InitializationResult();
}

StorageReadResult ReliableStorage::Read(
    std::wstring_view relative_path, std::size_t max_bytes) noexcept {
    if (!impl_) {
        return {Failure(StorageError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY), {}};
    }
    try {
        return impl_->Read(relative_path, max_bytes);
    } catch (...) {
        return {ExceptionFailure(), {}};
    }
}

StorageResult ReliableStorage::WriteAtomic(
    std::wstring_view relative_path, std::span<const std::byte> bytes) noexcept {
    if (!impl_) return Failure(StorageError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY);
    try {
        return impl_->WriteAtomic(relative_path, bytes);
    } catch (...) {
        return ExceptionFailure();
    }
}

StorageResult ReliableStorage::Delete(std::wstring_view relative_path) noexcept {
    if (!impl_) return Failure(StorageError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY);
    try {
        return impl_->Delete(relative_path);
    } catch (...) {
        return ExceptionFailure();
    }
}

}  // namespace anomaly
