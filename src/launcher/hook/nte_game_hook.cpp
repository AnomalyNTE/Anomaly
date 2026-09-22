// Injected into the official launcher so a session can start without the user touching it.
//
// The launcher is NTEGame.exe. It owns the OneSDK pipe server that the login and support buttons come
// from, which is why it is kept rather than replaced: driving the launcher's own start action keeps
// every part of a normal session -- pipe setup, the shared-memory handshake, the environment the game
// is given -- exactly as it is when the user clicks the button.
//
// Three jobs, in order:
//
//   1. Hide the launcher's window from the inside, before it has been painted.
//   2. Signal the host that the launcher is ready, so its capture loop can be bounded to the seconds
//      around the launch instead of running blind from the moment the launcher starts.
//   3. Ask the launcher to start the game, by calling the same slot its start button is wired to.
//
// --- Why the start action is reached the way it is -----------------------------------------------
//
// The launcher UI is QML, and the start button is wired like this:
//
//     QmButton { id: startBtn
//                enabled: BackgroudStageScheduler.generalLauncherHasAttachGame()
//                onClicked: BackgroudStageScheduler.actionButtonClicked() }
//
// Qt UI Automation is not available in this build (no qwindowsuiautomation.dll, no UIAutomationCore
// import), and btn_play.png is packed into resources rather than being on disk, so neither an
// accessibility tree nor template matching can find the button. The scheduler object it belongs to
// can be reached directly instead.
//
// Four details in that path were each wrong in an earlier attempt and are load-bearing:
//
//   * QObject::metaObject() is virtual, and the exported QObject implementation answers "QObject" for
//     every object. The real meta-object comes from vtable slot 0, which this file confirms by
//     checking that the class name it returns is a plausible identifier and is not "QObject" before
//     trusting the slot for the rest of the walk.
//
//   * GameClientAgent also declares actionButtonClicked. Resolving by name alone reaches that one and
//     does nothing, so the object is resolved by class name.
//
//   * QMetaObject::invokeMethod appends the argument-type list and its own parentheses to the member
//     string, so "actionButtonClicked()" becomes "actionButtonClicked()()" and matches nothing. The
//     name is passed without parentheses.
//
//   * QGenericArgument is { const void *data; const char *name; }, data first, matching Qt's member
//     order rather than its constructor's parameter order.
//
// Two further things are deliberately absent. qInstallMessageHandler is never called: an earlier
// version installed one to capture Qt diagnostics, and because it read QString's internal layout by
// offset, a startup message that did not match the assumed layout caused an out-of-bounds read and
// killed the launcher on every run. Qt's message handler is also not a place to be formatting text
// from another thread in a process this code does not own. The QMetaMethod::invoke route is likewise
// not used.
//
// --- Diagnostics ---------------------------------------------------------------------------------
//
// The hook appends a line-oriented trace to logs\nte_game_hook.log under the runtime payload it was
// loaded from. It writes one unconditionally rather than on request, because the launcher is normally
// started by double-clicking it: a diagnostic that needs a prepared environment is not there when the
// launch that failed is the one worth reading. ANOMALY_NTE_GAME_HOOK_LOG redirects it elsewhere.

#include <Windows.h>
#include <tlhelp32.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

// Qt::QueuedConnection. The scheduler lives on the launcher's main thread, so the call has to be
// queued: a direct call would run the handler on this thread instead.
constexpr int kQueuedConnection = 2;

// The host creates this before the launcher is started, and waits on it to decide when to begin
// capturing the game process. The game is created by ACE-BASE.sys, which offers nothing to subscribe
// to, so capture has to poll; this event is what keeps the poll bounded.
constexpr wchar_t kArmedEventName[] = L"Local\\Anomaly.NteGameHook.Armed";

constexpr wchar_t kGameExecutable[] = L"HTGame.exe";

// GameMulClientMgr::GameStatus is the launcher's own view of the client -- uninstalled, checking,
// downloading, installing, updating, ready. Reading it would let the hook leave a pending update to the
// user instead of driving the launcher's start button at it, and that is not implemented, because the
// status cannot be read from here. It is a property the launcher's QML object declares, so it lives on
// that object's dynamic meta-object: QMetaObject::invokeMethod does not walk those and reported nothing
// at all, and QObject::property does reach them but evaluates through the QML engine, which cannot be
// done from a foreign thread -- the one attempt at it took the launcher down mid-launch. Doing it
// properly means running the read on the launcher's own thread, which needs a receiver object parked in
// its event loop, and that is a larger piece of work than this is worth. The start action is therefore
// attempted whatever the status, which is the behaviour that was in place before any of this and is
// known to work; the cost is that a pending update is driven by the click rather than handed over.

