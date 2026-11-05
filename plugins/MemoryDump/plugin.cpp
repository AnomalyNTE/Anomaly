// One-click live image dump for the running game module.
//
// The plugin executes inside the game process, so the whole module image is already mapped in
// this address space: no handle to open, no cross-process read, no external tool. It walks the
// image region by region, copies the committed pages in 1 MiB steps and streams them to disk,
// which keeps the peak working set at one chunk instead of one extra copy of the image.
//
// Two files come out of one click:
//   *.bin  the raw memory image, base .. base + SizeOfImage
//   *.exe  the same bytes with every section header's PointerToRawData rewritten to its
//          VirtualAddress, so a disassembler loads it as a memory image instead of a file image
//   metadata.json  pid, base, size, hash-free layout record and the section table
//
// The work runs on its own thread. The game tick only observes progress; the render callback only
// draws it. Nothing here touches game objects, so no Dispatcher lease is involved.

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/localization.hpp"

#include <windows.h>
#include <combaseapi.h>
#include <objbase.h>
#include <shobjidl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kChunkBytes = 1U << 20U;
constexpr std::size_t kPathCapacity = 1024U;
constexpr std::size_t kMaximumSections = 96U;
constexpr int kStopWaitMilliseconds = 5000;

enum class Phase { idle, running, finished, failed };

struct SectionRecord final {
    char name[9]{};
    std::uint32_t virtual_address{};
    std::uint32_t virtual_size{};
    std::uint32_t raw_size{};
    std::uint32_t raw_pointer{};
    std::uint32_t characteristics{};
};

struct Context final {
    const AnomalyHostApiV1* host{};
    anomaly::sdk::Localizer localizer;
    const AnomalyCoreServiceV1* core{};
    const AnomalyUiServiceV1* ui{};
    std::string default_root;
    std::array<char, kPathCapacity> output_path{};
    std::array<char, 128> file_stem{};
    std::string settings_path;

    std::mutex mutex;
    Phase phase{Phase::idle};
    std::string bin_path;
    std::string exe_path;
    std::string meta_path;
    std::string error;
    std::string module_name;
    std::uintptr_t base{};
    std::uint64_t image_size{};
    std::uint64_t bytes_written{};
    double elapsed_seconds{};
    std::atomic_bool request{false};
    std::atomic_bool cancel{false};
    std::atomic<std::uint64_t> progress{0};
    std::thread worker;
};

void LogLine(Context& context, const std::uint32_t level, const std::string_view message) {
    if (context.core == nullptr || context.core->log == nullptr) return;
    context.core->log(context.core->user, level, anomaly::sdk::StringView(message));
}

bool UiReady(const AnomalyUiServiceV1* ui) noexcept {
    return ui != nullptr && ui->begin_window != nullptr && ui->end_window != nullptr &&
           ui->text != nullptr && ui->button_enabled != nullptr;
}

std::string PluginDirectory(Context& context) {
    std::array<char, kPathCapacity> buffer{};
    std::size_t size = buffer.size();
    if (context.core == nullptr || context.core->plugin_directory == nullptr) return {};
    const AnomalyStatusV1 status =
        context.core->plugin_directory(context.core->user, buffer.data(), &size);
    if (status.code == ANOMALY_STATUS_V1_OK && size > 1U) {
        return std::string(buffer.data(), size - 1U);
    }
    return {};
}

std::string FallbackDirectory() {
    std::array<wchar_t, kPathCapacity> module_path{};
    const DWORD length = GetModuleFileNameW(nullptr, module_path.data(),
                                            static_cast<DWORD>(module_path.size()));
    if (length == 0 || length >= module_path.size()) return ".";
    std::wstring path(module_path.data(), length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return ".";
    path.resize(slash);
    std::string narrow(path.size() * 3U + 1U, '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, path.c_str(),
                                            static_cast<int>(path.size()), narrow.data(),
                                            static_cast<int>(narrow.size()), nullptr, nullptr);
    if (written <= 0) return ".";
    narrow.resize(static_cast<std::size_t>(written));
    return narrow;
}

// Every path handed to the Win32 layer is built as UTF-16 so a game directory outside the ANSI
// code page still resolves.
std::wstring Widen(const std::string_view utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

std::string Narrow(const std::wstring_view wide) {
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (length <= 0) return {};
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(),
                        length, nullptr, nullptr);
    return narrow;
}

// The folder picker is a shell dialog, so the click has to enter COM. The render callback is not
// guaranteed to be on an apartment thread, hence the guard: a changed mode is still usable.
class ComApartment final {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }
    [[nodiscard]] bool Usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    HRESULT result_{};
};

