#include "anomaly/launcher/configuration.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace anomaly::launcher {
namespace {

constexpr std::uint64_t kMaximumConfigurationBytes = 64U * 1024U;
constexpr std::wstring_view kGameExecutableName = L"HTGame.exe";
constexpr std::wstring_view kMainlandLauncherExecutableName = L"NTELauncher.exe";
constexpr std::wstring_view kGlobalLauncherExecutableName = L"NTEGlobalLauncher.exe";

std::wstring_view LauncherExecutableName(const NteClient client) noexcept {
    return client == NteClient::Global
        ? kGlobalLauncherExecutableName : kMainlandLauncherExecutableName;
}

class RegistryKey final {
public:
    RegistryKey() = default;
    ~RegistryKey() {
        if (value_ != nullptr) RegCloseKey(value_);
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    [[nodiscard]] HKEY* Put() noexcept { return &value_; }
    [[nodiscard]] HKEY Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    HKEY value_{};
};

struct SystemLocations final {
    std::vector<std::filesystem::path> roots;
    std::vector<std::filesystem::path> launcher_executables;
};

std::wstring Fold(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool NameEquals(const std::filesystem::path& path, const std::wstring_view expected) {
    return Fold(path.filename().wstring()) == Fold(std::wstring(expected));
}

bool IsRegularFile(const std::filesystem::path& path) noexcept {
    if (path.empty()) return false;
    std::error_code error;
    const bool regular = std::filesystem::is_regular_file(path, error);
    return regular && !error;
}

std::string WideUtf8(const std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::system_error(GetLastError(), std::system_category());
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        throw std::system_error(GetLastError(), std::system_category());
    }
    return result;
}

std::wstring Utf8Wide(const std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) throw std::system_error(GetLastError(), std::system_category());
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size) != size) {
        throw std::system_error(GetLastError(), std::system_category());
    }
    return result;
}

std::string ReadText(const std::filesystem::path& path, DWORD& win32_error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        win32_error = GetLastError();
        if (win32_error == ERROR_SUCCESS) win32_error = ERROR_OPEN_FAILED;
        return {};
    }
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > kMaximumConfigurationBytes) {
        win32_error = ERROR_FILE_TOO_LARGE;
        return {};
    }
    std::string result(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (!input && !input.eof()) {
        win32_error = ERROR_READ_FAULT;
        return {};
    }
    win32_error = ERROR_SUCCESS;
    return result;
}

