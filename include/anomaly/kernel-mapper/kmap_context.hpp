// KmapContext layout shared with the APC bootstrap stub in apc_boot.asm.
// Offsets are hardcoded in the assembly; keep both in sync.
#ifndef KMAP_CONTEXT_HPP
#define KMAP_CONTEXT_HPP

#include <cstdint>

struct KmapContext {
    uint64_t image_base;       // 0
    uint64_t exception_table;  // 8   remote VA of .pdata
    uint32_t exception_count;  // 16
    uint32_t done;             // 20  1 = claimed, 2 = finished
    uint64_t rtl_add;          // 24  remote RtlAddFunctionTable
    uint64_t rtl_delete;       // 32  remote RtlDeleteFunctionTable
    uint64_t entry_point;      // 40  remote AnomalyStart / LoadLibraryW
    uint64_t start_info;       // 48  remote AnomalyStartInfo / path
    uint32_t result;           // 56  entry return code
    uint32_t bootstrap_error;  // 60
    uint64_t saved_rsp;        // 64  RSP holding the APC dispatcher's return
                               //     address, restored before the plain ret
    uint64_t core_dllmain;     // 72  remote core DllMain: CRT dynamic
                               //     initializers (g_runtime_control et al.)
                               //     run here, before AnomalyStart
    uint64_t tls_callbacks;    // 80  remote null-terminated callback array,
                               //     invoked ATTACH before DllMain, DETACH
                               //     only on failure
};                             // 88 bytes

#endif  // KMAP_CONTEXT_HPP