template <typename Interface>
class ComPtr final {
public:
    ComPtr() noexcept = default;
    ~ComPtr() {
        if (value_ != nullptr) value_->Release();
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    [[nodiscard]] Interface* operator->() const noexcept { return value_; }
    [[nodiscard]] Interface** put() noexcept { return &value_; }
    [[nodiscard]] Interface* get() const noexcept { return value_; }

private:
    Interface* value_{};
};

// Opens on the folder that is currently configured, and only accepts an existing filesystem path.
bool ChooseFolder(const std::string_view current_utf8, std::string& chosen_utf8) {
    ComApartment apartment;
    if (!apartment.Usable()) return false;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return false;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        static_cast<void>(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                             FOS_PATHMUSTEXIST));
    }
    const std::wstring current = Widen(current_utf8);
    if (!current.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(
                SHCreateItemFromParsingName(current.c_str(), nullptr, IID_PPV_ARGS(folder.put())))) {
            static_cast<void>(dialog->SetFolder(folder.get()));
        }
    }
    if (FAILED(dialog->Show(nullptr))) return false;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(selected.put()))) return false;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) return false;
    chosen_utf8 = Narrow(raw);
    CoTaskMemFree(raw);
    return !chosen_utf8.empty();
}

std::string JoinPath(const std::string_view root, const std::string_view leaf) {
    std::string path(root);
    if (!path.empty() && path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path.append(leaf);
    return path;
}

std::string Timestamp() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u_%02u%02u%02u",
                  local.wYear, local.wMonth, local.wDay,
                  local.wHour, local.wMinute, local.wSecond);
    return buffer;
}

// `CreateDirectoryW` only creates one level, so the configured folder is built from the drive root
// down. An existing path is not an error; only a real failure stops the chain.
bool EnsureDirectory(const std::string_view utf8_path) {
    const std::wstring wide = Widen(utf8_path);
    if (wide.empty()) return false;
    std::wstring partial;
    partial.reserve(wide.size());
    for (std::size_t index = 0; index < wide.size(); ++index) {
        const wchar_t character = wide[index];
        partial.push_back(character);
        const bool separator = character == L'\\' || character == L'/';
        const bool trailing = index + 1U == wide.size();
        if (!separator && !trailing) continue;
        if (partial.size() <= 3U && partial.size() >= 2U && partial[1] == L':') continue;
        CreateDirectoryW(partial.c_str(), nullptr);
    }
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void SaveSettings(const Context& context) {
    if (context.settings_path.empty()) return;
    const std::wstring wide = Widen(context.settings_path);
    if (wide.empty()) return;
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string text = "output=";
    text.append(context.output_path.data());
    text.append("\nname=");
    text.append(context.file_stem.data());
    text.push_back('\n');
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

void LoadSettings(const Context& context, std::string& output, std::string& name) {
    output.clear();
    name.clear();
    if (context.settings_path.empty()) return;
    const std::wstring wide = Widen(context.settings_path);
    if (wide.empty()) return;
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::array<char, kPathCapacity * 2U> buffer{};
    DWORD read = 0;
    const bool ok = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size() - 1U), &read,
                             nullptr) != 0;
    CloseHandle(file);
    if (!ok || read == 0) return;
    std::string_view text(buffer.data(), read);
    constexpr std::string_view kOutputPrefix = "output=";
    constexpr std::string_view kNamePrefix = "name=";
    while (!text.empty()) {
        const std::size_t line_end = text.find('\n');
        const std::string_view line = text.substr(0, line_end);
        if (line.rfind(kOutputPrefix, 0) == 0) {
            output = std::string(line.substr(kOutputPrefix.size()));
        } else if (line.rfind(kNamePrefix, 0) == 0) {
            name = std::string(line.substr(kNamePrefix.size()));
        }
        if (line_end == std::string_view::npos) break;
        text.remove_prefix(line_end + 1U);
    }
    const auto trim = [](std::string& value) {
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    };
    trim(output);
    trim(name);
}

