; 伤害值放大 stub（MASM x64）
;
; 挂点：CharacterOnDamaged 的调用点之前那条取值指令，寄存器已摆好实参：
;   r14 = const FHTDamageEvent*（Damage 在 +0x10），rdi = victim
; 职责：把 [r14+0x10] 乘上倍率并写回，再跳到 trampoline 执行原始指令。
; 不动栈、不改 rcx/rdx/r8/r9，无需 UNWIND_INFO。

EXTERN g_original:QWORD
EXTERN g_scale_flag:DWORD
EXTERN g_scale_factor:REAL4
EXTERN g_scale_hits:QWORD
EXTERN g_seen_hits:QWORD
EXTERN g_last_victim:QWORD
EXTERN g_last_before:REAL4
EXTERN g_last_after:REAL4
EXTERN g_one:REAL4
EXTERN g_pending_victim:QWORD
EXTERN g_pending_extra:REAL4
EXTERN g_pending_flag:DWORD

.code

ScaleStub PROC
    test r14, r14
    jz   done
    inc  qword ptr [g_seen_hits]
    mov  qword ptr [g_last_victim], rdi
    cmp  dword ptr [g_scale_flag], 0
    je   done

    movss  xmm0, dword ptr [r14+10h]
    xorps  xmm1, xmm1
    comiss xmm0, xmm1
    jbe    done
    movss  xmm2, dword ptr [g_scale_factor]
    comiss xmm2, xmm1
    jbe    done

    movss  dword ptr [g_last_before], xmm0
    movaps xmm3, xmm0
    mulss  xmm3, xmm2
    movss  dword ptr [r14+10h], xmm3
    movss  dword ptr [g_last_after], xmm3
    inc    qword ptr [g_scale_hits]

    ; 待补扣量 = (倍率-1)*原始，交由主循环在下一帧写入 HP
    movaps xmm5, xmm2
    subss  xmm5, dword ptr [g_one]
    comiss xmm5, xmm1
    jbe    done
    mulss  xmm5, xmm0
    movss  dword ptr [g_pending_extra], xmm5
    mov    qword ptr [g_pending_victim], rdi
    mov    dword ptr [g_pending_flag], 1

done:
    jmp qword ptr [g_original]
ScaleStub ENDP

END
