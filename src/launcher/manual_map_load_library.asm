; Remote dependency loader.
;
; A manual map resolves every import against a module that is actually loaded in the target, so a
; dependency the target does not already have has to be loaded into it first. Doing that through
; CreateRemoteThread is the same thing as injecting a thread, which is exactly what a running game may
; refuse; this stub is therefore run through the same deliveries as the bootstrap itself.
;
; It records the module handle LoadLibraryW produced and the error it left behind, so a failure can be
; reported with the real result instead of a truncated thread exit code.
;
; RemoteLoadLibraryContext offsets are asserted in manual_map.cpp.

PUBLIC AnomalyRemoteLoadLibraryBegin
PUBLIC AnomalyRemoteLoadLibraryEnd

.code

AnomalyRemoteLoadLibraryBegin:
    push rbx
    sub rsp, 20h
    mov rbx, rcx

    mov qword ptr [rbx + 24], 0
    mov dword ptr [rbx + 32], 0
    mov dword ptr [rbx + 40], 0

    ; One owner only: an APC is queued to every thread and several threads may be redirected, and the
    ; caller waits for the stamp this path sets.
    mov eax, 1
    lock xchg dword ptr [rbx + 36], eax
    test eax, eax
    jz load_library_call
    jmp load_library_done

load_library_call:
    mov rcx, qword ptr [rbx + 0]
    call qword ptr [rbx + 8]
    mov qword ptr [rbx + 24], rax
    ; GetLastError has to follow the call immediately: anything else in between can overwrite it.
    call qword ptr [rbx + 16]
    mov dword ptr [rbx + 40], eax
    mov dword ptr [rbx + 32], 444F4E45h

load_library_done:
    add rsp, 20h
    pop rbx
    ret

AnomalyRemoteLoadLibraryEnd:

END
