#include "clone_enter.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "dungeon.hpp"  // kCloneEnterPluginId
#include "state.hpp"
#include "text_view.hpp"

namespace daily {
namespace {

// 宿主允许的 enablement 文档上限（plugin_enablement.cpp 里是 1 MiB）。
constexpr std::size_t kMaximumEnablementBytes = 1024u * 1024u;

enum class ReadResult {
    Missing,
    Ok,
    Failed,
};

void SetControlError(const char* text) {
    std::snprintf(S().clone_enter_error, sizeof(S().clone_enter_error), "%s",
                  text != nullptr ? text : "");
}

ReadResult ReadWholeFile(const std::wstring& path, std::string& out) {
    out.clear();
    if (path.empty()) return ReadResult::Failed;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        // 文件不存在 = 宿主还没写过启用表，可以按空表重建；
        // 其他错误（被占用/无权限）不能贸然覆盖。
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ReadResult::Missing
                                                                             : ReadResult::Failed;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart >= 0 &&
              static_cast<unsigned long long>(size.QuadPart) <= kMaximumEnablementBytes;
    if (ok) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (ok && offset < out.size()) {
            DWORD read = 0;
            const DWORD request = static_cast<DWORD>(out.size() - offset);
            ok = ::ReadFile(file, out.data() + offset, request, &read, nullptr) != 0 && read > 0;
            offset += read;
        }
        if (ok && offset != out.size()) ok = false;
    }
    CloseHandle(file);
    if (!ok) {
        out.clear();
        return ReadResult::Failed;
    }
    return ReadResult::Ok;
}

bool WriteWholeFile(const std::wstring& path, const std::string& document) {
    if (path.empty()) return false;
    const std::wstring temporary =
        path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = document.size() <= 0xFFFFFFFFull;
    if (ok) {
        DWORD written = 0;
        ok = ::WriteFile(file, document.data(), static_cast<DWORD>(document.size()), &written,
                       nullptr) != 0 &&
             written == static_cast<DWORD>(document.size());
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

void AppendEscaped(std::string& out, const std::string& value) {
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char escape[8]{};
                    std::snprintf(escape, sizeof(escape), "\\u%04x", ch);
                    out += escape;
                } else {
                    out += static_cast<char>(ch);
                }
                break;
        }
    }
}