// The leaf file name of the module that hosts this plugin, as UTF-8.
std::string ModuleLeafName() {
    std::array<wchar_t, kPathCapacity> wide{};
    const DWORD length = GetModuleFileNameW(nullptr, wide.data(), static_cast<DWORD>(wide.size()));
    if (length == 0) return "HTGame.exe";
    std::wstring path(wide.data(), length);
    const auto slash = path.find_last_of(L"\\/");
    const std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1U);
    std::string narrow = Narrow(leaf);
    return narrow.empty() ? std::string("HTGame.exe") : narrow;
}

bool DescribeImage(const HMODULE module, std::vector<SectionRecord>& sections,
                   std::uint64_t& image_size) {
    if (module == nullptr) return false;
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    image_size = nt->OptionalHeader.SizeOfImage;
    const std::size_t count = nt->FileHeader.NumberOfSections;
    sections.clear();
    sections.reserve(count < kMaximumSections ? count : kMaximumSections);
    const auto* table = IMAGE_FIRST_SECTION(nt);
    for (std::size_t index = 0; index < count && index < kMaximumSections; ++index) {
        SectionRecord record{};
        std::memcpy(record.name, table[index].Name, 8);
        record.virtual_address = table[index].VirtualAddress;
        record.virtual_size = table[index].Misc.VirtualSize;
        record.raw_size = table[index].SizeOfRawData;
        record.raw_pointer = table[index].PointerToRawData;
        record.characteristics = table[index].Characteristics;
        sections.push_back(record);
    }
    return true;
}

// Copies the committed part of the image into the destination chunk by chunk. A region that is
// not committed, or is guarded, is left as zeroes and recorded so the dump never claims data it
// could not read.
bool CopyImage(Context& context, const HANDLE file, const std::uintptr_t base,
               const std::uint64_t image_size, std::string& error) {
    std::vector<std::uint8_t> buffer(kChunkBytes);
    std::uint64_t offset = 0;
    while (offset < image_size) {
        if (context.cancel.load(std::memory_order_relaxed)) {
            error = "cancelled";
            return false;
        }
        MEMORY_BASIC_INFORMATION info{};
        const auto address = reinterpret_cast<LPCVOID>(base + offset);
        if (VirtualQuery(address, &info, sizeof(info)) == 0) {
            error = "VirtualQuery failed";
            return false;
        }
        std::size_t region = info.RegionSize;
        if (region > image_size - offset) region = static_cast<std::size_t>(image_size - offset);
        const bool readable = info.State == MEM_COMMIT &&
                              (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
        std::size_t consumed = 0;
        while (consumed < region) {
            const std::size_t span = (region - consumed) < kChunkBytes ? (region - consumed)
                                                                      : kChunkBytes;
            std::memset(buffer.data(), 0, span);
            if (readable) {
                std::memcpy(buffer.data(), reinterpret_cast<const void*>(base + offset + consumed),
                            span);
            }
            DWORD written = 0;
            if (WriteFile(file, buffer.data(), static_cast<DWORD>(span), &written, nullptr) == 0 ||
                written != span) {
                error = "WriteFile failed";
                return false;
            }
            consumed += span;
            context.progress.store(offset + consumed, std::memory_order_relaxed);
        }
        offset += region;
    }
    return true;
}

// Rewrites every section header so PointerToRawData equals VirtualAddress. A disassembler then
// maps the file exactly where the loader had mapped the image.
bool RemapSections(const std::string_view exe_path, const std::vector<SectionRecord>& sections,
                   std::string& error) {
    const std::wstring wide = Widen(exe_path);
    if (wide.empty()) {
        error = "path conversion failed";
        return false;
    }
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "section remap open failed";
        return false;
    }
    bool ok = true;
    std::vector<std::uint8_t> header(0x400);
    DWORD read = 0;
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    if (ReadFile(file, header.data(), static_cast<DWORD>(header.size()), &read, nullptr) == 0 ||
        read < 0x40) {
        ok = false;
        error = "section remap read failed";
    }
    std::size_t table_offset = 0;
    if (ok) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(header.data());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(header.data() + dos->e_lfanew);
        table_offset = reinterpret_cast<const std::uint8_t*>(IMAGE_FIRST_SECTION(nt)) - header.data();
    }
    for (std::size_t index = 0; ok && index < sections.size(); ++index) {
        const std::uint32_t value = sections[index].virtual_address;
        const LONG offset = static_cast<LONG>(table_offset + index * 40U + 20U);
        if (SetFilePointer(file, offset, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
            ok = false;
            error = "section remap seek failed";
            break;
        }
        DWORD written = 0;
        if (WriteFile(file, &value, sizeof(value), &written, nullptr) == 0 ||
            written != sizeof(value)) {
            ok = false;
            error = "section remap write failed";
        }
    }
    CloseHandle(file);
    return ok;
}

