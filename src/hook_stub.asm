; Windows x64 ABI。NativeHookContext 偏移由 C++ static_assert 校验。
.CODE
EXTERN HookDispatch:PROC
EXTERN g_activeDetours:QWORD
PUBLIC HookDetourEntry
PUBLIC NativeInvoke

HookDetourEntry PROC FRAME
    push r12
    .pushreg r12
    sub rsp, 144
    .allocstack 144
    .endprolog
    mov r12, rsp
    lock inc qword ptr [g_activeDetours]
    mov [r12+32+00h], rcx
    mov [r12+32+08h], rdx
    mov [r12+32+10h], r8
    mov [r12+32+18h], r9
    movsd qword ptr [r12+32+20h], xmm0
    movsd qword ptr [r12+32+28h], xmm1
    movsd qword ptr [r12+32+30h], xmm2
    movsd qword ptr [r12+32+38h], xmm3
    mov [r12+32+48h], rax
    lea rax, [r12+192] ; 入口 RSP + 40：第一个栈参数。
    mov [r12+32+40h], rax
    xor eax, eax
    mov [r12+32+50h], rax
    mov [r12+32+58h], rax
    mov [r12+32+60h], r11 ; stable trampoline slot in the per-method thunk
    mov [r12+32+68h], rax
    lea rcx, [r12+32]
    call HookDispatch
    mov r11, [r12+32+60h]
    test r11, r11
    jnz bypass_lua
    mov rax, [r12+32+50h]
    movsd xmm0, qword ptr [r12+32+58h]
    lock dec qword ptr [g_activeDetours]
    add rsp, 144
    pop r12
    ret
bypass_lua:
    ; Quarantine retains this DLL and every trampoline. Restore the original
    ; call frame and tail-call without Lua, metadata, or registry locks.
    mov rcx, [r12+32+00h]
    mov rdx, [r12+32+08h]
    mov r8, [r12+32+10h]
    mov r9, [r12+32+18h]
    movsd xmm0, qword ptr [r12+32+20h]
    movsd xmm1, qword ptr [r12+32+28h]
    movsd xmm2, qword ptr [r12+32+30h]
    movsd xmm3, qword ptr [r12+32+38h]
    lock dec qword ptr [g_activeDetours]
    add rsp, 144
    pop r12
    jmp qword ptr [r11] ; ModRM mod=00: valid Windows x64 tail epilogue
HookDetourEntry ENDP

; RCX=context，EDX=栈参数数量（最多 63）。前 32 字节为 shadow space。
NativeInvoke PROC FRAME
    push r12
    .pushreg r12
    sub rsp, 544
    .allocstack 544
    .endprolog
    mov r12, rcx
    mov r10, [r12+40h]
    xor eax, eax
copy_args:
    cmp eax, edx
    jae args_ready
    mov r11, [r10+rax*8]
    mov [rsp+32+rax*8], r11
    inc eax
    jmp copy_args
args_ready:
    mov rcx, [r12+00h]
    mov rdx, [r12+08h]
    mov r8, [r12+10h]
    mov r9, [r12+18h]
    movsd xmm0, qword ptr [r12+20h]
    movsd xmm1, qword ptr [r12+28h]
    movsd xmm2, qword ptr [r12+30h]
    movsd xmm3, qword ptr [r12+38h]
    call qword ptr [r12+60h]
    mov [r12+50h], rax
    movsd qword ptr [r12+58h], xmm0
    add rsp, 544
    pop r12
    ret
NativeInvoke ENDP
END
