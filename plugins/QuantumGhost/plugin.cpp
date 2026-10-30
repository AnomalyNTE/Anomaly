// 瞬秒-量子幽灵计划：在伤害结算前把 FHTDamageEvent.damage 放大到固定倍率，
// 并在客户端补扣血量、按住 60 秒，使目标停在"血条已空但服务器仍判定存活"的状态。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ue5.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// scale_stub.asm 引用的全局量
extern "C" {
void ScaleStub();
extern std::uintptr_t g_original;
extern int g_scale_flag;
extern float g_scale_factor;
extern unsigned long long g_scale_hits;
extern unsigned long long g_seen_hits;
extern std::uintptr_t g_last_victim;
extern float g_last_before;
extern float g_last_after;
extern unsigned long long g_hp_applied;
extern float g_one;
extern std::uintptr_t g_pending_victim;
extern float g_pending_extra;
extern int g_pending_flag;
}

namespace {

using anomaly::sdk::StringView;

#define HAS(service, member) ((service) != nullptr && (service)->member != nullptr)

constexpr std::uint32_t kCondFirstUseEver = 4u;
constexpr std::size_t kDamageOffset = 0x10;
constexpr std::uintptr_t kOnDamagedRva = 0x7CB1710;
constexpr const char* kOnDamagedPattern =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC B0 01 00 00 48 8B E9 49 8B F1 "
    "48 8D 4C 24 20 49 8B F8 48 8B DA E8 ?? ?? ?? ??";
constexpr int kMaxCallSites = 8;

// 固定参数：倍率、压制时长（秒）、同时改基础血量
constexpr float kFactor = 400.0F;
constexpr int kHoldSeconds = 60;

constexpr const char* kDisplayName = "瞬秒-量子幽灵计划";
constexpr const char* kDescriptionLines[] = {
    "面壁者计划量子幽灵：攻击命中后直接清空血条，",
    "让目标在观察者眼里已经死去，但决定生死的观测权不在客户端，",
    "所以它只是看起来死了；目标因此停在非死非活的量子叠加态。"};

// ASC（victim + 0x940）上的属性偏移
constexpr std::uintptr_t kAscOffset = 0x940;
constexpr std::uintptr_t kHpOffset = 0x1A38;
constexpr std::uintptr_t kBaseHpOffset = 0x1A40;
constexpr std::uintptr_t kPropWindowOffset = 0x1A00;
constexpr int kPropWindowCount = 24;
constexpr float kHpSanityMax = 1.0e9F;
constexpr double kLogIntervalMs = 1000.0;

// 服务
const AnomalyCoreServiceV1* g_core{};
const AnomalyUiServiceV1* g_ui{};
const AnomalySignatureServiceV1* g_signature{};
const AnomalyHookServiceV1* g_hook{};
const AnomalyNteCombatServiceV1* g_combat{};

// 状态
AnomalyGenerationHandleV1 g_hook_handle{};
std::atomic<int> g_hook_state{0};
std::atomic<std::uintptr_t> g_hooked_site{0};
std::atomic<int> g_site_count{0};
char g_resolve_note[96]{};

std::atomic<int> g_enabled{1};

std::uintptr_t g_hold_victim{};
float g_hold_target{};
double g_hold_until{};
int g_hold_writes{};
int g_hold_frames{};

std::atomic<std::uint64_t> g_damage_events{0};
std::atomic<long long> g_last_damage{0};
std::uint64_t g_logged_hits{};
std::uint64_t g_logged_seen{};
double g_now_ms{};
double g_last_log_ms{-1.0e9};

std::uint64_t g_combat_cursor{};
bool g_combat_cursor_ready{};
std::atomic<int> g_damage_log_budget{60};

void Log(std::uint32_t level, const char* text) noexcept {
    if (g_core == nullptr || g_core->log == nullptr || text == nullptr) return;
    g_core->log(g_core->user, level, StringView(text));
}

void LogInfo(const char* text) noexcept { Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, text); }

