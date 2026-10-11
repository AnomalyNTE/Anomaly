// AnomalyDriver.sys -- ring-0 mapping primitive driver for the Anomaly kernel-mapper
// test fixture.
//
// What it does, and deliberately nothing more:
//   * PsSetCreateProcessNotifyRoutine (non-Ex: the Ex form's signature
//     requirements exceed what an audit-allowed load satisfies) records
//     creation of the one armed executable name, so capture needs no
//     user-mode process scanning.
//   * Stateless IOCTLs: modules / read / write / alloc / protect / user-APC
//     queue / freeze / drain. Each request opens the target through
//     PsLookup* + ObOpenObjectByPointer with OBJ_KERNEL_HANDLE, performs one
//     Zw* operation with PreviousMode=KernelMode, and closes the handle before
//     returning. The driver never keeps a target handle across requests, so a
//     handle-table snapshot of the game taken between IOCTLs attributes
//     nothing to any user-mode process -- including this one.
//
// Load: sc create AnomalyDriver type= kernel start= demand binPath= <this.sys>
#include "anomaly/kernel-mapper/akr_ioctl.h"

#include <ntifs.h>
#include <ntddk.h>
#include <wdmsec.h>

// APC queuing exports the WDK headers omit (present in ntoskrnl.exe and
// ntoskrnl.lib). Signatures from the documented KeInitializeApc contract.
typedef VOID(NTAPI* AKR_NORMAL_ROUTINE)(
    PVOID NormalContext, PVOID SystemArgument1, PVOID SystemArgument2);
typedef VOID(NTAPI* AKR_KERNEL_ROUTINE)(
    PKAPC Apc, AKR_NORMAL_ROUTINE* NormalRoutine, PVOID* NormalContext,
    PVOID* SystemArgument1, PVOID* SystemArgument2);
typedef VOID(NTAPI* AKR_RUNDOWN_ROUTINE)(PKAPC Apc);
extern "C" {
VOID NTAPI KeInitializeApc(
    PKAPC Apc, PETHREAD Thread, UCHAR ApcStateIndex,
    AKR_KERNEL_ROUTINE KernelRoutine, AKR_RUNDOWN_ROUTINE RundownRoutine,
    AKR_NORMAL_ROUTINE NormalRoutine, KPROCESSOR_MODE ApcMode,
    PVOID NormalContext);
BOOLEAN NTAPI KeInsertQueueApc(
    PKAPC Apc, PVOID SystemArgument1, PVOID SystemArgument2,
    KPRIORITY Increment);
}

// ZwAlertThread is exported by ntoskrnl but absent from the WDK's stale
// ntifs.h declarations: it forces a thread out of an alertable-capable wait
// and into APC delivery (or, if exiting, APC rundown). Declared up front:
// the force-drain below calls it before the memory-export block further down.
extern "C" NTSTATUS NTAPI ZwAlertThread(HANDLE ThreadHandle);

// Outstanding queued user APCs. A queued KAPC carries KernelRoutine and
// RundownRoutine pointers into this driver image: unloading while any are
// still queued leaves those pointers dangling, and the next delivery (or
// rundown -- which a thread exit triggers, even hours later) jumps into
// unmapped memory -- bugcheck 0xCE. The count rises in AkrHandleQueueApc when
// an APC is queued and falls in whichever routine releases it. Waiting for
// threads to become alertable is not enough (a thread that never waits holds
// its APC until it exits), so each queued APC also keeps the target thread
// REFERENCED; the force-drain IOCTL alerts every referenced thread, which
// forces immediate delivery or rundown of its queued APCs.
static volatile LONG g_pending_apcs = 0;
#define AKR_MAX_TRACKED_THREADS 512
static PETHREAD g_apc_threads[AKR_MAX_TRACKED_THREADS];
static LONG g_apc_thread_count = 0;

// Tracks the thread a queued APC is attached to, so the force drain can
// alert it later. Silently gives up beyond the cap: force-drain then just
// misses some threads and the bounded unload spin remains the fallback.
static void AkrTrackApcThread(PETHREAD thread) {
    if (InterlockedIncrement(&g_apc_thread_count) > AKR_MAX_TRACKED_THREADS) {
        InterlockedDecrement(&g_apc_thread_count);
        return;
    }
    ObfReferenceObject(thread);
    const LONG index = g_apc_thread_count - 1;
    g_apc_threads[index] = thread;
}

// The kernel part of the queued APC: it runs at APC_LEVEL in the target
// thread before the user routine dispatch. The KAPC was pool-allocated for
// this single delivery, so freeing it here is the whole job.
static VOID NTAPI AkrApcKernelRoutine(
    PKAPC Apc, AKR_NORMAL_ROUTINE* NormalRoutine, PVOID* NormalContext,
    PVOID* SystemArgument1, PVOID* SystemArgument2) {
    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    ExFreePool(Apc);
    InterlockedDecrement(&g_pending_apcs);
}