// The launcher's start action needs its session established before it does anything, and a run that
// called the scheduler within a few hundred milliseconds of process start left the launcher dead. That
// danger is at the start, not later, so waiting a fixed span is either too short or needlessly long.
// Everything the action actually needs is observable -- the application object, its scheduler, and the
// built frame -- so the hook waits for those instead and never acts before they exist.
constexpr DWORD kReadinessTimeoutMilliseconds = 60000;
constexpr DWORD kReadinessProbeIntervalMilliseconds = 100;

// How long the hook keeps trying after the host has been told to expect a game.
constexpr DWORD kAttemptWindowMilliseconds = 180000;
constexpr DWORD kAttemptIntervalMilliseconds = 3000;
// Event hooks are delivered through the installing thread's message queue, so every wait pumps for at
// most this long before checking the deadline again.
constexpr DWORD kPumpIntervalMilliseconds = 50;

struct QListLayout {
    void* data;
};

struct QListDataLayout {
    int ref;
    int alloc;
    int begin;
    int end;
    void* array[1];
};

struct QGenericArgument {
    const void* data;
    const char* name;
};

using InstanceFn = void* (*)();
using ChildrenFn = const QListLayout* (*)(const void*);
using ClassNameFn = const char* (*)(const void*);
using InvokeMethodFn = bool (*)(void*, const char*, int, QGenericArgument, QGenericArgument,
    QGenericArgument, QGenericArgument, QGenericArgument, QGenericArgument, QGenericArgument,
    QGenericArgument, QGenericArgument, QGenericArgument);

struct QtCore {
    InstanceFn instance{};
    ChildrenFn children{};
    ClassNameFn class_name{};
    InvokeMethodFn invoke_method{};

    [[nodiscard]] bool Ready() const {
        return instance && children && class_name && invoke_method;
    }
};

QtCore g_core;
size_t g_meta_slot{static_cast<size_t>(-1)};

CRITICAL_SECTION g_log_lock;
bool g_log_enabled{};
wchar_t g_log_path[MAX_PATH]{};

void Log(const char* format, ...) {
    if (!g_log_enabled) return;
    EnterCriticalSection(&g_log_lock);
    if (FILE* file = nullptr; _wfopen_s(&file, g_log_path, L"a") == 0 && file != nullptr) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        std::fprintf(file, "[%02d:%02d:%02d.%03d] ", now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds);
        va_list arguments;
        va_start(arguments, format);
        std::vfprintf(file, format, arguments);
        va_end(arguments);
        std::fputc('\n', file);
        std::fclose(file);
    }
    LeaveCriticalSection(&g_log_lock);
}

// The hook runs inside a process this repository does not own, so a launch that does not start the
// game leaves nothing behind to look at. It therefore always writes a log, next to the runtime payload
// it was loaded from, and an environment variable only redirects it. Requiring the variable meant the
// ordinary way of starting the launcher -- from the shell, without a prepared environment -- produced
// no record at all of why the game never appeared.
void ResolveLogConfiguration(HMODULE module) {
    wchar_t value[MAX_PATH]{};
    const DWORD length =
        GetEnvironmentVariableW(L"ANOMALY_NTE_GAME_HOOK_LOG", value, MAX_PATH);
    if (length != 0 && length < MAX_PATH) {
        std::memcpy(g_log_path, value, (length + 1) * sizeof(wchar_t));
        g_log_enabled = true;
        return;
    }

    wchar_t path[MAX_PATH]{};
    const DWORD written = GetModuleFileNameW(module, path, MAX_PATH);
    if (written == 0 || written >= MAX_PATH) return;
    // <runtime root>\NTEGameHook.dll -> <runtime root>\logs\nte_game_hook.log
    const wchar_t* separator = std::wcsrchr(path, L'\\');
    if (separator == nullptr) return;
    const size_t directory_length = static_cast<size_t>(separator - path);
    constexpr wchar_t suffix[] = L"\\logs\\nte_game_hook.log";
    if (directory_length + (sizeof(suffix) / sizeof(suffix[0])) >= MAX_PATH) return;
    std::memcpy(g_log_path, path, directory_length * sizeof(wchar_t));
    std::memcpy(g_log_path + directory_length, suffix, sizeof(suffix));
    wchar_t directory[MAX_PATH]{};
    std::memcpy(directory, path, directory_length * sizeof(wchar_t));
    CreateDirectoryW(directory, nullptr);
    g_log_enabled = true;
}