void Logf(std::uint32_t level, const char* format, ...) noexcept {
    char buffer[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(level, buffer);
}

double NowMs() noexcept {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart == 0) return 0.0;
    return static_cast<double>(counter.QuadPart) * 1000.0 /
           static_cast<double>(frequency.QuadPart);
}

std::uintptr_t ModuleBase() noexcept {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"HTGame.exe"));
}

struct TextRange {
    std::uintptr_t begin{};
    std::size_t size{};
};

bool FindTextRange(TextRange& range) noexcept {
    const std::uintptr_t base = ModuleBase();
    if (base == 0) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        if (std::memcmp(section->Name, ".text", 5) != 0) continue;
        range.begin = base + section->VirtualAddress;
        range.size = section->Misc.VirtualSize;
        return range.size > 0x1000;
    }
    return false;
}

int FindCallSites(std::uintptr_t callee, std::uintptr_t* out, int capacity) noexcept {
    TextRange range{};
    if (!FindTextRange(range)) return 0;

    int found = 0;
    constexpr std::size_t kChunk = 1u << 20;
    for (std::size_t offset = 0; offset + kChunk <= range.size && found < capacity;
         offset += kChunk - 16) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(range.begin + offset);
        __try {
            for (std::size_t index = 0; index + 5 <= kChunk && found < capacity; ++index) {
                if (bytes[index] != 0xE8) continue;
                std::int32_t displacement{};
                std::memcpy(&displacement, bytes + index + 1, sizeof(displacement));
                const auto site = range.begin + offset + index;
                const auto destination = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(site) + 5 + displacement);
                if (destination == callee) out[found++] = site;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return found;
}

// CharacterOnDamaged 入口已被宿主挂钩，特征必然失配，因此特征失败后退回固定 RVA。
std::uintptr_t ResolveOnDamaged() noexcept {
    const std::uintptr_t module = ModuleBase();
    if (module == 0) return 0;

    if (HAS(g_signature, resolve)) {
        std::uintptr_t address{};
        const AnomalyStatusV1 status = g_signature->resolve(
            g_signature->user, StringView("HTGame.exe"), StringView(".text"),
            StringView(kOnDamagedPattern), &address);
        if (status.code == ANOMALY_STATUS_V1_OK && address != 0) {
            std::snprintf(g_resolve_note, sizeof(g_resolve_note), "特征扫描");
            return address;
        }
        std::snprintf(g_resolve_note, sizeof(g_resolve_note), "特征扫描 code=%u",
                      static_cast<unsigned>(status.code));
    } else {
        std::snprintf(g_resolve_note, sizeof(g_resolve_note), "无签名服务");
    }

    const std::uintptr_t candidate = module + kOnDamagedRva;
    bool hooked = false;
    __try {
        const auto* bytes = reinterpret_cast<const unsigned char*>(candidate);
        hooked = bytes[0] == 0xE9 || bytes[0] == 0xEB || (bytes[0] == 0xFF && bytes[1] == 0x25) ||
                 (bytes[0] == 0x48 && bytes[1] == 0xB8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hooked = false;
    }
    std::snprintf(g_resolve_note, sizeof(g_resolve_note), "RVA 0x%llX(%s)",
                  static_cast<unsigned long long>(kOnDamagedRva), hooked ? "已挂钩" : "未挂钩");
    return candidate;
}

// 结算前的取值指令：movss xmm1,[r14+0x10]
constexpr unsigned char kSettlementLead[] = {
    0xF3, 0x41, 0x0F, 0x10, 0x4E, 0x10, 0x4D, 0x8D,
    0x8E, 0x48, 0x01, 0x00, 0x00, 0x48, 0x89, 0x6C,
    0x24, 0x30};

std::uintptr_t FindSettlement(std::uintptr_t site) noexcept {
    if (site < 0x1000) return 0;
    constexpr std::size_t kWindow = 0x400;
    const std::uintptr_t begin = site - kWindow;
    constexpr std::size_t kCount = sizeof(kSettlementLead);
    __try {
        for (std::size_t index = 0; index + kCount <= kWindow; ++index) {
            if (std::memcmp(reinterpret_cast<const void*>(begin + index), kSettlementLead,
                            kCount) == 0) {
                return begin + index;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

bool AttachHook(std::uintptr_t target, const char* label) noexcept {
    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = target;
    request.detour = reinterpret_cast<void*>(&ScaleStub);
    request.label = StringView(label);
    std::uintptr_t original{};
    AnomalyGenerationHandleV1 handle{};
    const AnomalyStatusV1 status = g_hook->create(g_hook->user, &request, &original, &handle);
    Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
         "quantum-ghost: 挂钩 code=%u original=0x%llX 目标=0x%llX", static_cast<unsigned>(status.code),
         static_cast<unsigned long long>(original), static_cast<unsigned long long>(target));
    if (status.code != ANOMALY_STATUS_V1_OK || original == 0) return false;
    g_original = original;
    g_hook_handle = handle;
    g_hooked_site.store(target, std::memory_order_release);
    g_hook_state.store(1, std::memory_order_release);
    return true;
}

bool InstallHook() noexcept {
    if (g_hook == nullptr || !HAS(g_hook, create)) return false;

    const std::uintptr_t callee = ResolveOnDamaged();
    if (callee == 0) return false;

    std::uintptr_t sites[kMaxCallSites]{};
    const int count = FindCallSites(callee, sites, kMaxCallSites);
    g_site_count.store(count, std::memory_order_release);
    Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "quantum-ghost: 目标 0x%llX（%s） 调用点=%d",
         static_cast<unsigned long long>(callee), g_resolve_note, count);
    if (count == 0) return false;

    const std::uintptr_t settlement = FindSettlement(sites[0]);
    if (settlement != 0) {
        Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "quantum-ghost: 结算取值点 0x%llX（RVA 0x%llX）",
             static_cast<unsigned long long>(settlement),
             static_cast<unsigned long long>(settlement - ModuleBase()));
        if (AttachHook(settlement, "damage-chain.scale")) return true;
    }

    for (int index = 0; index < count; ++index) {
        if (AttachHook(sites[index], "damage-chain.scale")) return true;
    }
    return false;
}

void RemoveHook() noexcept {
    g_scale_flag = 0;
    if (g_hook_state.exchange(0, std::memory_order_acq_rel) == 0) return;
    if (g_hook != nullptr && HAS(g_hook, release)) {
        static_cast<void>(g_hook->release(g_hook->user, g_hook_handle));
    }
    g_hook_handle = AnomalyGenerationHandleV1{};
    g_hooked_site.store(0, std::memory_order_release);
    LogInfo("quantum-ghost: 挂钩已释放");
}

void LogPropertyWindow(const char* tag, std::uintptr_t asc) noexcept {
    if (asc < 0x10000) return;
    float values[kPropWindowCount]{};
    __try {
        const float* const raw = reinterpret_cast<const float*>(asc + kPropWindowOffset);
        for (int index = 0; index < kPropWindowCount; ++index) values[index] = raw[index];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    char line[512]{};
    for (int row = 0; row < 2; ++row) {
        int used = 0;
        for (int index = row * 12; index < (row + 1) * 12 && index < kPropWindowCount; ++index) {
            used += std::snprintf(line + used, static_cast<std::size_t>(static_cast<int>(sizeof(line)) - used),
                                  " %04X=%.1f",
                                  static_cast<unsigned>(kPropWindowOffset + index * 4),
                                  static_cast<double>(values[index]));
            if (used >= static_cast<int>(sizeof(line)) - 32) break;
        }
        Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "quantum-ghost: 属性窗口 %s%s", tag, line);
    }
}

// 在伤害调用栈里写 HP 会被结算函数覆盖，改到下一帧执行。
void ApplyPendingDamage() noexcept {
    if (g_pending_flag == 0) return;
    const std::uintptr_t victim = g_pending_victim;
    const float extra = g_pending_extra;
    g_pending_flag = 0;
    if (victim < 0x10000 || (victim & 7) != 0 || !(extra > 0.0F)) return;
    __try {
        const std::uintptr_t asc = *reinterpret_cast<const std::uintptr_t*>(victim + kAscOffset);
        if (asc < 0x10000 || (asc & 7) != 0) return;
        float* const hp = reinterpret_cast<float*>(asc + kHpOffset);
        const float before = *hp;
        if (!(before > 0.0F) || before > kHpSanityMax) return;
        LogPropertyWindow("命中前", asc);
        float after = before - extra;
        if (after < 0.0F) after = 0.0F;
        *hp = after;
        // 基础血量一起压到当前血量，避免上限把血条拉回去。
        reinterpret_cast<float*>(asc + kBaseHpOffset)[0] = after;
        LogPropertyWindow("命中后", asc);
        g_hp_applied = g_hp_applied + 1;
        g_hold_victim = victim;
        g_hold_target = after;
        g_hold_until = g_now_ms + static_cast<double>(kHoldSeconds) * 1000.0;
        g_hold_writes = 0;
        g_hold_frames = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
}

// 游戏会用权威值覆盖血量，窗口内持续按回目标值（只往下压）。
void HoldHp() noexcept {
    if (g_hold_victim == 0) return;
    if (g_now_ms > g_hold_until) {
        if (g_hold_frames > 0) {
            Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                 "quantum-ghost: 压制结束 目标=%.1f 重写=%d 帧=%d",
                 static_cast<double>(g_hold_target), g_hold_writes, g_hold_frames);
        }
        g_hold_victim = 0;
        return;
    }
    ++g_hold_frames;
    float hp = -1.0F;
    __try {
        const std::uintptr_t asc = *reinterpret_cast<const std::uintptr_t*>(g_hold_victim + kAscOffset);
        if (asc < 0x10000 || (asc & 7) != 0) {
            g_hold_victim = 0;
            return;
        }
        float* const field = reinterpret_cast<float*>(asc + kHpOffset);
        hp = *field;
        if (!(hp > 0.0F) || hp > kHpSanityMax) {
            g_hold_victim = 0;
            return;
        }
        if (hp <= 0.05F) {
            Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "quantum-ghost: 目标血量归零，停止压制 帧=%d",
                 g_hold_frames);
            g_hold_victim = 0;
            return;
        }
        if (hp > g_hold_target + 0.01F) {
            *field = g_hold_target;
            reinterpret_cast<float*>(asc + kBaseHpOffset)[0] = g_hold_target;
            ++g_hold_writes;
            hp = g_hold_target;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_hold_victim = 0;
        return;
    }
    if (g_hold_frames % 30 == 0) {
        Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "quantum-ghost: 压制中 帧=%d hp=%.1f 目标=%.1f 重写=%d",
             g_hold_frames, static_cast<double>(hp), static_cast<double>(g_hold_target),
             g_hold_writes);
    }
}

void PollDamage() noexcept {
    if (g_combat == nullptr || !HAS(g_combat, next_event)) return;
    if (!g_combat_cursor_ready) {
        if (HAS(g_combat, latest_event_sequence)) {
            g_combat_cursor = g_combat->latest_event_sequence(g_combat->user);
        }
        g_combat_cursor_ready = true;
        return;
    }
    for (int step = 0; step < 32; ++step) {
        AnomalyNteCombatEventV1 event{};
        event.struct_size = sizeof(event);
        const AnomalyStatusV1 status = g_combat->next_event(g_combat->user, g_combat_cursor, &event);
        if (status.code != ANOMALY_STATUS_V1_OK) return;
        g_combat_cursor = event.sequence;
        if (event.kind != ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE) continue;
        g_damage_events.fetch_add(1, std::memory_order_relaxed);
        g_last_damage.store(event.value, std::memory_order_release);
        if (g_damage_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
            Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                 "quantum-ghost: 伤害 seq=%llu value=%lld basic=%lld final=%lld",
                 static_cast<unsigned long long>(event.sequence),
                 static_cast<long long>(event.value), static_cast<long long>(event.basic_value),
                 static_cast<long long>(event.final_value));
        }
    }
}

float ReadVictimHp(std::uintptr_t victim) noexcept {
    if (victim < 0x10000 || (victim & 7) != 0) return -1.0F;
    __try {
        const std::uintptr_t asc = *reinterpret_cast<const std::uintptr_t*>(victim + kAscOffset);
        if (asc < 0x10000 || (asc & 7) != 0) return -1.0F;
        return *reinterpret_cast<const float*>(asc + kHpOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0F;
    }
}

void LogCounters() noexcept {
    const std::uint64_t seen = g_seen_hits;
    if (seen != g_logged_seen) {
        g_logged_seen = seen;
        Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
             "quantum-ghost: 命中#%llu 本次 %.2f->%.2f 目标HP=%.1f 补扣=%llu 目标=0x%llX",
             static_cast<unsigned long long>(seen), static_cast<double>(g_last_before),
             static_cast<double>(g_last_after), static_cast<double>(ReadVictimHp(g_last_victim)),
             static_cast<unsigned long long>(g_hp_applied),
             static_cast<unsigned long long>(g_last_victim));
    }
    const std::uint64_t hits = g_scale_hits;
    if (hits == g_logged_hits) return;
    if (g_now_ms - g_last_log_ms < kLogIntervalMs) return;
    g_last_log_ms = g_now_ms;
    g_logged_hits = hits;
    Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
         "quantum-ghost: 调用=%llu 放大=%llu 倍率=%.2f 上次 %.2f->%.2f 伤害事件=%llu 最近=%lld",
         static_cast<unsigned long long>(g_seen_hits), static_cast<unsigned long long>(hits),
         static_cast<double>(g_scale_factor), static_cast<double>(g_last_before),
         static_cast<double>(g_last_after),
         static_cast<unsigned long long>(g_damage_events.load(std::memory_order_relaxed)),
         static_cast<long long>(g_last_damage.load(std::memory_order_relaxed)));
}

