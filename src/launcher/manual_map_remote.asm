PUBLIC AnomalyRemoteBootstrapBegin
PUBLIC AnomalyRemoteBootstrapEnd

.code

; RemoteBootstrapContext offsets are asserted in manual_map.cpp.
AnomalyRemoteBootstrapBegin:
    push rbx
    push rsi
    sub rsp, 28h
    mov rbx, rcx

    mov dword ptr [rbx + 72], 0
    mov dword ptr [rbx + 76], 0
    mov dword ptr [rbx + 80], 0

    mov edx, dword ptr [rbx + 16]
    test edx, edx
    jz bootstrap_tls
    mov rcx, qword ptr [rbx + 8]
    mov r8, qword ptr [rbx + 0]
    call qword ptr [rbx + 24]
    test eax, eax
    jnz unwind_registered
    mov dword ptr [rbx + 72], 193
    jmp bootstrap_done

unwind_registered:
    mov dword ptr [rbx + 80], 1

bootstrap_tls:
    mov rsi, qword ptr [rbx + 40]
    test rsi, rsi
    jz bootstrap_entry

tls_loop:
    mov rax, qword ptr [rsi]
    test rax, rax
    jz bootstrap_entry
    mov rcx, qword ptr [rbx + 0]
    mov edx, 1
    xor r8d, r8d
    call rax
    add rsi, 8
    jmp tls_loop

bootstrap_entry:
    mov rax, qword ptr [rbx + 48]
    test rax, rax
    jz bootstrap_start
    mov rcx, qword ptr [rbx + 0]
    mov edx, 1
    xor r8d, r8d
    call rax
    test eax, eax
    jnz bootstrap_start
    mov dword ptr [rbx + 72], 1114
    jmp bootstrap_tls_detach

bootstrap_start:
    mov rcx, qword ptr [rbx + 64]
    call qword ptr [rbx + 56]
    mov dword ptr [rbx + 76], eax
    test eax, eax
    jz bootstrap_done
    mov dword ptr [rbx + 72], eax

    mov rax, qword ptr [rbx + 48]
    test rax, rax
    jz bootstrap_unregister
    mov rcx, qword ptr [rbx + 0]
    xor edx, edx
    xor r8d, r8d
    call rax

bootstrap_tls_detach:
    mov rsi, qword ptr [rbx + 40]
    test rsi, rsi
    jz bootstrap_unregister

tls_detach_loop:
    mov rax, qword ptr [rsi]
    test rax, rax
    jz bootstrap_unregister
    mov rcx, qword ptr [rbx + 0]
    xor edx, edx
    xor r8d, r8d
    call rax
    add rsi, 8
    jmp tls_detach_loop

bootstrap_unregister:
    cmp dword ptr [rbx + 80], 0
    je bootstrap_done
    mov rcx, qword ptr [rbx + 8]
    call qword ptr [rbx + 32]
    mov dword ptr [rbx + 80], 0

bootstrap_done:
    mov eax, dword ptr [rbx + 72]
    add rsp, 28h
    pop rsi
    pop rbx
    ret

AnomalyRemoteBootstrapEnd:

END
