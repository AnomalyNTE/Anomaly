#include "anomaly/apphelp_fault_guard.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <winternl.h>

#include <atomic>
#include <cstdint>

namespace anomaly {
namespace {

using RtlLookupFunctionEntryFn = PRUNTIME_FUNCTION(NTAPI*)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);
using RtlVirtualUnwindFn = PEXCEPTION_ROUTINE(NTAPI*)(
    ULONG, DWORD64, DWORD64, PRUNTIME_FUNCTION, PCONTEXT, PVOID*, PDWORD64, PUNWIND_HISTORY_TABLE);

std::atomic<unsigned long long> g_caught{};
void* g_handler{};
// The range of apphelp, resolved once: the guard only ever acts inside it.
std::uintptr_t g_apphelp_base{};
std::uintptr_t g_apphelp_end{};

// Answers the faulting query the way a missing database entry does: unwind the frame that faulted, put zero in
// the return register, and let the caller carry on.
[[nodiscard]] bool SkipFaultingQuery(PCONTEXT context) noexcept {
    const auto in_ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto lookup = reinterpret_cast<RtlLookupFunctionEntryFn>(
        GetProcAddress(in_ntdll, "RtlLookupFunctionEntry"));
    const auto unwind = reinterpret_cast<RtlVirtualUnwindFn>(
        GetProcAddress(in_ntdll, "RtlVirtualUnwind"));
    if (lookup != nullptr && unwind != nullptr) {
        DWORD64 image_base{};
        PRUNTIME_FUNCTION entry = lookup(context->Rip, &image_base, nullptr);
        if (entry != nullptr) {
            PVOID handler_data{};
            DWORD64 establisher_frame{};
            static_cast<void>(unwind(
                UNW_FLAG_NHANDLER, image_base, context->Rip, entry, context, &handler_data,
                &establisher_frame, nullptr));
            context->Rax = 0;
            return true;
        }
    }
    // No unwind information: the faulting call pushed its return address last, so stepping over it is exact.
    const auto* stack = reinterpret_cast<const std::uintptr_t*>(context->Rsp);
    if (stack == nullptr || IsBadReadPtr(stack, sizeof(std::uintptr_t))) return false;
    context->Rip = *stack;
    context->Rsp += sizeof(std::uintptr_t);
    context->Rax = 0;
    return true;
}

LONG CALLBACK GuardHandler(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto& record = *info->ExceptionRecord;
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const auto rip = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
    if (g_apphelp_base == 0 || rip < g_apphelp_base || rip >= g_apphelp_end) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Only the null dereference this exists for: a read of an address below the first page, which is what a
    // lookup that found nothing hands back.
    const std::uint64_t read = record.ExceptionInformation[1];
    if (read >= 0x10000) return EXCEPTION_CONTINUE_SEARCH;
    if (!SkipFaultingQuery(info->ContextRecord)) return EXCEPTION_CONTINUE_SEARCH;
    g_caught.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

ApphelpFaultGuardReport InstallApphelpFaultGuard() noexcept {
    ApphelpFaultGuardReport report;
    const HMODULE apphelp = GetModuleHandleW(L"apphelp.dll");
    if (apphelp == nullptr) return report;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(apphelp), &region, sizeof(region)) == 0) return report;
    g_apphelp_base = reinterpret_cast<std::uintptr_t>(region.AllocationBase);
    // Walk the allocation the image lives in to find its end; the view is one allocation.
    std::uintptr_t cursor = g_apphelp_base;
    for (;;) {
        MEMORY_BASIC_INFORMATION step{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &step, sizeof(step)) == 0) break;
        if (step.AllocationBase != region.AllocationBase) break;
        cursor = reinterpret_cast<std::uintptr_t>(step.BaseAddress) + step.RegionSize;
    }
    g_apphelp_end = cursor;
    if (g_handler == nullptr) {
        g_handler = AddVectoredExceptionHandler(1, &GuardHandler);
        if (g_handler == nullptr) return report;
    }
    report.installed = true;
    report.caught = g_caught.load(std::memory_order_relaxed);
    return report;
}

}  // namespace anomaly