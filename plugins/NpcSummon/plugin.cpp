// NPC 召唤 · 阶段三:链路修正 + NPC 类发现
//
// 仍然是**纯只读**:扫签名、读内存、查快照、解析名字,不写内存、不调用游戏函数。
// 本版三件事:
//   1. 修 names 服务版本(反向查找 find_utf8 是 v2 才有的字段)
//   2. 诊断 ProcessEvent 签名:多组 pattern 变体逐一试,看哪一组能命中
//   3. 用 anomaly.nte.actors 枚举世界里的 actor,统计类名,找出「路人 / 市民」这类可召唤目标
#include "anomaly/sdk/anomaly_sdk.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HAS_FIELD(table, type, field) \
    ((table) != NULL && (table)->struct_size >= offsetof(type, field) + sizeof((table)->field))

static AnomalyStringViewV1 sv(const char* text) {
    AnomalyStringViewV1 result = {text, (text != NULL) ? strlen(text) : 0u};
    return result;
}

static AnomalyStatusV1 mk_status(unsigned int code) {
    AnomalyStatusV1 result = {code, 0u, {0, 0}};
    return result;
}

static int ok(AnomalyStatusV1 result) { return result.code == ANOMALY_STATUS_V1_OK; }

// ---------------------------------------------------------------------------
// 服务
// ---------------------------------------------------------------------------

static const AnomalyHostApiV1* g_host = NULL;
static const AnomalyCoreServiceV1* g_core = NULL;
static const AnomalyUiServiceV1* g_ui = NULL;
static const AnomalyWindowServiceV1* g_window = NULL;
static const AnomalyUe5FrameworkServiceV1* g_framework = NULL;
static const AnomalyUe5ObjectsServiceV1* g_objects = NULL;
static const AnomalyUe5NamesServiceV1* g_names = NULL;
static const AnomalySignatureServiceV1* g_signature = NULL;
static const AnomalyNteActorsServiceV1* g_actors = NULL;
static AnomalyGenerationHandleV1 g_window_handle = {0u, 0u};
static AnomalyGenerationHandleV1 g_record_window_handle = {0u, 0u};

typedef struct ProbeEntry {
    const char* id;
    int available;
    uint32_t reported_version;
    uint32_t status_code;
} ProbeEntry;

static ProbeEntry g_probes[] = {
    {"anomaly.core", 0, 0u, 0u},
    {"anomaly.ui", 0, 0u, 0u},
    {"anomaly.window", 0, 0u, 0u},
    {"anomaly.ue5.framework", 0, 0u, 0u},
    {"anomaly.ue5.objects", 0, 0u, 0u},
    {"anomaly.ue5.names", 0, 0u, 0u},
    {"anomaly.interop.signature", 0, 0u, 0u},
    {"anomaly.nte.actors", 0, 0u, 0u},
};
static const int g_probe_count = (int)(sizeof(g_probes) / sizeof(g_probes[0]));

static const void* fetch_version(const char* service_id, uint32_t version) {
    const void* table = NULL;
    if (g_host == NULL || g_host->query_service == NULL) return NULL;
    if (!ok(g_host->query_service(g_host->host_context, sv(service_id), version, &table))) return NULL;
    return table;
}

static void probe_services(void) {
    int i;
    for (i = 0; i < g_probe_count; ++i) {
        const void* table = NULL;
        AnomalyStatusV1 result;
        g_probes[i].available = 0;
        g_probes[i].reported_version = 0u;
        g_probes[i].status_code = 0u;
        if (g_host == NULL || g_host->query_service == NULL) return;
        result = g_host->query_service(g_host->host_context, sv(g_probes[i].id), 1u, &table);
        g_probes[i].status_code = result.code;
        if (ok(result) && table != NULL) {
            g_probes[i].available = 1;
            g_probes[i].reported_version = ((const uint32_t*)table)[1];
        }
    }
}

// ---------------------------------------------------------------------------
// 只读内存
// ---------------------------------------------------------------------------

static int read_bytes(uintptr_t address, void* destination, size_t size) {
    AnomalyMutableByteSpanV1 span;
    if (g_core == NULL || g_core->read_memory == NULL || address == 0u || destination == NULL) return 0;
    span.data = (uint8_t*)destination;
    span.size = size;
    return ok(g_core->read_memory(g_core->user, address, span));
}

template <typename T>
static int read_value(uintptr_t address, T* out) { return read_bytes(address, (void*)out, sizeof(T)); }

static uintptr_t resolve_rip32(uintptr_t instruction, int disp_offset, int instruction_size) {
    int32_t disp = 0;
    if (!read_value(instruction + (uintptr_t)disp_offset, &disp)) return 0u;
    return (uintptr_t)((intptr_t)instruction + (intptr_t)instruction_size + (intptr_t)disp);
}

// ---------------------------------------------------------------------------
// 报告
// ---------------------------------------------------------------------------

#define PIPE_MAX_LINES 140

typedef struct PipeReport {
    char lines[PIPE_MAX_LINES][200];
    int line_count;
} PipeReport;

static PipeReport g_pipe;

static void pipe_add(const char* fmt, ...) {
    va_list args;
    if (g_pipe.line_count >= PIPE_MAX_LINES) return;
    va_start(args, fmt);
    vsnprintf(g_pipe.lines[g_pipe.line_count], sizeof(g_pipe.lines[0]), fmt, args);
    va_end(args);
    g_pipe.line_count++;
}

static void pipe_flush_to_log(void) {
    int i;
    if (g_core == NULL || g_core->log == NULL) return;
    for (i = 0; i < g_pipe.line_count; ++i) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(g_pipe.lines[i]));
    }
}

// 阶段标记:立即落盘,不进管道缓冲(崩溃在探测中途时也能看到走到哪一步)
static void stage_log(const char* fmt, ...) {
    char line[220];
    va_list args;
    if (g_core == NULL || g_core->log == NULL) return;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
}

// 把管道里已收集的行立即写日志并清空(分阶段落盘,后面的阶段崩溃不会带走前面的证据)
static void stage_flush(void) {
    pipe_flush_to_log();
    g_pipe.line_count = 0;
}

// 两段式缓冲的名字解析
static int resolve_utf8_2stage(
    AnomalyStatusV1 (*fn)(void*, uint32_t, char*, size_t*), void* user,
    uint32_t id, char* buffer, size_t capacity) {
    size_t inout = 0;
    if (fn == NULL || buffer == NULL || capacity == 0) return 0;
    buffer[0] = '\0';
    (void)fn(user, id, NULL, &inout);
    inout = capacity;
    if (ok(fn(user, id, buffer, &inout))) {
        size_t n = (inout < capacity) ? inout : capacity - 1u;
        buffer[n] = '\0';
        return 1;
    }
    buffer[0] = '\0';
    return 0;
}

static int resolve_name(uint32_t name_id, char* buffer, size_t capacity) {
    if (g_names == NULL) return 0;
    return resolve_utf8_2stage(g_names->resolve_utf8, g_names->user, name_id, buffer, capacity);
}

// actors 的类名 / 实体名(两段式缓冲)
static int actor_class_name(uint64_t class_id, char* buffer, size_t capacity) {
    size_t inout = 0;
    if (g_actors == NULL || g_actors->class_name_utf8 == NULL || buffer == NULL || capacity == 0) return 0;
    buffer[0] = '\0';
    (void)g_actors->class_name_utf8(g_actors->user, class_id, NULL, &inout);
    inout = capacity;
    if (ok(g_actors->class_name_utf8(g_actors->user, class_id, buffer, &inout))) {
        size_t n = (inout < capacity) ? inout : capacity - 1u;
        buffer[n] = '\0';
        return 1;
    }
    buffer[0] = '\0';
    return 0;
}

static int actor_entity_name(uint64_t entity_id, char* buffer, size_t capacity) {
    size_t inout = 0;
    if (g_actors == NULL || g_actors->entity_name_utf8 == NULL || buffer == NULL || capacity == 0) return 0;
    buffer[0] = '\0';
    (void)g_actors->entity_name_utf8(g_actors->user, entity_id, NULL, &inout);
    inout = capacity;
    if (ok(g_actors->entity_name_utf8(g_actors->user, entity_id, buffer, &inout))) {
        size_t n = (inout < capacity) ? inout : capacity - 1u;
        buffer[n] = '\0';
        return 1;
    }
    buffer[0] = '\0';
    return 0;
}

// ---------------------------------------------------------------------------
// 生成记录 / 标签 / 黑名单 / 备注 / 导入导出(0.23.0)
// ---------------------------------------------------------------------------

#define MAX_RECORDS 80

typedef struct NpcRecord {
    char class_name[100];
    char note[140];
    int verdict;             // 0=未标记 1=成功 2=失败(进黑名单,主列表不显示)
    uint32_t spawn_count;
} NpcRecord;

static NpcRecord g_records[MAX_RECORDS];
static int g_record_count = 0;
static char g_record_status[220] = {0};

static NpcRecord* record_find(const char* class_name) {
    int i;
    if (class_name == NULL) return NULL;
    for (i = 0; i < g_record_count; ++i) {
        if (strcmp(g_records[i].class_name, class_name) == 0) return &g_records[i];
    }
    return NULL;
}

static NpcRecord* record_ensure(const char* class_name) {
    NpcRecord* r = record_find(class_name);
    if (r != NULL) {
        r->spawn_count++;
        return r;
    }
    if (g_record_count >= MAX_RECORDS) return NULL;
    r = &g_records[g_record_count];
    snprintf(r->class_name, sizeof(r->class_name), "%s", class_name);
    r->note[0] = '\0';
    r->verdict = 0;
    r->spawn_count = 1;
    g_record_count++;
    return r;
}

// 黑名单:标记为"失败"的类不在主列表里出现
static int record_is_blacklisted(const char* class_name) {
    NpcRecord* r = record_find(class_name);
    return (r != NULL && r->verdict == 2) ? 1 : 0;
}

static int records_blacklist_count(void) {
    int i;
    int n = 0;
    for (i = 0; i < g_record_count; ++i) {
        if (g_records[i].verdict == 2) n++;
    }
    return n;
}

// <游戏目录>\Anomaly 目录(记录文件与测试开关都放这里,玩家能直接找到)
static int anomaly_dir_path(wchar_t* out, size_t capacity) {
    wchar_t module_path[MAX_PATH];
    wchar_t* slash;
    DWORD n = GetModuleFileNameW(NULL, module_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    slash = wcsrchr(module_path, L'\\');
    if (slash == NULL) return 0;
    *slash = L'\0';
    if (swprintf(out, capacity, L"%s\\Anomaly", module_path) < 0) return 0;
    return 1;
}

// 数据目录兜底创建:某些安装方式下 <游戏目录>\Anomaly 可能还不存在,
// 直接 CreateFileW 会静默失败,看起来就像「设置保存不了」。
static int ensure_anomaly_dir(void) {
    wchar_t dir[MAX_PATH];
    if (!anomaly_dir_path(dir, MAX_PATH)) return 0;
    if (CreateDirectoryW(dir, NULL) != 0) return 1;
    return (GetLastError() == ERROR_ALREADY_EXISTS) ? 1 : 0;
}

// 先写 .tmp 再替换,避免游戏崩溃/退出时留下半截文件
static int write_text_atomic(const wchar_t* path, const char* data, int length) {
    wchar_t temp[MAX_PATH];
    HANDLE file;
    DWORD written = 0;

    if (path == NULL || data == NULL || length < 0) return 0;
    if (!ensure_anomaly_dir()) return 0;
    if (swprintf(temp, MAX_PATH, L"%s.tmp", path) < 0) return 0;
    file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (WriteFile(file, data, (DWORD)length, &written, NULL) == 0 ||
        written != (DWORD)length) {
        CloseHandle(file);
        DeleteFileW(temp);
        return 0;
    }
    // 不做 FlushFileBuffers:设置文件不值得为断电持久化在游戏线程上卡一帧,
    // 原子替换已经保证读到的要么是旧文件、要么是完整的新文件。
    CloseHandle(file);
    if (MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING) == 0) {
        DeleteFileW(temp);
        return 0;
    }
    return 1;
}

static void log_warning(const char* text) {
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, sv(text));
    }
}

// 记录文件位置:<游戏目录>\Anomaly\npc-summon-records.txt(玩家能直接找到)
static int records_file_path(wchar_t* out, size_t capacity) {
    wchar_t dir[MAX_PATH];
    if (!anomaly_dir_path(dir, MAX_PATH)) return 0;
    if (swprintf(out, capacity, L"%s\\npc-summon-records.txt", dir) < 0) return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// 白名单:用户自选的类,永远排在最前,并持久化到
// <游戏目录>\Anomaly\npc-summon-whitelist.txt(每行一个类名)
// ---------------------------------------------------------------------------

#define MAX_WHITELIST 128
static char g_whitelist[MAX_WHITELIST][100];
static int g_whitelist_count = 0;
static int g_whitelist_loaded = 0;
// draw 在渲染线程、update 在游戏线程:名字和标志必须一起同步,
// 否则 update 可能读到上一次(甚至空)的名字,点了「收藏」却没写进文件。
static SRWLOCK g_pending_lock = SRWLOCK_INIT;
static LONG g_pending_whitelist_toggle = 0;
static char g_pending_whitelist_name[100] = {0};

static int whitelist_index_of(const char* name) {
    int i;
    if (name == NULL) return -1;
    for (i = 0; i < g_whitelist_count; ++i) {
        if (strcmp(g_whitelist[i], name) == 0) return i;
    }
    return -1;
}

static int whitelist_file_path(wchar_t* out, size_t capacity) {
    wchar_t dir[MAX_PATH];
    if (!anomaly_dir_path(dir, MAX_PATH)) return 0;
    if (swprintf(out, capacity, L"%s\\npc-summon-whitelist.txt", dir) < 0) return 0;
    return 1;
}

static void whitelist_load(void) {
    wchar_t path[MAX_PATH];
    static char buffer[16384];
    HANDLE file;
    DWORD read = 0;
    char* line;
    int loaded;

    g_whitelist_count = 0;
    if (!whitelist_file_path(path, MAX_PATH)) return;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    memset(buffer, 0, sizeof(buffer));
    (void)ReadFile(file, buffer, (DWORD)(sizeof(buffer) - 1u), &read, NULL);
    CloseHandle(file);
    buffer[sizeof(buffer) - 1u] = '\0';

    line = buffer;
    while (line != NULL && *line != '\0') {
        char* next = strchr(line, '\n');
        size_t length;
        if (next != NULL) {
            *next = '\0';
            next++;
        }
        length = strlen(line);
        while (length > 0 && (line[length - 1u] == '\r' || line[length - 1u] == ' ')) {
            line[--length] = '\0';
        }
        if (line[0] != '#' && line[0] != '\0' &&
            g_whitelist_count < MAX_WHITELIST && whitelist_index_of(line) < 0) {
            snprintf(g_whitelist[g_whitelist_count], sizeof(g_whitelist[0]), "%s", line);
            g_whitelist_count++;
        }
        line = next;
    }
    loaded = g_whitelist_count;
    {
        char message[120];
        snprintf(message, sizeof(message), "白名单:已加载 %d 个类", loaded);
        if (g_core != NULL && g_core->log != NULL) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(message));
        }
    }
}

static void whitelist_save(void) {
    wchar_t path[MAX_PATH];
    static char buffer[16384];
    int length = 0;
    int i;

    if (!whitelist_file_path(path, MAX_PATH)) {
        log_warning("白名单保存失败:定位不到 <游戏目录>\\Anomaly");
        return;
    }
    length += snprintf(buffer + length, sizeof(buffer) - (size_t)length,
        "# npc-summon whitelist v1  每行一个类名\r\n");
    for (i = 0; i < g_whitelist_count && length < (int)sizeof(buffer) - 200; ++i) {
        snprintf(buffer + length, sizeof(buffer) - (size_t)length, "%s\r\n", g_whitelist[i]);
        length = (int)strlen(buffer);
    }
    if (!write_text_atomic(path, buffer, length)) {
        log_warning("白名单保存失败:<游戏目录>\\Anomaly 不可写");
    }
}

static void whitelist_toggle(const char* name) {
    int index;
    char message[200];

    if (name == NULL || name[0] == '\0') return;
    index = whitelist_index_of(name);
    if (index >= 0) {
        int i;
        for (i = index; i + 1 < g_whitelist_count; ++i) {
            snprintf(g_whitelist[i], sizeof(g_whitelist[0]), "%s", g_whitelist[i + 1]);
        }
        g_whitelist_count--;
        snprintf(message, sizeof(message), "白名单:移除 %s", name);
    } else if (g_whitelist_count < MAX_WHITELIST) {
        snprintf(g_whitelist[g_whitelist_count], sizeof(g_whitelist[0]), "%s", name);
        g_whitelist_count++;
        snprintf(message, sizeof(message), "白名单:加入 %s", name);
    } else {
        snprintf(message, sizeof(message), "白名单已满(%d 个)", MAX_WHITELIST);
    }
    whitelist_save();
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(message));
    }
}

// ---------------------------------------------------------------------------
// 界面设置持久化:<游戏目录>\Anomaly\npc-summon-settings.txt
//   只存主窗口上的勾选和搜索词;白名单单独在 npc-summon-whitelist.txt
// ---------------------------------------------------------------------------

// 分类勾选与搜索词(UI 在 draw 线程改,update 线程读;都是单个 int/短字符串,
// 读写竞争只影响一次落盘内容,下一次 poll 会纠正)
static int g_show_npc = 1;
static int g_show_vehicle = 1;
static int g_show_all_classes = 0;
static char g_search[64] = {0};

typedef struct PluginSettings {
    int show_npc;
    int show_vehicle;
    int show_all_classes;
    char search[64];
} PluginSettings;

static PluginSettings g_settings_snapshot = {1, 1, 0, {0}};
static int g_settings_loaded = 0;
static int g_settings_dirty = 0;
static int g_settings_save_failed_logged = 0;
static double g_settings_dirty_elapsed = 0.0;

static PluginSettings settings_current(void) {
    PluginSettings current;
    memset(&current, 0, sizeof(current));
    current.show_npc = g_show_npc;
    current.show_vehicle = g_show_vehicle;
    current.show_all_classes = g_show_all_classes;
    snprintf(current.search, sizeof(current.search), "%s", g_search);
    return current;
}

static int settings_file_path(wchar_t* out, size_t capacity) {
    wchar_t dir[MAX_PATH];
    size_t dir_length;
    if (!anomaly_dir_path(dir, MAX_PATH)) return 0;
    dir_length = wcslen(dir);
    if (dir_length + 24u >= capacity) return 0;
    wcscpy(out, dir);
    wcscat(out, L"\\npc-summon-settings.txt");
    return 1;
}

static void settings_apply(const PluginSettings* settings) {
    if (settings == NULL) return;
    g_show_npc = (settings->show_npc != 0) ? 1 : 0;
    g_show_vehicle = (settings->show_vehicle != 0) ? 1 : 0;
    g_show_all_classes = (settings->show_all_classes != 0) ? 1 : 0;
    snprintf(g_search, sizeof(g_search), "%s", settings->search);
}

static void settings_load(void) {
    wchar_t path[MAX_PATH];
    static char buffer[2048];
    HANDLE file;
    DWORD read = 0;
    char* line;
    PluginSettings loaded = {1, 1, 0, {0}};

    g_settings_snapshot = settings_current();
    if (!settings_file_path(path, MAX_PATH)) return;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    memset(buffer, 0, sizeof(buffer));
    (void)ReadFile(file, buffer, (DWORD)(sizeof(buffer) - 1u), &read, NULL);
    CloseHandle(file);
    buffer[sizeof(buffer) - 1u] = '\0';

    line = buffer;
    while (line != NULL && *line != '\0') {
        char* next = strchr(line, '\n');
        char* equals;
        size_t length;
        if (next != NULL) {
            *next = '\0';
            next++;
        }
        length = strlen(line);
        while (length > 0 && (line[length - 1u] == '\r' || line[length - 1u] == ' ')) {
            line[--length] = '\0';
        }
        if (line[0] == '#' || line[0] == '\0') {
            line = next;
            continue;
        }
        equals = strchr(line, '=');
        if (equals != NULL) {
            const char* value = equals + 1;
            *equals = '\0';
            if (strcmp(line, "show_npc") == 0) {
                loaded.show_npc = (atoi(value) != 0) ? 1 : 0;
            } else if (strcmp(line, "show_vehicle") == 0) {
                loaded.show_vehicle = (atoi(value) != 0) ? 1 : 0;
            } else if (strcmp(line, "show_all_classes") == 0) {
                loaded.show_all_classes = (atoi(value) != 0) ? 1 : 0;
            } else if (strcmp(line, "search") == 0) {
                snprintf(loaded.search, sizeof(loaded.search), "%s", value);
            }
        }
        line = next;
    }
    settings_apply(&loaded);
    g_settings_snapshot = settings_current();
    g_settings_dirty = 0;
    {
        char message[160];
        snprintf(message, sizeof(message),
            "设置:已加载 NPC=%d 车辆=%d 其它=%d 搜索=\"%s\"",
            g_show_npc, g_show_vehicle, g_show_all_classes, g_search);
        if (g_core != NULL && g_core->log != NULL) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(message));
        }
    }
}