template <typename T>
T Resolve(HMODULE module, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

bool ResolveQtCore(QtCore& core, HMODULE& module) {
    module = GetModuleHandleW(L"Qt5Core.dll");
    if (module == nullptr) return false;
    core.instance = Resolve<InstanceFn>(module, "?instance@QCoreApplication@@SAPEAV1@XZ");
    core.children =
        Resolve<ChildrenFn>(module, "?children@QObject@@QEBAAEBV?$QList@PEAVQObject@@@@XZ");
    core.class_name = Resolve<ClassNameFn>(module, "?className@QMetaObject@@QEBAPEBDXZ");
    core.invoke_method = Resolve<InvokeMethodFn>(module,
        "?invokeMethod@QMetaObject@@SA_NPEAVQObject@@PEBDW4ConnectionType@Qt@@"
        "VQGenericArgument@@333333333@Z");
    return core.Ready();
}

bool PlausibleClassName(const char* name) {
    if (name == nullptr || std::isalpha(static_cast<unsigned char>(name[0])) == 0) return false;
    for (int index = 0; index < 64 && name[index] != '\0'; ++index) {
        const unsigned char character = static_cast<unsigned char>(name[index]);
        if (std::isalnum(character) == 0 && character != '_' && character != ':' &&
            character != '<' && character != '>' && character != ',') {
            return false;
        }
    }
    return true;
}

const void* MetaObjectAtSlot(const void* object, size_t slot) {
    const void* meta = nullptr;
    __try {
        void* const* vtable = *reinterpret_cast<void* const* const*>(object);
        using MetaObjectFn = const void* (*)(const void*);
        meta = reinterpret_cast<MetaObjectFn>(vtable[slot])(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (meta == nullptr) return nullptr;
    const char* name = nullptr;
    __try {
        name = g_core.class_name(meta);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    // "QObject" is what the base implementation answers for any object, so a slot that returns it has
    // not been shown to be the virtual meta-object accessor yet.
    if (!PlausibleClassName(name) || std::strcmp(name, "QObject") == 0) return nullptr;
    return meta;
}

const void* RealMetaObject(const void* object) {
    if (g_meta_slot != static_cast<size_t>(-1)) return MetaObjectAtSlot(object, g_meta_slot);
    for (size_t slot = 0; slot < 8; ++slot) {
        if (const void* meta = MetaObjectAtSlot(object, slot)) {
            g_meta_slot = slot;
            return meta;
        }
    }
    return nullptr;
}

void* FindByClass(void* object, int depth, const char* wanted) {
    if (object == nullptr || depth > 24) return nullptr;
    const void* meta = RealMetaObject(object);
    if (meta == nullptr) return nullptr;
    const char* name = g_core.class_name(meta);
    if (name != nullptr && std::strcmp(name, wanted) == 0) return object;

    const QListLayout* list = g_core.children(object);
    if (list == nullptr || list->data == nullptr) return nullptr;
    const auto* data = static_cast<const QListDataLayout*>(list->data);
    const int count = data->end - data->begin;
    if (count <= 0 || count > 20000 || data->begin < 0) return nullptr;
    auto* const* items = reinterpret_cast<void* const*>(data->array + data->begin);
    for (int index = 0; index < count; ++index) {
        if (void* found = FindByClass(items[index], depth + 1, wanted)) return found;
    }
    return nullptr;
}

// Windows the hook hid. Restoration is limited to these, because the process also owns invisible Qt
// internals and showing those would put stray windows on screen.
constexpr size_t kMaxHiddenWindows = 16;
HWND g_hidden_windows[kMaxHiddenWindows]{};
size_t g_hidden_count{};
HWINEVENTHOOK g_show_hook{};

void RememberHidden(HWND window) {
    for (size_t index = 0; index < g_hidden_count; ++index) {
        if (g_hidden_windows[index] == window) return;
    }
    if (g_hidden_count < kMaxHiddenWindows) g_hidden_windows[g_hidden_count++] = window;
}

// True for a window this process owns that behaves like the launcher's own frame: top level and not
// owned by another window. Qt's helper windows are invisible or owned, so they are left alone, and so
// is the tray icon, which is not a window at all -- the launcher keeps running from the tray once the
// game starts.
bool IsOwnFrameWindow(HWND window) {
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != GetCurrentProcessId()) return false;
    if (GetWindow(window, GW_OWNER) != nullptr) return false;
    return true;
}

// Hiding the frame the moment it is shown is what keeps it off screen. The launcher selects the
// windows platform itself and overrides QT_QPA_PLATFORM, so there is no way to ask it for no window
// at all, and any interval-based sweep leaves the frame painted until the sweep comes round again.
void CALLBACK OnWindowShown(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child,
    DWORD, DWORD) {
    if (event != EVENT_OBJECT_SHOW) return;
    if (object != OBJID_WINDOW || child != CHILDID_SELF) return;
    if (window == nullptr || !IsOwnFrameWindow(window)) return;
    ShowWindow(window, SW_HIDE);
    RememberHidden(window);
}

// Puts back everything that was hidden. Hiding is only safe while the hook is still going to act: if
// it returns before the game has been asked to start, the launcher is left running and invisible,
// which cannot be recovered from without killing the process.
void GiveUp() {
    if (g_show_hook != nullptr) {
        UnhookWinEvent(g_show_hook);
        g_show_hook = nullptr;
    }
    for (size_t index = 0; index < g_hidden_count; ++index) {
        if (IsWindow(g_hidden_windows[index])) ShowWindow(g_hidden_windows[index], SW_SHOW);
    }
    g_hidden_count = 0;
}

// Stops hiding the launcher's windows and leaves the ones already hidden alone. The launcher owns its
// window lifecycle from here: once the game is running it puts itself in the tray, and that is a shell
// notification it raises on its own schedule -- this hook only has to get out of the way. It is not
// GiveUp: putting the frames back would show a launcher the user asked never to see, and the unhook
// alone is what lets the tray icon through.
void StopHiding() {
    if (g_show_hook != nullptr) {
        UnhookWinEvent(g_show_hook);
        g_show_hook = nullptr;
    }
    g_hidden_count = 0;
    Log("hook: stopped hiding the launcher's windows");
}

// Waits while still draining this thread's message queue. An out-of-context event hook only runs when
// the thread that installed it pumps messages, so a plain sleep would let a frame that was just shown
// stay on screen until the wait ended.
void PumpWait(DWORD milliseconds) {
    const DWORD deadline = GetTickCount() + milliseconds;
    for (;;) {
        const DWORD now = GetTickCount();
        if (now >= deadline) break;
        DWORD slice = deadline - now;
        if (slice > kPumpIntervalMilliseconds) slice = kPumpIntervalMilliseconds;
        MsgWaitForMultipleObjectsEx(0, nullptr, slice, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

// Catches a frame that was already on screen when the hook went in. The hook normally arrives before
// Qt builds anything, so this only covers the race it cannot rule out.
void SweepExistingWindows() {
    struct State {
        DWORD process;
    } state{GetCurrentProcessId()};

    struct Helper {
        static BOOL CALLBACK OnWindow(HWND window, LPARAM parameter) {
            auto* sweep = reinterpret_cast<State*>(parameter);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != sweep->process) return TRUE;
            if (GetWindow(window, GW_OWNER) != nullptr) return TRUE;
            if (!IsWindowVisible(window)) return TRUE;
            ShowWindow(window, SW_HIDE);
            RememberHidden(window);
            return TRUE;
        }
    };

    EnumWindows(&Helper::OnWindow, reinterpret_cast<LPARAM>(&state));
}

bool GameProcessRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kGameExecutable) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool Fire(void* object, const char* member) {
    const QGenericArgument empty{};
    return g_core.invoke_method(object, member, kQueuedConnection, empty, empty, empty, empty, empty,
        empty, empty, empty, empty, empty);
}

// Reads the scheduler's gameStatus, which is not implemented: see the note near kGameExecutable. It
// stays as the single place that answers "the status is unknown", and the value is logged, so the day
// the read is implemented on the launcher's own thread there is one function to change and a log line
// that already shows what it returns.
int ReadGameStatus(void* scheduler) {
    static_cast<void>(scheduler);
    return -1;
}

// The host creates these, so a failure to open one means the host is not waiting and is worth saying.
void SignalEvent(const wchar_t* name, const char* description) {
    if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) {
        SetEvent(event);
        CloseHandle(event);
        Log("hook: signalled the host: %s", description);
    } else {
        Log("hook: %s event unavailable error=%lu", description, GetLastError());
    }
}

DWORD WINAPI HookThread(void*) {
    QtCore core;
    HMODULE qt_core{};
    // The launcher loads Qt early, but the hook arrives during startup, so this is given a generous
    // bound rather than assuming the module is already resident.
    for (int attempt = 0; attempt < 400 && !core.Ready(); ++attempt) {
        if (!ResolveQtCore(core, qt_core)) Sleep(250);
    }
    if (!core.Ready()) {
        Log("hook: Qt5Core.dll entry points unavailable");
        return 0;
    }
    g_core = core;
    Log("hook: attached");

    // The launcher selects the windows platform itself and overrides QT_QPA_PLATFORM, so it cannot be
    // asked for no window: the frame has to be caught as it is shown. The hook goes in before Qt
    // builds anything, and the sweep afterwards covers the one race that leaves.
    g_show_hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnWindowShown,
        GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    if (g_show_hook == nullptr) {
        Log("hook: window hook unavailable error=%lu", GetLastError());
    } else {
        Log("hook: window hook installed");
    }
    SweepExistingWindows();
    const DWORD startup_began = GetTickCount();

    const DWORD readiness_deadline = startup_began + kReadinessTimeoutMilliseconds;
    void* application = nullptr;
    void* scheduler = nullptr;
    while (GetTickCount() < readiness_deadline) {
        application = core.instance();
        if (application != nullptr) {
            scheduler = FindByClass(application, 0, "BackgroudStageScheduler");
            // The frame having been built is the last of the three: the launcher paints it after its
            // session is up, so requiring it keeps the first call clear of the window that has left
            // the launcher dead.
            if (scheduler != nullptr && g_hidden_count != 0) break;
        }
        PumpWait(kReadinessProbeIntervalMilliseconds);
    }
    if (application == nullptr) {
        Log("hook: QCoreApplication::instance() never appeared, restoring the window");
        GiveUp();
        return 0;
    }
    if (scheduler == nullptr) {
        Log("hook: BackgroudStageScheduler never appeared, restoring the window");
        GiveUp();
        return 0;
    }
    if (g_hidden_count == 0) {
        Log("hook: the launcher never built a frame, restoring");
        GiveUp();
        return 0;
    }
    Log("hook: ready after %lu ms", GetTickCount() - startup_began);
    void* controller = FindByClass(application, 0, "MainController");
    Log("hook: scheduler=%p controller=%p (meta-object vtable slot %zu)", scheduler, controller,
        g_meta_slot);
    Log("hook: gameStatus=%d", ReadGameStatus(scheduler));

    // The host is waiting on this. It is signalled before the first attempt rather than after,
    // because the game appears within a few seconds of the launcher acting and a capture loop that
    // starts late has nothing left to catch.
    SignalEvent(kArmedEventName, "begin capture");

    const DWORD deadline = GetTickCount() + kAttemptWindowMilliseconds;
    int round = 0;
    while (GetTickCount() < deadline) {
        ++round;
        const bool accepted = Fire(scheduler, "actionButtonClicked");
        // reStartGame is a fallback for the case where the button slot declines; it is not expected to
        // be the one that works.
        const bool fallback = controller != nullptr ? Fire(controller, "reStartGame") : false;

        PumpWait(kAttemptIntervalMilliseconds);

        Log("hook: round %d accepted=%d fallback=%d", round, accepted ? 1 : 0,
            fallback ? 1 : 0);

        if (GameProcessRunning()) {
            Log("hook: game process present after round %d, stopping", round);
            break;
        }
    }

    if (!GameProcessRunning()) {
        // Nothing was started, so the launcher is still the only way in and has to be usable again.
        Log("hook: the game never started, restoring the window");
        GiveUp();
    } else {
        // The launcher is left running: it is the SDK server the game session depends on, and it is
        // what puts itself in the tray once the game is up. Hiding its frames through the window
        // manager never reached its own visibility state, so it never ran that path and the process
        // was left with no window and no tray icon -- not reachable, not quittable. Letting go of the
        // hook is what lets the notification through.
        StopHiding();
    }
    Log("hook: done");
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&g_log_lock);
        ResolveLogConfiguration(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}