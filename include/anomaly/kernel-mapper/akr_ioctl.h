// Shared IOCTL contract between AnomalyDriver.sys and the user-mode client.
// Kernel mapper test fixture: the driver performs every target-process operation
// (allocation, read, write, protection, user-APC queuing, termination) with
// PreviousMode=KernelMode through temporary kernel handles, so no user-mode
// process ever holds a handle to the game or appears in its attribution chain.
#ifndef AKR_IOCTL_H
#define AKR_IOCTL_H

#include <stdint.h>

#ifndef _KERNEL_MODE
#include <windows.h>
#include <winioctl.h>
#endif

#define AKR_DEVICE_NAME L"\\Device\\AnomalyDriver"
#define AKR_DOS_DEVICE_NAME L"\\DosDevices\\AnomalyDriver"
#define AKR_USER_DEVICE_NAME L"\\\\.\\AnomalyDriver"

// SDDL: administrators and local system only.
#define AKR_DEVICE_SDDL L"D:P(A;;GA;;;BA)(A;;GA;;;SY)"

#define AKR_MAX_NAME 64
#define AKR_MAX_MODULES 256

#define IOCTL_AKR_ARM_CAPTURE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_GET_EVENT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_MODULES CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_ALLOC CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_PROTECT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_HIJACK CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_TERMINATE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_TESTCTX CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_SET_TARGET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_FREEZE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_RESUME CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_APC_DRAIN CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80D, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AKR_APC_FORCE_DRAIN CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80E, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 8)

// --- capture ---------------------------------------------------------------

// in: wide executable name to watch for (e.g. L"FakeGame.exe"); empty disarms.
typedef struct AkrArmCaptureIn {
    uint32_t pid_ignore;  // reserved zero
    uint32_t name_length; // characters, without terminator
    wchar_t name[AKR_MAX_NAME];
} AkrArmCaptureIn;

// out: most recent creation event for the armed name; pid 0 = none recorded yet.
typedef struct AkrProcessEvent {
    uint32_t pid;
    uint32_t is_new; // 1 = created, 0 = terminated
    wchar_t name[AKR_MAX_NAME];
} AkrProcessEvent;

// --- modules ---------------------------------------------------------------

typedef struct AkrModuleInfo {
    uint64_t base;
    uint64_t size;
    wchar_t name[AKR_MAX_NAME];
} AkrModuleInfo;

typedef struct AkrModulesIn {
    uint32_t pid;
} AkrModulesIn;

typedef struct AkrModulesOutHeader {
    uint32_t count; // AKR_MAX_MODULES cap
    uint32_t truncated;
} AkrModulesOutHeader;  // AkrModuleInfo modules[] follow

// --- read / write / alloc / protect ----------------------------------------

typedef struct AkrReadWriteIn {
    uint32_t pid;
    uint32_t size;
    uint64_t address;
    // for write: `size` bytes of payload follow
} AkrReadWriteIn;

typedef struct AkrAllocIn {
    uint32_t pid;
    uint32_t protect;
    uint64_t size;
    uint64_t hint; // 0 = anywhere; otherwise preferred base (rounded to 64K)
} AkrAllocIn;

typedef struct AkrAllocOut {
    uint64_t address;
} AkrAllocOut;

typedef struct AkrProtectIn {
    uint32_t pid;
    uint32_t protect;
    uint64_t address;
    uint64_t size;
} AkrProtectIn;

typedef struct AkrProtectOut {
    uint32_t previous_protect;
} AkrProtectOut;

// --- user APC queue ---------------------------------------------------------

// Queues one user APC onto an existing thread: `rip` is the user-mode routine
// (the bootstrap stub) and `rcx` its single argument. Delivery happens when
// the thread next enters an alertable wait; the kernel's APC dispatch saves
// and restores the interrupted context, so the routine may clobber every
// register.
typedef struct AkrHijackIn {
    uint32_t thread_id;
    uint64_t rip;
    uint64_t rcx;
} AkrHijackIn;

// out: NTSTATUS of the queue attempt (e.g. STATUS_THREAD_IS_TERMINATING when
// the thread was already exiting and the APC was refused).
typedef struct AkrHijackOut {
    int32_t status;
} AkrHijackOut;

// --- terminate -------------------------------------------------------------

typedef struct AkrTerminateIn {
    uint32_t pid;
} AkrTerminateIn;

// --- hot attach ------------------------------------------------------------

// in: pid of an already-running process to adopt as the mapping target. The
// create-process callback stays disarmed; every pid-carrying IOCTL works on
// this target exactly as if the callback had captured it.
typedef struct AkrSetTargetIn {
    uint32_t pid;
} AkrSetTargetIn;

// --- process freeze ---------------------------------------------------------

// in: pid of the target. FREEZE calls PsSuspendProcess -- every existing
// thread stops (anti-cheat watchdogs included). RESUME is the matching
// PsResumeProcess. Both take a PEPROCESS object reference; no handle is kept.
typedef struct AkrFreezeIn {
    uint32_t pid;
} AkrFreezeIn;

// --- APC drain ---------------------------------------------------------------

// out: number of queued user APCs still pending delivery. A queued KAPC
// carries KernelRoutine/RundownRoutine pointers into the driver image, so the
// service must not be stopped until this reads zero -- unloading with pending
// APCs leaves those pointers dangling (bugcheck 0xCE). The client polls this
// after the attach finishes, before stopping the service.
typedef struct AkrApcDrainOut {
    uint32_t pending;
} AkrApcDrainOut;

// --- context self-test (fixture diagnostics) --------------------------------

// Runs only PsGetContextThread on the thread -- no redirect, no memory writes.
// Test fixture diagnostics: separates "the API call itself fails" from "the
// hijack sequence fails".
typedef struct AkrTestCtxIn {
    uint32_t pid;
    uint32_t thread_id;
} AkrTestCtxIn;

typedef struct AkrTestCtxOut {
    int32_t status;   // PsGetContextThread result
    uint32_t flags;   // ContextFlags echoed back
    uint64_t rip;     // captured RIP on success
} AkrTestCtxOut;

#pragma pack(pop)

#endif  // AKR_IOCTL_H