static int settings_save(void) {
    wchar_t path[MAX_PATH];
    static char buffer[512];
    int length = 0;

    if (!settings_file_path(path, MAX_PATH)) return 0;
    length += snprintf(buffer + length, sizeof(buffer) - (size_t)length,
        "# npc-summon settings v1\r\n");
    length += snprintf(buffer + length, sizeof(buffer) - (size_t)length,
        "show_npc=%d\r\n", g_show_npc);
    length += snprintf(buffer + length, sizeof(buffer) - (size_t)length,
        "show_vehicle=%d\r\n", g_show_vehicle);
    length += snprintf(buffer + length, sizeof(buffer) - (size_t)length,
        "show_all_classes=%d\r\n", g_show_all_classes);
    length += snprintf(buffer + length, sizeof(buffer) - (size_t)length,
        "search=%s\r\n", g_search);
    length = (int)strlen(buffer);
    return write_text_atomic(path, buffer, length);
}
// update 里调用:值变了先记下来,静置 0.75 秒再落盘(搜索框每敲一个字都会变)
static void settings_poll(double delta_seconds) {
    PluginSettings current;

    if (g_settings_loaded == 0) {
        g_settings_loaded = 1;
        settings_load();
        return;
    }
    current = settings_current();
    if (memcmp(&current, &g_settings_snapshot, sizeof(current)) != 0) {
        g_settings_snapshot = current;
        g_settings_dirty = 1;
        g_settings_dirty_elapsed = 0.0;
        g_settings_save_failed_logged = 0;
        return;
    }
    if (g_settings_dirty == 0) return;
    g_settings_dirty_elapsed += delta_seconds;
    if (g_settings_dirty_elapsed < 0.75) return;
    if (settings_save()) {
        g_settings_dirty = 0;
        g_settings_snapshot = settings_current();
    } else {
        g_settings_dirty_elapsed = 0.0;
        if (g_settings_save_failed_logged == 0) {
            g_settings_save_failed_logged = 1;
            log_warning("设置保存失败:<游戏目录>\\Anomaly 不可写");
        }
    }
}

// 退出前强制落盘,避免最后 0.75 秒内的改动丢失
static void settings_flush(void) {
    if (g_settings_loaded == 0) return;
    if (g_settings_dirty == 0) return;
    if (settings_save()) {
        g_settings_dirty = 0;
        g_settings_snapshot = settings_current();
    }
}

// 二分测试开关文件:<游戏目录>\Anomaly\npc-summon-test.txt,内容一个数字(不存在/非法 = 0)
//   0 正常
//   1 加载但不探测(验证崩溃与探测是否相关)
//   2 探测但不取 actor 快照(跳过 actors_probe)
//   3 取快照但不解析类名/实体名(跳过快照里的名字解析)
//   4 只取 frame() 不翻页(跳过 page 循环)
//   5 用「签名扫描当场命中」的那个 PE 候选当入口(默认用的是 base+RVA 那个,做 A/B)
static int g_test_mode = 0;

static void test_mode_load(void) {
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    char buf[16];
    DWORD read = 0;
    HANDLE file;
    int i;
    int value = 0;

    g_test_mode = 0;
    if (!anomaly_dir_path(dir, MAX_PATH)) return;
    if (swprintf(path, MAX_PATH, L"%s\\npc-summon-test.txt", dir) < 0) return;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (ReadFile(file, buf, (DWORD)sizeof(buf) - 1u, &read, NULL)) {
        buf[read] = '\0';
        for (i = 0; buf[i] >= '0' && buf[i] <= '9'; ++i) {
            value = value * 10 + (buf[i] - '0');
        }
    }
    CloseHandle(file);
    g_test_mode = (value >= 0 && value <= 5) ? value : 0;
}

// 导出:UTF-8 文本,每行 "类名|verdict|备注|次数"(自己写的简单格式,便于手改)
static int records_export(void) {
    wchar_t path[MAX_PATH];
    static char buffer[32768];
    int length = 0;
    int i;
    HANDLE file;
    DWORD written = 0;

    if (!records_file_path(path, MAX_PATH)) {
        snprintf(g_record_status, sizeof(g_record_status), "导出失败:取不到游戏目录");
        return 0;
    }
    length += snprintf(buffer + length, sizeof(buffer) - (size_t)length,
        "# npc-summon records v1  格式:类名|verdict(0未标/1成功/2失败)|备注|生成次数\r\n");
    for (i = 0; i < g_record_count && length < (int)sizeof(buffer) - 300; ++i) {
        snprintf(buffer + length, sizeof(buffer) - (size_t)length, "%s|%d|%s|%u\r\n",
            g_records[i].class_name, g_records[i].verdict, g_records[i].note,
            g_records[i].spawn_count);
        length = (int)strlen(buffer);
    }
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        snprintf(g_record_status, sizeof(g_record_status), "导出失败:无法写入文件(错误 %lu)",
            GetLastError());
        return 0;
    }
    (void)WriteFile(file, buffer, (DWORD)length, &written, NULL);
    CloseHandle(file);
    snprintf(g_record_status, sizeof(g_record_status),
        "已导出 %d 条 -> <游戏目录>\\Anomaly\\npc-summon-records.txt", g_record_count);
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(g_record_status));
    }
    return 1;
}

// 导入:合并(同名类覆盖 verdict/备注,次数取较大值)
static int records_import(void) {
    wchar_t path[MAX_PATH];
    static char buffer[32768];
    HANDLE file;
    DWORD read = 0;
    char* line;
    int imported = 0;

    if (!records_file_path(path, MAX_PATH)) {
        snprintf(g_record_status, sizeof(g_record_status), "导入失败:取不到游戏目录");
        return 0;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        snprintf(g_record_status, sizeof(g_record_status),
            "导入失败:找不到文件(先导出一次生成模板,错误 %lu)", GetLastError());
        return 0;
    }
    memset(buffer, 0, sizeof(buffer));
    (void)ReadFile(file, buffer, (DWORD)(sizeof(buffer) - 1), &read, NULL);
    CloseHandle(file);
    buffer[sizeof(buffer) - 1] = '\0';

    line = buffer;
    while (line != NULL && *line != '\0') {
        char* next = strchr(line, '\n');
        char* sep1;
        char* sep2;
        char* sep3;
        if (next != NULL) {
            *next = '\0';
            next++;
        }
        // 记事本存的是 CRLF:去掉行尾 \r,否则备注尾巴会带一个回车
        {
            size_t line_length = strlen(line);
            if (line_length > 0 && line[line_length - 1] == '\r') {
                line[line_length - 1] = '\0';
            }
        }
        if (line[0] != '#' && line[0] != '\0') {
            sep1 = strchr(line, '|');
            if (sep1 != NULL) {
                *sep1 = '\0';
                sep2 = strchr(sep1 + 1, '|');
                if (sep2 != NULL) {
                    *sep2 = '\0';
                    sep3 = strchr(sep2 + 1, '|');
                    if (sep3 != NULL) {
                        NpcRecord* r;
                        *sep3 = '\0';
                        r = record_ensure(line);
                        if (r != NULL) {
                            int verdict = atoi(sep1 + 1);
                            r->verdict = (verdict >= 0 && verdict <= 2) ? verdict : 0;
                            snprintf(r->note, sizeof(r->note), "%s", sep2 + 1);
                            r->spawn_count = (uint32_t)atoi(sep3 + 1);
                            imported++;
                        }
                    }
                }
            }
        }
        line = next;
    }
    snprintf(g_record_status, sizeof(g_record_status), "已从文件导入 %d 条记录", imported);
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(g_record_status));
    }
    return 1;
}

static int records_clear_all(void) {
    g_record_count = 0;
    memset(g_records, 0, sizeof(g_records));
    snprintf(g_record_status, sizeof(g_record_status), "已清空全部记录");
    return 1;
}

// 记录视图的待执行动作(UI 在 Render 域,文件 IO 放到 Game 域做更稳)
static int g_pending_export = 0;
static int g_pending_import = 0;
static int g_pending_clear_records = 0;
static int g_pending_open_records = 0;

// ---------------------------------------------------------------------------
// 链路探测(布局 + 签名)
// ---------------------------------------------------------------------------

static const char* kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
static const char* kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";

// ProcessEvent 序言候选:由长到短逐一试,定位哪一组能被签名服务命中
static const char* kProcessEventPatterns[] = {
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 48 8D 6C 24 30 48 89 9D 28 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 C0 00 00 00 8B 41 08 4D 8B F0 C1 E8 1E 48 8B FA F6 D0 4C 8B F9 A8 01 0F 84 ?? ?? ?? ?? 33 F6 F7 82 B0 00 00 00 00 04 00 00",
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 48 8D 6C 24 30 48 89 9D 28 01 00 00",
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 48 8D 6C 24 30",
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00",
    "48 81 EC 00 01 00 00 48 8D 6C 24 30 48 89 9D 28 01 00 00",
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC",
};
static const int kProcessEventPatternCount =
    (int)(sizeof(kProcessEventPatterns) / sizeof(kProcessEventPatterns[0]));

static uintptr_t g_gobjects_items = 0u;
static uintptr_t g_process_event = 0u;
static uintptr_t g_gworld_slot = 0u;

// mode 1(chunk 指针表) + stride 24 已在上一次真机探测中由 idx=1/1000 双向验证
static uintptr_t object_by_index(uint32_t index) {
    const uint32_t chunk_index = index / 65536u;
    const uint32_t within = index % 65536u;
    uintptr_t chunk = 0u;
    uintptr_t item = 0u;
    uintptr_t object = 0u;
    if (g_gobjects_items == 0u) return 0u;
    if (!read_value(g_gobjects_items + (uintptr_t)chunk_index * 8u, &chunk) || chunk == 0u) return 0u;
    item = chunk + (uintptr_t)within * 24u;
    if (!read_value(item, &object)) return 0u;
    return object;
}

static void signature_probe(void) {
    uintptr_t insn = 0u;
    int i;

    if (g_signature == NULL || g_signature->resolve == NULL) {
        pipe_add("链路: interop.signature 不可用");
        return;
    }

    if (ok(g_signature->resolve(g_signature->user, sv("HTGame.exe"), sv(".text"), sv(kGObjectsPattern), &insn))) {
        uintptr_t slot = resolve_rip32(insn, 3, 7);
        uintptr_t items = 0u;
        if (slot != 0u && read_value(slot, &items)) g_gobjects_items = items;
        pipe_add("GObjects slot=0x%llX items=0x%llX (gobjects_struct=0x%llX)",
            (unsigned long long)slot, (unsigned long long)g_gobjects_items,
            (unsigned long long)((slot != 0u) ? slot - 16u : 0u));
    } else {
        pipe_add("GObjects 签名未命中");
    }

    insn = 0u;
    if (ok(g_signature->resolve(g_signature->user, sv("HTGame.exe"), sv(".text"), sv(kGWorldPattern), &insn))) {
        uintptr_t slot = resolve_rip32(insn, 3, 7);
        uintptr_t world = 0u;
        g_gworld_slot = slot;
        if (slot != 0u) (void)read_value(slot, &world);
        pipe_add("GWorld slot=0x%llX world=0x%llX", (unsigned long long)slot, (unsigned long long)world);
    } else {
        pipe_add("GWorld 签名未命中");
    }

    for (i = 0; i < kProcessEventPatternCount; ++i) {
        uintptr_t address = 0u;
        size_t pattern_length = strlen(kProcessEventPatterns[i]);
        AnomalyStatusV1 result = g_signature->resolve(
            g_signature->user, sv("HTGame.exe"), sv(".text"), sv(kProcessEventPatterns[i]), &address);
        if (ok(result) && address != 0u) {
            uint8_t head[4] = {0, 0, 0, 0};
            (void)read_bytes(address, head, 4);
            pipe_add("ProcessEvent 命中[#%d len=%u]=0x%llX head=%02X %02X %02X %02X",
                i, (unsigned)pattern_length, (unsigned long long)address,
                head[0], head[1], head[2], head[3]);
            // 只认 #0(profile 里的完整签名)。#1~#5 是通用函数序言,任何同款序言的函数
            // 都会命中,2026-10-18 就是这样拿到了一个假入口(调用后返回值全 0,随后崩溃)。
            if (i == 0 && g_process_event == 0u) g_process_event = address;
        } else {
            pipe_add("ProcessEvent 未命中[#%d len=%u status=%u]", i, (unsigned)pattern_length, result.code);
        }
    }
}

// ---------------------------------------------------------------------------
// NPC 类发现:枚举世界 actor,统计类名
// ---------------------------------------------------------------------------

#define CLASS_TABLE_MAX 512

// 带实例 handle 的完整类表:UI 列表、生成都用它(生成需要实例的 UClass)
#define MAX_CLASS_LIST 512
typedef struct ClassEntry {
    char name[100];
    uint32_t count;
    AnomalyGenerationHandleV1 sample;
    int kind;                 // 1=NPC(有胶囊/骨骼网格根组件,或名字像 NPC) 0=其它
    char root_class[96];      // 样例 actor 的根组件类名(用于判断有没有模型)
} ClassEntry;

static ClassEntry g_class_entries[MAX_CLASS_LIST];
static int g_class_entry_count = 0;
static int g_npc_class_count = 0;
static int g_vehicle_class_count = 0;

// 分类缓存:每个类只判定一次(读根组件)。
// 没有实体的类(体积/触发物/空壳)只计数、不占类表名额,这样 512 个名额全留给 NPC / 车辆。
typedef struct ClassKindCache {
    char name[100];
    char root_class[96];
    int kind;                 // 1=NPC 2=车辆 0=其它
} ClassKindCache;

static ClassKindCache g_kind_cache[512];
static int g_kind_cache_count = 0;
static int g_selected_class = -1;
static int g_pending_spawn_class = -1;
static int g_pending_clear = 0;
static int g_pending_rescan = 0;
static uintptr_t g_spawned[32];
static int g_spawned_count = 0;

static int classify_actor(uintptr_t actor, const char* class_name, char* root_out,
    size_t root_capacity);

static int kind_cache_lookup(const char* name, char* root_out, size_t root_capacity) {
    int i;
    for (i = 0; i < g_kind_cache_count; ++i) {
        if (strcmp(g_kind_cache[i].name, name) == 0) {
            if (root_out != NULL && root_capacity > 0) {
                snprintf(root_out, root_capacity, "%s", g_kind_cache[i].root_class);
            }
            return g_kind_cache[i].kind;
        }
    }
    return -1;
}

static void kind_cache_store(const char* name, int kind, const char* root_class) {
    if (g_kind_cache_count >= (int)(sizeof(g_kind_cache) / sizeof(g_kind_cache[0]))) return;
    snprintf(g_kind_cache[g_kind_cache_count].name,
        sizeof(g_kind_cache[0].name), "%s", name);
    snprintf(g_kind_cache[g_kind_cache_count].root_class,
        sizeof(g_kind_cache[0].root_class), "%s", (root_class != NULL) ? root_class : "");
    g_kind_cache[g_kind_cache_count].kind = kind;
    g_kind_cache_count++;
}

static void class_entry_add(const char* name, AnomalyGenerationHandleV1 handle,
    int kind, const char* root_class) {
    int i;
    for (i = 0; i < g_class_entry_count; ++i) {
        if (strcmp(g_class_entries[i].name, name) == 0) {
            g_class_entries[i].count++;
            return;
        }
    }
    if (g_class_entry_count >= MAX_CLASS_LIST) return;
    snprintf(g_class_entries[g_class_entry_count].name,
        sizeof(g_class_entries[0].name), "%s", name);
    g_class_entries[g_class_entry_count].count = 1;
    g_class_entries[g_class_entry_count].sample = handle;
    g_class_entries[g_class_entry_count].kind = kind;
    snprintf(g_class_entries[g_class_entry_count].root_class,
        sizeof(g_class_entries[0].root_class), "%s", (root_class != NULL) ? root_class : "");
    g_class_entry_count++;
}

typedef struct ClassCount {
    char name[100];
    char sample[100];
    uint32_t count;
} ClassCount;

static ClassCount g_classes[CLASS_TABLE_MAX];
static int g_class_count = 0;
static uint32_t g_actor_scanned = 0;

static void class_count_add(const char* name, const char* sample) {
    int i;
    for (i = 0; i < g_class_count; ++i) {
        if (strcmp(g_classes[i].name, name) == 0) {
            g_classes[i].count++;
            return;
        }
    }
    if (g_class_count >= CLASS_TABLE_MAX) return;
    snprintf(g_classes[g_class_count].name, sizeof(g_classes[g_class_count].name), "%s", name);
    snprintf(g_classes[g_class_count].sample, sizeof(g_classes[g_class_count].sample), "%s",
        (sample != NULL) ? sample : "");
    g_classes[g_class_count].count = 1;
    g_class_count++;
}

// 关键词判定:用于把「路人 / 市民 / NPC」这类目标挑出来
static int looks_like_npc_class(const char* name) {
    static const char* keys[] = {"npc", "citizen", "pedestrian", "people", "civil", "townsman",
        "villager", "crowd", "human", "passerby", "resident", "player_"};
    size_t i;
    char lower[100];
    size_t n = strlen(name);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1u;
    for (i = 0; i < n; ++i) {
        char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (strstr(lower, keys[i]) != NULL) return 1;
    }
    return 0;
}

// 车辆关键词判定:列表里「NPC」和「车辆」分成两组,其余一律不显示。
// 只认明确的车辆命名(实测清单里真车是 Vehicle002/003… 与 BP_Vehicle_xxx);
// 不写 "bus",否则 BP_Anomaly_BusStop* / BusStation_Mgr 这些站台与逻辑物会被误判成车。
static int looks_like_vehicle_class(const char* name) {
    static const char* keys[] = {"vehicle", "motorboat", "motorcycle", "scooter", "taxi",
        "truck", "car_", "_car", "bike", "wheel"};
    size_t i;
    char lower[100];
    size_t n = strlen(name);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1u;
    for (i = 0; i < n; ++i) {
        char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (strstr(lower, keys[i]) != NULL) return 1;
    }
    return 0;
}

// 场景 / 外观类名黑名单:这些即使根组件是胶囊或骨骼网格,也不是可召唤的 NPC。
// 实测:HTPlayerStart 根是胶囊、Glid_Fashion_* 根是骨骼网格,生成后都看不见。
static int looks_like_scene_class(const char* name) {
    static const char* keys[] = {"playerstart", "spawnpoint", "volume", "trigger",
        "manager", "controller", "logicbox", "logicactor", "camera", "sound",
        "navmesh", "occlusion", "postprocess", "weather", "sky", "light", "decal",
        "niagara", "staticmesh", "meshactor", "glid", "fashion", "skin", "costume",
        "outfit", "cosmetic", "environment", "skybox", "fog", "walkway", "door",
        "cmpc", "spoiler"};
    size_t i;
    char lower[100];
    size_t n = strlen(name);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1u;
    for (i = 0; i < n; ++i) {
        char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (strstr(lower, keys[i]) != NULL) return 1;
    }
    return 0;
}

// 把完整类清单写进日志(便于离线分类)
static void dump_class_catalog(void) {
    char line[420];
    int i;
    if (g_core == NULL || g_core->log == NULL) return;
    g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv("CLASS-CATALOG 开始"));
    for (i = 0; i < g_class_entry_count; i += 3) {
        int k;
        line[0] = '\0';
        for (k = i; k < i + 3 && k < g_class_entry_count; ++k) {
            char part[130];
            snprintf(part, sizeof(part), "%s(%u)  ", g_class_entries[k].name,
                g_class_entries[k].count);
            strncat(line, part, sizeof(line) - strlen(line) - 1u);
        }
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
    }
    g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv("CLASS-CATALOG 结束"));
}

static void sort_class_entries(void) {
    int i;
    int j;
    for (i = 0; i < g_class_entry_count; ++i) {
        for (j = i + 1; j < g_class_entry_count; ++j) {
            // 白名单最前,然后 NPC(1)、车辆(2)、其它(0);同类按数量降序
            int ai = (whitelist_index_of(g_class_entries[i].name) >= 0) ? 0
                : ((g_class_entries[i].kind == 1) ? 1
                : ((g_class_entries[i].kind == 2) ? 2 : 3));
            int bj = (whitelist_index_of(g_class_entries[j].name) >= 0) ? 0
                : ((g_class_entries[j].kind == 1) ? 1
                : ((g_class_entries[j].kind == 2) ? 2 : 3));
            int swap_needed = 0;
            if (bj < ai) {
                swap_needed = 1;
            } else if (bj == ai && g_class_entries[j].count > g_class_entries[i].count) {
                swap_needed = 1;
            }
            if (swap_needed != 0) {
                ClassEntry tmp = g_class_entries[i];
                g_class_entries[i] = g_class_entries[j];
                g_class_entries[j] = tmp;
            }
        }
    }
}

static void classify_class_entries(void);