bool WriteMetadata(const std::string_view meta_path, const std::string_view stamp,
                   const std::uintptr_t base, const std::uint64_t image_size,
                   const std::string_view module_label,
                   const std::vector<SectionRecord>& sections, std::string& error) {
    std::string text;
    text.reserve(2048U + sections.size() * 160U);
    char scratch[512]{};
    text.append("{\n");
    std::snprintf(scratch, sizeof(scratch),
                  "  \"pid\": %lu,\n  \"base\": \"0x%llx\",\n  \"size\": %llu,\n"
                  "  \"chunk\": %zu,\n  \"dumped\": \"%s\",\n"
                  "  \"source\": \"anomaly MemoryDump plugin (in-process image copy)\",\n"
                  "  \"module\": \"%.*s\",\n  \"sections\": [\n",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(base),
                  static_cast<unsigned long long>(image_size), kChunkBytes,
                  std::string(stamp).c_str(),
                  static_cast<int>(module_label.size()), module_label.data());
    text.append(scratch);
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const SectionRecord& section = sections[index];
        std::snprintf(scratch, sizeof(scratch),
                      "    {\"index\": %zu, \"name\": \"%s\", \"virtual_size\": %u, "
                      "\"virtual_address\": %u, \"raw_size\": %u, \"raw_pointer\": %u, "
                      "\"characteristics\": %u}%s\n",
                      index, section.name, section.virtual_size, section.virtual_address,
                      section.raw_size, section.raw_pointer, section.characteristics,
                      index + 1U == sections.size() ? "" : ",");
        text.append(scratch);
    }
    text.append("  ],\n  \"unreadable_ranges\": []\n}\n");

    const std::wstring wide = Widen(meta_path);
    if (wide.empty()) {
        error = "path conversion failed";
        return false;
    }
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "metadata open failed";
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                              nullptr) != 0 &&
                    written == text.size();
    CloseHandle(file);
    if (!ok) error = "metadata write failed";
    return ok;
}