// Same release for an APC that never delivered (thread exited first).
static VOID NTAPI AkrApcRundownRoutine(PKAPC Apc) {
    ExFreePool(Apc);
    InterlockedDecrement(&g_pending_apcs);
}

// APC drain probe: reports how many queued user APCs are still outstanding.
// The client polls this after the attach and only stops the service at zero;
// unload spins on it as the last line of defence.
static NTSTATUS AkrHandleApcDrain(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (output_length < sizeof(AkrApcDrainOut)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    AkrApcDrainOut* out =
        static_cast<AkrApcDrainOut*>(irp->AssociatedIrp.SystemBuffer);
    out->pending = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_pending_apcs, 0, 0));
    irp->IoStatus.Information = sizeof(AkrApcDrainOut);
    return STATUS_SUCCESS;
}

// Force drain: alerts every thread a queued APC is still attached to. An
// alerted thread delivers or runs down its APCs immediately regardless of
// what it was waiting on, so this converts the unbounded "wait for threads
// to become alertable" into an immediate sweep. Alerts are delivered
// asynchronously, so the caller still polls the drain count afterwards --
// but only for the alert propagation itself, not for the threads' goodwill.
static NTSTATUS AkrHandleApcForceDrain() {
    const LONG count = InterlockedCompareExchange(&g_apc_thread_count, 0, 0);
    for (LONG index = 0; index < count; ++index) {
        PETHREAD thread = g_apc_threads[index];
        if (thread == nullptr) continue;
        HANDLE handle = nullptr;
        NTSTATUS status = ObOpenObjectByPointer(
            thread, OBJ_KERNEL_HANDLE, nullptr, THREAD_ALERT,
            *PsThreadType, KernelMode, &handle);
        if (NT_SUCCESS(status)) {
            // ZwAlertThread wakes the thread out of any non-alertable wait
            // and forces APC delivery; a thread already exiting runs its APC
            // rundown instead. Either way the KAPC is released and the
            // pending count falls.
            ZwAlertThread(handle);
            ZwClose(handle);
        }
    }
    return STATUS_SUCCESS;
}

// Releases the tracked-thread references once every APC is settled: the
// kernel/rundown routines already dropped the pending count to zero, so the
// tracking array is stale by definition here.
static void AkrClearTrackedApcThreads() {
    const LONG count = InterlockedExchange(&g_apc_thread_count, 0);
    for (LONG index = 0; index < count; ++index) {
        PETHREAD thread = g_apc_threads[index];
        g_apc_threads[index] = nullptr;
        if (thread != nullptr) ObfDereferenceObject(thread);
    }
}

// Object-based kernel exports the WDK headers do not declare. The x64
// ntoskrnl.lib omits the handle-based Zw* thread/memory calls, but exposes
// these equivalents that operate on the object pointers directly -- which is
// also cleaner: the memory paths need no handles at all. Ps*ContextThread
// take an explicit PreviousMode; passing KernelMode skips the user-address
// probe for the kernel-stack CONTEXT record.
extern "C" {
NTSTATUS NTAPI MmCopyVirtualMemory(
    PEPROCESS SourceProcess, PVOID SourceAddress, PEPROCESS TargetProcess,
    PVOID TargetAddress, SIZE_T Size, KPROCESSOR_MODE PreviousMode,
    PSIZE_T ReturnSize);
NTSTATUS NTAPI PsSuspendProcess(PEPROCESS Process);
NTSTATUS NTAPI PsResumeProcess(PEPROCESS Process);
NTSTATUS NTAPI PsGetContextThread(
    PETHREAD Thread, PCONTEXT ThreadContext, KPROCESSOR_MODE PreviousMode);
// Declared in no header, but exported by ntoskrnl.lib.
NTSTATUS NTAPI ZwQueryInformationProcess(
    HANDLE, ULONG, PVOID, ULONG, PULONG);
NTSTATUS NTAPI ZwProtectVirtualMemory(
    HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);
}
#ifndef PROCESS_VM_OPERATION
#define PROCESS_VM_OPERATION 0x0008
#endif
#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE 0x0001
#endif

// ZwQueryInformationProcess(ProcessBasicInformation) result; declared locally
// because the WDK headers expose the call but not this shape.
typedef struct AkrProcessBasicInformation {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    KPRIORITY BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} AkrProcessBasicInformation;

// ---------------------------------------------------------------------------
// capture state

typedef struct AkrCaptureState {
    KSPIN_LOCK lock;
    UNICODE_STRING armed_name; // empty when disarmed
    WCHAR armed_buffer[AKR_MAX_NAME];
    AkrProcessEvent latest; // most recent match
} AkrCaptureState;

static AkrCaptureState g_capture;