static void actors_probe(void) {
    static AnomalyNteEntitySnapshotV1 page[256];
    AnomalyNteEntityFrameV1 frame;
    AnomalyNteEntityPageRequestV1 request;
    AnomalyNteEntityPageResultV1 result;
    uint32_t offset = 0u;
    int guard = 0;
    int slot;
    int i;

    if (g_actors == NULL || g_actors->frame == NULL || g_actors->page == NULL) {
        pipe_add("actors: 服务或 page 接口不可用,跳过 NPC 类发现");
        return;
    }
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = sizeof(frame);
    if (!ok(g_actors->frame(g_actors->user, &frame))) {
        pipe_add("actors.frame 失败(世界可能未就绪)");
        return;
    }
    pipe_add("actors: count=%u generation=%llu fov=%.1f",
        frame.entity_count, (unsigned long long)frame.generation, (double)frame.horizontal_fov_degrees);

    for (slot = 0; slot < 256; ++slot) page[slot].struct_size = sizeof(AnomalyNteEntitySnapshotV1);

    if (g_test_mode == 4) {
        pipe_add("actors: 模式4 只取 frame,跳过 page 翻页");
    }
    while (g_test_mode != 4 && guard < 400) {
        guard++;
        memset(&request, 0, sizeof(request));
        request.struct_size = sizeof(request);
        request.generation = frame.generation;
        request.offset = offset;
        request.capacity = 256u;
        memset(&result, 0, sizeof(result));
        result.struct_size = sizeof(result);
        if (!ok(g_actors->page(g_actors->user, &request, page, &result))) {
            pipe_add("actors.page(offset=%u) 失败", offset);
            break;
        }
        for (i = 0; i < (int)result.returned; ++i) {
            char cls[100];
            char ent[100];
            char root_cls[96];
            cls[0] = '\0';
            ent[0] = '\0';
            root_cls[0] = '\0';
            if (g_test_mode == 3) {
                snprintf(cls, sizeof(cls), "(class_id=%llu)", (unsigned long long)page[i].class_id);
            } else if (!actor_class_name(page[i].class_id, cls, sizeof(cls))) {
                snprintf(cls, sizeof(cls), "(class_id=%llu)", (unsigned long long)page[i].class_id);
            }
            if (g_test_mode != 3 && !actor_entity_name(page[i].entity_id, ent, sizeof(ent))) {
                ent[0] = '\0';
            }
            class_count_add(cls, ent);
            {
                // 每个类只判定一次(缓存),没有实体的类只计数、不占类表名额
                int kind = kind_cache_lookup(cls, root_cls, sizeof(root_cls));
                if (kind < 0) {
                    uintptr_t actor = (page[i].handle.id != 0u)
                        ? object_by_index((uint32_t)page[i].handle.id - 1u) : 0u;
                    kind = classify_actor(actor, cls, root_cls, sizeof(root_cls));
                    if (kind < 0) {
                        kind = 0;   // 判定失败:先按「其它」处理,不写缓存,下次再试
                    } else {
                        kind_cache_store(cls, kind, root_cls);
                    }
                }
                if (kind != 0) {
                    class_entry_add(cls, page[i].handle, kind, root_cls);
                }
            }
            g_actor_scanned++;
        }
        if (result.returned == 0u || result.next_offset >= result.total_matches) {
            pipe_add("actors: total_matches=%u", result.total_matches);
            break;
        }
        offset = result.next_offset;
    }
    pipe_add("actors 扫描完成: %u 个 actor, %d 个不同类", g_actor_scanned, g_class_count);
    classify_class_entries();
    sort_class_entries();
    dump_class_catalog();
}

static void report_top_classes(void) {
    int i;
    int j;
    int emitted = 0;
    int npc_like = 0;

    // 简单选择排序:按计数降序挑前 22 个
    for (i = 0; i < g_class_count; ++i) {
        for (j = i + 1; j < g_class_count; ++j) {
            if (g_classes[j].count > g_classes[i].count) {
                ClassCount tmp = g_classes[i];
                g_classes[i] = g_classes[j];
                g_classes[j] = tmp;
            }
        }
    }

    pipe_add("---- 出现最多的 actor 类(前 12) ----");
    for (i = 0; i < g_class_count && emitted < 12; ++i) {
        pipe_add("  %-46s x%-4u 例:%s", g_classes[i].name, g_classes[i].count, g_classes[i].sample);
        emitted++;
        if (looks_like_npc_class(g_classes[i].name)) npc_like++;
    }
    pipe_add("---- 关键词像 NPC 的类 ----");
    for (i = 0; i < g_class_count; ++i) {
        if (looks_like_npc_class(g_classes[i].name)) {
            pipe_add("  NPC候选 %-40s x%-4u 例:%s", g_classes[i].name, g_classes[i].count,
                g_classes[i].sample);
        }
    }
    if (npc_like == 0) pipe_add("  (前 22 里没有关键词命中的,全量表见上方)");
}

// ---------------------------------------------------------------------------
// 世界 → PlayerController → UClass → UFunction 探测
// ---------------------------------------------------------------------------

// profile 里验证过的布局偏移
static const int32_t L_world_gameInstance = 560;
static const int32_t L_gameInstance_localPlayers = 56;
static const int32_t L_localPlayer_controller = 48;
static const int32_t L_object_internalIndex = 12;
static const int32_t L_object_class = 16;
static const int32_t L_object_name = 24;
static const int32_t L_object_outer = 32;
static const int32_t L_actor_rootComponent = 456;
static const int32_t L_uclass_classDefaultObject = 272;
static const int32_t L_struct_superStruct = 64;
static const int32_t L_struct_children = 72;
static const int32_t L_field_next = 40;
static const int32_t L_field_name = 32;
static const int32_t L_function_flags = 176;
static const int32_t L_function_numParms = 180;
static const int32_t L_function_parmsSize = 182;
static const int32_t L_function_returnValueOffset = 184;
static const uint32_t FUNC_NATIVE = 0x400u;

static uintptr_t g_world = 0u;
static uintptr_t g_controller = 0u;
static uintptr_t g_controller_class = 0u;

static uintptr_t object_by_handle(AnomalyGenerationHandleV1 handle) {
    if (handle.id == 0u) return 0u;
    return object_by_index((uint32_t)handle.id - 1u);
}

static int object_name(uintptr_t object, char* buffer, size_t capacity) {
    uint32_t name_id = 0u;
    if (object == 0u || g_names == NULL) return 0;
    if (!read_value(object + (uintptr_t)L_object_name, &name_id)) return 0;
    return resolve_name(name_id, buffer, capacity);
}

static int object_class_name(uintptr_t object, char* buffer, size_t capacity) {
    uintptr_t cls = 0u;
    if (object == 0u) return 0;
    if (!read_value(object + (uintptr_t)L_object_class, &cls) || cls == 0u) return 0;
    return object_name(cls, buffer, capacity);
}

// 给类表定性:1=NPC 2=车辆 0=其它(不显示)
//   先看样例 actor 的根组件(最可靠的证据):
//     CapsuleComponent / CharacterMovementComponent / SkeletalMeshComponent → NPC
//     BrushComponent / SilentCheckComponent / 空 → 一定不是可显示对象(体积/触发物/空壳)
//   再看类名关键词(根组件是 StaticMesh 之类的,只能靠名字)
static int classify_actor(uintptr_t actor, const char* class_name, char* root_out,
    size_t root_capacity) {
    uintptr_t root = 0u;
    char root_cls[96];
    root_cls[0] = '\0';
    if (root_out != NULL && root_capacity > 0) root_out[0] = '\0';
    if (actor == 0u) return -1;
    if (!read_value(actor + (uintptr_t)L_actor_rootComponent, &root)) return -1;
    if (root == 0u) {
        if (root_out != NULL && root_capacity > 0) snprintf(root_out, root_capacity, "空");
        return 0;
    }
    if (!object_class_name(root, root_cls, sizeof(root_cls))) return -1;
    if (root_out != NULL && root_capacity > 0) {
        snprintf(root_out, root_capacity, "%s", root_cls);
    }
    if (strcmp(root_cls, "BrushComponent") == 0 ||
        strcmp(root_cls, "SilentCheckComponent") == 0) {
        return 0;
    }
    if (looks_like_scene_class(class_name)) return 0;
    if (looks_like_vehicle_class(class_name)) return 2;
    if (strcmp(root_cls, "CapsuleComponent") == 0 ||
        strcmp(root_cls, "CharacterMovementComponent") == 0 ||
        strcmp(root_cls, "SkeletalMeshComponent") == 0) {
        return 1;
    }
    return looks_like_npc_class(class_name) ? 1 : 0;
}

// 扫描结束后统计 NPC / 车辆数量(分类已在收集时做完,这里只汇总)
static void classes_report_write(void);

static void classify_class_entries(void) {
    int i;
    int npc = 0;
    int vehicle = 0;
    for (i = 0; i < g_class_entry_count; ++i) {
        int kind = g_class_entries[i].kind;
        if (kind < 0) kind = 0;
        g_class_entries[i].kind = kind;
        if (kind == 1) npc++;
        else if (kind == 2) vehicle++;
    }
    g_npc_class_count = npc;
    g_vehicle_class_count = vehicle;
    {
        char line[220];
        snprintf(line, sizeof(line), "CLASS 分类完成: NPC=%d 车辆=%d 其它=%d(默认不显示)",
            npc, vehicle, g_class_entry_count - npc - vehicle);
        if (g_core != NULL && g_core->log != NULL) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
        }
    }
    classes_report_write();
}

// 把分类结果写到 <游戏目录>\Anomaly\npc-summon-classes.txt,方便直接看「这个类到底是什么」
static void classes_report_write(void) {
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    static char buffer[32768];
    int length = 0;
    int i;
    HANDLE file;
    DWORD written = 0;

    if (!anomaly_dir_path(dir, MAX_PATH)) return;
    if (swprintf(path, MAX_PATH, L"%s\\npc-summon-classes.txt", dir) < 0) return;
    length += snprintf(buffer + length, sizeof(buffer) - (size_t)length,
        "# npc-summon classes v1  类别:NPC / 车辆 / 其它  格式:类名|类别|数量|根组件\r\n");
    for (i = 0; i < g_class_entry_count && length < (int)sizeof(buffer) - 300; ++i) {
        const char* kind = (g_class_entries[i].kind == 1) ? "NPC"
            : ((g_class_entries[i].kind == 2) ? "车辆" : "其它");
        snprintf(buffer + length, sizeof(buffer) - (size_t)length, "%s|%s|%u|%s\r\n",
            g_class_entries[i].name, kind, g_class_entries[i].count,
            g_class_entries[i].root_class);
        length = (int)strlen(buffer);
    }
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    (void)WriteFile(file, buffer, (DWORD)length, &written, NULL);
    CloseHandle(file);
    pipe_add("classes 清单 -> <游戏目录>\\Anomaly\\npc-summon-classes.txt");
}

// 指针是否落在模块代码/数据区(用于筛 Func 候选)
static int looks_like_module_pointer(uintptr_t value) {
    return value >= 0x140000000ull && value < 0x160000000ull;
}

// FField 版本的名字在 +32,UObject 版本在 +24,两个都试
static int ffield_or_object_name(uintptr_t address, char* buffer, size_t capacity) {
    uint32_t name_id = 0u;
    if (address == 0u) return 0;
    if (read_value(address + (uintptr_t)L_field_name, &name_id) && name_id != 0u &&
        resolve_name(name_id, buffer, capacity)) {
        return 1;
    }
    if (read_value(address + (uintptr_t)L_object_name, &name_id) && name_id != 0u &&
        resolve_name(name_id, buffer, capacity)) {
        return 1;
    }
    return 0;
}

// 字段指纹扫描:Children 为空时,在对象前 0x320 字节里找「像字段/函数指针且名字可解析」的槽位
static void fingerprint_scan(uintptr_t object, const char* label) {
    int offset;
    int emitted = 0;
    for (offset = 0x28; offset < 0x320 && emitted < 14; offset += 8) {
        uintptr_t value = 0u;
        char name[120];
        if (!read_value(object + (uintptr_t)offset, &value)) continue;
        if (value < 0x10000ull || value > 0x7FFFFFFFFFFFull) continue;
        name[0] = '\0';
        if (ffield_or_object_name(value, name, sizeof(name))) {
            pipe_add("   指纹 %s+0x%X -> 0x%llX 名=%s", label, offset,
                (unsigned long long)value, name);
            emitted++;
        }
    }
    if (emitted == 0) pipe_add("   指纹扫描 %s: 0x28~0x320 无候选", label);
}

// 沿 SuperStruct 链上溯,每层都看 Children 链;蓝图层通常为空,native 父类才有函数
static void dump_class_hierarchy(uintptr_t uclass, const char* label) {
    uintptr_t cls = uclass;
    int depth = 0;
    int total_summon = 0;

    while (cls != 0u && depth < 8) {
        char cls_name[120];
        uintptr_t children = 0u;
        cls_name[0] = '\0';
        if (!object_name(cls, cls_name, sizeof(cls_name))) {
            snprintf(cls_name, sizeof(cls_name), "<?>");
        }
        (void)read_value(cls + (uintptr_t)L_struct_children, &children);
        pipe_add("%s[%d] 类=%s children=0x%llX", label, depth, cls_name,
            (unsigned long long)children);

        if (children != 0u) {
            uintptr_t child = children;
            int index = 0;
            int hits = 0;
            while (child != 0u && index < 3000) {
                char field_name[120];
                field_name[0] = '\0';
                if (object_name(child, field_name, sizeof(field_name))) {
                    if (strstr(field_name, "Summon") != NULL || strstr(field_name, "summon") != NULL) {
                        uint32_t flags = 0u;
                        uint16_t num_parms = 0u;
                        uint16_t parms_size = 0u;
                        uint16_t return_offset = 0u;
                        int probe_offset;
                        hits++;
                        (void)read_value(child + (uintptr_t)L_function_flags, &flags);
                        (void)read_value(child + (uintptr_t)L_function_numParms, &num_parms);
                        (void)read_value(child + (uintptr_t)L_function_parmsSize, &parms_size);
                        (void)read_value(child + (uintptr_t)L_function_returnValueOffset, &return_offset);
                        pipe_add("  ★ UFunction=0x%llX 名=%s flags=0x%X native=%d parms=%u size=%u retOff=%u",
                            (unsigned long long)child, field_name, flags,
                            (flags & FUNC_NATIVE) ? 1 : 0, (unsigned)num_parms,
                            (unsigned)parms_size, (unsigned)return_offset);
                        for (probe_offset = 0x30; probe_offset < 0x120; probe_offset += 8) {
                            uintptr_t candidate = 0u;
                            if (!read_value(child + (uintptr_t)probe_offset, &candidate)) continue;
                            if (looks_like_module_pointer(candidate)) {
                                pipe_add("     函数体候选 +0x%X -> 0x%llX", probe_offset,
                                    (unsigned long long)candidate);
                            }
                        }
                    }
                }
                if (!read_value(child + (uintptr_t)L_field_next, &child)) break;
                index++;
            }
            pipe_add("   %s 字段=%d Summon命中=%d", cls_name, index, hits);
            total_summon += hits;
        } else if (depth < 2) {
            fingerprint_scan(cls, cls_name);
        } else {
            pipe_add("   %s children 为空(仅前两层做指纹扫描)", cls_name);
        }

        if (!read_value(cls + (uintptr_t)L_struct_superStruct, &cls)) break;
        depth++;
    }
    pipe_add("%s 总计 Summon* 命中=%d", label, total_summon);
}

// 直接按「类路径:函数名」试探(UE 的 UFunction 对外路径格式)
static void function_path_probe(void) {
    static const char* kPaths[] = {
        "/Script/HTGame.HTPlayerController:SummonAI",
        "/Script/HTGame.HTPlayerController:ServerSummonAI_HybridMode",
        "/Script/HTGame.HTPlayerController:SummonAI_HybridMode",
        "/Script/HTGame.HTAbilityCharacter:SummonAI",
        "/Script/HTGame.HTAbilityCharacter:ServerSummonAI_HybridMode",
        "/Script/HTGame.HTAICharacter:SummonAI",
        "/Script/HTGame.HTAICharacter:ServerSummonAI_HybridMode",
        "/Script/HTGame.HTAICharacter:DismissSummonedActor",
    };
    int i;
    if (g_objects == NULL || g_objects->find_exact == NULL) return;
    for (i = 0; i < (int)(sizeof(kPaths) / sizeof(kPaths[0])); ++i) {
        AnomalyGenerationHandleV1 handle = {0u, 0u};
        AnomalyStatusV1 result = g_objects->find_exact(g_objects->user, sv(kPaths[i]), &handle);
        if (ok(result)) {
            pipe_add("func路径命中 %s -> idx=%u", kPaths[i], (uint32_t)handle.id - 1u);
        } else {
            pipe_add("func路径 %s -> status=%u", kPaths[i], result.code);
        }
    }
}

static const int32_t L_property_elementSize = 52;
static const int32_t L_property_offsetInternal = 68;
static const int32_t L_ffield_next = 24;
static const int32_t L_ffield_class = 8;

// 读出某个 UFunction 的完整调用签名:头部字段 + 参数链(FProperty) + 函数体指针候选
static void dump_function_layout(const char* path) {
    AnomalyGenerationHandleV1 handle = {0u, 0u};
    uintptr_t func = 0u;
    uint32_t flags = 0u;
    uint16_t num_parms = 0u;
    uint16_t parms_size = 0u;
    uint16_t return_offset = 0u;
    int offset;
    int chain_found = 0;

    if (g_objects == NULL || g_objects->find_exact == NULL) return;
    if (!ok(g_objects->find_exact(g_objects->user, sv(path), &handle))) {
        pipe_add("布局 %s: find_exact 未命中", path);
        return;
    }
    func = object_by_handle(handle);
    if (func == 0u) {
        pipe_add("布局 %s: handle 换算地址失败", path);
        return;
    }
    (void)read_value(func + (uintptr_t)L_function_flags, &flags);
    (void)read_value(func + (uintptr_t)L_function_numParms, &num_parms);
    (void)read_value(func + (uintptr_t)L_function_parmsSize, &parms_size);
    (void)read_value(func + (uintptr_t)L_function_returnValueOffset, &return_offset);
    pipe_add("=== %s @0x%llX flags=0x%X native=%d numParms=%u parmsSize=%u retOff=%u ===",
        path, (unsigned long long)func, flags, (flags & FUNC_NATIVE) ? 1 : 0,
        (unsigned)num_parms, (unsigned)parms_size, (unsigned)return_offset);

    // 参数链:扫 0x28~0x200 找指向 FField 的指针(其名字可解析),再沿 Next 遍历
    for (offset = 0x28; offset < 0x200 && chain_found < 3; offset += 8) {
        uintptr_t candidate = 0u;
        char first_name[120];
        if (!read_value(func + (uintptr_t)offset, &candidate)) continue;
        if (candidate < 0x10000ull || candidate > 0x7FFFFFFFFFFFull) continue;
        first_name[0] = '\0';
        if (!ffield_or_object_name(candidate, first_name, sizeof(first_name))) continue;
        {
            uintptr_t field = candidate;
            int i = 0;
            pipe_add("  字段链 +0x%X -> 0x%llX 首项=%s", offset,
                (unsigned long long)candidate, first_name);
            while (field != 0u && i < 24) {
                char pname[120];
                char tname[100];
                uintptr_t pclass = 0u;
                int32_t p_offset = -1;
                int32_t p_size = 0;
                pname[0] = '\0';
                (void)ffield_or_object_name(field, pname, sizeof(pname));
                (void)read_value(field + (uintptr_t)L_ffield_class, &pclass);
                tname[0] = '\0';
                if (pclass != 0u && !object_name(pclass, tname, sizeof(tname))) {
                    snprintf(tname, sizeof(tname), "<?>");
                }
                (void)read_value(field + (uintptr_t)L_property_offsetInternal, &p_offset);
                (void)read_value(field + (uintptr_t)L_property_elementSize, &p_size);
                pipe_add("    参数[%d] %-24s : %-18s offset=%d size=%d", i, pname, tname,
                    p_offset, p_size);
                if (!read_value(field + (uintptr_t)L_ffield_next, &field)) break;
                i++;
            }
            chain_found++;
        }
    }
    if (chain_found == 0) pipe_add("  未定位到参数链(0x28~0x200 内无 FField 候选)");

    // 函数体指针候选(native 实现入口)
    for (offset = 0x28; offset < 0x200; offset += 8) {
        uintptr_t candidate = 0u;
        if (!read_value(func + (uintptr_t)offset, &candidate)) continue;
        if (looks_like_module_pointer(candidate)) {
            pipe_add("    函数体候选 +0x%X -> 0x%llX", offset, (unsigned long long)candidate);
        }
    }
}

// ---------------------------------------------------------------------------
// ProcessEvent 定位(靠「被 MinHook 改写过」这一特征判定)与调用测试
// ---------------------------------------------------------------------------

// Runtime 自报的 ue5.ProcessEvent RVA(来自 0.2.0 日志的 symbols 表)。
// 0.27 的 A/B 实测证明它指向的不是 ProcessEvent(头字节 60 48 85 C0…,x64 下 0x60 不是
// 合法指令入口):经它调用 GetTransform 会让栈帧被写坏,6~9 秒后 GS cookie 检查失败,
// 崩溃签名 0xc0000409 / FAST_FAIL_STACK_COOKIE_CHECK_FAILURE。现在只作诊断输出,禁止用于调用。
static const uintptr_t kProcessEventRva = 0x18AACD0ull;

static uintptr_t g_process_event_fn = 0u;
static int g_call_pending = 0;
static char g_call_result[220] = {0};

typedef void (*ProcessEventFn)(void* object, void* function, void* parms);

static uintptr_t module_base(void) {
    return (uintptr_t)GetModuleHandleA("HTGame.exe");
}

// MinHook 的内联 hook 会把函数头改成 jmp 系列指令
static int looks_hooked(const uint8_t* bytes, int count) {
    if (count >= 5 && bytes[0] == 0xE9) return 1;                                  // jmp rel32
    if (count >= 6 && bytes[0] == 0xFF && bytes[1] == 0x25) return 1;              // jmp [rip+disp32]
    if (count >= 12 && bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) {
        return 1;                                                                  // mov rax,imm64; jmp rax
    }
    return 0;
}

// 字段是函数(UFunction)还是属性(FProperty)?函数的字段类名含 Function
static int is_function_field(uintptr_t field) {
    uintptr_t field_class = 0u;
    char class_name[80];
    if (field == 0u) return 0;
    if (!read_value(field + (uintptr_t)L_object_class, &field_class) || field_class == 0u) return 0;
    class_name[0] = '\0';
    if (!object_name(field_class, class_name, sizeof(class_name))) return 0;
    return (strstr(class_name, "Function") != NULL) ? 1 : 0;
}

