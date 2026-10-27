; Remote thread-hijack trampoline.
;
; This code is copied into the target process and entered by rewriting the instruction pointer of a thread
; that is parked in one of ntdll's system call stubs. The thread resumes the stub afterwards as if nothing
; had happened, so everything the stub had live -- the argument registers, the flags, the vector registers
; -- is saved here and restored before the return.
;
; Two 64-bit slots inside this block are patched by manual_map.cpp before the block is written, because
; neither value can be known when it is assembled: the address of the state block, and the instruction
; pointer to resume at. The state block itself is writable and lives in the second page of the same
; reservation, so this page can be left execute-read.
;
; RemoteHijackState offsets and the slot separations are asserted in manual_map.cpp.

PUBLIC AnomalyRemoteHijackBegin
PUBLIC AnomalyRemoteHijackEnd
PUBLIC AnomalyRemoteHijackStateSlot
PUBLIC AnomalyRemoteHijackResumeSlot

.code

AnomalyRemoteHijackBegin:
    push qword ptr [AnomalyRemoteHijackResumeSlot]
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbx

    mov rbx, qword ptr [AnomalyRemoteHijackStateSlot]
    mov qword ptr [rbx + 16], rsp

    movups xmmword ptr [rbx + 24], xmm0
    movups xmmword ptr [rbx + 40], xmm1
    movups xmmword ptr [rbx + 56], xmm2
    movups xmmword ptr [rbx + 72], xmm3
    movups xmmword ptr [rbx + 88], xmm4
    movups xmmword ptr [rbx + 104], xmm5

    ; The payload is an ordinary function: it wants a 16-byte aligned stack with 32 bytes of shadow space,
    ; and it returns to this trampoline.
    mov rsp, qword ptr [rbx + 16]
    and rsp, -16
    sub rsp, 32
    mov rcx, qword ptr [rbx + 8]
    call qword ptr [rbx]

    movups xmm0, xmmword ptr [rbx + 24]
    movups xmm1, xmmword ptr [rbx + 40]
    movups xmm2, xmmword ptr [rbx + 56]
    movups xmm3, xmmword ptr [rbx + 72]
    movups xmm4, xmmword ptr [rbx + 88]
    movups xmm5, xmmword ptr [rbx + 104]

    mov rsp, qword ptr [rbx + 16]
    pop rbx
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    popfq
    ret

ALIGN 8
AnomalyRemoteHijackStateSlot:
    dq 0
AnomalyRemoteHijackResumeSlot:
    dq 0
AnomalyRemoteHijackEnd:

END