static VOID AkrCreateProcessCallback(
    HANDLE parent_id, HANDLE process_id, BOOLEAN create) {
    UNREFERENCED_PARAMETER(parent_id);
    KIRQL irql;
    KeAcquireSpinLock(&g_capture.lock, &irql);
    const BOOLEAN armed = g_capture.armed_name.Length != 0;
    KeReleaseSpinLock(&g_capture.lock, irql);
    if (!armed) return;
    if (!create) {
        // Exit notification: only interesting while the same pid is the match.
        KeAcquireSpinLock(&g_capture.lock, &irql);
        if (g_capture.latest.pid == (ULONG)(ULONG_PTR)process_id) {
            g_capture.latest.is_new = 0;
        }
        KeReleaseSpinLock(&g_capture.lock, irql);
        return;
    }
    // The create notification runs at PASSIVE_LEVEL with the process object and
    // its mapped image already in place, so the image name is resolvable here.
    // The non-Ex registration is deliberate: the Ex form demands more of the
    // calling image's signature than an audit-allowed load satisfies.
    PEPROCESS process = nullptr;
    if (!NT_SUCCESS(PsLookupProcessByProcessId(process_id, &process))) return;
    PUNICODE_STRING image_name = nullptr;
    if (NT_SUCCESS(SeLocateProcessImageName(process, &image_name)) &&
        image_name != nullptr && image_name->Buffer != nullptr) {
        // Copy the armed needle under the lock so the comparison below reads a
        // stable copy even if a disarm races it.
        WCHAR needle_buffer[AKR_MAX_NAME];
        KeAcquireSpinLock(&g_capture.lock, &irql);
        const USHORT needle_length = g_capture.armed_name.Length;
        RtlCopyMemory(needle_buffer, g_capture.armed_buffer, sizeof(needle_buffer));
        KeReleaseSpinLock(&g_capture.lock, irql);

        // Suffix match on the full path, case-insensitive.
        if (needle_length != 0 && image_name->Length >= needle_length) {
            const USHORT chars = needle_length / sizeof(WCHAR);
            const USHORT start =
                (image_name->Length - needle_length) / sizeof(WCHAR);
            BOOLEAN match = TRUE;
            for (USHORT index = 0; index < chars && match; ++index) {
                match = RtlUpcaseUnicodeChar(needle_buffer[index]) ==
                    RtlUpcaseUnicodeChar(image_name->Buffer[start + index]);
            }
            if (match) {
                KeAcquireSpinLock(&g_capture.lock, &irql);
                RtlZeroMemory(&g_capture.latest, sizeof(g_capture.latest));
                g_capture.latest.pid = (ULONG)(ULONG_PTR)process_id;
                g_capture.latest.is_new = 1;
                USHORT copy = image_name->Length;
                if (copy > sizeof(g_capture.latest.name) - sizeof(WCHAR)) {
                    copy = sizeof(g_capture.latest.name) - sizeof(WCHAR);
                }
                RtlCopyMemory(g_capture.latest.name, image_name->Buffer, copy);
                KeReleaseSpinLock(&g_capture.lock, irql);
                DbgPrint(
                    "AnomalyDriver: captured pid=%lu\n",
                    (ULONG)(ULONG_PTR)process_id);
            }
        }
        ExFreePool(image_name);
    }
    ObDereferenceObject(process);
}

// ---------------------------------------------------------------------------
// target handles: open by id, use, close. never cached.

static NTSTATUS AkrOpenProcess(ACCESS_MASK access, ULONG pid, PHANDLE out) {
    PEPROCESS process = nullptr;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &process);
    if (!NT_SUCCESS(status)) return status;
    status = ObOpenObjectByPointer(
        process, OBJ_KERNEL_HANDLE, nullptr, access, *PsProcessType, KernelMode,
        out);
    ObDereferenceObject(process);
    return status;
}

// ---------------------------------------------------------------------------
// PEB module walk (the only operation that needs KeStackAttachProcess)

// Minimal layouts for the user-mode structures walked here; the kernel headers do
// not expose PEB/LDR for arbitrary target processes.
typedef struct AkrPebLdrData {
    ULONG Length;
    UCHAR Initialized;
    PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleLinks; // offset 0x10 on x64
} AkrPebLdrData;

typedef struct AkrLdrEntry {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;  // offset 0x38
    UNICODE_STRING BaseDllName;  // offset 0x48
} AkrLdrEntry;

typedef struct AkrPeb {
    UCHAR InheritedAddressSpace;
    UCHAR ReadImageFileExecOptions;
    UCHAR BeingDebugged;
    UCHAR BitField;
    PVOID Mutant;
    PVOID ImageBaseAddress;
    AkrPebLdrData* Ldr;  // offset 0x18
} AkrPeb;