// 入口健全性检查:拒绝明显不是函数开头的字节。
//   0x60 = x64 下非法指令(0.27 那个 base+RVA 候选正是 60 48 85 C0…)
//   全 0x00 / 0xCC 填充 = 没映射到代码
static int looks_like_code_entry(const uint8_t* bytes, int count) {
    int i;
    int nonzero = 0;
    if (count < 4) return 0;
    if (bytes[0] == 0x60) return 0;
    for (i = 0; i < count; ++i) {
        if (bytes[i] != 0x00 && bytes[i] != 0xCC) nonzero = 1;
    }
    return nonzero;
}

// ---------------------------------------------------------------------------
// ProcessEvent 语义锚点扫描
// ---------------------------------------------------------------------------
// 游戏更新会重编译,函数序言(压栈/开栈帧的大小)一变,profile 的长签名就失效 ——
// 2026-10-18 更新后 #0 长签名 status=3,只剩通用序言能命中,拿到的是假入口。
// 这里改按「语义骨架」找,这三段是 ProcessEvent 的逻辑核心,不随编译变化:
//   锚点 A: F7 82 B0 00 00 00 00 04 00 00
//           test dword ptr [rdx+0B0h], 400h —— UFunction::FunctionFlags & FUNC_Native
//   锚点 B: C1 E8 1E —— shr eax,1Eh(开头判断 UObject 标志)
//   锚点 C: 48 33 C5 —— 栈 cookie 校验(xor reg, rsp)
// 只在 PE 异常目录(.pdata)登记的函数起点上找候选,避免把函数中段当入口。
static const uint8_t kPeNativeTest[10] = {0xF7,0x82,0xB0,0x00,0x00,0x00,0x00,0x04,0x00,0x00};
static const uint8_t kPeFlagShift[3] = {0xC1,0xE8,0x1E};
static const uint8_t kPeCookie[3] = {0x48,0x33,0xC5};

typedef struct RtfEntryV1 {
    uint32_t begin;
    uint32_t end;
    uint32_t unwind;
} RtfEntryV1;

static uintptr_t g_pe_anchor_cache = 0u;
static int g_pe_anchor_done = 0;

static int window_has(const uint8_t* p, size_t n, const uint8_t* needle, size_t needle_len) {
    size_t i = 0u;
    if (needle_len == 0u || n < needle_len) return 0;
    // 用 memchr 找首字节(内部是 SIMD),避免对几十万个函数逐字节扫
    while (i + needle_len <= n) {
        const uint8_t* hit = (const uint8_t*)memchr(p + i, needle[0], n - needle_len + 1u - i);
        if (hit == NULL) return 0;
        if (memcmp(hit, needle, needle_len) == 0) return 1;
        i = (size_t)(hit - p) + 1u;
    }
    return 0;
}

static uintptr_t find_process_event_by_anchors(char* note, size_t note_size) {
    uint8_t* base = (uint8_t*)GetModuleHandleA("HTGame.exe");
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS64* nt;
    IMAGE_DATA_DIRECTORY* dir;
    RtfEntryV1* table;
    uint32_t count;
    uint32_t i;
    uintptr_t best = 0u;
    size_t best_size = 0u;
    int hits = 0;

    if (note != NULL && note_size > 0u) note[0] = '\0';
    if (base == NULL) {
        if (note != NULL && note_size > 0u) snprintf(note, note_size, "锚点扫描: 拿不到模块基址");
        return 0u;
    }
    dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        if (note != NULL && note_size > 0u) snprintf(note, note_size, "锚点扫描: DOS 头不认");
        return 0u;
    }
    nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        if (note != NULL && note_size > 0u) snprintf(note, note_size, "锚点扫描: NT 头不认");
        return 0u;
    }
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir->VirtualAddress == 0u || dir->Size < sizeof(RtfEntryV1)) {
        if (note != NULL && note_size > 0u) {
            snprintf(note, note_size, "锚点扫描: 没有 .pdata(size=%lu),无法按函数起点定位",
                (unsigned long)dir->Size);
        }
        return 0u;
    }
    table = (RtfEntryV1*)(base + dir->VirtualAddress);
    count = (uint32_t)(dir->Size / sizeof(RtfEntryV1));
    for (i = 0; i < count; ++i) {
        uint32_t begin = table[i].begin;
        uint32_t end = table[i].end;
        const uint8_t* p;
        size_t size;
        size_t window;
        if (begin >= end) continue;
        size = (size_t)(end - begin);
        if (size < 24u || size > 0x20000u) continue;
        p = base + begin;
        window = (size < 0x200u) ? size : 0x200u;
        if (!window_has(p, window, kPeNativeTest, sizeof(kPeNativeTest))) continue;
        if (!window_has(p, window, kPeFlagShift, sizeof(kPeFlagShift))) continue;
        if (!window_has(p, window, kPeCookie, sizeof(kPeCookie))) continue;
        ++hits;
        if (size > best_size) {
            best_size = size;
            best = (uintptr_t)p;
        }
    }
    if (note != NULL && note_size > 0u) {
        snprintf(note, note_size, "锚点扫描: .pdata=%lu 个函数, 命中=%d, 采用=0x%llX",
            (unsigned long)count, hits, (unsigned long long)best);
    }
    return best;
}

// 候选自检:入口像代码、函数体里同时有「FUNC_Native 检查」和「标志右移」
static int pe_candidate_ok(uintptr_t address) {
    uint8_t buf[0x200];
    if (address == 0u) return 0;
    memset(buf, 0, sizeof(buf));
    if (!read_bytes(address, buf, sizeof(buf))) return 0;
    if (!looks_like_code_entry(buf, 16)) return 0;
    if (!window_has(buf, sizeof(buf), kPeNativeTest, sizeof(kPeNativeTest))) return 0;
    if (!window_has(buf, sizeof(buf), kPeFlagShift, sizeof(kPeFlagShift))) return 0;
    return 1;
}

static void process_event_probe(void) {
    uintptr_t base = module_base();
    uintptr_t sig = g_process_event;
    uintptr_t scanned = 0u;
    char note[260];

    // 1) 官方完整签名(#0)—— 游戏更新后可能失效
    // 2) 语义锚点扫描 —— 不依赖函数序言,作为兜底
    if (g_pe_anchor_done == 0) {
        g_pe_anchor_cache = find_process_event_by_anchors(note, sizeof(note));
        g_pe_anchor_done = 1;
        if (note[0] != '\0') pipe_add("%s", note);
    } else {
        pipe_add("锚点扫描: 复用上次结果 0x%llX", (unsigned long long)g_pe_anchor_cache);
    }
    scanned = g_pe_anchor_cache;

    pipe_add("模块基址=0x%llX 签名=0x%llX 锚点=0x%llX",
        (unsigned long long)base, (unsigned long long)sig, (unsigned long long)scanned);

    g_process_event_fn = 0u;
    if (scanned != 0u && pe_candidate_ok(scanned)) {
        g_process_event_fn = scanned;
        pipe_add("PE: 采用锚点扫描 0x%llX", (unsigned long long)g_process_event_fn);
    } else if (sig != 0u && pe_candidate_ok(sig)) {
        g_process_event_fn = sig;
        pipe_add("PE: 采用签名命中 0x%llX(锚点自检通过)", (unsigned long long)g_process_event_fn);
    } else if (sig != 0u) {
        // 2026-10-18 的崩溃就是这条:签名命中的函数体里没有 ProcessEvent 的锚点
        pipe_add("PE: 签名命中 0x%llX 但函数体没有 ProcessEvent 锚点 —— 拒绝使用",
            (unsigned long long)sig);
    } else {
        pipe_add("PE: 没有可用入口 —— 本次禁用生成调用(不去调用,避免写坏游戏栈)");
    }
    stage_log("PE 采用=0x%llX mode=%d 签名=0x%llX 锚点=0x%llX",
        (unsigned long long)g_process_event_fn, g_test_mode,
        (unsigned long long)sig, (unsigned long long)scanned);
}

// controller 实例的字段表:给 AIClass / LeaderNetCharacter 找现成值来源
static void controller_field_dump(void) {
    static const char* kKeys[] = {"Summon", "AIClass", "AI", "Leader", "NetCharacter",
        "Character", "Pet", "Camp", "Class"};
    AnomalyGenerationHandleV1 handle = {0u, 0u};
    uintptr_t cls = 0u;
    uintptr_t child = 0u;
    int index = 0;
    int emitted = 0;

    if (g_objects == NULL || g_objects->find_exact == NULL) return;
    if (!ok(g_objects->find_exact(g_objects->user, sv("/Script/HTGame.HTPlayerController"), &handle))) {
        return;
    }
    cls = object_by_handle(handle);
    if (cls == 0u || !read_value(cls + (uintptr_t)L_struct_children, &child) || child == 0u) return;

    pipe_add("---- HTPlayerController 相关字段(供参数取值参考) ----");
    while (child != 0u && index < 400 && emitted < 26) {
        char fname[120];
        fname[0] = '\0';
        if (object_name(child, fname, sizeof(fname)) && is_function_field(child) == 0) {
            int k;
            for (k = 0; k < (int)(sizeof(kKeys) / sizeof(kKeys[0])); ++k) {
                if (strstr(fname, kKeys[k]) != NULL) {
                    int32_t p_offset = -1;
                    int32_t p_size = 0;
                    (void)read_value(child + (uintptr_t)L_property_offsetInternal, &p_offset);
                    (void)read_value(child + (uintptr_t)L_property_elementSize, &p_size);
                    pipe_add("  字段 %-34s offset=%-5d size=%d", fname, p_offset, p_size);
                    emitted++;
                    break;
                }
            }
        }
        if (!read_value(child + (uintptr_t)L_field_next, &child)) break;
        index++;
    }
    if (emitted == 0) pipe_add("  (没找到相关字段名)");
}

// 在 Game 线程域执行的调用测试:用 ProcessEvent 调一个只读 getter
static int run_call_test(void) {
    AnomalyGenerationHandleV1 handle = {0u, 0u};
    uintptr_t func = 0u;
    uint8_t parms[128];
    double x = 0.0, y = 0.0, z = 0.0;
    uint16_t parms_size = 0u;
    uint16_t return_offset = 0u;

    if (g_process_event_fn == 0u || g_controller == 0u) {
        snprintf(g_call_result, sizeof(g_call_result), "跳过:PE=0x%llX controller=0x%llX",
            (unsigned long long)g_process_event_fn, (unsigned long long)g_controller);
        return 0;
    }
    if (g_objects == NULL ||
        !ok(g_objects->find_exact(g_objects->user, sv("/Script/Engine.Actor:K2_GetActorLocation"), &handle))) {
        snprintf(g_call_result, sizeof(g_call_result), "跳过:找不到 K2_GetActorLocation");
        return 0;
    }
    func = object_by_handle(handle);
    if (func == 0u) {
        snprintf(g_call_result, sizeof(g_call_result), "跳过:函数地址换算失败");
        return 0;
    }
    (void)read_value(func + (uintptr_t)L_function_parmsSize, &parms_size);
    (void)read_value(func + (uintptr_t)L_function_returnValueOffset, &return_offset);
    if (parms_size == 0u || parms_size > (uint16_t)sizeof(parms)) {
        snprintf(g_call_result, sizeof(g_call_result), "跳过:parmsSize=%u 不合理", (unsigned)parms_size);
        return 0;
    }
    memset(parms, 0, sizeof(parms));

    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            sv("CALLTEST 开始:即将调用 ProcessEvent(K2_GetActorLocation)"));
    }
    ((ProcessEventFn)g_process_event_fn)((void*)g_controller, (void*)func, parms);
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            sv("CALLTEST 返回:没有崩溃,已拿到返回值"));
    }
    memcpy(&x, parms + return_offset, sizeof(x));
    memcpy(&y, parms + return_offset + 8u, sizeof(y));
    memcpy(&z, parms + return_offset + 16u, sizeof(z));
    snprintf(g_call_result, sizeof(g_call_result),
        "调用成功:controller 坐标 = (%.1f, %.1f, %.1f)", x, y, z);
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(g_call_result));
    }
    return 1;
}

// 两步构造 AIClass 要的软类引用:
//   MakeSoftClassPath(FString) -> 32 字节
//   Conv_SoftClassPathToSoftClassRef(上一步结果) -> 40 字节
// 顺便把第一步返回值里的 FName 解析出来,自校验路径是否正确
static int run_softclassref_test(void) {
    static wchar_t path_buffer[600];
    static const wchar_t* kPath =
        L"/Game/Blueprints/Character/Monster/NPC_Sub_fs_Simple_think.NPC_Sub_fs_Simple_think_C";
    AnomalyGenerationHandleV1 handle = {0u, 0u};
    AnomalyGenerationHandleV1 lib_handle = {0u, 0u};
    uintptr_t lib_class = 0u;
    uintptr_t lib_cdo = 0u;
    uintptr_t func1 = 0u;
    uintptr_t func2 = 0u;
    uint8_t parms1[192];
    uint8_t parms2[192];
    uint16_t size1 = 0u;
    uint16_t size2 = 0u;
    uint16_t ret1 = 0u;
    uint16_t ret2 = 0u;
    uintptr_t string_address = 0u;
    int32_t string_num = 0;
    char name_a[200];
    char name_b[200];
    char hex[200];
    uint32_t name_id_a = 0u;
    uint32_t name_id_b = 0u;
    int i;
    int n;
    size_t copy_len;

    if (g_process_event_fn == 0u || g_objects == NULL) {
        snprintf(g_call_result, sizeof(g_call_result), "③ 跳过:前置条件不足");
        return 0;
    }
    if (!ok(g_objects->find_exact(g_objects->user,
            sv("/Script/Engine.KismetSystemLibrary"), &lib_handle))) {
        snprintf(g_call_result, sizeof(g_call_result), "③ 跳过:拿不到函数库类");
        return 0;
    }
    lib_class = object_by_handle(lib_handle);
    if (lib_class != 0u) {
        (void)read_value(lib_class + (uintptr_t)L_uclass_classDefaultObject, &lib_cdo);
    }
    if (lib_cdo == 0u ||
        !ok(g_objects->find_exact(g_objects->user,
            sv("/Script/Engine.KismetSystemLibrary:MakeSoftClassPath"), &handle))) {
        snprintf(g_call_result, sizeof(g_call_result), "③ 跳过:MakeSoftClassPath 不可用");
        return 0;
    }
    func1 = object_by_handle(handle);
    if (!ok(g_objects->find_exact(g_objects->user,
            sv("/Script/Engine.KismetSystemLibrary:Conv_SoftClassPathToSoftClassRef"), &handle))) {
        snprintf(g_call_result, sizeof(g_call_result), "③ 跳过:Conv 函数不可用");
        return 0;
    }
    func2 = object_by_handle(handle);
    if (func1 == 0u || func2 == 0u) {
        snprintf(g_call_result, sizeof(g_call_result), "③ 跳过:函数地址换算失败");
        return 0;
    }

    (void)read_value(func1 + (uintptr_t)L_function_parmsSize, &size1);
    (void)read_value(func1 + (uintptr_t)L_function_returnValueOffset, &ret1);
    (void)read_value(func2 + (uintptr_t)L_function_parmsSize, &size2);
    (void)read_value(func2 + (uintptr_t)L_function_returnValueOffset, &ret2);
    if (size1 == 0u || size2 == 0u || size1 > (uint16_t)sizeof(parms1) ||
        size2 > (uint16_t)sizeof(parms2) || ret1 + 32u > size1) {
        snprintf(g_call_result, sizeof(g_call_result),
            "③ 跳过:参数尺寸异常 size1=%u ret1=%u size2=%u", (unsigned)size1, (unsigned)ret1,
            (unsigned)size2);
        return 0;
    }

    // ---- 第一步:MakeSoftClassPath ----
    memset(parms1, 0, sizeof(parms1));
    n = 0;
    while (kPath[n] != L'\0' && n < 599) {
        path_buffer[n] = kPath[n];
        ++n;
    }
    path_buffer[n] = L'\0';
    string_address = (uintptr_t)path_buffer;
    string_num = (int32_t)n + 1;
    memcpy(parms1 + 0, &string_address, 8);
    memcpy(parms1 + 8, &string_num, 4);
    memcpy(parms1 + 12, &string_num, 4);

    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            sv("SOFTCLASSREF 第1步:MakeSoftClassPath"));
    }
    ((ProcessEventFn)g_process_event_fn)((void*)lib_cdo, (void*)func1, parms1);

    memcpy(&name_id_a, parms1 + ret1, 4);
    memcpy(&name_id_b, parms1 + ret1 + 8u, 4);
    name_a[0] = '\0';
    name_b[0] = '\0';
    if (!resolve_name(name_id_a, name_a, sizeof(name_a))) {
        snprintf(name_a, sizeof(name_a), "<?>(0x%X)", name_id_a);
    }
    if (!resolve_name(name_id_b, name_b, sizeof(name_b))) {
        snprintf(name_b, sizeof(name_b), "<?>(0x%X)", name_id_b);
    }
    {
        char line[400];
        snprintf(line, sizeof(line), "SOFTCLASSREF 第1步返回: FName#1(0x%X)=%s | FName#2(0x%X)=%s",
            name_id_a, name_a, name_id_b, name_b);
        if (g_core != NULL && g_core->log != NULL) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
        }
    }

    // ---- 第二步:Conv_SoftClassPathToSoftClassRef ----
    memset(parms2, 0, sizeof(parms2));
    copy_len = (size_t)size2;
    if (copy_len > 32u) copy_len = 32u;
    memcpy(parms2 + 0, parms1 + ret1, copy_len);

    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            sv("SOFTCLASSREF 第2步:Conv_SoftClassPathToSoftClassRef"));
    }
    ((ProcessEventFn)g_process_event_fn)((void*)lib_cdo, (void*)func2, parms2);
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            sv("SOFTCLASSREF 第2步返回:没有崩溃"));
    }

    hex[0] = '\0';
    for (i = 0; i < 40 && (ret2 + i) < (int)sizeof(parms2); ++i) {
        char part[8];
        snprintf(part, sizeof(part), "%02X", parms2[ret2 + i]);
        strncat(hex, part, sizeof(hex) - strlen(hex) - 1u);
    }
    snprintf(g_call_result, sizeof(g_call_result),
        "③ size1=%u ret1=%u size2=%u ret2=%u 软类引用(40B)=%s",
        (unsigned)size1, (unsigned)ret1, (unsigned)size2, (unsigned)ret2, hex);
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(g_call_result));
    }
    return 1;
}

static const int32_t L_controller_pawn = 776;

// 前置声明(这些 helper 定义在本节之后)
static uintptr_t get_pawn(void);
static int call_by_path(uintptr_t object, const char* path, uint8_t* parms, size_t capacity,
    uint16_t* ret_off);
static uintptr_t library_cdo(const char* class_path);

// ---------------------------------------------------------------------------
// 0.10.0:真正的召唤 —— 组装 SummonAI 的九个参数并调用
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 0.12.0:绕开 DPS 工具 —— 用引擎自带的 GameplayStatics 生成 Actor
//    Runtime 自己 spawn 载具就是走这两步(Runtime 的错误串里可见),说明该路径在本游戏可用
// ---------------------------------------------------------------------------

#define MAX_PARAMS 12

typedef struct ParamInfo {
    char name[72];
    int32_t offset;
    int32_t size;
} ParamInfo;

// 读一个 UFunction 的参数表(名字/偏移/大小)。参数链实测固定落在 +0x50
static int read_function_params(uintptr_t func, ParamInfo* params) {
    uintptr_t field = 0u;
    int i = 0;
    if (func == 0u) return 0;
    if (!read_value(func + 0x50u, &field) || field == 0u) return 0;
    while (field != 0u && i < MAX_PARAMS) {
        params[i].name[0] = '\0';
        (void)ffield_or_object_name(field, params[i].name, sizeof(params[i].name));
        params[i].offset = -1;
        params[i].size = 0;
        (void)read_value(field + (uintptr_t)L_property_offsetInternal, &params[i].offset);
        (void)read_value(field + (uintptr_t)L_property_elementSize, &params[i].size);
        i++;
        if (!read_value(field + (uintptr_t)L_ffield_next, &field)) break;
    }
    return i;
}

// 按参数名写入值;返回是否写入成功
static int set_param_by_name(
    const ParamInfo* params, int count, uint8_t* parms, size_t capacity,
    const char* name, const void* value, int32_t value_size) {
    int i;
    for (i = 0; i < count; ++i) {
        if (strcmp(params[i].name, name) != 0) continue;
        if (params[i].offset < 0 || (size_t)params[i].offset + (size_t)value_size > capacity) return 0;
        memcpy(parms + params[i].offset, value, (size_t)value_size);
        return 1;
    }
    return 0;
}

static const char* find_param_name(const ParamInfo* params, int count, int32_t size_wanted,
    const char* fallback) {
    int i;
    for (i = 0; i < count; ++i) {
        if (params[i].size == size_wanted) return params[i].name;
    }
    return fallback;
}

// 从世界里的实例反查它的 UClass —— 免去猜蓝图对象路径
static uintptr_t find_class_from_world(const char* class_name_substring) {
    static AnomalyNteEntitySnapshotV1 page[256];
    AnomalyNteEntityFrameV1 frame;
    AnomalyNteEntityPageRequestV1 request;
    AnomalyNteEntityPageResultV1 result;
    uint32_t offset = 0u;
    int guard = 0;
    int slot;
    int i;

    if (g_actors == NULL || g_actors->frame == NULL || g_actors->page == NULL) return 0u;
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = sizeof(frame);
    if (!ok(g_actors->frame(g_actors->user, &frame))) return 0u;
    for (slot = 0; slot < 256; ++slot) page[slot].struct_size = sizeof(AnomalyNteEntitySnapshotV1);

    while (guard++ < 16) {
        memset(&request, 0, sizeof(request));
        request.struct_size = sizeof(request);
        request.generation = frame.generation;
        request.offset = offset;
        request.capacity = 256u;
        memset(&result, 0, sizeof(result));
        result.struct_size = sizeof(result);
        if (!ok(g_actors->page(g_actors->user, &request, page, &result))) break;
        for (i = 0; i < (int)result.returned; ++i) {
            char cls[140];
            uintptr_t actor = 0u;
            uintptr_t uclass = 0u;
            cls[0] = '\0';
            if (!actor_class_name(page[i].class_id, cls, sizeof(cls))) continue;
            if (strstr(cls, class_name_substring) == NULL) continue;
            actor = object_by_handle(page[i].handle);
            if (actor == 0u) continue;
            if (read_value(actor + (uintptr_t)L_object_class, &uclass) && uclass != 0u) {
                return uclass;
            }
        }
        if (result.returned == 0u || result.next_offset >= result.total_matches) break;
        offset = result.next_offset;
    }
    return 0u;
}

