#include "state.hpp"

#include <cstdarg>
#include <cstdio>

namespace daily {
namespace {

// 每次 Load 重建一份，热重载后不会残留上一代的运行状态。
State* g_instance{};

}  // namespace

State& S() {
    if (g_instance == nullptr) g_instance = new State();
    return *g_instance;
}

void ResetState() {
    delete g_instance;
    g_instance = new State();
}

void LogInfo(const char* text) {
    State& state = S();
    if (text == nullptr || state.core == nullptr || state.core->log == nullptr) return;
    state.core->log(state.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, Str(text));
}

void LogWarn(const char* text) {
    State& state = S();
    if (text == nullptr || state.core == nullptr || state.core->log == nullptr) return;
    state.core->log(state.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, Str(text));
}

void WinLog(const char* text) {
    LogInfo(text);
    if (text == nullptr) return;
    State& state = S();
    std::snprintf(state.log_lines[state.log_head], kLogLineBytes, "%s", text);
    state.log_head = (state.log_head + 1) % static_cast<int>(kMaxLogLines);
    if (state.log_count < static_cast<int>(kMaxLogLines)) ++state.log_count;
}

void WinLogf(const char* format, ...) {
    char buffer[kLogLineBytes]{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    WinLog(buffer);
}

void SetStatus(const char* text) {
    State& state = S();
    std::snprintf(state.status, sizeof(state.status), "%s", text != nullptr ? text : "");
}

void SetStatusText(const std::string& text) { SetStatus(text.c_str()); }

}  // namespace daily