// 只含 POD 的绘制体：不用 C++ 对象，才能用 SEH 兜住宿主服务缺失导致的访问违例。
bool DrawPanelContent(const AnomalyUiServiceV1* ui) noexcept {
    __try {
        if (HAS(ui, set_next_window_size)) {
            ui->set_next_window_size(ui->user, 460.0F, 0.0F, kCondFirstUseEver);
        }
        if (HAS(ui, checkbox)) {
            int enabled = g_enabled.load(std::memory_order_acquire);
            if (ui->checkbox(ui->user, StringView("启动量子幽灵计划"), &enabled) != 0) {
                g_enabled.store(enabled != 0 ? 1 : 0, std::memory_order_release);
                g_scale_flag = enabled != 0 ? 1 : 0;
            }
        }
        if (HAS(ui, separator)) ui->separator(ui->user);
        for (const char* line : kDescriptionLines) {
            if (HAS(ui, text)) {
                ui->text(ui->user, StringView(line));
            } else if (HAS(ui, text_wrapped)) {
                ui->text_wrapped(ui->user, StringView(line));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void DrawPanel(const AnomalyUiServiceV1* ui) noexcept {
    if (ui == nullptr) ui = g_ui;
    if (ui == nullptr) return;
    if (!HAS(ui, begin_window) || !HAS(ui, end_window)) return;

    int open = 1;
    anomaly::sdk::UiWindow window(ui, std::string_view(kDisplayName), &open);
    if (!window) return;
    if (!DrawPanelContent(ui)) LogInfo("quantum-ghost: 绘制面板异常，已跳过本帧");
}

void ResetState() noexcept {
    g_scale_hits = 0;
    g_seen_hits = 0;
    g_hp_applied = 0;
    g_last_before = 0.0F;
    g_last_after = 0.0F;
    g_scale_flag = 0;
    g_scale_factor = kFactor;
    g_logged_hits = 0;
    g_logged_seen = 0;
    g_last_log_ms = -1.0e9;
    g_combat_cursor = 0;
    g_combat_cursor_ready = false;
    g_damage_events.store(0, std::memory_order_relaxed);
    g_last_damage.store(0, std::memory_order_relaxed);
    g_damage_log_budget.store(60, std::memory_order_relaxed);
    g_hold_victim = 0;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) noexcept {
    const anomaly::sdk::Host sdk(host);
    const auto ui = sdk.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID);
    if (!ui) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    g_ui = ui.get();
    g_core = sdk.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
    g_signature = sdk.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
    g_hook = sdk.Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID).get();
    g_combat = sdk.Query<AnomalyNteCombatServiceV1>(ANOMALY_NTE_COMBAT_SERVICE_V1_ID).get();

    if (context != nullptr) *context = nullptr;
    ResetState();
    Logf(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "quantum-ghost: 7.3.2 加载 ui=%s sig=%s hook=%s combat=%s",
         g_ui != nullptr ? "有" : "无", g_signature != nullptr ? "有" : "无",
         g_hook != nullptr ? "有" : "无", g_combat != nullptr ? "有" : "无");
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* /*context*/) noexcept {
    g_scale_factor = kFactor;
    if (!InstallHook()) {
        Logf(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "quantum-ghost: 挂钩失败（调用点=%d）",
             g_site_count.load(std::memory_order_relaxed));
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* /*context*/, std::uint32_t /*deadline*/) noexcept {
    RemoveHook();
    ResetState();
    LogInfo("quantum-ghost: 已停止");
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* /*context*/) noexcept {
    RemoveHook();
    g_ui = nullptr;
    g_core = nullptr;
    g_signature = nullptr;
    g_hook = nullptr;
    g_combat = nullptr;
}

void ANOMALY_CALL Update(void* /*context*/, double /*delta_seconds*/) noexcept {
    g_now_ms = NowMs();
    g_scale_factor = kFactor;
    g_scale_flag = g_enabled.load(std::memory_order_acquire) != 0 ? 1 : 0;
    ApplyPendingDamage();
    HoldHp();
    PollDamage();
    LogCounters();
}

void ANOMALY_CALL Draw(void* /*context*/, const AnomalyUiServiceV1* ui) noexcept {
    DrawPanel(ui);
}

}  // namespace

// scale_stub.asm 通过 EXTERN 引用
std::uintptr_t g_original{};
int g_scale_flag{};
float g_scale_factor{3.0F};
unsigned long long g_scale_hits{};
unsigned long long g_seen_hits{};
std::uintptr_t g_last_victim{};
float g_last_before{};
float g_last_after{};
unsigned long long g_hp_applied{};
float g_one{1.0F};
std::uintptr_t g_pending_victim{};
float g_pending_extra{};
int g_pending_flag{};

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL
AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {sizeof(*descriptor),
                   ANOMALY_PLUGIN_API_V1_MAJOR,
                   ANOMALY_PLUGIN_API_V1_MINOR,
                   StringView("anomaly.local.quantum-ghost"),
                   StringView(kDisplayName),
                   StringView("Anomaly"),
                   StringView("7.3.2"),
                   Load,
                   Start,
                   Stop,
                   Unload,
                   Update,
                   Draw};
    return anomaly::sdk::Ok();
}
