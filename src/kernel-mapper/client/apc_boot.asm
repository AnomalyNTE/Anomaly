; User-APC bootstrap for the kernel-mapper client. Queued onto EVERY thread
; of the target through KeInitializeApc/KeInsertQueueApc; delivery happens
; when a thread next enters an alertable wait. The kernel's
; KiUserApcDispatcher has already saved the interrupted thread's complete
; user context, and a plain ret returns to the dispatcher, which
; NtContinue()s that context -- every register, XMM included, is restored
; exactly. The stub may therefore clobber everything; only its own
; bookkeeping matters. Offsets mirror kmap_context.hpp.
;
; Because the same context is queued on many threads, the first APC to
; deliver claims it atomically (done: 0 -> 1 "claimed" -> 2 "finished");
; every later delivery sees a non-zero done and returns immediately.
;
; The sequence mirrors the product's remote bootstrap (manual_map_remote.asm)
; exactly: register unwind table -> TLS callbacks (ATTACH) -> core DllMain
; (DLL_PROCESS_ATTACH; the CRT's dynamic initializers -- g_runtime_control
; -- run here) -> AnomalyStart(start_info). Success keeps the unwind table
; registered; failure runs DllMain DETACH, TLS DETACH and unregisters.
.code

; Plain labels, deliberately NOT PROC: with /INCREMENTAL the linker routes
; references to FUNCTION symbols through ILT jump thunks (whose order in the
; thunk table is arbitrary), so &End - &Begin stops being the code range.
; manual_map_remote.asm marks its copy range the same way.
PUBLIC AkrApcBootstrapBegin
PUBLIC AkrApcBootstrapEnd

AkrApcBootstrapBegin:
    ; --- TLS gate: a thread without a loader-initialized TLS array must
    ; never run the sequence. Game/anti-cheat threads created with bare
    ; NtCreateThreadEx bypass ntdll's thread init, so TEB->
    ; ThreadLocalStoragePointer stays NULL and the first static-TLS access
    ; inside any CRT entry dereferences zero (observed live: d3dcompiler_47
    ; DllMain, _tls_index 66, AV reading [0x210] with rax=0). Such a
    ; delivery bows out WITHOUT claiming -- the APCs still queued on the
    ; other threads deliver on one of them instead.
    mov     rax, gs:[58h]
    test    rax, rax
    jz      done_nothing

    ; --- claim: only the first APC to deliver runs the sequence
    xor     eax, eax
    mov     r8d, 1
    lock cmpxchg dword ptr [rcx+20], r8d
    jnz     done_nothing

    ; Keep the dispatcher's return address reachable: [rsp] holds it on the
    ; entry stack, so save that RSP before realigning for our calls.
    mov     [rcx+64], rsp        ; saved_rsp slot (context-relative)
    mov     r12, rcx             ; r12 = context
    and     rsp, -16
    sub     rsp, 20h             ; shadow space
    mov     dword ptr [r12+60], 0    ; bootstrap_error = ERROR_SUCCESS
    xor     r15d, r15d               ; r15 = unwind table registered flag

    ; --- RtlAddFunctionTable(ExceptionTable, Count, ImageBase), when present
    mov     rcx, [r12+8]         ; exception_table
    test    rcx, rcx
    jz      tls_attach
    mov     edx, dword ptr [r12+16]
    mov     r8,  [r12+0]         ; image_base
    call    qword ptr [r12+24]   ; rtl_add
    test    eax, eax
    jz      add_failed
    mov     r15d, 1
    jmp     tls_attach
add_failed:
    mov     dword ptr [r12+60], 193  ; ERROR_BAD_EXE_FORMAT, product parity
    jmp     finish

    ; --- TLS callbacks, DLL_PROCESS_ATTACH
tls_attach:
    mov     rsi, [r12+80]        ; tls_callbacks array
    test    rsi, rsi
    jz      dllmain_attach
tls_attach_loop:
    mov     rax, [rsi]
    test    rax, rax
    jz      dllmain_attach
    mov     rcx, [r12+0]         ; image_base
    mov     edx, 1               ; DLL_PROCESS_ATTACH
    xor     r8d, r8d
    call    rax
    add     rsi, 8
    jmp     tls_attach_loop

    ; --- core DllMain(base, DLL_PROCESS_ATTACH, nullptr)
dllmain_attach:
    mov     rax, [r12+72]        ; core_dllmain
    test    rax, rax
    jz      anomaly_start
    mov     rcx, [r12+0]         ; image_base
    mov     edx, 1               ; DLL_PROCESS_ATTACH
    xor     r8d, r8d
    call    rax
    test    eax, eax
    jnz     anomaly_start
    mov     dword ptr [r12+60], 1114  ; ERROR_DLL_INIT_FAILED, product parity
    jmp     tls_detach

    ; --- AnomalyStart(start_info) -> result
anomaly_start:
    mov     rcx, [r12+48]        ; start_info
    call    qword ptr [r12+40]   ; entry_point = AnomalyStart
    mov     dword ptr [r12+56], eax
    test    eax, eax
    jz      finish               ; success: unwind table stays registered

    ; --- failure: DllMain DETACH, TLS DETACH, unregister, record error
    mov     dword ptr [r12+60], eax
    mov     rax, [r12+72]
    test    rax, rax
    jz      tls_detach
    mov     rcx, [r12+0]
    xor     edx, edx             ; DLL_PROCESS_DETACH
    xor     r8d, r8d
    call    rax

tls_detach:
    mov     rsi, [r12+80]
    test    rsi, rsi
    jz      unregister
tls_detach_loop:
    mov     rax, [rsi]
    test    rax, rax
    jz      unregister
    mov     rcx, [r12+0]
    xor     edx, edx             ; DLL_PROCESS_DETACH
    xor     r8d, r8d
    call    rax
    add     rsi, 8
    jmp     tls_detach_loop

unregister:
    test    r15d, r15d
    jz      finish
    mov     rcx, [r12+8]         ; exception_table
    call    qword ptr [r12+32]   ; rtl_delete
    mov     r15d, 0

finish:
    ; --- finished, then return to KiUserApcDispatcher -> NtContinue
    mov     dword ptr [r12+20], 2
    add     rsp, 20h
    mov     rsp, [r12+64]
    ret
done_nothing:
    ret

AkrApcBootstrapEnd:
    int     3                    ; never executed; marks the copy range end

END
