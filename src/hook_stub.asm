; ============================================================
; hook_stub.asm - IL2CPP Hook 共享跳板（x64 MASM）
; ============================================================
; 所有被 Hook 的方法入口都会先跳到各自的 thunk:
;   mov eax, <hookId>
;   jmp qword ptr [rip+0]  -> HookDetourEntry
; 因此进入 HookDetourEntry 时:
;   RAX          = hookId
;   RCX/RDX/R8/R9 = 前 4 个整数参数
;   XMM0-XMM3    = 前 4 个浮点参数
;   [RSP+40]     = 第一个栈参数
;
; 流程:
;   1. 保存全部易失寄存器到 NativeHookContext
;   2. 调用 C++ 分发器 HookDispatch(ctx)
;   3. 若 ctx.original != NULL -> 恢复寄存器并跳转原始 trampoline
;   4. 否则按 ctx.resultInt / ctx.resultFloat 返回
;
; NativeHookContext 布局必须与 il2cpp_hook.cpp 完全一致
; ============================================================

; 上下文在帧内的起始偏移（前 32 字节留给被调函数的影子空间）
; 下面的每个字段偏移 = 32 + NativeHookContext 字段偏移
; （ml64 不允许两个 EQU 符号相加 因此直接折叠为字面量）
HookCtx_rcx        EQU 32 + 00h
HookCtx_rdx        EQU 32 + 08h
HookCtx_r8         EQU 32 + 10h
HookCtx_r9         EQU 32 + 18h
HookCtx_xmm0       EQU 32 + 20h
HookCtx_xmm1       EQU 32 + 28h
HookCtx_xmm2       EQU 32 + 30h
HookCtx_xmm3       EQU 32 + 38h
HookCtx_stackArgs  EQU 32 + 40h
HookCtx_hookId     EQU 32 + 48h
HookCtx_resultInt  EQU 32 + 50h
HookCtx_resultFlt  EQU 32 + 58h
HookCtx_original   EQU 32 + 60h
HookCtx_hasResult  EQU 32 + 68h
HookCtx_reserved   EQU 32 + 6ch

; 上下文实际大小 112 字节 + 32 字节影子空间 = 144（16 对齐）
HookFrame_Size     EQU 144

.CODE

EXTERN HookDispatch:PROC
; C++ 将该入口视为不透明代码地址，显式公开名称供链接器解析。
PUBLIC HookDetourEntry

; ============================================================
; HookDetourEntry - 所有 Hook 的公共入口
; ============================================================
HookDetourEntry PROC FRAME
    push rbp
    .pushreg rbp
    mov rbp, rsp
    .setframe rbp, 0
    sub rsp, HookFrame_Size
    .allocstack HookFrame_Size
    .endprolog

    ; ---- 保存整数参数寄存器 ----
    mov qword ptr [rsp + HookCtx_rcx], rcx
    mov qword ptr [rsp + HookCtx_rdx], rdx
    mov qword ptr [rsp + HookCtx_r8], r8
    mov qword ptr [rsp + HookCtx_r9], r9

    ; ---- 保存浮点参数寄存器（原始 64 位 低 32 位为 float）----
    movsd qword ptr [rsp + HookCtx_xmm0], xmm0
    movsd qword ptr [rsp + HookCtx_xmm1], xmm1
    movsd qword ptr [rsp + HookCtx_xmm2], xmm2
    movsd qword ptr [rsp + HookCtx_xmm3], xmm3

    ; ---- thunk 通过 RAX 传递 hookId ----
    mov qword ptr [rsp + HookCtx_hookId], rax

    ; ---- 初始化输出字段 ----
    mov qword ptr [rsp + HookCtx_resultInt], 0
    mov qword ptr [rsp + HookCtx_resultFlt], 0
    mov qword ptr [rsp + HookCtx_original], 0
    mov dword ptr [rsp + HookCtx_hasResult], 0

    ; ---- 计算原始栈参数指针 ----
    ; 入口时: [rsp] = 返回地址, [rsp+8..40] = 影子空间, [rsp+40] = 第一个栈参数
    ; 经过 push rbp 后: 入口 rsp = rbp + 8
    lea rax, [rbp + 48]
    mov qword ptr [rsp + HookCtx_stackArgs], rax

    ; ---- 调用 C++ 分发器 ----
    lea rcx, [rsp + 32]            ; 第一个参数: &ctx
    call HookDispatch

    ; ---- 检查是否需要跳转原始函数 ----
    cmp qword ptr [rsp + HookCtx_original], 0
    jne jump_original

    ; ---- 正常返回: 恢复输出寄存器 ----
    mov rax, qword ptr [rsp + HookCtx_resultInt]
    movsd xmm0, qword ptr [rsp + HookCtx_resultFlt]
    mov rsp, rbp
    pop rbp
    ret

jump_original:
    ; ---- 恢复入口寄存器并跳转原始 trampoline ----
    ; 原始栈参数与影子空间从未被修改 直接跳转等价于调用原函数
    mov rcx, qword ptr [rsp + HookCtx_rcx]
    mov rdx, qword ptr [rsp + HookCtx_rdx]
    mov r8, qword ptr [rsp + HookCtx_r8]
    mov r9, qword ptr [rsp + HookCtx_r9]
    movsd xmm0, qword ptr [rsp + HookCtx_xmm0]
    movsd xmm1, qword ptr [rsp + HookCtx_xmm1]
    movsd xmm2, qword ptr [rsp + HookCtx_xmm2]
    movsd xmm3, qword ptr [rsp + HookCtx_xmm3]
    mov rax, qword ptr [rsp + HookCtx_original]
    mov rsp, rbp
    pop rbp
    jmp rax

HookDetourEntry ENDP

END