void RunDump(Context* context) {
    const auto started = std::chrono::steady_clock::now();
    std::string error;

    const HMODULE module = GetModuleHandleW(nullptr);
    std::vector<SectionRecord> sections;
    std::uint64_t image_size = 0;
    if (!DescribeImage(module, sections, image_size)) {
        std::scoped_lock lock(context->mutex);
        context->phase = Phase::failed;
        context->error = "the host module is not a 64-bit PE image";
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(module);

    std::string module_label("HTGame.exe");
    {
        std::array<wchar_t, kPathCapacity> wide{};
        const DWORD length = GetModuleFileNameW(nullptr, wide.data(),
                                                static_cast<DWORD>(wide.size()));
        if (length > 0) {
            std::wstring path(wide.data(), length);
            const auto slash = path.find_last_of(L"\\/");
            const std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1U);
            std::string narrow(leaf.size() * 3U + 1U, '\0');
            const int written = WideCharToMultiByte(CP_UTF8, 0, leaf.c_str(),
                                                    static_cast<int>(leaf.size()), narrow.data(),
                                                    static_cast<int>(narrow.size()), nullptr, nullptr);
            if (written > 0) {
                narrow.resize(static_cast<std::size_t>(written));
                module_label = narrow;
            }
        }
    }

    const std::string stamp = Timestamp();
    std::string name_prefix;
    std::string out_dir;
    {
        std::scoped_lock lock(context->mutex);
        name_prefix = context->file_stem.data();
        out_dir = context->output_path.data();
    }
    if (name_prefix.empty()) name_prefix = "HTGame_live";
    if (out_dir.empty()) out_dir = context->default_root.empty() ? "." : context->default_root;
    EnsureDirectory(out_dir);
    char leaf[320]{};
    std::snprintf(leaf, sizeof(leaf), "%s_%s_pid%lu", name_prefix.c_str(), stamp.c_str(),
                  static_cast<unsigned long>(GetCurrentProcessId()));
    const std::string stem = out_dir;
    const std::string bin_path = JoinPath(stem, std::string(leaf) + ".bin");
    const std::string exe_path = JoinPath(stem, std::string(leaf) + ".exe");
    const std::string meta_path = JoinPath(stem, "metadata.json");
    {
        std::scoped_lock lock(context->mutex);
        context->base = base;
        context->image_size = image_size;
        context->module_name = module_label;
        context->bin_path = bin_path;
        context->exe_path = exe_path;
        context->meta_path = meta_path;
    }

    const std::wstring wide_bin = Widen(bin_path);
    if (wide_bin.empty()) {
        std::scoped_lock lock(context->mutex);
        context->phase = Phase::failed;
        context->error = "dump path conversion failed";
        return;
    }
    const HANDLE file = CreateFileW(wide_bin.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::scoped_lock lock(context->mutex);
        context->phase = Phase::failed;
        context->error = "dump file could not be created";
        return;
    }
    const bool copied = CopyImage(*context, file, base, image_size, error);
    CloseHandle(file);
    if (!copied) {
        {
            std::scoped_lock lock(context->mutex);
            context->phase = Phase::failed;
            context->error = error;
        }
        DeleteFileW(wide_bin.c_str());
        return;
    }

    const std::wstring wide_exe = Widen(exe_path);
    if (wide_exe.empty() || CopyFileW(wide_bin.c_str(), wide_exe.c_str(), FALSE) == 0) {
        std::scoped_lock lock(context->mutex);
        context->phase = Phase::failed;
        context->error = "the memory-image copy could not be written";
        return;
    }
    if (!RemapSections(exe_path, sections, error)) {
        std::scoped_lock lock(context->mutex);
        context->phase = Phase::failed;
        context->error = error;
        return;
    }

    if (!WriteMetadata(meta_path, stamp, base, image_size, module_label, sections, error)) {
        std::scoped_lock lock(context->mutex);
        context->phase = Phase::failed;
        context->error = error;
        return;
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::scoped_lock lock(context->mutex);
    context->phase = Phase::finished;
    context->bytes_written = image_size;
    context->elapsed_seconds = seconds;
    context->error.clear();
}

void DrawStatus(Context& context, const AnomalyUiServiceV1& ui, const Phase phase) {
    const auto text = [&ui](const std::string& value) {
        if (!value.empty()) ui.text(ui.user, anomaly::sdk::StringView(value));
    };
    std::array<char, 64> scratch{};
    text(context.localizer.Format("label.module", "Module", {}));
    ui.same_line(ui.user, 0.0F, 8.0F);
    std::snprintf(scratch.data(), scratch.size(), "%s  0x%llx", context.module_name.c_str(),
                  static_cast<unsigned long long>(context.base));
    ui.text(ui.user, anomaly::sdk::StringView(std::string_view(scratch.data())));

    std::snprintf(scratch.data(), scratch.size(), "%.2f MiB", static_cast<double>(context.image_size) / (1024.0 * 1024.0));
    text(context.localizer.Format("label.size", "Image size", {}));
    ui.same_line(ui.user, 0.0F, 8.0F);
    ui.text(ui.user, anomaly::sdk::StringView(std::string_view(scratch.data())));

    switch (phase) {
    case Phase::idle:
        text(context.localizer.Text("status.idle", "Ready. The dump runs on its own thread."));
        break;
    case Phase::running: {
        const double done = static_cast<double>(context.progress.load(std::memory_order_relaxed));
        std::snprintf(scratch.data(), scratch.size(), "%.1f%%",
                      context.image_size == 0 ? 0.0 : done * 100.0 / static_cast<double>(context.image_size));
        const std::array arguments{std::string_view(scratch.data())};
        text(context.localizer.Format("status.running", "Dumping: {0}", arguments));
        break;
    }
    case Phase::finished: {
        std::snprintf(scratch.data(), scratch.size(), "%.1fs", context.elapsed_seconds);
        const std::array arguments{std::string_view(scratch.data()), std::string_view(context.bin_path)};
        text(context.localizer.Format("status.done", "Finished in {0}. Raw image: {1}", arguments));
        break;
    }
    case Phase::failed: {
        const std::array arguments{std::string_view(context.error)};
        text(context.localizer.Format("status.failed", "Failed: {0}", arguments));
        break;
    }
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {"host or context is null", 23}};
    }
    auto context = std::make_unique<Context>();
    context->host = host;
    context->localizer = anomaly::sdk::Localizer(host);
    const auto core = anomaly::sdk::Host(host).Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    context->core = core.get();
    const std::string plugin_directory = PluginDirectory(*context);
    context->settings_path = plugin_directory.empty()
                                 ? std::string()
                                 : JoinPath(plugin_directory, "settings.txt");
    std::string root = plugin_directory;
    if (root.empty()) root = FallbackDirectory();
    context->default_root = JoinPath(root, "dumps");
    std::string saved_output;
    std::string saved_name;
    LoadSettings(*context, saved_output, saved_name);
    std::string initial = saved_output.empty() ? context->default_root : saved_output;
    if (initial.size() >= context->output_path.size()) {
        initial = context->default_root;
    }
    std::snprintf(context->output_path.data(), context->output_path.size(), "%s", initial.c_str());
    std::snprintf(context->file_stem.data(), context->file_stem.size(), "%s",
                  saved_name.empty() ? "HTGame_live" : saved_name.c_str());
    EnsureDirectory(initial);
    *plugin_context = context.release();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {"plugin context is null", 22}};
    }
    const auto ui = anomaly::sdk::Host(context->host).Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    context->ui = ui.get();
    // Describe the image now, so the window shows the module and its size before the first dump
    // instead of zeroes. The dump re-reads the same values on its own thread.
    {
        std::vector<SectionRecord> sections;
        std::uint64_t image_size = 0;
        const HMODULE module = GetModuleHandleW(nullptr);
        if (DescribeImage(module, sections, image_size)) {
            std::scoped_lock lock(context->mutex);
            context->base = reinterpret_cast<std::uintptr_t>(module);
            context->image_size = image_size;
            context->module_name = ModuleLeafName();
        }
    }
    LogLine(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "MemoryDump ready: one click writes the live image, the memory-image copy and metadata");
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t deadline_milliseconds) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return anomaly::sdk::Ok();
    context->cancel.store(true, std::memory_order_relaxed);
    if (!context->worker.joinable()) return anomaly::sdk::Ok();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(deadline_milliseconds == 0
                                                        ? kStopWaitMilliseconds
                                                        : static_cast<int>(deadline_milliseconds));
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::scoped_lock lock(context->mutex);
            if (context->phase != Phase::running) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (context->worker.joinable()) context->worker.join();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    context->cancel.store(true, std::memory_order_relaxed);
    if (context->worker.joinable()) context->worker.join();
    delete context;
}