// 读宿主启用表 → 只改副本插件那一项 → 重新生成文档。
// 现有的其他插件开关原样保留；文档损坏时拒绝写入，避免把用户的启用表写坏。
bool BuildEnablementDocument(const AnomalyJsonServiceV1* json, const std::string& current,
                             const bool enabled, std::string& out) {
    bool default_enabled = false;
    std::vector<std::pair<std::string, bool>> entries;
    if (!current.empty()) {
        if (json == nullptr || json->parse == nullptr || json->release == nullptr ||
            json->object_find == nullptr || json->object_size == nullptr ||
            json->object_key_at == nullptr || json->boolean_value == nullptr ||
            json->number_value == nullptr) {
            SetControlError("the JSON service is unavailable");
            return false;
        }
        AnomalyGenerationHandleV1 root{};
        if (json->parse(json->user, Str(current.data(), current.size()), &root).code !=
            ANOMALY_STATUS_V1_OK) {
            SetControlError("plugin-enablement.json is not valid JSON");
            return false;
        }
        bool ok = true;
        bool has_version = false;
        bool has_default = false;
        AnomalyGenerationHandleV1 child{};
        if (json->object_find(json->user, root, Str("schemaVersion"), &child).code ==
            ANOMALY_STATUS_V1_OK) {
            double version = 0.0;
            has_version =
                json->number_value(json->user, child, &version).code == ANOMALY_STATUS_V1_OK &&
                version == 1.0;
            static_cast<void>(json->release(json->user, child));
        }
        if (has_version &&
            json->object_find(json->user, root, Str("defaultEnabled"), &child).code ==
                ANOMALY_STATUS_V1_OK) {
            int32_t value = 0;
            has_default =
                json->boolean_value(json->user, child, &value).code == ANOMALY_STATUS_V1_OK;
            if (has_default) default_enabled = value != 0;
            static_cast<void>(json->release(json->user, child));
        }
        bool has_plugins = false;
        AnomalyGenerationHandleV1 plugins{};
        if (has_default &&
            json->object_find(json->user, root, Str("plugins"), &plugins).code ==
                ANOMALY_STATUS_V1_OK) {
            has_plugins = true;
            std::size_t count = 0;
            ok = json->object_size(json->user, plugins, &count).code == ANOMALY_STATUS_V1_OK;
            for (std::size_t index = 0; ok && index < count; ++index) {
                char key[512]{};
                std::size_t key_size = sizeof(key);
                if (json->object_key_at(json->user, plugins, index, key, &key_size).code !=
                    ANOMALY_STATUS_V1_OK) {
                    ok = false;
                    break;
                }
                AnomalyGenerationHandleV1 item{};
                if (json->object_find(json->user, plugins,
                                      Str(key, key_size > 0 ? key_size - 1 : 0), &item)
                        .code != ANOMALY_STATUS_V1_OK) {
                    ok = false;
                    break;
                }
                int32_t value = 0;
                if (json->boolean_value(json->user, item, &value).code ==
                    ANOMALY_STATUS_V1_OK) {
                    entries.emplace_back(
                        std::string(key, key_size > 0 ? key_size - 1 : 0), value != 0);
                }
                static_cast<void>(json->release(json->user, item));
            }
            static_cast<void>(json->release(json->user, plugins));
        }
        static_cast<void>(json->release(json->user, root));
        if (!ok || !has_version || !has_default || !has_plugins) {
            SetControlError("plugin-enablement.json has an unexpected shape");
            return false;
        }
    }

    if (current.empty()) {
        // 宿主还没写过启用表：把自己显式设为启用，免得重建后下一轮同步把自己关掉。
        entries.emplace_back(kPluginId, true);
    }

    bool replaced = false;
    for (auto& entry : entries) {
        if (entry.first == kCloneEnterPluginId) {
            entry.second = enabled;
            replaced = true;
        }
    }
    if (!replaced) entries.emplace_back(kCloneEnterPluginId, enabled);

    out.clear();
    out.reserve(256 + entries.size() * 48);
    out += "{\n  \"schemaVersion\": 1,\n  \"defaultEnabled\": ";
    out += default_enabled ? "true" : "false";
    out += ",\n  \"plugins\": {";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) out += ',';
        out += "\n    \"";
        AppendEscaped(out, entries[index].first);
        out += "\": ";
        out += entries[index].second ? "true" : "false";
    }
    out += "\n  }\n}\n";
    return true;
}

void ScheduleCloneEnterWrite();

void ANOMALY_CALL ApplyCloneEnterState(void*, AnomalyGenerationHandleV1) {
    State& state = S();
    const bool enabled = state.clone_enter_write_target.load(std::memory_order_acquire) != 0;
    bool ok = false;
    SetControlError("");
    if (state.json != nullptr) {
        std::string current;
        const ReadResult read = ReadWholeFile(state.clone_enter_enablement_path, current);
        if (read != ReadResult::Failed) {
            std::string document;
            if (BuildEnablementDocument(state.json, read == ReadResult::Ok ? current : std::string(),
                                        enabled, document)) {
                ok = WriteWholeFile(state.clone_enter_enablement_path, document);
                if (!ok) SetControlError("plugin-enablement.json could not be written");
            }
        } else {
            SetControlError("plugin-enablement.json could not be read");
        }
    } else {
        SetControlError("the JSON service is unavailable");
    }
    if (ok) {
        WinLogf("clone-enter enablement written: enabled=%d", enabled ? 1 : 0);
    } else {
        WinLogf("clone-enter enablement write failed: %s", state.clone_enter_error);
    }
    state.clone_enter_write_in_flight.store(false, std::memory_order_release);
    if (state.clone_enter_write_pending.exchange(false, std::memory_order_acq_rel)) {
        ScheduleCloneEnterWrite();
    }
}