// 用 GameplayStatics 两步生成 NPC;do_call=0 只打印参数表(安全)
static int run_spawn(int do_call) {
    static const wchar_t* kTarget =
        L"/Game/Blueprints/Character/Monster/NPC_Sub_fs_Simple_think.NPC_Sub_fs_Simple_think_C";
    AnomalyGenerationHandleV1 cls_handle = {0u, 0u};
    uintptr_t lib_cdo = 0u;
    uintptr_t pawn = 0u;
    uintptr_t npc_class = 0u;
    uintptr_t func_begin = 0u;
    uintptr_t func_finish = 0u;
    uint8_t xform[192];
    uint8_t parms[320];
    uint16_t r_xf = 0u;
    uint16_t r_begin = 0u;
    uint16_t r_finish = 0u;
    uintptr_t spawned = 0u;
    ParamInfo params[MAX_PARAMS];
    int count = 0;
    int ok_field = 0;
    int i;
    char line[420];

#define SPAWN_LOG(level_, text_) \
    do { if (g_core != NULL && g_core->log != NULL) { \
        g_core->log(g_core->user, (level_), sv(text_)); } } while (0)
#define SPAWN_FAIL(text_) \
    do { snprintf(g_call_result, sizeof(g_call_result), "%s", (text_)); \
        if (g_core != NULL && g_core->log != NULL) { \
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR, sv(g_call_result)); } \
        return 0; } while (0)

    if (g_process_event_fn == 0u) SPAWN_FAIL("⑥ 跳过:ProcessEvent 未确认");
    if (g_objects == NULL) SPAWN_FAIL("⑥ 跳过:ue5.objects 不可用");
    pawn = get_pawn();
    if (pawn == 0u) SPAWN_FAIL("⑥ 跳过:pawn 取不到");
    lib_cdo = library_cdo("/Script/Engine.GameplayStatics");
    if (lib_cdo == 0u) SPAWN_FAIL("⑥ 跳过:GameplayStatics CDO 取不到");

    // 目标类的 UClass:优先从世界里的实例反查(不依赖蓝图对象路径)
    npc_class = find_class_from_world("NPC_Sub_fs_Simple_think");
    if (npc_class == 0u) {
        npc_class = find_class_from_world("NPC_Sub_");
    }
    if (npc_class == 0u) {
        npc_class = find_class_from_world("BP_NPC_Mass");
    }
    if (npc_class == 0u) {
        AnomalyGenerationHandleV1 fallback_handle = {0u, 0u};
        if (ok(g_objects->find_exact(g_objects->user, sv(
                "/Game/Blueprints/Character/Monster/NPC_Sub_fs_Simple_think.NPC_Sub_fs_Simple_think_C"),
                &fallback_handle))) {
            npc_class = object_by_handle(fallback_handle);
        }
    }
    if (npc_class == 0u) {
        SPAWN_FAIL("⑥ 跳过:世界上找不到该类实例,猜的路径也命不中");
    }
    {
        char cls_name[140];
        char outer_name[140];
        uintptr_t outer = 0u;
        cls_name[0] = '\0';
        outer_name[0] = '\0';
        if (!object_name(npc_class, cls_name, sizeof(cls_name))) {
            snprintf(cls_name, sizeof(cls_name), "<?>");
        }
        if (read_value(npc_class + (uintptr_t)L_object_outer, &outer) && outer != 0u) {
            if (!object_name(outer, outer_name, sizeof(outer_name))) {
                snprintf(outer_name, sizeof(outer_name), "<?>");
            }
        }
        snprintf(line, sizeof(line), "SPAWN 目标类=0x%llX 名=%s 外层=%s",
            (unsigned long long)npc_class, cls_name, outer_name);
        SPAWN_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, line);
    }

    // pawn 的 transform(96 字节)
    memset(xform, 0, sizeof(xform));
    if (!call_by_path(pawn, "/Script/Engine.Actor:GetTransform", xform, sizeof(xform), &r_xf)) {
        SPAWN_FAIL("⑥ 失败:读 pawn transform 失败");
    }

    if (!ok(g_objects->find_exact(g_objects->user,
            sv("/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass"), &cls_handle))) {
        SPAWN_FAIL("⑥ 跳过:BeginDeferredActorSpawnFromClass 不可用");
    }
    func_begin = object_by_handle(cls_handle);
    count = read_function_params(func_begin, params);
    {
        char names[260];
        names[0] = '\0';
        for (i = 0; i < count; ++i) {
            strncat(names, params[i].name, sizeof(names) - strlen(names) - 1u);
            strncat(names, " ", sizeof(names) - strlen(names) - 1u);
        }
        snprintf(line, sizeof(line), "SPAWN BeginDeferred 参数(%d): %s", count, names);
        SPAWN_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, line);
    }
    if (count <= 0) SPAWN_FAIL("⑥ 失败:读不到 BeginDeferred 的参数表");

    memset(parms, 0, sizeof(parms));
    {
        uintptr_t world_context = pawn;
        uintptr_t actor_class = npc_class;
        uintptr_t owner_or_instigator = pawn;
        uint8_t collision = 0;
        (void)set_param_by_name(params, count, parms, sizeof(parms), "WorldContextObject",
            &world_context, 8);
        (void)set_param_by_name(params, count, parms, sizeof(parms), "WorldContext",
            &world_context, 8);
        ok_field = set_param_by_name(params, count, parms, sizeof(parms), "ActorClass",
            &actor_class, 8);
        (void)set_param_by_name(params, count, parms, sizeof(parms), "SpawnTransform",
            xform + r_xf, 96);
        (void)set_param_by_name(params, count, parms, sizeof(parms), "CollisionHandlingOverride",
            &collision, 1);
        (void)set_param_by_name(params, count, parms, sizeof(parms), "Owner",
            &owner_or_instigator, 8);
        (void)set_param_by_name(params, count, parms, sizeof(parms), "Instigator",
            &owner_or_instigator, 8);
        if (ok_field == 0) SPAWN_FAIL("⑥ 失败:ActorClass 参数没找到,无法指定要生成的类");
    }

    if (do_call == 0) {
        snprintf(g_call_result, sizeof(g_call_result),
            "⑥ 参数表已读出(%d 个),未调用。可点 ⑦ 真正生成", count);
        SPAWN_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, g_call_result);
        return 1;
    }

    // 第一步:BeginDeferred...
    SPAWN_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "SPAWN 第1步:BeginDeferredActorSpawnFromClass");
    if (!call_by_path(lib_cdo,
            "/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass",
            parms, sizeof(parms), &r_begin)) {
        SPAWN_FAIL("⑥ 失败:BeginDeferred 调用入口不可用");
    }
    SPAWN_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "SPAWN 第1步返回:没有崩溃");
    memcpy(&spawned, parms + r_begin, 8);
    snprintf(line, sizeof(line), "SPAWN 第1步结果 actor=0x%llX retOff=%u",
        (unsigned long long)spawned, (unsigned)r_begin);
    SPAWN_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, line);
    if (spawned == 0u) {
        snprintf(g_call_result, sizeof(g_call_result),
            "⑥ BeginDeferred 返回空 —— 引擎拒绝生成(类不可 spawn 或参数不符)");
        SPAWN_LOG(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, g_call_result);
        return 0;
    }

    // 第二步:FinishSpawningActor
    if (ok(g_objects->find_exact(g_objects->user,
            sv("/Script/Engine.GameplayStatics:FinishSpawningActor"), &cls_handle))) {
        func_finish = object_by_handle(cls_handle);
    }
    if (func_finish != 0u) {
        ParamInfo fparams[MAX_PARAMS];
        int fcount = read_function_params(func_finish, fparams);
        memset(parms, 0, sizeof(parms));
        (void)set_param_by_name(fparams, fcount, parms, sizeof(parms), "Actor", &spawned, 8);
        (void)set_param_by_name(fparams, fcount, parms, sizeof(parms), "SpawnTransform",
            xform + r_xf, 96);
        (void)set_param_by_name(fparams, fcount, parms, sizeof(parms), "UserSpawnTransform",
            xform + r_xf, 96);
        SPAWN_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "SPAWN 第2步:FinishSpawningActor");
        if (call_by_path(lib_cdo, "/Script/Engine.GameplayStatics:FinishSpawningActor",
                parms, sizeof(parms), &r_finish)) {
            SPAWN_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "SPAWN 第2步返回:没有崩溃");
        }
    }

    snprintf(g_call_result, sizeof(g_call_result),
        "⑦ 生成成功!Actor=0x%llX —— 看画面里有没有出现", (unsigned long long)spawned);
    SPAWN_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, g_call_result);
    return 1;

#undef SPAWN_FAIL
#undef SPAWN_LOG
}

// 回读一个 actor 的包围盒底部 Z。UE 里函数名是 GetActorBounds(不是 K2_ 前缀),
// 但不同版本/蓝图库命名有差异,所以逐个候选试。
static int read_actor_bottom(uintptr_t actor, uint8_t* buf, size_t capacity, double* out_bottom) {
    static const char* kPaths[2] = {
        "/Script/Engine.Actor:GetActorBounds",
        "/Script/Engine.Actor:K2_GetActorBounds",
    };
    ParamInfo params[MAX_PARAMS];
    int path_index;

    if (g_objects == NULL || g_process_event_fn == 0u || actor == 0u) return 0;

    for (path_index = 0; path_index < 2; ++path_index) {
        AnomalyGenerationHandleV1 handle = {0u, 0u};
        uintptr_t func = 0u;
        uint16_t parms_size = 0u;
        uint16_t ret_off = 0u;
        int count;
        int i;
        int have_extent = 0;
        // true:只用碰撞胶囊。实测 false 会返回畸形包围盒(把世界 bounds 算进来,
        // extent 达 1726),把落点算成浮空
        uint8_t only_colliding = 1u;
        uint8_t include_children = 0u;
        double oz = 0.0;
        double ez = 0.0;

        if (!ok(g_objects->find_exact(g_objects->user, sv(kPaths[path_index]), &handle))) continue;
        func = object_by_handle(handle);
        if (func == 0u) continue;
        (void)read_value(func + (uintptr_t)L_function_parmsSize, &parms_size);
        if (parms_size == 0u || parms_size > (uint16_t)capacity) continue;
        count = read_function_params(func, params);
        if (count <= 0) continue;

        memset(buf, 0, capacity);
        (void)set_param_by_name(params, count, buf, capacity, "bOnlyCollidingComponents",
            &only_colliding, 1);
        (void)set_param_by_name(params, count, buf, capacity, "bIncludeFromChildActors",
            &include_children, 1);
        if (!call_by_path(actor, kPaths[path_index], buf, capacity, &ret_off)) continue;

        for (i = 0; i < count; ++i) {
            if (params[i].offset < 0 || (size_t)params[i].offset + 24u > capacity) continue;
            if (strcmp(params[i].name, "Origin") == 0) {
                memcpy(&oz, buf + params[i].offset + 16u, 8);
            } else if (strcmp(params[i].name, "BoxExtent") == 0) {
                memcpy(&ez, buf + params[i].offset + 16u, 8);
                have_extent = 1;
            }
        }
        if (have_extent == 0) continue;
        *out_bottom = oz - ez;
        {
            char line[220];
            snprintf(line, sizeof(line), "SPAWN 包围盒:用 %s 底部=%.1f (OriginZ=%.1f ExtentZ=%.1f)",
                kPaths[path_index], oz - ez, oz, ez);
            if (g_core != NULL && g_core->log != NULL) {
                g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
            }
        }
        return 1;
    }
    return 0;
}

// 把 actor 沿 Z 平移(用 K2_SetActorLocation)
static int move_actor_z(uintptr_t actor, double dz) {
    AnomalyGenerationHandleV1 handle = {0u, 0u};
    uintptr_t func = 0u;
    uint16_t parms_size = 0u;
    uint16_t ret_off = 0u;
    uint16_t r_loc = 0u;
    ParamInfo params[MAX_PARAMS];
    int count;
    uint8_t loc[128];
    uint8_t buf[600];
    uint8_t sweep = 0u;      // 不扫掠:否则与玩家重叠时移动会被碰撞挡掉
    uint8_t teleport = 1u;
    double new_location[3];
    double x = 0.0, y = 0.0, z = 0.0;

    if (g_objects == NULL || g_process_event_fn == 0u || actor == 0u) return 0;
    memset(loc, 0, sizeof(loc));
    if (!call_by_path(actor, "/Script/Engine.Actor:K2_GetActorLocation",
            loc, sizeof(loc), &r_loc)) {
        return 0;
    }
    memcpy(&x, loc + r_loc, 8);
    memcpy(&y, loc + r_loc + 8u, 8);
    memcpy(&z, loc + r_loc + 16u, 8);
    z += dz;

    if (!ok(g_objects->find_exact(g_objects->user,
            sv("/Script/Engine.Actor:K2_SetActorLocation"), &handle))) {
        return 0;
    }
    func = object_by_handle(handle);
    if (func == 0u) return 0;
    (void)read_value(func + (uintptr_t)L_function_parmsSize, &parms_size);
    if (parms_size == 0u || parms_size > (uint16_t)sizeof(buf)) return 0;
    count = read_function_params(func, params);
    if (count <= 0) return 0;

    memset(buf, 0, sizeof(buf));
    new_location[0] = x;
    new_location[1] = y;
    new_location[2] = z;
    (void)set_param_by_name(params, count, buf, sizeof(buf), "NewLocation", new_location, 24);
    (void)set_param_by_name(params, count, buf, sizeof(buf), "bSweep", &sweep, 1);
    (void)set_param_by_name(params, count, buf, sizeof(buf), "bTeleport", &teleport, 1);
    return call_by_path(actor, "/Script/Engine.Actor:K2_SetActorLocation",
        buf, sizeof(buf), &ret_off);
}

static int actor_looks_alive(uintptr_t actor);

// 贴地修正:把 actor 的「位置」对齐到参考对象(玩家)的「胶囊底部」。
// 依据(实测得出):这些 NPC 的 mesh 原点在 actor 位置(脚底),而玩家 ActorLocation 是胶囊中心
// (离地约 82cm),所以正确落点 = 玩家胶囊底部,而不是"两边包围盒底部对齐"。
static int apply_ground_fix(uintptr_t actor, uintptr_t reference, const char* tag) {
    uint8_t ref_bounds[200];
    uint8_t npc_bounds[200];
    uint8_t npc_loc[128];
    double ref_bottom = 0.0;
    double npc_bottom = 0.0;
    double npc_z = 0.0;
    double delta = 0.0;
    uint16_t r_npc_loc = 0u;
    int have_npc_bounds = 0;
    char line[260];

#define GROUND_LOG(level_, text_) \
    do { if (g_core != NULL && g_core->log != NULL) { \
        g_core->log(g_core->user, (level_), sv(text_)); } } while (0)

    if (actor == 0u || reference == 0u) return 0;
    if (actor_looks_alive(actor) == 0) {
        snprintf(line, sizeof(line), "SPAWN %s 贴地:目标 actor 已销毁,跳过", tag);
        GROUND_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, line);
        return 0;
    }
    memset(ref_bounds, 0, sizeof(ref_bounds));
    if (read_actor_bottom(reference, ref_bounds, sizeof(ref_bounds), &ref_bottom) == 0) {
        snprintf(line, sizeof(line), "SPAWN %s 贴地检查不可用(玩家包围盒取不到)", tag);
        GROUND_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, line);
        return 0;
    }
    memset(npc_loc, 0, sizeof(npc_loc));
    if (!call_by_path(actor, "/Script/Engine.Actor:K2_GetActorLocation",
            npc_loc, sizeof(npc_loc), &r_npc_loc)) {
        snprintf(line, sizeof(line), "SPAWN %s 贴地检查不可用(NPC 位置读不到)", tag);
        GROUND_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, line);
        return 0;
    }
    memcpy(&npc_z, npc_loc + r_npc_loc + 16u, 8);
    // 这些 NPC 的根组件是 CapsuleComponent:actor 位置 = 胶囊中心,不是脚底。
    // 必须拿 NPC 自己的包围盒底部去对玩家的胶囊底部;直接对 actor 位置会整只埋进去
    // (2026-10-18 实测:生成后 NPC 在地下,差值恰好是半个胶囊高度)。
    memset(npc_bounds, 0, sizeof(npc_bounds));
    if (read_actor_bottom(actor, npc_bounds, sizeof(npc_bounds), &npc_bottom)) {
        have_npc_bounds = 1;
        delta = ref_bottom - npc_bottom;
        snprintf(line, sizeof(line),
            "SPAWN %s 贴地:玩家底=%.1f NPC底=%.1f NPC位置Z=%.1f 差值=%.1f",
            tag, ref_bottom, npc_bottom, npc_z, delta);
    } else {
        delta = ref_bottom - npc_z;
        snprintf(line, sizeof(line),
            "SPAWN %s 贴地:NPC 包围盒取不到,退回对齐 actor 位置(玩家底=%.1f NPC位置Z=%.1f 差值=%.1f)",
            tag, ref_bottom, npc_z, delta);
    }
    GROUND_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, line);
    // 安全阀:异常差值直接放弃,避免把对象抛飞(曾因畸形包围盒把 NPC 抬高 160cm)
    if (delta < -300.0 || delta > 300.0) {
        snprintf(line, sizeof(line), "SPAWN %s 贴地:差值 %.1f 超出合理范围,放弃修正", tag, delta);
        GROUND_LOG(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, line);
        return 0;
    }
    if (delta > -5.0 && delta < 5.0) return 1;   // 已经到位,不再动

    if (move_actor_z(actor, delta) == 0) {
        snprintf(line, sizeof(line), "SPAWN %s 移动失败(K2_SetActorLocation 不可用)", tag);
        GROUND_LOG(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, line);
        return 0;
    }
    {
        uint8_t verify_bounds[200];
        double verify_bottom = 0.0;
        memset(verify_bounds, 0, sizeof(verify_bounds));
        if (have_npc_bounds &&
            read_actor_bottom(actor, verify_bounds, sizeof(verify_bounds), &verify_bottom)) {
            snprintf(line, sizeof(line), "SPAWN %s 修正后 NPC底=%.1f(目标 %.1f)",
                tag, verify_bottom, ref_bottom);
        } else {
            uint8_t verify_loc[128];
            uint16_t r_verify = 0u;
            double verify_z = 0.0;
            memset(verify_loc, 0, sizeof(verify_loc));
            if (call_by_path(actor, "/Script/Engine.Actor:K2_GetActorLocation",
                    verify_loc, sizeof(verify_loc), &r_verify)) {
                memcpy(&verify_z, verify_loc + r_verify + 16u, 8);
                snprintf(line, sizeof(line), "SPAWN %s 修正后 NPC位置Z=%.1f(目标 %.1f)",
                    tag, verify_z, ref_bottom);
            } else {
                snprintf(line, sizeof(line), "SPAWN %s 已移动 %.1f,修正后位置读不回", tag, delta);
            }
        }
        GROUND_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, line);
    }
    return 1;

#undef GROUND_LOG
}

// 延迟贴地修正:mesh 是异步加载的,spawn 那一刻的包围盒可能还不是最终大小
static uintptr_t g_late_fix_actor = 0u;
static uintptr_t g_late_fix_reference = 0u;
static double g_late_fix_delay = 0.0;

// 本次生成出来的 actor 有没有可见模型(根组件为空 / SilentCheck / Brush 的都是空壳)
static char g_spawn_root_class[96] = {0};
static int g_spawn_has_model = 1;

// 防御:actor 可能已经被销毁(例如刚点了「清除」,或游戏自己回收了它)。
// 此时再调任何引擎函数都是 use-after-free —— 2026-10-18 实测崩溃就是这条路径:
// 生成 → 清除(销毁)→ 2 秒后延迟贴地仍对已释放的 actor 调 K2_GetActorLocation。
static int actor_looks_alive(uintptr_t actor) {
    uintptr_t uclass = 0u;
    char name[128];
    if (actor == 0u) return 0;
    if (!read_value(actor + (uintptr_t)L_object_class, &uclass) || uclass == 0u) return 0;
    if (g_names == NULL) return 1;   // 无法校验时不拦,避免误伤正常路径
    if (!object_name(uclass, name, sizeof(name))) return 0;
    return name[0] != '\0' ? 1 : 0;
}