void ANOMALY_CALL Update(void* plugin_context, double /*delta_seconds*/) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    if (!context->request.exchange(false, std::memory_order_relaxed)) return;
    {
        std::scoped_lock lock(context->mutex);
        if (context->phase == Phase::running) return;
        context->phase = Phase::running;
        context->bytes_written = 0;
        context->elapsed_seconds = 0;
        context->error.clear();
        context->progress.store(0, std::memory_order_relaxed);
    }
    if (context->worker.joinable()) context->worker.join();
    context->cancel.store(false, std::memory_order_relaxed);
    context->worker = std::thread(RunDump, context);
    LogLine(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "MemoryDump started");
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* supplied_ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    const AnomalyUiServiceV1* ui = UiReady(supplied_ui) ? supplied_ui : context->ui;
    if (!UiReady(ui)) return;

    Phase phase{};
    std::string bin_path;
    std::string error;
    std::string module_name;
    std::uintptr_t base{};
    std::uint64_t image_size{};
    double elapsed{};
    {
        std::scoped_lock lock(context->mutex);
        phase = context->phase;
        bin_path = context->bin_path;
        error = context->error;
        module_name = context->module_name;
        base = context->base;
        image_size = context->image_size;
        elapsed = context->elapsed_seconds;
    }
    // DrawStatus reads the fields through the context it is given; keep the copy consistent.
    Context view{};
    view.module_name = module_name;
    view.base = base;
    view.image_size = image_size;
    view.bin_path = bin_path;
    view.error = error;
    view.elapsed_seconds = elapsed;
    view.localizer = context->localizer;
    view.progress.store(context->progress.load(std::memory_order_relaxed), std::memory_order_relaxed);

    int open = 1;
    // A window title is plain text as well; it carries no dynamic content that would need a
    // pinned identity, so it must not gain a "###" suffix either.
    const std::string title = context->localizer.Text("window.title", "Memory Dump");
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    DrawStatus(view, *ui, phase);
    ui->separator(ui->user);

    const bool idle = phase != Phase::running;

    // Output folder: the path is shown next to its label; the picker is the only control that
    // changes it. Browsing the folder is offered only once there is something in it, so the two
    // actions never sit next to each other doing nothing.
    // The line is plain text, so it uses Text(): a Label() here would print its "###id" suffix,
    // because text() does not split display text from the widget identity.
    std::string folder_line = context->localizer.Text("label.output", "Output folder");
    folder_line.append("  ");
    folder_line.append(context->output_path.data());
    ui->text(ui->user, anomaly::sdk::StringView(folder_line));
    // Widget labels here are plain text on purpose. Every control in this window is stateless
    // (buttons fire once, the field is the only stateful one and its caption never changes), so
    // nothing needs a pinned identity, and a "###id" suffix would either be split off by the widget
    // or, as observed with button_enabled, drawn verbatim.
    const std::string choose_label =
        context->localizer.Text("action.choose_folder", "Choose...");
    if (ui->button_enabled(ui->user, anomaly::sdk::StringView(choose_label), 0.0F, 0.0F,
                           idle ? 1 : 0) != 0) {
        std::string chosen;
        if (ChooseFolder(context->output_path.data(), chosen)) {
            {
                std::scoped_lock lock(context->mutex);
                std::snprintf(context->output_path.data(), context->output_path.size(), "%s",
                              chosen.c_str());
            }
            EnsureDirectory(chosen);
            SaveSettings(*context);
        }
    }

    // File name: the prefix the timestamp and pid are appended to. The edit buffer is local so the
    // worker never observes a half-written name while it starts.
    std::array<char, 128> stem{};
    {
        std::scoped_lock lock(context->mutex);
        std::memcpy(stem.data(), context->file_stem.data(), stem.size());
    }
    const std::string name_label =
        context->localizer.Text("label.filename", "File name prefix");
    if (ui->input_text(ui->user, anomaly::sdk::StringView(name_label), stem.data(), stem.size(),
                       ANOMALY_UI_TEXT_INPUT_V1_NONE) != 0) {
        {
            std::scoped_lock lock(context->mutex);
            std::memcpy(context->file_stem.data(), stem.data(), context->file_stem.size());
        }
        SaveSettings(*context);
    }

    ui->separator(ui->user);
    const std::string label = context->localizer.Text("action.dump", "Dump module image");
    if (ui->button_enabled(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F,
                           idle ? 1 : 0) != 0) {
        context->request.store(true, std::memory_order_relaxed);
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {"descriptor is invalid", 22}};
    }
    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.memory-dump"),
        anomaly::sdk::StringView("Memory Dump"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.0.0"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw};
    return anomaly::sdk::Ok();
}