static NTSTATUS AkrCollectModules(
    HANDLE process_handle, PEPROCESS process, AkrModuleInfo* modules,
    ULONG capacity, ULONG* count, ULONG* truncated) {
    AkrProcessBasicInformation basic{};
    NTSTATUS status = ZwQueryInformationProcess(
        process_handle, 0 /* ProcessBasicInformation */, &basic, sizeof(basic),
        nullptr);
    if (!NT_SUCCESS(status)) return status;
    AkrPeb* peb = static_cast<AkrPeb*>(basic.PebBaseAddress);
    if (peb == nullptr) return STATUS_INVALID_ADDRESS;

    KAPC_STATE apc;
    KeStackAttachProcess(process, &apc);
    ULONG written = 0;
    ULONG overflowed = 0;
    status = STATUS_SUCCESS;
    __try {
        if (peb->Ldr == nullptr) {
            status = STATUS_DLL_NOT_FOUND;
        } else {
            LIST_ENTRY* head = &peb->Ldr->InLoadOrderModuleLinks;
            for (LIST_ENTRY* link = head->Flink; link != head; link = link->Flink) {
                AkrLdrEntry* entry =
                    CONTAINING_RECORD(link, AkrLdrEntry, InLoadOrderLinks);
                if (entry->DllBase == nullptr || entry->SizeOfImage == 0) continue;
                if (written == capacity) {
                    overflowed = 1;
                    break;
                }
                AkrModuleInfo* slot = &modules[written++];
                slot->base = reinterpret_cast<uint64_t>(entry->DllBase);
                slot->size = entry->SizeOfImage;
                RtlZeroMemory(slot->name, sizeof(slot->name));
                USHORT copy = entry->BaseDllName.Length;
                if (copy > sizeof(slot->name) - sizeof(WCHAR)) {
                    copy = sizeof(slot->name) - sizeof(WCHAR);
                }
                if (entry->BaseDllName.Buffer != nullptr) {
                    RtlCopyMemory(slot->name, entry->BaseDllName.Buffer, copy);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_ACCESS_VIOLATION;
    }
    KeUnstackDetachProcess(&apc);
    *count = written;
    *truncated = overflowed;
    return status;
}

// ---------------------------------------------------------------------------
// IOCTL handlers

static NTSTATUS AkrHandleArmCapture(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    if (input_length < sizeof(AkrArmCaptureIn)) return STATUS_BUFFER_TOO_SMALL;
    const AkrArmCaptureIn* in =
        static_cast<const AkrArmCaptureIn*>(irp->AssociatedIrp.SystemBuffer);
    if (in->name_length > AKR_MAX_NAME - 1) return STATUS_INVALID_PARAMETER;

    KIRQL irql;
    KeAcquireSpinLock(&g_capture.lock, &irql);
    RtlZeroMemory(g_capture.armed_buffer, sizeof(g_capture.armed_buffer));
    RtlCopyMemory(
        g_capture.armed_buffer, in->name, in->name_length * sizeof(WCHAR));
    if (in->name_length == 0) {
        RtlZeroMemory(&g_capture.armed_name, sizeof(g_capture.armed_name));
    } else {
        g_capture.armed_name.Buffer = g_capture.armed_buffer;
        g_capture.armed_name.Length =
            static_cast<USHORT>(in->name_length * sizeof(WCHAR));
        g_capture.armed_name.MaximumLength = sizeof(g_capture.armed_buffer);
    }
    RtlZeroMemory(&g_capture.latest, sizeof(g_capture.latest));
    KeReleaseSpinLock(&g_capture.lock, irql);
    DbgPrint("AnomalyDriver: capture armed for %ws\n", g_capture.armed_buffer);
    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS AkrHandleGetEvent(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (output_length < sizeof(AkrProcessEvent)) return STATUS_BUFFER_TOO_SMALL;
    KIRQL irql;
    KeAcquireSpinLock(&g_capture.lock, &irql);
    *static_cast<AkrProcessEvent*>(irp->AssociatedIrp.SystemBuffer) =
        g_capture.latest;
    KeReleaseSpinLock(&g_capture.lock, irql);
    irp->IoStatus.Information = sizeof(AkrProcessEvent);
    return STATUS_SUCCESS;
}

// Hot attach: adopt an already-running pid as the target. The lookup is
// validated here so a typo'd pid fails the IOCTL instead of every later
// operation; the create callback stays disarmed, so nothing races this.
static NTSTATUS AkrHandleSetTarget(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    if (input_length < sizeof(AkrSetTargetIn)) return STATUS_BUFFER_TOO_SMALL;
    const AkrSetTargetIn* in =
        static_cast<const AkrSetTargetIn*>(irp->AssociatedIrp.SystemBuffer);
    if (in->pid == 0) return STATUS_INVALID_PARAMETER;

    PEPROCESS process = nullptr;
    NTSTATUS status = PsLookupProcessByProcessId(
        (HANDLE)(ULONG_PTR)in->pid, &process);
    if (!NT_SUCCESS(status)) return status;

    KIRQL irql;
    KeAcquireSpinLock(&g_capture.lock, &irql);
    RtlZeroMemory(&g_capture.latest, sizeof(g_capture.latest));
    g_capture.latest.pid = in->pid;
    g_capture.latest.is_new = 1;
    KeReleaseSpinLock(&g_capture.lock, irql);
    ObDereferenceObject(process);
    DbgPrint("AnomalyDriver: target set to pid=%lu\n", in->pid);
    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

// Process freeze: PsSuspendProcess stops every existing thread in the target
// -- anti-cheat watchdogs included -- so the mapping session's kernel I/O runs
// against a target whose own threads are all parked. The object reference is
// taken and dropped here; nothing persists between IOCTLs.
static NTSTATUS AkrHandleFreeze(
    PIRP irp, PIO_STACK_LOCATION stack, BOOLEAN suspend) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    if (input_length < sizeof(AkrFreezeIn)) return STATUS_BUFFER_TOO_SMALL;
    const AkrFreezeIn* in =
        static_cast<const AkrFreezeIn*>(irp->AssociatedIrp.SystemBuffer);
    if (in->pid == 0) return STATUS_INVALID_PARAMETER;

    PEPROCESS process = nullptr;
    NTSTATUS status = PsLookupProcessByProcessId(
        (HANDLE)(ULONG_PTR)in->pid, &process);
    if (!NT_SUCCESS(status)) return status;
    status = suspend ? PsSuspendProcess(process) : PsResumeProcess(process);
    ObDereferenceObject(process);
    DbgPrint(
        "AnomalyDriver: pid=%lu %hs -> 0x%x\n", in->pid,
        suspend ? "freeze" : "resume", status);
    irp->IoStatus.Information = 0;
    return status;
}

static NTSTATUS AkrHandleModules(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (input_length < sizeof(AkrModulesIn) ||
        output_length < sizeof(AkrModulesOutHeader) + sizeof(AkrModuleInfo)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    const AkrModulesIn* in =
        static_cast<const AkrModulesIn*>(irp->AssociatedIrp.SystemBuffer);
    const ULONG capacity =
        (output_length - sizeof(AkrModulesOutHeader)) / sizeof(AkrModuleInfo);

    HANDLE process = nullptr;
    NTSTATUS status = AkrOpenProcess(PROCESS_ALL_ACCESS, in->pid, &process);
    if (!NT_SUCCESS(status)) return status;
    PEPROCESS process_object = nullptr;
    status = ObReferenceObjectByHandle(
        process, 0, nullptr, KernelMode,
        reinterpret_cast<PVOID*>(&process_object), nullptr);
    if (NT_SUCCESS(status)) {
        AkrModulesOutHeader* out =
            static_cast<AkrModulesOutHeader*>(irp->AssociatedIrp.SystemBuffer);
        AkrModuleInfo* modules = reinterpret_cast<AkrModuleInfo*>(out + 1);
        ULONG count = 0;
        ULONG truncated = 0;
        status = AkrCollectModules(
            process, process_object, modules, capacity, &count, &truncated);
        out->count = count;
        out->truncated = truncated;
        ObDereferenceObject(process_object);
        if (NT_SUCCESS(status)) {
            irp->IoStatus.Information =
                sizeof(AkrModulesOutHeader) + out->count * sizeof(AkrModuleInfo);
        }
    }
    ZwClose(process);
    return status;
}

static NTSTATUS AkrHandleRead(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (input_length < sizeof(AkrReadWriteIn)) return STATUS_BUFFER_TOO_SMALL;
    const AkrReadWriteIn* in =
        static_cast<const AkrReadWriteIn*>(irp->AssociatedIrp.SystemBuffer);
    if (in->size == 0 || in->size > output_length) return STATUS_INVALID_PARAMETER;

    PEPROCESS process = nullptr;
    NTSTATUS status = PsLookupProcessByProcessId(
        (HANDLE)(ULONG_PTR)in->pid, &process);
    if (!NT_SUCCESS(status)) return status;
    SIZE_T returned = 0;
    status = MmCopyVirtualMemory(
        process, reinterpret_cast<PVOID>(in->address), PsGetCurrentProcess(),
        irp->AssociatedIrp.SystemBuffer, in->size, KernelMode, &returned);
    ObDereferenceObject(process);
    if (!NT_SUCCESS(status)) return status;
    irp->IoStatus.Information = returned;
    return STATUS_SUCCESS;
}

static NTSTATUS AkrHandleWrite(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    if (input_length < sizeof(AkrReadWriteIn)) return STATUS_BUFFER_TOO_SMALL;
    const AkrReadWriteIn* in =
        static_cast<const AkrReadWriteIn*>(irp->AssociatedIrp.SystemBuffer);
    if (in->size == 0 || in->size > input_length - sizeof(AkrReadWriteIn)) {
        return STATUS_INVALID_PARAMETER;
    }
    const UCHAR* payload =
        static_cast<const UCHAR*>(irp->AssociatedIrp.SystemBuffer) +
        sizeof(AkrReadWriteIn);

    PEPROCESS process = nullptr;
    NTSTATUS status = PsLookupProcessByProcessId(
        (HANDLE)(ULONG_PTR)in->pid, &process);
    if (!NT_SUCCESS(status)) return status;
    SIZE_T returned = 0;
    status = MmCopyVirtualMemory(
        PsGetCurrentProcess(),
        const_cast<PVOID>(static_cast<const void*>(payload)), process,
        reinterpret_cast<PVOID>(in->address), in->size, KernelMode, &returned);
    ObDereferenceObject(process);
    if (!NT_SUCCESS(status)) return status;
    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS AkrHandleAlloc(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (input_length < sizeof(AkrAllocIn) || output_length < sizeof(AkrAllocOut)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    const AkrAllocIn* in =
        static_cast<const AkrAllocIn*>(irp->AssociatedIrp.SystemBuffer);

    HANDLE process = nullptr;
    NTSTATUS status = AkrOpenProcess(PROCESS_VM_OPERATION, in->pid, &process);
    if (!NT_SUCCESS(status)) return status;
    void* base = reinterpret_cast<void*>(in->hint);
    SIZE_T size = in->size;
    status = ZwAllocateVirtualMemory(
        process, &base, 0, &size, MEM_COMMIT | MEM_RESERVE, in->protect);
    ZwClose(process);
    if (!NT_SUCCESS(status)) return status;
    AkrAllocOut* out = static_cast<AkrAllocOut*>(irp->AssociatedIrp.SystemBuffer);
    out->address = reinterpret_cast<uint64_t>(base);
    irp->IoStatus.Information = sizeof(AkrAllocOut);
    DbgPrint(
        "AnomalyDriver: alloc pid=%lu hint=0x%llx -> 0x%llx size=0x%llx\n",
        in->pid, in->hint, out->address, static_cast<unsigned long long>(size));
    return STATUS_SUCCESS;
}

static NTSTATUS AkrHandleProtect(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (input_length < sizeof(AkrProtectIn) ||
        output_length < sizeof(AkrProtectOut)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    const AkrProtectIn* in =
        static_cast<const AkrProtectIn*>(irp->AssociatedIrp.SystemBuffer);

    HANDLE process = nullptr;
    NTSTATUS status = AkrOpenProcess(PROCESS_VM_OPERATION, in->pid, &process);
    if (!NT_SUCCESS(status)) return status;
    void* base = reinterpret_cast<void*>(in->address);
    SIZE_T size = in->size;
    ULONG previous = 0;
    status = ZwProtectVirtualMemory(process, &base, &size, in->protect, &previous);
    ZwClose(process);
    if (!NT_SUCCESS(status)) return status;
    AkrProtectOut* out =
        static_cast<AkrProtectOut*>(irp->AssociatedIrp.SystemBuffer);
    out->previous_protect = previous;
    irp->IoStatus.Information = sizeof(AkrProtectOut);
    return STATUS_SUCCESS;
}

// Scratch diagnostics: DbgPrint is invisible without a kernel debugger
// attached, so boot and test-ctx report their progress through the service
// key instead. Test-fixture only -- a product driver would never do this.
static VOID AkrDiag(PCWSTR value, ULONG number) {
    UNICODE_STRING key = RTL_CONSTANT_STRING(
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\AnomalyDriver");
    HANDLE handle = nullptr;
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(
        &attributes, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr,
        nullptr);
    if (!NT_SUCCESS(ZwOpenKey(&handle, KEY_SET_VALUE, &attributes))) return;
    UNICODE_STRING name{};
    RtlInitUnicodeString(&name, value);
    WCHAR buffer[64]{};
    UNICODE_STRING data{};
    RtlInitUnicodeString(&data, buffer);
    if (number != 0) {
        // Swprintf-free formatting: hex digits, fixed width.
        WCHAR digits[16]{};
        for (int i = 15; i >= 0; --i) {
            digits[i] = L"0123456789ABCDEF"[number & 0xF];
            number >>= 4;
        }
        for (int i = 0; i < 16; ++i) buffer[i] = digits[i];
        data.Length = 32;
    } else {
        data.Length = 0;
    }
    ZwSetValueKey(handle, &name, 0, REG_SZ, data.Buffer, data.Length + sizeof(WCHAR));
    ZwClose(handle);
}

// User-APC queue: queues the stub (rip) with its single argument (rcx) onto an
// EXISTING thread (the client enumerates the process's thread ids and sends
// one request per thread). No new thread is created (no LdrInitializeThunk on
// the path), no context is captured or patched (the kernel's APC dispatch
// saves and, through KiUserApcDispatcher -> NtContinue, restores the full user
// context), and no module byte is touched. Delivery happens when the thread
// next enters an alertable wait; queueing on many threads means any alertable
// one is enough, and the stub claims the context atomically so only the first
// delivery runs the entry.
static NTSTATUS AkrHandleQueueApc(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (input_length < sizeof(AkrHijackIn) ||
        output_length < sizeof(AkrHijackOut)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    const AkrHijackIn* in =
        static_cast<const AkrHijackIn*>(irp->AssociatedIrp.SystemBuffer);
    if (in->thread_id == 0) return STATUS_INVALID_PARAMETER;

    PETHREAD thread = nullptr;
    NTSTATUS status = PsLookupThreadByThreadId(
        (HANDLE)(ULONG_PTR)in->thread_id, &thread);
    if (!NT_SUCCESS(status)) return status;
    PKAPC apc = static_cast<PKAPC>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KAPC), 'kKrA'));
    if (apc == nullptr) {
        ObDereferenceObject(thread);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    // User APC: NormalRoutine (the stub) and NormalContext (its RCX) are
    // user-mode addresses; the RundownRoutine frees an APC whose thread exits
    // before delivery. ApcStateIndex 0 = OriginalApcEnvironment.
    KeInitializeApc(
        apc, thread, 0, AkrApcKernelRoutine, AkrApcRundownRoutine,
        reinterpret_cast<AKR_NORMAL_ROUTINE>(in->rip), UserMode,
        reinterpret_cast<PVOID>(in->rcx));
    if (KeInsertQueueApc(apc, nullptr, nullptr, 0)) {
        status = STATUS_SUCCESS;
        InterlockedIncrement(&g_pending_apcs);
        AkrTrackApcThread(thread);
    } else {
        // The thread is already terminating; the APC was never queued and
        // nobody will free it but us.
        ExFreePool(apc);
        status = STATUS_THREAD_IS_TERMINATING;
    }
    ObDereferenceObject(thread);
    DbgPrint(
        "AnomalyDriver: apc-queue tid=%lu rip=0x%llx -> 0x%x\n",
        in->thread_id, in->rip, status);
    AkrHijackOut* out =
        static_cast<AkrHijackOut*>(irp->AssociatedIrp.SystemBuffer);
    out->status = status;
    irp->IoStatus.Information = sizeof(AkrHijackOut);
    return STATUS_SUCCESS;
}

static NTSTATUS AkrHandleTerminate(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    if (input_length < sizeof(AkrTerminateIn)) return STATUS_BUFFER_TOO_SMALL;
    const AkrTerminateIn* in =
        static_cast<const AkrTerminateIn*>(irp->AssociatedIrp.SystemBuffer);

    HANDLE process = nullptr;
    NTSTATUS status = AkrOpenProcess(PROCESS_TERMINATE, in->pid, &process);
    if (!NT_SUCCESS(status)) return status;
    status = ZwTerminateProcess(process, 0);
    ZwClose(process);
    DbgPrint("AnomalyDriver: terminate pid=%lu -> 0x%x\n", in->pid, status);
    irp->IoStatus.Information = 0;
    return status;
}

static NTSTATUS AkrHandleTestCtx(PIRP irp, PIO_STACK_LOCATION stack) {
    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output_length =
        stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (input_length < sizeof(AkrTestCtxIn) ||
        output_length < sizeof(AkrTestCtxOut)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    const AkrTestCtxIn* in =
        static_cast<const AkrTestCtxIn*>(irp->AssociatedIrp.SystemBuffer);

    PETHREAD thread = nullptr;
    NTSTATUS status = PsLookupThreadByThreadId(
        (HANDLE)(ULONG_PTR)in->thread_id, &thread);
    AkrTestCtxOut* out =
        static_cast<AkrTestCtxOut*>(irp->AssociatedIrp.SystemBuffer);
    out->status = status;
    out->flags = 0;
    out->rip = 0;
    if (NT_SUCCESS(status)) {
        PEPROCESS process = nullptr;
        if (NT_SUCCESS(PsLookupProcessByProcessId(
                (HANDLE)(ULONG_PTR)in->pid, &process))) {
            // Same attach discipline as the module walk: PsGetContextThread
            // only accepts threads of the current process.
            KAPC_STATE apc_state{};
            KeStackAttachProcess(process, &apc_state);
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            status = PsGetContextThread(thread, &context, KernelMode);
            out->status = status;
            out->flags = context.ContextFlags;
            out->rip = context.Rip;
            KeUnstackDetachProcess(&apc_state);
            ObDereferenceObject(process);
        }
        ObDereferenceObject(thread);
    }
    AkrDiag(L"DiagTest", static_cast<ULONG>(out->status));
    DbgPrint(
        "AnomalyDriver: testctx tid=%lu -> 0x%x rip=0x%llx\n",
        in->thread_id, out->status, out->rip);
    irp->IoStatus.Information = sizeof(AkrTestCtxOut);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// dispatch / entry / unload

static NTSTATUS AkrDispatchCreate(PDEVICE_OBJECT device, PIRP irp) {
    UNREFERENCED_PARAMETER(device);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS AkrDispatchControl(PDEVICE_OBJECT device, PIRP irp) {
    UNREFERENCED_PARAMETER(device);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    const ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS status;
    switch (code) {
        case IOCTL_AKR_ARM_CAPTURE: status = AkrHandleArmCapture(irp, stack); break;
        case IOCTL_AKR_GET_EVENT: status = AkrHandleGetEvent(irp, stack); break;
        case IOCTL_AKR_MODULES: status = AkrHandleModules(irp, stack); break;
        case IOCTL_AKR_READ: status = AkrHandleRead(irp, stack); break;
        case IOCTL_AKR_WRITE: status = AkrHandleWrite(irp, stack); break;
        case IOCTL_AKR_ALLOC: status = AkrHandleAlloc(irp, stack); break;
        case IOCTL_AKR_PROTECT: status = AkrHandleProtect(irp, stack); break;
        case IOCTL_AKR_HIJACK: status = AkrHandleQueueApc(irp, stack); break;
        case IOCTL_AKR_TERMINATE: status = AkrHandleTerminate(irp, stack); break;
        case IOCTL_AKR_TESTCTX: status = AkrHandleTestCtx(irp, stack); break;
        case IOCTL_AKR_SET_TARGET: status = AkrHandleSetTarget(irp, stack); break;
        case IOCTL_AKR_FREEZE:
            status = AkrHandleFreeze(irp, stack, TRUE);
            break;
        case IOCTL_AKR_RESUME:
            status = AkrHandleFreeze(irp, stack, FALSE);
            break;
        case IOCTL_AKR_APC_DRAIN:
            status = AkrHandleApcDrain(irp, stack);
            break;
        case IOCTL_AKR_APC_FORCE_DRAIN:
            status = AkrHandleApcForceDrain();
            irp->IoStatus.Information = 0;
            break;
        default: status = STATUS_INVALID_DEVICE_REQUEST; break;
    }
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static VOID AkrUnload(PDRIVER_OBJECT driver) {
    PsSetCreateProcessNotifyRoutine(AkrCreateProcessCallback, TRUE);
    // A queued KAPC still holds KernelRoutine/RundownRoutine pointers into
    // this image, and a thread that exits later -- minutes or hours -- runs
    // the rundown against the unloaded driver (bugcheck 0xCE, seen twice).
    // Waiting is not enough: a thread that never enters an alertable wait
    // holds its APC forever. So unload first ALERTS every tracked thread,
    // which forces delivery or rundown right now, then spins only for the
    // alert propagation (bounded -- a wedged thread must not hang the SCM).
    if (InterlockedCompareExchange(&g_pending_apcs, 0, 0) != 0) {
        AkrHandleApcForceDrain();
        const ULONGLONG deadline =
            KeQueryInterruptTime() + 10ull * 1000ull * 1000ull * 10ull;
        while (InterlockedCompareExchange(&g_pending_apcs, 0, 0) != 0) {
            if (KeQueryInterruptTime() > deadline) break;
            LARGE_INTEGER interval{};
            interval.QuadPart = -100;  // 10us
            KeDelayExecutionThread(KernelMode, FALSE, &interval);
        }
    }
    AkrClearTrackedApcThreads();
    UNICODE_STRING link = RTL_CONSTANT_STRING(AKR_DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&link);
    if (driver->DeviceObject != nullptr) IoDeleteDevice(driver->DeviceObject);
    DbgPrint("AnomalyDriver: unloaded\n");
}

extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT driver, PUNICODE_STRING registry) {
    UNREFERENCED_PARAMETER(registry);
    RtlZeroMemory(&g_capture, sizeof(g_capture));
    KeInitializeSpinLock(&g_capture.lock);

    UNICODE_STRING device_name = RTL_CONSTANT_STRING(AKR_DEVICE_NAME);
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(AKR_DEVICE_SDDL);
    PDEVICE_OBJECT device = nullptr;
    NTSTATUS status = IoCreateDeviceSecure(
        driver, 0, &device_name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
        FALSE, &sddl, nullptr, &device);
    if (!NT_SUCCESS(status)) return status;

    UNICODE_STRING link = RTL_CONSTANT_STRING(AKR_DOS_DEVICE_NAME);
    status = IoCreateSymbolicLink(&link, &device_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        return status;
    }

    driver->MajorFunction[IRP_MJ_CREATE] = AkrDispatchCreate;
    driver->MajorFunction[IRP_MJ_CLOSE] = AkrDispatchCreate;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AkrDispatchControl;
    driver->DriverUnload = AkrUnload;

    status = PsSetCreateProcessNotifyRoutine(AkrCreateProcessCallback, FALSE);
    if (!NT_SUCCESS(status)) {
        IoDeleteSymbolicLink(&link);
        IoDeleteDevice(device);
        return status;
    }
    DbgPrint("AnomalyDriver: loaded\n");
    AkrDiag(L"DiagBoot", 0x1234);
    return STATUS_SUCCESS;
}