// 用引擎接口把某个 UClass 生成到 pawn 面前(0.12.1 已真机验证可行)
static int spawn_uclass(uintptr_t npc_class) {
    AnomalyGenerationHandleV1 handle = {0u, 0u};
    uintptr_t lib_cdo = 0u;
    uintptr_t pawn = 0u;
    uintptr_t func_begin = 0u;
    uintptr_t func_finish = 0u;
    uint8_t xform[192];
    uint8_t parms[320];
    uint16_t r_xf = 0u;
    uint16_t r_begin = 0u;
    uint16_t r_finish = 0u;
    uintptr_t spawned = 0u;
    ParamInfo params[MAX_PARAMS];
    int count = 0;
    char line[300];

    if (g_process_event_fn == 0u || g_objects == NULL || npc_class == 0u) {
        stage_log("SPAWN 中止:前置不足(pe=0x%llX objects=%p uclass=0x%llX)",
            (unsigned long long)g_process_event_fn, (void*)g_objects, (unsigned long long)npc_class);
        return 0;
    }
    stage_log("SPAWN 1/9 前置 OK pe=0x%llX uclass=0x%llX", (unsigned long long)g_process_event_fn,
        (unsigned long long)npc_class);
    pawn = get_pawn();
    if (pawn == 0u) {
        stage_log("SPAWN 中止:pawn=0(controller=0x%llX)", (unsigned long long)g_controller);
        return 0;
    }
    stage_log("SPAWN 2/9 pawn=0x%llX", (unsigned long long)pawn);
    lib_cdo = library_cdo("/Script/Engine.GameplayStatics");
    if (lib_cdo == 0u) {
        stage_log("SPAWN 中止:拿不到 GameplayStatics 的 CDO");
        return 0;
    }
    stage_log("SPAWN 3/9 lib_cdo=0x%llX", (unsigned long long)lib_cdo);
    memset(xform, 0, sizeof(xform));
    stage_log("SPAWN 4/9 调 GetTransform(xform 容量 %u)", (unsigned)sizeof(xform));
    if (!call_by_path(pawn, "/Script/Engine.Actor:GetTransform", xform, sizeof(xform), &r_xf)) {
        stage_log("SPAWN 中止:GetTransform 调用失败");
        return 0;
    }
    stage_log("SPAWN 5/9 GetTransform OK 返回值偏移=%u", (unsigned)r_xf);
    // 落点从"脚下"挪到身前 250cm:先在 transform 里按坐标定位 Translation,再沿前向平移
    {
        uint8_t loc[128];
        uint8_t fwd[128];
        uint16_t r_loc = 0u;
        uint16_t r_fwd = 0u;
        double px = 0.0, py = 0.0, pz = 0.0;
        double fx = 0.0, fy = 0.0, fz = 0.0;
        int translation_offset = -1;
        int off;
        memset(loc, 0, sizeof(loc));
        memset(fwd, 0, sizeof(fwd));

        if (call_by_path(pawn, "/Script/Engine.Actor:K2_GetActorLocation",
                loc, sizeof(loc), &r_loc)) {
            memcpy(&px, loc + r_loc, 8);
            memcpy(&py, loc + r_loc + 8u, 8);
            memcpy(&pz, loc + r_loc + 16u, 8);
            for (off = 0; off + 24 <= 96; off += 8) {
                double vx = 0.0, vy = 0.0, vz = 0.0;
                memcpy(&vx, xform + r_xf + off, 8);
                memcpy(&vy, xform + r_xf + off + 8u, 8);
                memcpy(&vz, xform + r_xf + off + 16u, 8);
                if (vx > px - 1.0 && vx < px + 1.0 && vy > py - 1.0 && vy < py + 1.0 &&
                    vz > pz - 1.0 && vz < pz + 1.0) {
                    translation_offset = off;
                    break;
                }
            }
        }
        if (call_by_path(pawn, "/Script/Engine.Actor:GetActorForwardVector",
                fwd, sizeof(fwd), &r_fwd)) {
            memcpy(&fx, fwd + r_fwd, 8);
            memcpy(&fy, fwd + r_fwd + 8u, 8);
            memcpy(&fz, fwd + r_fwd + 16u, 8);
        }
        if (translation_offset >= 0) {
            double nx = px + fx * 250.0;
            double ny = py + fy * 250.0;
            double nz = pz + fz * 250.0;
            uint8_t player_bounds[200];
            double ground_z = 0.0;
            // Z 单独处理:这些 NPC 的 mesh 原点在脚底,落在"玩家胶囊底部"才不会浮空
            memset(player_bounds, 0, sizeof(player_bounds));
            if (read_actor_bottom(pawn, player_bounds, sizeof(player_bounds), &ground_z)) {
                nz = ground_z;
            }
            memcpy(xform + r_xf + translation_offset, &nx, 8);
            memcpy(xform + r_xf + translation_offset + 8u, &ny, 8);
            memcpy(xform + r_xf + translation_offset + 16u, &nz, 8);
            snprintf(line, sizeof(line),
                "SPAWN 落点 Translation+%d -> (%.0f, %.0f, %.0f) 玩家底Z=%.1f",
                translation_offset, nx, ny, nz, ground_z);
            if (g_core != NULL && g_core->log != NULL) {
                g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
            }
        } else if (g_core != NULL && g_core->log != NULL) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                sv("SPAWN 没能在 transform 里定位 Translation,落点仍是脚下"));
        }
    }
    stage_log("SPAWN 6/9 找 BeginDeferredActorSpawnFromClass");
    if (!ok(g_objects->find_exact(g_objects->user,
            sv("/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass"), &handle))) {
        stage_log("SPAWN 中止:找不到 BeginDeferredActorSpawnFromClass");
        return 0;
    }
    func_begin = object_by_handle(handle);
    count = read_function_params(func_begin, params);
    if (count <= 0) {
        stage_log("SPAWN 中止:BeginDeferred 参数表读不出(count=%d fn=0x%llX)", count,
            (unsigned long long)func_begin);
        return 0;
    }
    {
        int pi;
        line[0] = '\0';
        for (pi = 0; pi < count; ++pi) {
            char part[80];
            snprintf(part, sizeof(part), "%s@%d+%d ", params[pi].name, params[pi].offset,
                params[pi].size);
            strncat(line, part, sizeof(line) - strlen(line) - 1u);
        }
        stage_log("SPAWN 6/9 BeginDeferred 参数表(%d):%s", count, line);
    }
    memset(parms, 0, sizeof(parms));
    {
        uintptr_t actor_class = npc_class;
        uint8_t collision = 0;
        (void)set_param_by_name(params, count, parms, sizeof(parms), "WorldContextObject", &pawn, 8);
        if (!set_param_by_name(params, count, parms, sizeof(parms), "ActorClass", &actor_class, 8)) {
            stage_log("SPAWN 中止:参数表里没有 ActorClass");
            return 0;
        }
        (void)set_param_by_name(params, count, parms, sizeof(parms), "SpawnTransform", xform + r_xf, 96);
        (void)set_param_by_name(params, count, parms, sizeof(parms), "CollisionHandlingOverride",
            &collision, 1);
        (void)set_param_by_name(params, count, parms, sizeof(parms), "Owner", &pawn, 8);
        (void)set_param_by_name(params, count, parms, sizeof(parms), "Instigator", &pawn, 8);
    }
    stage_log("SPAWN 7/9 调 BeginDeferred(parms 容量 %u)", (unsigned)sizeof(parms));
    if (!call_by_path(lib_cdo, "/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass",
            parms, sizeof(parms), &r_begin)) {
        stage_log("SPAWN 中止:BeginDeferred 调用入口不可用");
        return 0;
    }
    memcpy(&spawned, parms + r_begin, 8);
    if (spawned == 0u) {
        stage_log("SPAWN 中止:BeginDeferred 返回 actor=0(引擎拒绝)");
        return 0;
    }
    stage_log("SPAWN 8/9 BeginDeferred OK actor=0x%llX", (unsigned long long)spawned);
    if (ok(g_objects->find_exact(g_objects->user,
            sv("/Script/Engine.GameplayStatics:FinishSpawningActor"), &handle))) {
        func_finish = object_by_handle(handle);
    }
    if (func_finish == 0u) {
        snprintf(line, sizeof(line),
            "SPAWN 警告:找不到 FinishSpawningActor —— actor 会停在 deferred 状态(不可见)");
        if (g_core != NULL && g_core->log != NULL) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR, sv(line));
        }
    } else {
        ParamInfo fparams[MAX_PARAMS];
        int fcount = read_function_params(func_finish, fparams);
        uintptr_t finished = 0u;
        char names[220];
        int k;
        memset(parms, 0, sizeof(parms));
        (void)set_param_by_name(fparams, fcount, parms, sizeof(parms), "Actor", &spawned, 8);
        (void)set_param_by_name(fparams, fcount, parms, sizeof(parms), "UserSpawnTransform",
            xform + r_xf, 96);
        (void)set_param_by_name(fparams, fcount, parms, sizeof(parms), "SpawnTransform",
            xform + r_xf, 96);
        names[0] = '\0';
        for (k = 0; k < fcount; ++k) {
            strncat(names, fparams[k].name, sizeof(names) - strlen(names) - 1u);
            strncat(names, " ", sizeof(names) - strlen(names) - 1u);
        }
        if (!call_by_path(lib_cdo, "/Script/Engine.GameplayStatics:FinishSpawningActor",
                parms, sizeof(parms), &r_finish)) {
            snprintf(line, sizeof(line), "SPAWN 警告:FinishSpawningActor 调用入口不可用");
            if (g_core != NULL && g_core->log != NULL) {
                g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR, sv(line));
            }
        } else {
            memcpy(&finished, parms + r_finish, 8);
            snprintf(line, sizeof(line),
                "SPAWN FinishSpawning 完成 返回=0x%llX 参数(%d):%s",
                (unsigned long long)finished, fcount, names);
            if (g_core != NULL && g_core->log != NULL) {
                g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
            }
            if (finished != 0u) spawned = finished;
        }
    }
    // 校验:新 actor 是否真的进入了世界(能读出世界坐标就说明它在场景里)
    {
        uint8_t vloc[128];
        uint16_t r_vloc = 0u;
        memset(vloc, 0, sizeof(vloc));
        if (call_by_path(spawned, "/Script/Engine.Actor:K2_GetActorLocation",
                vloc, sizeof(vloc), &r_vloc)) {
            double ax = 0.0, ay = 0.0, az = 0.0;
            memcpy(&ax, vloc + r_vloc, 8);
            memcpy(&ay, vloc + r_vloc + 8u, 8);
            memcpy(&az, vloc + r_vloc + 16u, 8);
            snprintf(line, sizeof(line), "SPAWN 校验:新 actor 世界坐标=(%.0f, %.0f, %.0f)",
                ax, ay, az);
        } else {
            snprintf(line, sizeof(line), "SPAWN 校验:读不到新 actor 坐标(对象可能已失效)");
        }
        if (g_core != NULL && g_core->log != NULL) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
        }
    }
    // 组件检查:rootComponent 为空、或类名不是 Mesh 组件 → 该 actor 在客户端就是个"空壳"
    {
        uintptr_t root = 0u;
        char root_cls[120];
        root_cls[0] = '\0';
        g_spawn_root_class[0] = '\0';
        g_spawn_has_model = 1;
        if (read_value(spawned + (uintptr_t)L_actor_rootComponent, &root) && root != 0u) {
            if (!object_class_name(root, root_cls, sizeof(root_cls))) {
                snprintf(root_cls, sizeof(root_cls), "<?>");
            }
            snprintf(g_spawn_root_class, sizeof(g_spawn_root_class), "%s", root_cls);
            // 已知没有模型的根组件:逻辑/触发物(SilentCheckComponent)与画刷体积(BrushComponent)
            if (strcmp(root_cls, "SilentCheckComponent") == 0 ||
                strcmp(root_cls, "BrushComponent") == 0) {
                g_spawn_has_model = 0;
            }
            snprintf(line, sizeof(line), "SPAWN 组件检查 root=0x%llX 类=%s%s",
                (unsigned long long)root, root_cls,
                (g_spawn_has_model != 0) ? "" : "(没有模型)");
        } else {
            g_spawn_has_model = 0;
            snprintf(g_spawn_root_class, sizeof(g_spawn_root_class), "空");
            snprintf(line, sizeof(line),
                "SPAWN 组件检查:rootComponent 为空 —— 该 actor 没有实体组件(客户端不可见)");
        }
        if (g_core != NULL && g_core->log != NULL) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
        }
    }
    // 贴地修正:立即来一次,并在 2 秒后(mesh 异步加载完成)再补一次。
    // 没有模型的空壳不做贴地:它们的包围盒恒为 0,修正没有意义,还会多碰一次引擎
    if (g_spawn_has_model != 0) {
        stage_log("SPAWN 9/9 开始贴地修正(actor=0x%llX)", (unsigned long long)spawned);
        (void)apply_ground_fix(spawned, pawn, "立即");
        g_late_fix_actor = spawned;
        g_late_fix_reference = pawn;
        g_late_fix_delay = 2.0;
    } else {
        g_late_fix_actor = 0u;
        g_late_fix_reference = 0u;
        stage_log("SPAWN 9/9 跳过贴地:根组件 %s 没有模型", g_spawn_root_class);
    }
    if (g_spawned_count < (int)(sizeof(g_spawned) / sizeof(g_spawned[0]))) {
        g_spawned[g_spawned_count++] = spawned;
    }
    snprintf(line, sizeof(line), "SPAWN OK actor=0x%llX 已记账(%d 个)",
        (unsigned long long)spawned, g_spawned_count);
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
    }
    return 1;
}

static int run_spawn_class(int index) {
    uintptr_t actor = 0u;
    uintptr_t uclass = 0u;
    if (index < 0 || index >= g_class_entry_count) {
        snprintf(g_call_result, sizeof(g_call_result), "未选中类");
        stage_log("SPAWN 请求无效:%s (index=%d 条目=%d)", g_call_result, index, g_class_entry_count);
        return 0;
    }
    stage_log("SPAWN 请求 index=%d 类=%s 句柄=%llu", index, g_class_entries[index].name,
        (unsigned long long)g_class_entries[index].sample.id);
    actor = object_by_handle(g_class_entries[index].sample);
    if (actor == 0u) {
        snprintf(g_call_result, sizeof(g_call_result), "选中类的实例句柄已失效,请重新探测");
        stage_log("SPAWN 中止:%s (句柄=%llu)", g_call_result,
            (unsigned long long)g_class_entries[index].sample.id);
        return 0;
    }
    if (!read_value(actor + (uintptr_t)L_object_class, &uclass) || uclass == 0u) {
        snprintf(g_call_result, sizeof(g_call_result), "读不到选中类的 UClass");
        stage_log("SPAWN 中止:%s (actor=0x%llX)", g_call_result, (unsigned long long)actor);
        return 0;
    }
    stage_log("SPAWN 目标 actor=0x%llX uclass=0x%llX", (unsigned long long)actor,
        (unsigned long long)uclass);
    if (spawn_uclass(uclass) != 0) {
        NpcRecord* rec = record_ensure(g_class_entries[index].name);   // 记入生成记录
        if (rec != NULL && g_spawn_has_model == 0) {
            // 根组件没有模型 → 生成成功但客户端看不见。自动标「失败」进黑名单,
            // 这样它不会再出现在列表里(用户可以到生成记录窗口改回来)
            rec->verdict = 2;
            snprintf(rec->note, sizeof(rec->note),
                "根组件 %s:没有模型,生成后看不见(自动加入黑名单)", g_spawn_root_class);
            snprintf(g_call_result, sizeof(g_call_result),
                "已生成 %s,但根组件 %s 没有模型 —— 看不见,已自动标记失败",
                g_class_entries[index].name, g_spawn_root_class);
        } else {
            snprintf(g_call_result, sizeof(g_call_result), "已生成 %s 到面前(累计 %d 个)",
                g_class_entries[index].name, g_spawned_count);
        }
    } else {
        snprintf(g_call_result, sizeof(g_call_result), "生成 %s 失败(引擎拒绝或类不可 spawn)",
            g_class_entries[index].name);
    }
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(g_call_result));
    }
    return 1;
}

// 清除自己生成的(K2_DestroyActor 是 Actor 上的无参方法)
static int run_clear(void) {
    uint8_t parms[128];
    uint16_t ret = 0u;
    int i;
    int destroyed = 0;
    int already_gone = 0;
    // 先取消待执行的延迟贴地:否则 2 秒后会对刚销毁的 actor 调引擎函数(use-after-free,
    // 2026-10-18 的崩溃就是这样发生的:生成 → 清除 → 延迟贴地打到已释放对象)
    g_late_fix_actor = 0u;
    g_late_fix_reference = 0u;
    for (i = 0; i < g_spawned_count; ++i) {
        if (g_spawned[i] == 0u) continue;
        if (actor_looks_alive(g_spawned[i]) == 0) {
            already_gone++;
            g_spawned[i] = 0u;
            continue;
        }
        memset(parms, 0, sizeof(parms));
        if (call_by_path(g_spawned[i], "/Script/Engine.Actor:K2_DestroyActor",
                parms, sizeof(parms), &ret)) {
            destroyed++;
        }
        g_spawned[i] = 0u;
    }
    g_spawned_count = 0;
    snprintf(g_call_result, sizeof(g_call_result),
        "已发送 %d 个销毁请求(K2_DestroyActor;销毁在下一帧生效)%s",
        destroyed, (already_gone > 0) ? " · 另有对象已自行销毁,已跳过" : "");
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(g_call_result));
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 0.11.0:偷听真实调用 —— 订阅 process-event,捕获 DPS 工具召唤时的真实参数
// ---------------------------------------------------------------------------

static const AnomalyUe5ProcessEventServiceV1* g_process_event_svc = NULL;
static AnomalyGenerationHandleV1 g_pe_sub = {0u, 0u};
static uintptr_t g_func_summon_ai = 0u;
static uintptr_t g_func_server_summon = 0u;
static uintptr_t g_func_configure_summon = 0u;
static int g_pe_capture_count = 0;
static char g_pe_lines[10][200];
static int g_pe_line_count = 0;

static void ANOMALY_CALL on_process_event(
    void* user, uintptr_t object, uintptr_t function, void* parameters) {
    uint8_t buf[64];
    char hex[140];
    char line[210];
    int i;
    (void)user;

    if (function == 0u) return;
    if (function != g_func_summon_ai && function != g_func_server_summon &&
        function != g_func_configure_summon) {
        return;
    }
    if (g_pe_capture_count >= 50) return;
    g_pe_capture_count++;

    memset(buf, 0, sizeof(buf));
    if (parameters != NULL) {
        (void)read_bytes((uintptr_t)parameters, buf, sizeof(buf));
    }
    hex[0] = '\0';
    for (i = 0; i < 48; ++i) {
        char part[8];
        snprintf(part, sizeof(part), "%02X", buf[i]);
        strncat(hex, part, sizeof(hex) - strlen(hex) - 1u);
    }
    snprintf(line, sizeof(line), "CAPTURE #%d func=0x%llX obj=0x%llX 参数48B=%s",
        g_pe_capture_count, (unsigned long long)function, (unsigned long long)object, hex);
    if (g_pe_line_count < 10) {
        snprintf(g_pe_lines[g_pe_line_count], sizeof(g_pe_lines[0]), "%s", line);
        g_pe_line_count++;
    }
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, sv(line));
    }
}

// 记录目标函数的当前地址(call_by_path 用过的路径已能命中)
static void remember_summon_addresses(void) {
    static const struct {
        const char* path;
        uintptr_t* slot;
    } kTargets[3] = {
        {"/Script/HTGame.HTAbilityCharacter:SummonAI", &g_func_summon_ai},
        {"/Script/HTGame.HTPlayerController:ServerSummonAI_HybridMode", &g_func_server_summon},
        {"/Script/HTGame.HTPlayerController:ServerConfigureSummonedAI_HybridMode", &g_func_configure_summon},
    };
    int i;
    if (g_objects == NULL || g_objects->find_exact == NULL) return;
    for (i = 0; i < 3; ++i) {
        AnomalyGenerationHandleV1 handle = {0u, 0u};
        if (ok(g_objects->find_exact(g_objects->user, sv(kTargets[i].path), &handle))) {
            *kTargets[i].slot = object_by_handle(handle);
        }
    }
    if (g_core != NULL && g_core->log != NULL) {
        char line[220];
        snprintf(line, sizeof(line), "CAPTURE 监听目标: SummonAI=0x%llX ServerSummon=0x%llX Configure=0x%llX",
            (unsigned long long)g_func_summon_ai, (unsigned long long)g_func_server_summon,
            (unsigned long long)g_func_configure_summon);
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(line));
    }
}

static void subscribe_capture(void) {
    if (g_pe_sub.id != 0u || g_process_event_svc == NULL) return;
    if (g_process_event_svc->subscribe == NULL) return;
    if (ok(g_process_event_svc->subscribe(g_process_event_svc->user, on_process_event, NULL,
            &g_pe_sub))) {
        if (g_core != NULL && g_core->log != NULL) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                sv("CAPTURE 已订阅 process-event,等待召唤调用出现"));
        }
    } else {
        g_pe_sub = AnomalyGenerationHandleV1{0u, 0u};
    }
}

static void unsubscribe_capture(void) {
    if (g_pe_sub.id != 0u && g_process_event_svc != NULL &&
        g_process_event_svc->unsubscribe != NULL) {
        (void)g_process_event_svc->unsubscribe(g_process_event_svc->user, g_pe_sub);
    }
    g_pe_sub = AnomalyGenerationHandleV1{0u, 0u};
}

static uintptr_t get_pawn(void) {
    uintptr_t pawn = 0u;
    if (g_controller == 0u) return 0u;
    if (!read_value(g_controller + (uintptr_t)L_controller_pawn, &pawn)) return 0u;
    return pawn;
}

