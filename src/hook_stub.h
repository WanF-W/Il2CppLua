#pragma once

/**
 * hook_stub.h - C++ 与 x64 MASM Hook 跳板之间的 ABI 边界
 *
 * 跳板入口由 hook_stub.asm 实现，而不是由 C++ 源文件实现。这里把它声明为
 * 不透明的代码地址符号，避免 C++ 静态分析器错误地要求一个 C++ 函数体。
 */

#if !defined(_M_X64)
#error HookDetourEntry is implemented only for the Windows x64 ABI.
#endif

extern "C" unsigned char HookDetourEntry[];

// 业务代码只取得入口地址，不会按 C++ 调用约定直接调用该汇编跳板。
inline void* GetHookDetourAddress() noexcept
{
    return static_cast<void*>(HookDetourEntry);
}
