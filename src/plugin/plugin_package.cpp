#include "anomaly/plugin_package.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <system_error>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

PluginPackagePathResult Failure(PluginPackageError error, std::string message) {
    return {error, {}, std::move(message)};
}

std::wstring FinalPath(HANDLE handle) {
    const DWORD required = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0) return {};
    std::wstring result(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, result.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= required) return {};
    result.resize(written);
    while (result.size() > 4 && (result.back() == L'\\' || result.back() == L'/')) {
        result.pop_back();
    }
    return result;
}

std::wstring FoldPath(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool IsBelow(std::wstring_view root, std::wstring_view candidate) {
    if (candidate.size() <= root.size() || !candidate.starts_with(root)) return false;
    return candidate[root.size()] == L'\\';
}

struct UniqueHandle final {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~UniqueHandle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : value(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE Release() noexcept {
        const HANDLE released = value;
        value = INVALID_HANDLE_VALUE;
        return released;
    }
};

bool HasDllExtension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension == L".dll";
}

}  // namespace

PluginPackagePathResult ValidatePluginPackageRelativePath(
    std::string_view utf8_path, bool require_dll) {
    if (utf8_path.empty()) return Failure(PluginPackageError::EmptyPath, "path is empty");
    if (utf8_path.starts_with("\\\\?\\") || utf8_path.starts_with("\\\\.\\") ||
        utf8_path.starts_with("\\??\\") || utf8_path.starts_with("//?/") ||
        utf8_path.starts_with("//./")) {
        return Failure(PluginPackageError::DevicePath, "device paths are not package paths");
    }
    if (utf8_path.front() == '\\' || utf8_path.front() == '/') {
        return Failure(PluginPackageError::AbsolutePath, "rooted paths are not package paths");
    }
    if (utf8_path.size() >= 2 && std::isalpha(static_cast<unsigned char>(utf8_path[0])) != 0 &&
        utf8_path[1] == ':') {
        return Failure(
            PluginPackageError::DriveRelativePath, "drive-qualified paths are not package paths");
    }

    std::string normalized;
    normalized.reserve(utf8_path.size());
    std::size_t segment_start{};
    for (std::size_t index = 0; index <= utf8_path.size(); ++index) {
        const bool boundary = index == utf8_path.size() || utf8_path[index] == '/' ||
            utf8_path[index] == '\\';
        if (!boundary) continue;
        const std::string_view segment = utf8_path.substr(segment_start, index - segment_start);
        if (segment.empty()) {
            return Failure(PluginPackageError::EmptySegment, "path contains an empty segment");
        }
        if (segment == "..") {
            return Failure(PluginPackageError::ParentTraversal, "parent traversal is not allowed");
        }
        if (segment == "." || segment.find(':') != std::string_view::npos ||
            segment.back() == '.' || segment.back() == ' ') {
            return Failure(
                PluginPackageError::InvalidSegment, "path contains a non-canonical segment");
        }
        for (const unsigned char ch : segment) {
            if (ch < 0x20 || ch == 0x7f || ch == '<' || ch == '>' || ch == '"' || ch == '|' ||
                ch == '?' || ch == '*') {
                return Failure(
                    PluginPackageError::InvalidSegment, "path contains a reserved character");
            }
        }
        if (!normalized.empty()) normalized.push_back('\\');
        normalized.append(segment);
        segment_start = index + 1;
    }

    std::filesystem::path result;
    try {
        const std::u8string encoded(
            reinterpret_cast<const char8_t*>(normalized.data()), normalized.size());
        result = std::filesystem::path(encoded);
    } catch (const std::exception&) {
        return Failure(PluginPackageError::InvalidSegment, "path is not valid UTF-8");
    }
    if (result.empty() || result.is_absolute() || result.has_root_name() || result.has_root_directory()) {
        return Failure(PluginPackageError::AbsolutePath, "path is not relative");
    }
    if (require_dll && !HasDllExtension(result)) {
        return Failure(PluginPackageError::InvalidExtension, "plugin entry must use .dll");
    }
    return {PluginPackageError::None, std::move(result), {}};
}

PluginPackagePathResult OpenConfinedPluginPackageFile(
    const std::filesystem::path& package_root,
    std::string_view utf8_relative_path,
    bool require_dll,
    HANDLE* opened_file) {
    if (opened_file != nullptr) *opened_file = INVALID_HANDLE_VALUE;
    PluginPackagePathResult lexical =
        ValidatePluginPackageRelativePath(utf8_relative_path, require_dll);
    if (!lexical.Ok()) return lexical;

    UniqueHandle root(CreateFileW(
        package_root.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (root.value == INVALID_HANDLE_VALUE) {
        return Failure(PluginPackageError::RootUnavailable, "package root could not be opened");
    }
    FILE_ATTRIBUTE_TAG_INFO root_attributes{};
    if (!GetFileInformationByHandleEx(
            root.value, FileAttributeTagInfo, &root_attributes, sizeof(root_attributes))) {
        return Failure(PluginPackageError::IoFailure, "package root attributes are unavailable");
    }
    if ((root_attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Failure(PluginPackageError::ReparsePoint, "package root is a reparse point");
    }
    if ((root_attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return Failure(PluginPackageError::NotDirectory, "package root is not a directory");
    }
    const std::wstring root_final = FoldPath(FinalPath(root.value));
    if (root_final.empty()) {
        return Failure(PluginPackageError::IoFailure, "package root identity is unavailable");
    }

    std::filesystem::path current = package_root;
    std::vector<std::filesystem::path> segments;
    for (const auto& segment : lexical.path) segments.push_back(segment);
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const bool last = index + 1 == segments.size();
        current /= segments[index];
        UniqueHandle target(CreateFileW(
            current.c_str(), FILE_READ_ATTRIBUTES | (last ? GENERIC_READ : FILE_LIST_DIRECTORY),
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr));
        if (target.value == INVALID_HANDLE_VALUE) {
            return Failure(PluginPackageError::TargetUnavailable, "package target is unavailable");
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(
                target.value, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
            return Failure(PluginPackageError::IoFailure, "package target attributes are unavailable");
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return Failure(PluginPackageError::ReparsePoint, "package path contains a reparse point");
        }
        const bool directory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if ((!last && !directory) || (last && directory)) {
            return Failure(
                last ? PluginPackageError::NotRegularFile : PluginPackageError::NotDirectory,
                last ? "package target is not a regular file" : "package component is not a directory");
        }
        const std::wstring final_path = FoldPath(FinalPath(target.value));
        if (final_path.empty() || !IsBelow(root_final, final_path)) {
            return Failure(
                PluginPackageError::OutsidePackageRoot, "package target resolves outside its root");
        }
        if (last) {
            if (opened_file != nullptr) *opened_file = target.Release();
            return {PluginPackageError::None, std::filesystem::path(final_path), {}};
        }
    }
    return Failure(PluginPackageError::TargetUnavailable, "package target is unavailable");
}

}  // namespace anomaly