// 找函数 -> 调用(parms 由调用者准备好;ret_off 回传返回值偏移)
static int call_by_path(
    uintptr_t object, const char* path, uint8_t* parms, size_t capacity, uint16_t* ret_off) {
    AnomalyGenerationHandleV1 handle = {0u, 0u};
    uintptr_t func = 0u;
    uint16_t parms_size = 0u;
    char fn_name[120];
    if (g_objects == NULL || g_process_event_fn == 0u || object == 0u) {
        stage_log("CALL 中止 %s : 前置不足(objects=%p pe=0x%llX obj=0x%llX)", path,
            (void*)g_objects, (unsigned long long)g_process_event_fn, (unsigned long long)object);
        return 0;
    }
    if (!ok(g_objects->find_exact(g_objects->user, sv(path), &handle))) {
        stage_log("CALL 中止 %s : find_exact 没找到这个 UFunction", path);
        return 0;
    }
    func = object_by_handle(handle);
    if (func == 0u) {
        stage_log("CALL 中止 %s : handle 换算不出函数地址", path);
        return 0;
    }
    // 注意:无参数且无返回值的函数(如 K2_DestroyActor) parmsSize 就是 0,那是合法的。
    // 早期版本把 0 当失败,导致"清除失效"。
    (void)read_value(func + (uintptr_t)L_function_parmsSize, &parms_size);
    if (parms_size > (uint16_t)capacity) {
        stage_log("CALL 中止 %s : parmsSize=%u > capacity=%u", path, (unsigned)parms_size,
            (unsigned)capacity);
        return 0;
    }
    (void)read_value(func + (uintptr_t)L_function_returnValueOffset, ret_off);
    fn_name[0] = '\0';
    if (!object_name(func, fn_name, sizeof(fn_name))) {
        snprintf(fn_name, sizeof(fn_name), "<?>");
    }
    stage_log("CALL 进入 %s obj=0x%llX fn=0x%llX fn名=%s parmsSize=%u capacity=%u pe_fn=0x%llX", path,
        (unsigned long long)object, (unsigned long long)func, fn_name, (unsigned)parms_size,
        (unsigned)capacity, (unsigned long long)g_process_event_fn);
    ((ProcessEventFn)g_process_event_fn)((void*)object, (void*)func, parms);
    stage_log("CALL 返回 OK %s", path);
    return 1;
}

static uintptr_t library_cdo(const char* class_path) {
    AnomalyGenerationHandleV1 handle = {0u, 0u};
    uintptr_t cls = 0u;
    uintptr_t cdo = 0u;
    if (g_objects == NULL || !ok(g_objects->find_exact(g_objects->user, sv(class_path), &handle))) {
        return 0u;
    }
    cls = object_by_handle(handle);
    if (cls != 0u) (void)read_value(cls + (uintptr_t)L_uclass_classDefaultObject, &cdo);
    {
        char nm[120];
        nm[0] = '\0';
        if (!object_name(cdo, nm, sizeof(nm))) {
            snprintf(nm, sizeof(nm), "<?>");
        }
        stage_log("LIBRARY %s -> cls=0x%llX cdo=0x%llX cdo名=%s", class_path,
            (unsigned long long)cls, (unsigned long long)cdo, nm);
    }
    return cdo;
}

// 把「路径字符串 -> 两步转换 -> 40 字节软类引用」写进 out(至少 40 字节)
static int build_soft_class_ref(uintptr_t lib_cdo, const wchar_t* path, uint8_t* out) {
    static wchar_t path_buffer[600];
    uint8_t sc1[192];
    uint8_t sc2[192];
    uint16_t r1 = 0u;
    uint16_t r2 = 0u;
    uintptr_t string_address = 0u;
    int32_t string_num = 0;
    int n = 0;
    size_t copy_len;

    while (path[n] != L'\0' && n < 599) {
        path_buffer[n] = path[n];
        ++n;
    }
    path_buffer[n] = L'\0';
    string_address = (uintptr_t)path_buffer;
    string_num = (int32_t)n + 1;

    memset(sc1, 0, sizeof(sc1));
    memcpy(sc1 + 0, &string_address, 8);
    memcpy(sc1 + 8, &string_num, 4);
    memcpy(sc1 + 12, &string_num, 4);
    if (!call_by_path(lib_cdo, "/Script/Engine.KismetSystemLibrary:MakeSoftClassPath",
            sc1, sizeof(sc1), &r1)) {
        return 0;
    }

    memset(sc2, 0, sizeof(sc2));
    copy_len = 32u;
    if (copy_len > sizeof(sc2)) copy_len = sizeof(sc2);
    memcpy(sc2 + 0, sc1 + r1, copy_len);
    if (!call_by_path(lib_cdo, "/Script/Engine.KismetSystemLibrary:Conv_SoftClassPathToSoftClassRef",
            sc2, sizeof(sc2), &r2)) {
        return 0;
    }
    memcpy(out, sc2 + r2, 40u);
    return 1;
}

// do_call=0 只组装并打印(安全);do_call=1 真正调用 SummonAI
static int run_summon(int do_call) {
    static const wchar_t* kTarget =
        L"/Game/Blueprints/Character/Monster/NPC_Sub_fs_Simple_think.NPC_Sub_fs_Simple_think_C";
    // transform 取值的候选路径,逐个尝试:很多 native getter 并没有反射成 UFUNCTION
    static const char* kTransformPaths[] = {
        "/Script/Engine.Actor:GetActorTransform",
        "/Script/Engine.Actor:K2_GetActorTransform",
        "/Script/Engine.Actor:GetTransform",
    };
    uintptr_t lib_cdo = 0u;
    uintptr_t pawn = 0u;
    uint8_t ai_class[40];
    uint8_t loc[192];
    uint8_t xform[192];
    uint8_t parms[256];
    uint16_t r_loc = 0u;
    uint16_t r_xf = 0u;
    uint16_t r_sum = 0u;
    uintptr_t generated = 0u;
    int32_t level = 1;
    int transform_ok = 0;
    int i;
    char line[420];
    char pawn_class[120];

#define SUMMON_LOG(level_, text_) \
    do { if (g_core != NULL && g_core->log != NULL) { \
        g_core->log(g_core->user, (level_), sv(text_)); } } while (0)
#define SUMMON_FAIL(text_) \
    do { snprintf(g_call_result, sizeof(g_call_result), "%s", (text_)); \
        if (g_core != NULL && g_core->log != NULL) { \
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR, sv(g_call_result)); } \
        return 0; } while (0)

    if (g_process_event_fn == 0u) SUMMON_FAIL("④ 跳过:ProcessEvent 未确认");
    if (g_objects == NULL) SUMMON_FAIL("④ 跳过:ue5.objects 不可用");

    pawn = get_pawn();
    if (pawn == 0u) SUMMON_FAIL("④ 跳过:pawn 取不到(controller+776 是 0)");
    pawn_class[0] = '\0';
    if (!object_class_name(pawn, pawn_class, sizeof(pawn_class))) {
        snprintf(pawn_class, sizeof(pawn_class), "<?>");
    }
    snprintf(line, sizeof(line), "SUMMON pawn=0x%llX 类=%s",
        (unsigned long long)pawn, pawn_class);
    SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, line);

    lib_cdo = library_cdo("/Script/Engine.KismetSystemLibrary");
    if (lib_cdo == 0u) SUMMON_FAIL("④ 跳过:函数库 CDO 取不到");

    // 1) AIClass
    if (!build_soft_class_ref(lib_cdo, kTarget, ai_class)) {
        SUMMON_FAIL("④ 失败:软类引用构造失败(两步链)");
    }
    SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "SUMMON 第1步 OK:AIClass 40 字节已构造");

    // 2) 世界坐标
    memset(loc, 0, sizeof(loc));
    if (!call_by_path(pawn, "/Script/Engine.Actor:K2_GetActorLocation", loc, sizeof(loc), &r_loc)) {
        SUMMON_FAIL("④ 失败:读坐标失败(K2_GetActorLocation 调用不成功)");
    }
    SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "SUMMON 第2步 OK:坐标已读到");

    // 3) transform:逐个候选路径试
    for (i = 0; i < (int)(sizeof(kTransformPaths) / sizeof(kTransformPaths[0])); ++i) {
        memset(xform, 0, sizeof(xform));
        if (call_by_path(pawn, kTransformPaths[i], xform, sizeof(xform), &r_xf)) {
            snprintf(line, sizeof(line), "SUMMON 第3步 OK:transform 用 %s (retOff=%u)",
                kTransformPaths[i], (unsigned)r_xf);
            SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, line);
            transform_ok = 1;
            break;
        }
        snprintf(line, sizeof(line), "SUMMON transform 候选不可用: %s", kTransformPaths[i]);
        SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, line);
    }
    if (transform_ok == 0) {
        SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "SUMMON 第3步 全部失败:SpawnTransform 将留空(全 0)继续尝试");
    }

    // 4) 组装参数
    memset(parms, 0, sizeof(parms));
    memcpy(parms + 0, ai_class, 40u);
    if (r_loc + 24u <= sizeof(loc)) memcpy(parms + 48, loc + r_loc, 24u);
    memcpy(parms + 76, &level, 4);
    memcpy(parms + 80, &level, 4);
    if (transform_ok != 0 && r_xf + 96u <= sizeof(xform)) {
        memcpy(parms + 96, xform + r_xf, 96u);
    }
    {
        double px = 0.0, py = 0.0, pz = 0.0;
        memcpy(&px, loc + r_loc, 8);
        memcpy(&py, loc + r_loc + 8u, 8);
        memcpy(&pz, loc + r_loc + 16u, 8);
        snprintf(line, sizeof(line), "SUMMON 组装完毕 坐标=(%.1f, %.1f, %.1f) transform=%s",
            px, py, pz, (transform_ok != 0) ? "有" : "空");
        SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, line);
    }
    if (do_call == 0) {
        snprintf(g_call_result, sizeof(g_call_result),
            "④ 已组装完成(未调用)。pawn 类=%s transform=%s",
            pawn_class, (transform_ok != 0) ? "有" : "空(候选路径都不可用)");
        SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, g_call_result);
        return 1;
    }

    // 5) 真正调用 SummonAI
    SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        "SUMMON 开始:调用 HTAbilityCharacter.SummonAI");
    if (!call_by_path(pawn, "/Script/HTGame.HTAbilityCharacter:SummonAI",
            parms, sizeof(parms), &r_sum)) {
        SUMMON_FAIL("⑤ 失败:SummonAI 调用入口不可用(路径或 parmsSize 有问题)");
    }
    SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "SUMMON 返回:没有崩溃");
    memcpy(&generated, parms + 200, 8);
    snprintf(g_call_result, sizeof(g_call_result),
        "⑤ SummonAI 完成:ReturnValue=0x%llX(%s) retOff=%u",
        (unsigned long long)generated, (generated != 0u) ? "拿到 Actor!" : "为空",
        (unsigned)r_sum);
    SUMMON_LOG(ANOMALY_CORE_LOG_LEVEL_V1_INFO, g_call_result);
    return 1;

#undef SUMMON_FAIL
#undef SUMMON_LOG
}

// 调用 KismetSystemLibrary:MakeSoftClassPath(FString) -> FSoftClassPath
// 目的:让游戏自己把那 40 字节的软类引用构造出来,我们只负责抄
static int run_softclass_test(void) {
    static wchar_t path_buffer[600];
    static const wchar_t* kPath =
        L"/Game/Blueprints/Character/Monster/NPC_Sub_fs_Simple_think.NPC_Sub_fs_Simple_think_C";
    AnomalyGenerationHandleV1 handle = {0u, 0u};
    AnomalyGenerationHandleV1 lib_handle = {0u, 0u};
    uintptr_t func = 0u;
    uintptr_t lib_class = 0u;
    uintptr_t lib_cdo = 0u;
    uint8_t parms[192];
    uint16_t parms_size = 0u;
    uint16_t return_offset = 0u;
    uintptr_t string_address = 0u;
    int32_t string_num = 0;
    int32_t string_max = 0;
    char hex[200];
    int i;
    int n;

    if (g_process_event_fn == 0u) {
        snprintf(g_call_result, sizeof(g_call_result), "② 跳过:ProcessEvent 未确认");
        return 0;
    }
    if (g_objects == NULL ||
        !ok(g_objects->find_exact(g_objects->user,
            sv("/Script/Engine.KismetSystemLibrary:MakeSoftClassPath"), &handle))) {
        snprintf(g_call_result, sizeof(g_call_result), "② 跳过:找不到 MakeSoftClassPath");
        return 0;
    }
    func = object_by_handle(handle);
    if (func == 0u) {
        snprintf(g_call_result, sizeof(g_call_result), "② 跳过:函数地址换算失败");
        return 0;
    }
    // 蓝图函数库的静态函数:Context 用该类的 CDO
    if (ok(g_objects->find_exact(g_objects->user, sv("/Script/Engine.KismetSystemLibrary"), &lib_handle))) {
        lib_class = object_by_handle(lib_handle);
        if (lib_class != 0u) {
            (void)read_value(lib_class + (uintptr_t)L_uclass_classDefaultObject, &lib_cdo);
        }
    }
    if (lib_cdo == 0u) {
        snprintf(g_call_result, sizeof(g_call_result), "② 跳过:KismetSystemLibrary CDO 取不到");
        return 0;
    }

    (void)read_value(func + (uintptr_t)L_function_parmsSize, &parms_size);
    (void)read_value(func + (uintptr_t)L_function_returnValueOffset, &return_offset);
    if (parms_size == 0u || parms_size > (uint16_t)sizeof(parms)) {
        snprintf(g_call_result, sizeof(g_call_result), "② 跳过:parmsSize=%u 不合理", (unsigned)parms_size);
        return 0;
    }
    memset(parms, 0, sizeof(parms));

    n = 0;
    while (kPath[n] != L'\0' && n < 599) {
        path_buffer[n] = kPath[n];
        ++n;
    }
    path_buffer[n] = L'\0';
    string_address = (uintptr_t)path_buffer;
    string_num = (int32_t)n + 1;
    string_max = string_num;
    memcpy(parms + 0, &string_address, 8);
    memcpy(parms + 8, &string_num, 4);
    memcpy(parms + 12, &string_max, 4);

    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            sv("SOFTCLASSTEST 开始:调用 MakeSoftClassPath"));
    }
    ((ProcessEventFn)g_process_event_fn)((void*)lib_cdo, (void*)func, parms);
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            sv("SOFTCLASSTEST 返回:没有崩溃"));
    }

    hex[0] = '\0';
    for (i = 0; i < 40 && (return_offset + i) < (int)sizeof(parms); ++i) {
        char part[8];
        snprintf(part, sizeof(part), "%02X", parms[return_offset + i]);
        strncat(hex, part, sizeof(hex) - strlen(hex) - 1u);
    }
    snprintf(g_call_result, sizeof(g_call_result),
        "② parmsSize=%u retOff=%u 返回 40 字节: %s",
        (unsigned)parms_size, (unsigned)return_offset, hex);
    if (g_core != NULL && g_core->log != NULL) {
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(g_call_result));
    }
    return 1;
}

static void function_layout_probe(void) {
    dump_function_layout("/Script/HTGame.HTAbilityCharacter:SummonAI");
    dump_function_layout("/Script/HTGame.HTPlayerController:ServerSummonAI_HybridMode");
}

static void world_chain_probe(void) {
    uintptr_t slot = 0u;
    uintptr_t game_instance = 0u;
    uintptr_t players_data = 0u;
    uint32_t players_num = 0u;
    uintptr_t local_player = 0u;
    uintptr_t controller = 0u;
    uintptr_t controller_class = 0u;
    char namebuf[120];

    if (g_gworld_slot == 0u) {
        pipe_add("世界链: GWorld slot 未知,跳过");
        return;
    }
    if (!read_value(g_gworld_slot, &g_world) || g_world == 0u) {
        pipe_add("世界链: GWorld 为空(可能还在主菜单)");
        return;
    }
    pipe_add("世界链: world=0x%llX", (unsigned long long)g_world);

    if (!read_value(g_world + (uintptr_t)L_world_gameInstance, &game_instance) || game_instance == 0u) {
        pipe_add("世界链: gameInstance 读失败(偏移 %d)", L_world_gameInstance);
        return;
    }
    if (!read_value(game_instance + (uintptr_t)L_gameInstance_localPlayers, &players_data) ||
        players_data == 0u) {
        pipe_add("世界链: localPlayers 数组读失败(偏移 %d)", L_gameInstance_localPlayers);
        return;
    }
    if (!read_value(game_instance + (uintptr_t)L_gameInstance_localPlayers + 8u, &players_num)) {
        players_num = 0u;
    }
    pipe_add("世界链: gameInstance=0x%llX localPlayers data=0x%llX num=%u",
        (unsigned long long)game_instance, (unsigned long long)players_data, players_num);
    if (players_num == 0u) return;

    if (!read_value(players_data, &local_player) || local_player == 0u) {
        pipe_add("世界链: localPlayer[0] 读失败");
        return;
    }
    if (!read_value(local_player + (uintptr_t)L_localPlayer_controller, &controller) || controller == 0u) {
        pipe_add("世界链: controller 读失败(偏移 %d)", L_localPlayer_controller);
        return;
    }
    namebuf[0] = '\0';
    if (!object_class_name(controller, namebuf, sizeof(namebuf))) {
        snprintf(namebuf, sizeof(namebuf), "(类名解析失败)");
    }
    pipe_add("世界链: controller=0x%llX 类=%s", (unsigned long long)controller, namebuf);

    if (!read_value(controller + (uintptr_t)L_object_class, &controller_class) || controller_class == 0u) {
        pipe_add("世界链: controller 的 UClass 读失败");
        return;
    }
    g_controller = controller;
    g_controller_class = controller_class;

    // 只保留玩家 pawn 的定位;类/函数逐个扫描属于调试路线,已从常规探测里移除
    (void)controller_class;
}

// ---------------------------------------------------------------------------
// 名字反向查找(names v2)
// ---------------------------------------------------------------------------

static void names_probe(void) {
    static const char* kNames[6] = {"SummonAI", "summonai", "AIClass", "aimclass",
        "HTPlayerController", "BP_WoodTest"};
    int i;
    if (g_names == NULL || !HAS_FIELD(g_names, AnomalyUe5NamesServiceV1, find_utf8) ||
        g_names->find_utf8 == NULL) {
        pipe_add("names: find_utf8 字段不可用(struct_size=%u, 需要 v2)",
            (g_names != NULL) ? g_names->struct_size : 0u);
        return;
    }
    pipe_add("names: struct_size=%u version=%u (find_utf8 字段在)", g_names->struct_size,
        g_names->service_version);
    for (i = 0; i < 6; ++i) {
        uint32_t name_id = 0u;
        AnomalyStatusV1 result = g_names->find_utf8(g_names->user, sv(kNames[i]), &name_id);
        if (ok(result)) {
            pipe_add("  find_utf8(%s) -> id=%u", kNames[i], name_id);
        } else {
            pipe_add("  find_utf8(%s) -> status=%u", kNames[i], result.code);
        }
    }
}

// ---------------------------------------------------------------------------
// 总探测
// ---------------------------------------------------------------------------

