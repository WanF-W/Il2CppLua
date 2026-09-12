// Windows x64 IL2CPP 方法 Hook。Lua API: method:hook(callback)、unhook、is_hooked。
// callback(this, original, ...) 的 this：静态为 Class，实例为 Instance，值类型为快照。
// original() 保留原调用参数，original(...) 替换声明参数；始终使用原生 this。
// ref/out、ref 返回和泛型方法 Hook 不支持，安装时拒绝。
// Hook 回调串行访问 Lua；Shutdown 必须在 LuaEngine::Shutdown 之前完成。
#pragma once
#include "common.h"

struct lua_State;

// HookMethod/UnhookMethod/UnhookAll 由调用方持有 LuaEngine 锁；最终 Shutdown 由工作线程独占执行。
namespace Il2CppHook
{
    bool HookMethod(lua_State* L, const Il2CppMethod* method, Il2CppClass* klass, int callbackIdx);
    // 普通卸载只禁用，保留 trampoline 直到 Shutdown，以保护在途调用。
    bool UnhookMethod(const Il2CppMethod* method);
    bool IsHooked(const Il2CppMethod* method);
    void UnhookAll();

    // Scheduler 后端；tick 身份与用户回调可以共存。
    bool InstallSchedulerTick(const Il2CppMethod* method, Il2CppClass* klass);
    bool IsSchedulerTickInstalled();
    bool InstallMainThreadProbe(const Il2CppMethod* method, Il2CppClass* klass);
    void Shutdown();
}
