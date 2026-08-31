/**
 * ============================================================
 * il2cpp_scheduler.h — Unity 主线程调度器
 * ============================================================
 * 管理 Lua 回调队列、tick 入口选择和 Unity 主线程识别。
 * 原生 tick Hook 的安装由 Il2CppHook 提供，调度策略不侵入用户 Hook API。
 * ============================================================
 */
#pragma once
#include "common.h"

struct lua_State;

namespace Il2CppScheduler
{
    // 将 Lua 函数加入主线程队列，首次调用会尝试安装默认 tick。
    bool Schedule(lua_State* L, int callbackIndex);

    // 使用精确 MethodInfo 替换当前 tick 入口。
    bool SetTick(const Il2CppMethod* method, Il2CppClass* klass);

    // 获取当前 tick 入口；未就绪时返回 false。
    bool GetTick(const Il2CppMethod*& method, Il2CppClass*& klass);

    bool IsReady();
    bool EnsureInstalled();

    // 由内部 tick Hook 调用；只在识别出的 Unity 主线程排空队列。
    void Drain(lua_State* L);

    // Lua VM 销毁前释放尚未执行的 registry 引用并重置状态。
    void Shutdown(lua_State* L);
}