// 服务可能在本插件 on_load 之后才发布(nte.actors 依赖世界就绪),每次探测前补齐
static void refresh_services(void) {
    if (g_actors == NULL) {
        g_actors = (const AnomalyNteActorsServiceV1*)fetch_version(ANOMALY_NTE_ACTORS_SERVICE_V1_ID, 1u);
    }
    if (g_names == NULL) {
        g_names = (const AnomalyUe5NamesServiceV1*)fetch_version(
            ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
    }
    if (g_signature == NULL) {
        g_signature = (const AnomalySignatureServiceV1*)fetch_version(
            ANOMALY_SIGNATURE_SERVICE_V1_ID, 1u);
    }
    if (g_objects == NULL) {
        g_objects = (const AnomalyUe5ObjectsServiceV1*)fetch_version(
            ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, 1u);
    }
    if (g_framework == NULL) {
        g_framework = (const AnomalyUe5FrameworkServiceV1*)fetch_version(
            ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1u);
    }

}

static void pipeline_probe(void) {
    memset(&g_pipe, 0, sizeof(g_pipe));
    memset(g_classes, 0, sizeof(g_classes));
    g_class_count = 0;
    g_class_entry_count = 0;
    g_npc_class_count = 0;
    g_vehicle_class_count = 0;
    g_kind_cache_count = 0;
    g_actor_scanned = 0u;
    g_gobjects_items = 0u;
    g_process_event = 0u;
    g_gworld_slot = 0u;
    g_world = 0u;
    g_controller = 0u;

    stage_log("PROBE 开始 mode=%d", g_test_mode);
    refresh_services();

    stage_log("PROBE 1/4 signature_probe 开始");
    signature_probe();        // GObjects / GWorld / ProcessEvent 地址
    stage_flush();
    stage_log("PROBE 1/4 signature_probe 完成 items=0x%llX gworld_slot=0x%llX",
        (unsigned long long)g_gobjects_items, (unsigned long long)g_gworld_slot);

    stage_log("PROBE 2/4 process_event_probe 开始");
    process_event_probe();    // 确认 ProcessEvent 真身(调用游戏函数要用)
    stage_flush();
    stage_log("PROBE 2/4 process_event_probe 完成 pe=0x%llX",
        (unsigned long long)g_process_event_fn);

    stage_log("PROBE 3/4 world_chain_probe 开始");
    world_chain_probe();      // 定位玩家 pawn(生成落点参照)
    stage_flush();
    stage_log("PROBE 3/4 world_chain_probe 完成 world=0x%llX controller=0x%llX",
        (unsigned long long)g_world, (unsigned long long)g_controller);

    if (g_test_mode == 2) {
        stage_log("PROBE 4/4 actors_probe 跳过(mode=2)");
    } else {
        stage_log("PROBE 4/4 actors_probe 开始");
        actors_probe();           // 收集类列表(UI 数据源)
        stage_flush();
        stage_log("PROBE 4/4 actors_probe 完成 actors=%u 类=%d 条目=%d",
            g_actor_scanned, g_class_count, g_class_entry_count);
    }

    if (g_objects != NULL && g_objects->count != NULL) {
        pipe_add("objects.count=%u", g_objects->count(g_objects->user));
    }
    if (g_framework != NULL && g_framework->is_game_thread != NULL) {
        pipe_add("is_game_thread=%d", g_framework->is_game_thread(g_framework->user));
    }
    stage_flush();
    stage_log("PROBE 完成");
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

static AnomalyStatusV1 ANOMALY_CALL plugin_load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == NULL || plugin_context == NULL) return mk_status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *plugin_context = NULL;
    g_host = host;
    probe_services();

    g_core = (const AnomalyCoreServiceV1*)fetch_version(ANOMALY_CORE_SERVICE_V1_ID, 1u);
    g_ui = (const AnomalyUiServiceV1*)fetch_version(ANOMALY_UI_SERVICE_V1_ID, 1u);
    g_window = (const AnomalyWindowServiceV1*)fetch_version(ANOMALY_WINDOW_SERVICE_V1_ID, 1u);
    g_framework = (const AnomalyUe5FrameworkServiceV1*)fetch_version(
        ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1u);
    g_objects = (const AnomalyUe5ObjectsServiceV1*)fetch_version(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, 1u);
    // find_utf8 是 names v2 才有的字段,必须按 v2 请求,否则拿到的是被截断的 v1 表
    g_names = (const AnomalyUe5NamesServiceV1*)fetch_version(
        ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
    g_signature = (const AnomalySignatureServiceV1*)fetch_version(ANOMALY_SIGNATURE_SERVICE_V1_ID, 1u);
    g_actors = (const AnomalyNteActorsServiceV1*)fetch_version(ANOMALY_NTE_ACTORS_SERVICE_V1_ID, 1u);

    if (g_core == NULL || g_ui == NULL || g_window == NULL) {
        return mk_status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    test_mode_load();
    stage_log("NPC 召唤 1.1.0 设置持久化版 已加载 mode=%d", g_test_mode);
    *plugin_context = (void*)g_probes;
    return mk_status(ANOMALY_STATUS_V1_OK);
}

static AnomalyStatusV1 ANOMALY_CALL plugin_start(void* plugin_context) {
    AnomalyWindowSpecV1 spec = {0};
    (void)plugin_context;
    if (g_window == NULL || g_window->register_window == NULL) {
        return mk_status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    spec.struct_size = sizeof(spec);
    spec.id = sv("npc-summon-main");
    spec.title = sv("NPC 召唤");
    spec.initial_width = 820.0F;
    spec.initial_height = 560.0F;
    spec.minimum_width = 520.0F;
    spec.minimum_height = 300.0F;
    spec.default_open = 1;
    {
        AnomalyStatusV1 result = g_window->register_window(g_window->user, &spec, &g_window_handle);
        if (!ok(result)) return result;
    }
    // 第二个窗口:生成记录 / 标签 / 黑名单 / 导入导出(默认不打开,需要时在宿主窗口列表里开)
    {
        AnomalyWindowSpecV1 spec2 = {0};
        spec2.struct_size = sizeof(spec2);
        spec2.id = sv("npc-summon-records");
        spec2.title = sv("NPC 生成记录");
        spec2.initial_width = 640.0F;
        spec2.initial_height = 480.0F;
        spec2.minimum_width = 440.0F;
        spec2.minimum_height = 240.0F;
        spec2.default_open = 0;
        (void)g_window->register_window(g_window->user, &spec2, &g_record_window_handle);
    }
    return mk_status(ANOMALY_STATUS_V1_OK);
}

static AnomalyStatusV1 ANOMALY_CALL plugin_stop(
    void* plugin_context, uint32_t deadline_milliseconds) {
    (void)plugin_context;
    (void)deadline_milliseconds;
    settings_flush();
    if (g_window != NULL && g_window->release_window != NULL) {
        if (g_window_handle.id != 0u) {
            (void)g_window->release_window(g_window->user, g_window_handle);
        }
        if (g_record_window_handle.id != 0u) {
            (void)g_window->release_window(g_window->user, g_record_window_handle);
        }
    }
    g_window_handle = AnomalyGenerationHandleV1{0u, 0u};
    g_record_window_handle = AnomalyGenerationHandleV1{0u, 0u};
    return mk_status(ANOMALY_STATUS_V1_OK);
}

static void ANOMALY_CALL plugin_unload(void* plugin_context) {
    (void)plugin_context;
    settings_flush();
    g_host = NULL;
    g_core = NULL;
    g_ui = NULL;
    g_window = NULL;
    g_framework = NULL;
    g_objects = NULL;
    g_names = NULL;
    g_signature = NULL;
    g_actors = NULL;
    g_window_handle = AnomalyGenerationHandleV1{0u, 0u};
    g_record_window_handle = AnomalyGenerationHandleV1{0u, 0u};
}

// 进世界后跑一次(actor 快照为空时每 8 秒重试,最多 5 次)
static double g_probe_elapsed = 0.0;
static int g_probe_attempts = 0;
static double g_hb_elapsed = 0.0;

static void ANOMALY_CALL plugin_update(void* plugin_context, double delta_seconds) {
    (void)plugin_context;
    // 心跳:每秒一条,崩溃时能确定"死在哪一刻、插件当时是什么状态"
    g_hb_elapsed += delta_seconds;
    if (g_hb_elapsed >= 1.0) {
        g_hb_elapsed = 0.0;
        stage_log("HB mode=%d scanned=%u 类=%d 条目=%d 生成=%d 探测次数=%d pe=0x%llX",
            g_test_mode, g_actor_scanned, g_class_count, g_class_entry_count,
            g_spawned_count, g_probe_attempts, (unsigned long long)g_process_event);
    }
    settings_poll(delta_seconds);
    if (g_late_fix_actor != 0u) {
        g_late_fix_delay -= delta_seconds;
        if (g_late_fix_delay <= 0.0) {
            uintptr_t late_actor = g_late_fix_actor;
            uintptr_t late_reference = g_late_fix_reference;
            int still_mine = 0;
            int k;
            g_late_fix_actor = 0u;
            g_late_fix_reference = 0u;
            for (k = 0; k < g_spawned_count; ++k) {
                if (g_spawned[k] == late_actor) {
                    still_mine = 1;
                    break;
                }
            }
            if (still_mine != 0 && actor_looks_alive(late_actor) != 0) {
                (void)apply_ground_fix(late_actor, late_reference, "延迟");
            } else {
                stage_log("SPAWN 延迟 贴地:目标已销毁或不再属于本插件,跳过");
            }
        }
    }
    if (g_whitelist_loaded == 0) {
        g_whitelist_loaded = 1;
        whitelist_load();
    }
    if (InterlockedExchange(&g_pending_whitelist_toggle, 0) != 0) {
        char pending_name[100];
        AcquireSRWLockShared(&g_pending_lock);
        snprintf(pending_name, sizeof(pending_name), "%s", g_pending_whitelist_name);
        ReleaseSRWLockShared(&g_pending_lock);
        whitelist_toggle(pending_name);
        return;
    }
    if (g_pending_clear != 0) {
        g_pending_clear = 0;
        (void)run_clear();
        return;
    }
    if (g_pending_export != 0) {
        g_pending_export = 0;
        (void)records_export();
        return;
    }
    if (g_pending_import != 0) {
        g_pending_import = 0;
        (void)records_import();
        return;
    }
    if (g_pending_clear_records != 0) {
        g_pending_clear_records = 0;
        (void)records_clear_all();
        return;
    }
    // 主窗口的「生成记录」按钮:把记录窗口打开(UI 在 Render 域只置标志)
    if (g_pending_open_records != 0) {
        g_pending_open_records = 0;
        if (g_window != NULL && g_window->set_open != NULL && g_record_window_handle.id != 0u) {
            (void)g_window->set_open(g_window->user, g_record_window_handle, 1);
        }
        return;
    }
    // 手动重新扫描:清空上次结果并立即重扫(主界面探测不到 actor,进世界后点一下即可)
    if (g_pending_rescan != 0) {
        g_pending_rescan = 0;
        g_probe_attempts = 0;
        g_probe_elapsed = 0.0;
        g_actor_scanned = 0u;
        g_class_entry_count = 0;
        g_selected_class = -1;
        g_late_fix_actor = 0u;
        test_mode_load();
        if (g_test_mode == 1) {
            stage_log("PROBE 跳过(重新扫描 mode=1:只加载不探测)");
            return;
        }
        refresh_services();
        pipeline_probe();
        return;
    }
    if (g_pending_spawn_class >= 0) {
        int spawn_index = g_pending_spawn_class;
        g_pending_spawn_class = -1;
        (void)run_spawn_class(spawn_index);
        return;
    }
    if (g_call_pending != 0) {
        int which = g_call_pending;
        g_call_pending = 0;
        if (which == 2) {
            (void)run_softclass_test();
        } else if (which == 3) {
            (void)run_softclassref_test();
        } else if (which == 4) {
            (void)run_summon(0);
        } else if (which == 5) {
            (void)run_summon(1);
        } else if (which == 6) {
            (void)run_spawn(0);
        } else if (which == 7) {
            (void)run_spawn(1);
        } else {
            (void)run_call_test();
        }
        // 结果一律写日志,避免"失败分支无痕"这个反复踩的坑
        if (g_call_result[0] != '\0' && g_core != NULL && g_core->log != NULL) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, sv(g_call_result));
        }
        return;
    }
    if (g_test_mode == 1) return;   // 测试模式 1:只加载不探测
    g_probe_elapsed += delta_seconds;
    if (g_actor_scanned > 0u) return;
    if (g_probe_attempts >= 30) return;   // 更耐心:主界面探测不到 actor,进世界后要能自动扫到
    if (g_probe_attempts > 0 && g_probe_elapsed < 8.0 * (double)g_probe_attempts) return;
    g_probe_attempts++;
    pipeline_probe();
}

static void ANOMALY_CALL plugin_draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    AnomalyWindowStateV1 state = {0};
    int visible = 0;
    int main_open = 0;
    int i;
    char line[220];

    (void)plugin_context;
    if (ui == NULL || g_window == NULL) return;
    // 主窗口关闭时不能直接 return:记录窗口是独立窗口,不依赖主窗口是否打开
    if (g_window_handle.id != 0u) {
        state.struct_size = sizeof(state);
        if (ok(g_window->state(g_window->user, g_window_handle, &state)) &&
            state.open != 0 &&
            ok(g_window->begin(g_window->user, g_window_handle, 0u, &visible))) {
            main_open = 1;
        }
    }

    if (visible != 0) {
        char label[220];
        int has_selected;
        int has_spawned;

        if (g_selected_class >= g_class_entry_count) g_selected_class = -1;
        has_selected = (g_selected_class >= 0) ? 1 : 0;
        has_spawned = (g_spawned_count > 0) ? 1 : 0;

        // ===== 操作按钮放最顶 =====
        if (ui->button_enabled != NULL) {
            if (ui->button_enabled(ui->user, sv("生成到面前"), 150.0F, 0.0F, has_selected) != 0) {
                g_pending_spawn_class = g_selected_class;
            }
            if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 10.0F);
            if (ui->button_enabled(ui->user, sv("清除我生成的"), 150.0F, 0.0F, has_spawned) != 0) {
                g_pending_clear = 1;
            }
            if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 10.0F);
            if (ui->button_enabled(ui->user, sv("重新扫描"), 110.0F, 0.0F, 1) != 0) {
                g_pending_rescan = 1;
            }
            if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 10.0F);
            if (ui->button_enabled(ui->user, sv("生成记录"), 110.0F, 0.0F, 1) != 0) {
                g_pending_open_records = 1;
            }
        } else if (ui->button != NULL) {
            if (ui->button(ui->user, sv("生成到面前"), 150.0F, 0.0F) != 0 && has_selected != 0) {
                g_pending_spawn_class = g_selected_class;
            }
            if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 10.0F);
            if (ui->button(ui->user, sv("清除我生成的"), 150.0F, 0.0F) != 0 && has_spawned != 0) {
                g_pending_clear = 1;
            }
            if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 10.0F);
            if (ui->button(ui->user, sv("重新扫描"), 110.0F, 0.0F) != 0) {
                g_pending_rescan = 1;
            }
            if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 10.0F);
            if (ui->button(ui->user, sv("生成记录"), 110.0F, 0.0F) != 0) {
                g_pending_open_records = 1;
            }
        }
        ui->separator(ui->user);
        snprintf(line, sizeof(line), "NPC %d 个 · 车辆 %d 个 · 白名单 %d 个 · 已生成 %d 个",
            g_npc_class_count, g_vehicle_class_count, g_whitelist_count, g_spawned_count);
        ui->text(ui->user, sv(line));

        // 分类开关:默认只显示 NPC 和车辆;其它类(体积/触发物/空壳)不显示
        if (ui->checkbox != NULL) {
            ui->checkbox(ui->user, sv("NPC"), &g_show_npc);
            if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 10.0F);
            ui->checkbox(ui->user, sv("车辆"), &g_show_vehicle);
            if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 10.0F);
            ui->checkbox(ui->user, sv("其它(调试)"), &g_show_all_classes);
        }

        // 搜索框:按名字/关键词过滤下面的列表
        if (ui->input_text != NULL) {
            ui->input_text(ui->user, sv("搜索类名"), g_search, sizeof(g_search), 0u);
        }

        // 类列表(可滚动),按 NPC / 车辆 / 其它 分组
        // 注意:BeginChild 无论返回什么都必须 EndChild(与 tab 容器语义不同),
        // 否则每帧都会让 UI 栈多推一层 → 宿主报 "unbalanced UI stack" 并把插件判为故障
        if (ui->begin_child != NULL) {
            int child_open = ui->begin_child(ui->user, sv("npc-list"), 0.0F, 320.0F, 0u);
            if (child_open != 0) {
                int shown = 0;
                int cat;
                for (cat = 0; cat <= 3; ++cat) {
                    int header_drawn = 0;
                    int want = (cat == 0) ? 1
                             : ((cat == 1) ? g_show_npc
                             : ((cat == 2) ? g_show_vehicle : g_show_all_classes));
                    if (want == 0) continue;
                    for (i = 0; i < g_class_entry_count && shown < 300; ++i) {
                        int kind = g_class_entries[i].kind;
                        int in_whitelist = (whitelist_index_of(g_class_entries[i].name) >= 0);
                        int entry_cat = (in_whitelist != 0) ? 0
                            : ((kind == 1) ? 1 : ((kind == 2) ? 2 : 3));
                        int match = 1;
                        if (entry_cat != cat) continue;
                        if (g_search[0] != '\0') {
                            if (ui->filter_match != NULL) {
                                match = ui->filter_match(ui->user, sv(g_search),
                                    sv(g_class_entries[i].name));
                            } else {
                                match = (strstr(g_class_entries[i].name, g_search) != NULL) ? 1 : 0;
                            }
                        }
                        if (match == 0) continue;
                        if (record_is_blacklisted(g_class_entries[i].name) != 0) continue;
                        if (header_drawn == 0) {
                            ui->text(ui->user, sv(cat == 0 ? "── 白名单 ──"
                                : (cat == 1 ? "── NPC ──"
                                : (cat == 2 ? "── 车辆 ──" : "── 其它(调试) ──"))));
                            header_drawn = 1;
                        }
                        // 白名单开关:点一下加入 / 移出(用文字,避免字体缺 ★ 字形显示成问号)
                        {
                            char star[48];
                            snprintf(star, sizeof(star), "%s##w%d",
                                (in_whitelist != 0) ? "移出" : "收藏", i);
                            if (ui->button(ui->user, sv(star), 52.0F, 0.0F) != 0) {
                                AcquireSRWLockExclusive(&g_pending_lock);
                                snprintf(g_pending_whitelist_name,
                                    sizeof(g_pending_whitelist_name), "%s",
                                    g_class_entries[i].name);
                                ReleaseSRWLockExclusive(&g_pending_lock);
                                InterlockedExchange(&g_pending_whitelist_toggle, 1);
                            }
                            if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 6.0F);
                        }
                        if (i == g_selected_class) {
                            snprintf(label, sizeof(label), "[已选] %s  (%u)",
                                g_class_entries[i].name, g_class_entries[i].count);
                        } else {
                            snprintf(label, sizeof(label), "        %s  (%u)",
                                g_class_entries[i].name, g_class_entries[i].count);
                        }
                        if (ui->button(ui->user, sv(label), 0.0F, 0.0F) != 0) {
                            g_selected_class = i;
                            has_selected = 1;
                        }
                        shown++;
                    }
                }
                if (shown == 0) {
                    ui->text(ui->user, sv(g_class_entry_count == 0
                        ? "(正在扫描场景,请稍候几秒…)"
                        : "(没有匹配的 NPC / 车辆,换个关键词试试)"));
                }
            }
            if (ui->end_child != NULL) ui->end_child(ui->user);
        }

        if (has_selected != 0) {
            snprintf(line, sizeof(line), "已选: %s", g_class_entries[g_selected_class].name);
        } else {
            snprintf(line, sizeof(line), "未选类 —— 在上面的列表里点一个");
        }
        ui->text(ui->user, sv(line));

        ui->separator(ui->user);
        if (g_call_result[0] != '\0') {
            ui->text(ui->user, sv(g_call_result));
        } else {
            ui->text(ui->user, sv("生成的 NPC 出现在你前方并自动贴地;列表来自世界上真实运行的 actor。"));
        }
    }

    if (main_open != 0) {
        (void)g_window->end(g_window->user, g_window_handle);
    }

    // ===== 第二个窗口:生成记录 / 标签 / 黑名单 / 备注 / 导入导出 =====
    if (g_record_window_handle.id != 0u) {
        AnomalyWindowStateV1 rec_state = {0};
        int rec_visible = 0;
        rec_state.struct_size = sizeof(rec_state);
        if (ok(g_window->state(g_window->user, g_record_window_handle, &rec_state)) &&
            rec_state.open != 0 &&
            ok(g_window->begin(g_window->user, g_record_window_handle, 0u, &rec_visible))) {
            if (rec_visible != 0) {
                char note_label[150];
                char tag_a[60];
                char tag_b[60];
                char tag_c[60];
                int r;

                snprintf(line, sizeof(line), "已生成过 %d 个类 · 黑名单(标记失败) %d 个 · 共 %d 个自建 NPC",
                    g_record_count, records_blacklist_count(), g_spawned_count);
                ui->text(ui->user, sv(line));

                if (ui->button != NULL) {
                    if (ui->button(ui->user, sv("导出到文件"), 130.0F, 0.0F) != 0) {
                        g_pending_export = 1;
                    }
                    if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 8.0F);
                    if (ui->button(ui->user, sv("从文件导入"), 130.0F, 0.0F) != 0) {
                        g_pending_import = 1;
                    }
                    if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 8.0F);
                    if (ui->button(ui->user, sv("清空记录"), 110.0F, 0.0F) != 0) {
                        g_pending_clear_records = 1;
                    }
                }
                ui->text(ui->user,
                    sv("文件位置:<游戏目录>\\Anomaly\\npc-summon-records.txt(可手工编辑后再导入)"));
                if (g_record_status[0] != '\0') {
                    ui->text(ui->user, sv(g_record_status));
                }
                ui->separator(ui->user);

                if (ui->begin_child != NULL) {
                    int rec_open = ui->begin_child(ui->user, sv("record-list"), 0.0F, 360.0F, 0u);
                    if (rec_open != 0) {
                        for (r = 0; r < g_record_count; ++r) {
                            NpcRecord* rec = &g_records[r];
                            snprintf(line, sizeof(line), "%s   (生成 %u 次)   %s", rec->class_name,
                                rec->spawn_count,
                                (rec->verdict == 1) ? "[成功]"
                                                    : ((rec->verdict == 2) ? "[失败/黑名单]" : "[未标记]"));
                            ui->text(ui->user, sv(line));
                            snprintf(tag_a, sizeof(tag_a), "未标记##a%d", r);
                            snprintf(tag_b, sizeof(tag_b), "成功##b%d", r);
                            snprintf(tag_c, sizeof(tag_c), "失败##c%d", r);
                            if (ui->button != NULL) {
                                if (ui->button(ui->user, sv(tag_a), 84.0F, 0.0F) != 0) rec->verdict = 0;
                                if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 6.0F);
                                if (ui->button(ui->user, sv(tag_b), 74.0F, 0.0F) != 0) rec->verdict = 1;
                                if (ui->same_line != NULL) ui->same_line(ui->user, 0.0F, 6.0F);
                                if (ui->button(ui->user, sv(tag_c), 74.0F, 0.0F) != 0) rec->verdict = 2;
                            }
                            snprintf(note_label, sizeof(note_label), "备注##n%d", r);
                            if (ui->input_text != NULL) {
                                ui->input_text(ui->user, sv(note_label), rec->note,
                                    sizeof(rec->note), 0u);
                            } else if (rec->note[0] != '\0') {
                                ui->text(ui->user, sv(rec->note));
                            }
                            ui->separator(ui->user);
                        }
                        if (g_record_count == 0) {
                            ui->text(ui->user, sv("(还没有生成过任何 NPC;标记为失败的类不会出现在主列表里)"));
                        }
                    }
                    if (ui->end_child != NULL) ui->end_child(ui->user);
                }
            }
            (void)g_window->end(g_window->user, g_record_window_handle);
        }
    }
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return mk_status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = AnomalyPluginDescriptorV1{
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        sv("local.nte.npc-summon"), sv("NPC 召唤"), sv("OUO"), sv("1.1.0"),
        plugin_load, plugin_start, plugin_stop, plugin_unload, plugin_update, plugin_draw};
    return mk_status(ANOMALY_STATUS_V1_OK);
}