std::optional<std::wstring> ReadRegistryString(
    const HKEY key, const wchar_t* value_name) {
    DWORD size{};
    if (RegGetValueW(
            key, nullptr, value_name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    DWORD type{};
    if (RegGetValueW(
            key, nullptr, value_name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            &type, value.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(size / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    if (type == REG_EXPAND_SZ && !value.empty()) {
        const DWORD expanded_size = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (expanded_size != 0) {
            std::wstring expanded(expanded_size, L'\0');
            if (ExpandEnvironmentStringsW(
                    value.c_str(), expanded.data(), expanded_size) == expanded_size) {
                while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
                value = std::move(expanded);
            }
        }
    }
    return value.empty() ? std::nullopt : std::optional<std::wstring>(std::move(value));
}

std::filesystem::path ExecutableFromCommand(std::wstring value) {
    if (value.empty()) return {};
    if (value.front() == L'"') {
        const std::size_t closing = value.find(L'"', 1);
        return closing == std::wstring::npos
            ? std::filesystem::path{} : std::filesystem::path(value.substr(1, closing - 1));
    }
    const std::size_t comma = value.find(L',');
    if (comma != std::wstring::npos) value.resize(comma);
    const std::size_t executable = Fold(value).find(L".exe");
    if (executable != std::wstring::npos) value.resize(executable + 4);
    return std::filesystem::path(value);
}

bool LooksLikeNte(const std::wstring_view value) {
    const std::wstring folded = Fold(std::wstring(value));
    return folded.find(L"neverness") != std::wstring::npos ||
        folded.find(L"ntegloballauncher") != std::wstring::npos ||
        folded.find(L"nte global launcher") != std::wstring::npos ||
        folded.find(L"ntelauncher") != std::wstring::npos ||
        folded.find(L"nte launcher") != std::wstring::npos;
}

void CollectAppPath(
    SystemLocations& result, const HKEY hive, const REGSAM registry_view,
    const NteClient client) {
    RegistryKey key;
    const std::wstring subkey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" +
        std::wstring(LauncherExecutableName(client));
    if (RegOpenKeyExW(
            hive, subkey.c_str(), 0, KEY_READ | registry_view, key.Put()) != ERROR_SUCCESS) {
        return;
    }
    if (const auto executable = ReadRegistryString(key.Get(), nullptr)) {
        result.launcher_executables.emplace_back(*executable);
    }
    if (const auto path = ReadRegistryString(key.Get(), L"Path")) {
        result.roots.emplace_back(*path);
    }
}

void CollectUninstallLocations(
    SystemLocations& result, const HKEY hive, const REGSAM registry_view,
    const NteClient client) {
    RegistryKey uninstall;
    if (RegOpenKeyExW(
            hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
            KEY_READ | registry_view, uninstall.Put()) != ERROR_SUCCESS) {
        return;
    }
    std::array<wchar_t, 512> name{};
    for (DWORD index = 0;; ++index) {
        DWORD length = static_cast<DWORD>(name.size());
        const LSTATUS enumerated = RegEnumKeyExW(
            uninstall.Get(), index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS) break;
        if (enumerated != ERROR_SUCCESS) continue;
        RegistryKey entry;
        if (RegOpenKeyExW(
                uninstall.Get(), std::wstring(name.data(), length).c_str(), 0,
                KEY_READ | registry_view, entry.Put()) != ERROR_SUCCESS) {
            continue;
        }
        const auto display_name = ReadRegistryString(entry.Get(), L"DisplayName");
        const auto display_icon = ReadRegistryString(entry.Get(), L"DisplayIcon");
        if ((!display_name || !LooksLikeNte(*display_name)) &&
            (!display_icon || !LooksLikeNte(*display_icon))) {
            continue;
        }
        if (const auto location = ReadRegistryString(entry.Get(), L"InstallLocation")) {
            result.roots.emplace_back(*location);
        }
        if (display_icon) {
            const auto executable = ExecutableFromCommand(*display_icon);
            if (NameEquals(executable, LauncherExecutableName(client))) {
                result.launcher_executables.push_back(executable);
            } else if (!executable.empty()) {
                result.roots.push_back(executable.parent_path());
            }
        }
        if (const auto uninstall_command = ReadRegistryString(entry.Get(), L"UninstallString")) {
            const auto executable = ExecutableFromCommand(*uninstall_command);
            if (!executable.empty()) result.roots.push_back(executable.parent_path());
        }
    }
}

std::optional<std::filesystem::path> EnvironmentPath(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return std::nullopt;
    std::wstring value(size, L'\0');
    if (GetEnvironmentVariableW(name, value.data(), size) != size - 1) {
        return std::nullopt;
    }
    value.resize(size - 1);
    return std::filesystem::path(std::move(value));
}

void AddNamedRoots(
    std::vector<std::filesystem::path>& roots, const std::filesystem::path& parent) {
    if (parent.empty()) return;
    constexpr std::array<std::wstring_view, 6> names{
        L"NevernessToEverness", L"Neverness to Everness", L"NTE",
        L"Games\\NevernessToEverness", L"Games\\Neverness to Everness",
        L"Hotta\\NevernessToEverness",
    };
    for (const auto name : names) roots.push_back(parent / name);
}

SystemLocations CollectSystemLocations(const NteClient client) {
    SystemLocations result;
    for (const HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        CollectAppPath(result, hive, KEY_WOW64_64KEY, client);
        CollectAppPath(result, hive, KEY_WOW64_32KEY, client);
        CollectUninstallLocations(result, hive, KEY_WOW64_64KEY, client);
        CollectUninstallLocations(result, hive, KEY_WOW64_32KEY, client);
    }
    for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
        if (const auto parent = EnvironmentPath(variable)) AddNamedRoots(result.roots, *parent);
    }
    const DWORD drives = GetLogicalDrives();
    for (unsigned index = 0; index < 26; ++index) {
        if ((drives & (1UL << index)) == 0) continue;
        const std::filesystem::path root(
            std::wstring{static_cast<wchar_t>(L'A' + index), L':', L'\\'});
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED) AddNamedRoots(result.roots, root);
    }
    return result;
}

std::wstring PathKey(const std::filesystem::path& path) {
    return Fold(path.lexically_normal().wstring());
}

void AddUnique(
    std::vector<std::filesystem::path>& paths, std::filesystem::path path) {
    if (path.empty()) return;
    path = path.lexically_normal();
    const std::wstring key = PathKey(path);
    if (std::ranges::none_of(paths, [&](const auto& existing) {
            return PathKey(existing) == key;
        })) {
        paths.push_back(std::move(path));
    }
}

void AddAncestors(
    std::vector<std::filesystem::path>& roots, std::filesystem::path path) {
    if (path.has_filename() && IsRegularFile(path)) path = path.parent_path();
    for (unsigned depth = 0; depth < 8 && !path.empty(); ++depth) {
        AddUnique(roots, path);
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
}

std::optional<std::filesystem::path> FindGameDirectory(
    const std::vector<std::filesystem::path>& roots) {
    constexpr std::array<std::wstring_view, 9> candidates{
        L"", L"HTGame\\Binaries\\Win64", L"Game\\HTGame\\Binaries\\Win64",
        L"Games\\HTGame\\Binaries\\Win64", L"HT\\Binaries\\Win64",
        L"Client\\WindowsNoEditor\\HT\\Binaries\\Win64",
        L"NevernessToEverness\\HTGame\\Binaries\\Win64",
        L"Neverness to Everness\\HTGame\\Binaries\\Win64",
        L"NTE\\HTGame\\Binaries\\Win64",
    };
    for (const auto& root : roots) {
        for (const auto relative : candidates) {
            const auto directory = relative.empty() ? root : root / relative;
            if (IsNteGameDirectory(directory)) return directory.lexically_normal();
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> FindLauncherExecutable(
    const std::vector<std::filesystem::path>& roots, const NteClient client) {
    constexpr std::array<std::wstring_view, 7> parents{
        L"", L"Launcher", L"NevernessToEverness", L"Neverness to Everness",
        L"NTE", L"Games\\NevernessToEverness", L"Games\\Neverness to Everness",
    };
    for (const auto& root : roots) {
        for (const auto parent : parents) {
            const auto executable = parent.empty()
                ? root / LauncherExecutableName(client)
                : root / parent / LauncherExecutableName(client);
            if (IsNteLauncherExecutable(executable, client)) {
                return executable.lexically_normal();
            }
        }
    }
    return std::nullopt;
}

}  // namespace

std::filesystem::path LauncherConfigurationPath(
    const std::filesystem::path& payload_root) noexcept {
    try {
        return payload_root.empty()
            ? std::filesystem::path{}
            : payload_root.lexically_normal() / L"AnomalyLauncher.json";
    } catch (...) {
        return {};
    }
}

LauncherConfigurationLoadResult LoadLauncherConfiguration(
    const std::filesystem::path& path) noexcept {
    LauncherConfigurationLoadResult result;
    try {
        if (path.empty()) {
            result.win32_error = ERROR_PATH_NOT_FOUND;
            result.message = "launcher configuration path is unavailable";
            return result;
        }
        DWORD read_error{};
        const std::string text = ReadText(path, read_error);
        if (read_error != ERROR_SUCCESS) {
            result.win32_error = read_error;
            result.message = read_error == ERROR_FILE_NOT_FOUND || read_error == ERROR_PATH_NOT_FOUND
                ? "launcher configuration does not exist" : "launcher configuration could not be read";
            return result;
        }
        const auto document = nlohmann::json::parse(text);
        if (!document.is_object()) {
            throw std::invalid_argument("unsupported launcher configuration shape");
        }
        const int schema_version = document.value("schemaVersion", 0);
        if (schema_version == 1 && document.contains("proxy") &&
            document.at("proxy").is_object() && document.contains("attach") &&
            document.at("attach").is_object()) {
            result.configuration.mainland_china.game_directory = Utf8Wide(
                document.at("proxy").value("gameDirectory", std::string{}));
            result.configuration.mainland_china.launcher_executable = Utf8Wide(
                document.at("attach").value("launcherExecutable", std::string{}));
        } else if (schema_version == 2 && document.contains("clients") &&
                   document.at("clients").is_object()) {
            const auto& clients = document.at("clients");
            const auto read_client = [&](const char* key, LauncherClientConfiguration& client) {
                if (!clients.contains(key) || !clients.at(key).is_object()) return;
                const auto& value = clients.at(key);
                client.game_directory = Utf8Wide(
                    value.value("gameDirectory", std::string{}));
                client.launcher_executable = Utf8Wide(
                    value.value("launcherExecutable", std::string{}));
            };
            read_client("mainlandChina", result.configuration.mainland_china);
            read_client("global", result.configuration.global);
            result.configuration.selected_client =
                document.value("selectedClient", std::string{}) == "global"
                    ? NteClient::Global : NteClient::MainlandChina;
        } else {
            throw std::invalid_argument("unsupported launcher configuration shape");
        }
        result.loaded = true;
        result.message = "launcher configuration loaded";
    } catch (const std::exception& error) {
        result.configuration = {};
        result.win32_error = ERROR_INVALID_DATA;
        result.message = error.what();
    } catch (...) {
        result.configuration = {};
        result.win32_error = ERROR_INVALID_DATA;
        result.message = "launcher configuration is invalid";
    }
    return result;
}

LauncherConfigurationSaveResult SaveLauncherConfiguration(
    const std::filesystem::path& path,
    const LauncherConfiguration& configuration) noexcept {
    LauncherConfigurationSaveResult result;
    std::filesystem::path temporary;
    try {
        if (path.empty()) {
            result.win32_error = ERROR_PATH_NOT_FOUND;
            result.message = "launcher configuration path is unavailable";
            return result;
        }
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            result.win32_error = static_cast<DWORD>(error.value());
            result.message = "launcher configuration directory could not be created";
            return result;
        }
        const auto client_document = [](const LauncherClientConfiguration& client) {
            return nlohmann::json{
                {"gameDirectory", WideUtf8(client.game_directory.wstring())},
                {"launcherExecutable", WideUtf8(client.launcher_executable.wstring())},
            };
        };
        const nlohmann::json document{
            {"schemaVersion", 2},
            {"selectedClient", configuration.selected_client == NteClient::Global
                ? "global" : "mainlandChina"},
            {"clients", {
                {"mainlandChina", client_document(configuration.mainland_china)},
                {"global", client_document(configuration.global)},
            }},
        };
        const std::string text = document.dump(2) + '\n';
        temporary = path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetCurrentThreadId());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            result.win32_error = GetLastError();
            if (result.win32_error == ERROR_SUCCESS) result.win32_error = ERROR_OPEN_FAILED;
            result.message = "launcher configuration temporary file could not be opened";
            return result;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        output.close();
        if (!output) {
            result.win32_error = ERROR_WRITE_FAULT;
            result.message = "launcher configuration could not be written";
            std::filesystem::remove(temporary, error);
            return result;
        }
        if (MoveFileExW(
                temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
            result.win32_error = GetLastError();
            result.message = "launcher configuration could not be replaced atomically";
            std::filesystem::remove(temporary, error);
            return result;
        }
        result.message = "launcher configuration saved";
    } catch (const std::exception& error) {
        result.win32_error = ERROR_WRITE_FAULT;
        result.message = error.what();
        std::error_code ignored;
        if (!temporary.empty()) std::filesystem::remove(temporary, ignored);
    } catch (...) {
        result.win32_error = ERROR_WRITE_FAULT;
        result.message = "launcher configuration could not be saved";
        std::error_code ignored;
        if (!temporary.empty()) std::filesystem::remove(temporary, ignored);
    }
    return result;
}

bool IsNteGameDirectory(const std::filesystem::path& directory) noexcept {
    try {
        return !directory.empty() && IsRegularFile(directory / kGameExecutableName);
    } catch (...) {
        return false;
    }
}

bool IsNteLauncherExecutable(
    const std::filesystem::path& executable, const NteClient client) noexcept {
    try {
        return NameEquals(executable, LauncherExecutableName(client)) &&
            IsRegularFile(executable);
    } catch (...) {
        return false;
    }
}

LauncherClientConfiguration DiscoverLauncherConfiguration(
    const LauncherClientConfiguration& preferred,
    const LauncherDiscoveryOptions& options) {
    LauncherClientConfiguration result;
    if (IsNteGameDirectory(preferred.game_directory)) {
        result.game_directory = preferred.game_directory.lexically_normal();
    }
    if (IsNteLauncherExecutable(preferred.launcher_executable, options.client)) {
        result.launcher_executable = preferred.launcher_executable.lexically_normal();
    }

    std::vector<std::filesystem::path> roots;
    std::vector<std::filesystem::path> game_executables = options.running_game_executables;
    std::vector<std::filesystem::path> launcher_executables =
        options.running_launcher_executables;
    for (const auto& root : options.search_roots) AddUnique(roots, root);
    AddAncestors(roots, options.payload_root);

    if (options.include_system_locations) {
        SystemLocations system = CollectSystemLocations(options.client);
        for (auto& root : system.roots) AddUnique(roots, std::move(root));
        for (auto& executable : system.launcher_executables) {
            launcher_executables.push_back(std::move(executable));
        }
    }

    for (const auto& executable : game_executables) {
        if (NameEquals(executable, kGameExecutableName) && IsRegularFile(executable)) {
            if (result.game_directory.empty()) {
                result.game_directory = executable.parent_path().lexically_normal();
            }
            AddAncestors(roots, executable.parent_path());
        }
    }
    for (const auto& executable : launcher_executables) {
        if (IsNteLauncherExecutable(executable, options.client)) {
            if (result.launcher_executable.empty()) {
                result.launcher_executable = executable.lexically_normal();
            }
            AddAncestors(roots, executable.parent_path());
        }
    }
    AddAncestors(roots, result.game_directory);
    AddAncestors(roots, result.launcher_executable.parent_path());

    if (result.game_directory.empty()) {
        std::vector<std::filesystem::path> launcher_roots;
        AddAncestors(launcher_roots, result.launcher_executable.parent_path());
        if (const auto discovered = FindGameDirectory(launcher_roots)) {
            result.game_directory = *discovered;
            AddAncestors(roots, result.game_directory);
        } else if (options.allow_unpaired_game_discovery) {
            if (const auto searched = FindGameDirectory(roots)) {
                result.game_directory = *searched;
                AddAncestors(roots, result.game_directory);
            }
        }
    }
    if (result.launcher_executable.empty()) {
        if (const auto discovered = FindLauncherExecutable(roots, options.client)) {
            result.launcher_executable = *discovered;
        }
    }
    return result;
}

}  // namespace anomaly::launcher