void ScheduleCloneEnterWrite() {
    State& state = S();
    if (state.scheduler == nullptr || state.scheduler->schedule == nullptr) return;
    state.clone_enter_write_in_flight.store(true, std::memory_order_release);
    AnomalyGenerationHandleV1 task{};
    const AnomalyStatusV1 status =
        state.scheduler->schedule(state.scheduler->user, 0, &ApplyCloneEnterState, nullptr, &task);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        state.clone_enter_write_in_flight.store(false, std::memory_order_release);
        SetControlError("the clone-enter write could not be scheduled");
        WinLog("clone-enter enablement: the write task could not be scheduled");
    }
}

}  // namespace

bool BindCloneEnterControl() {
    State& state = S();
    state.clone_enter_control_ready = false;
    state.clone_enter_enablement_path.clear();
    if (state.plugin_state == nullptr || state.plugin_state->directory == nullptr) {
        WinLog("clone-enter control: anomaly.plugin-state is unavailable");
        return false;
    }
    std::size_t required = 0;
    if (state.plugin_state->directory(state.plugin_state->user, nullptr, &required).code !=
            ANOMALY_STATUS_V1_OK ||
        required == 0 || required > 1024) {
        WinLog("clone-enter control: the plugin state directory is unavailable");
        return false;
    }
    std::string encoded(required, '\0');
    std::size_t size = encoded.size();
    if (state.plugin_state->directory(state.plugin_state->user, encoded.data(), &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size == 0 || size > encoded.size()) {
        WinLog("clone-enter control: the plugin state directory could not be read");
        return false;
    }
    encoded.resize(size - 1);
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, encoded.data(),
                                                static_cast<int>(encoded.size()), nullptr, 0);
    if (wide_length <= 0) {
        WinLog("clone-enter control: the plugin state directory is not valid UTF-8");
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, encoded.data(), static_cast<int>(encoded.size()),
                        wide.data(), wide_length);
    const std::filesystem::path state_directory(wide);
    // <root>/state/plugins/<plugin id> → 上三级就是运行时根目录。
    const std::filesystem::path root =
        state_directory.parent_path().parent_path().parent_path();
    if (root.empty()) {
        WinLog("clone-enter control: the runtime root could not be derived");
        return false;
    }
    state.clone_enter_enablement_path =
        (root / L"config" / L"plugin-enablement.json").wstring();
    state.clone_enter_control_ready = true;
    const std::string path_text =
        std::filesystem::path(state.clone_enter_enablement_path).string();
    WinLogf("clone-enter control: enablement file = %s", path_text.c_str());
    return true;
}

bool CloneEnterControlAvailable() {
    const State& state = S();
    return state.clone_enter_control_ready && state.scheduler != nullptr &&
           state.scheduler->schedule != nullptr && state.json != nullptr &&
           state.json->parse != nullptr;
}

void CloneEnterRequestEnabled(const bool enabled) {
    State& state = S();
    if (!CloneEnterControlAvailable()) return;
    state.clone_enter_write_target.store(enabled ? 1 : 0, std::memory_order_release);
    if (state.clone_enter_write_in_flight.load(std::memory_order_acquire)) {
        // 上一次写盘还在路上：只更新目标，写完后会按最新目标补写一次。
        state.clone_enter_write_pending.store(true, std::memory_order_release);
        return;
    }
    ScheduleCloneEnterWrite();
}

bool CloneEnterWriteInFlight() {
    const State& state = S();
    return state.clone_enter_write_in_flight.load(std::memory_order_acquire) ||
           state.clone_enter_write_pending.load(std::memory_order_acquire);
}

}  // namespace daily
