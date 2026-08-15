/**
 * ============================================================
 * il2cpp_hook.h - IL2CPP 方法 Hook 模块
 * ============================================================
 * 基于 MinHook 实现 IL2CPP 方法级的原生 Hook
 * 接口参考 frida-il2cpp-bridge 的 method.implementation / method.revert
 *
 * Lua 接口（Method 元表）
 * 
 * ·mth:hook(function(this, original, ...) ... end) -- 替换方法实现
 * ·mth:unhook()                           -- 恢复原始实现
 * ·mth:hooked()                           -- 查询是否已 Hook
 * ·il2cpp.unhook_all()                    -- 卸载全部 Hook
 *
 * 回调签名与 frida-il2cpp-bridge 一致
 * 
 * ·实例方法: function(this, original, 参数1, 参数2, ...) ... return 返回值 end
 * ·静态方法: function(Class, original, 参数1, 参数2, ...) ... end
 * ·this 为 Instance userdata（值类型为装箱后的对象）
 * ·original 可在回调体内调用原方法:
 *   original()            -- 使用原始参数调用并返回原方法返回值
 *   original(参数1, ...)  -- 使用替换参数调用
 * ·回调的返回值会作为方法的返回值（void 方法忽略）
 *
 * 线程模型
 * 
 * ·Hook 回调可能发生在任意游戏线程
 * ·回调内部先附加 IL2CPP 线程 再获取 Lua 状态机互斥锁
 * ·LuaEngine 使用可重入互斥锁 支持回调内再次调用被 Hook 的方法
 * ·卸载时只禁用 Hook 不释放 trampoline 避免在途回调悬空
 *
 * 仅针对 Windows x64（MS x64 调用约定）
 * 假定 Unity 2018.3+（静态方法不再携带无用的 __this 参数）
 *
 * 已知限制
 * 
 * ·ref/out 参数目前只读 回调内修改不会写回
 * ·共享 methodPointer 的泛型实例化方法只能 Hook 其中一个 MethodInfo
 * ·回调内 Lua 状态机是单线程串行的 其他线程触发 Hook 会等待
 * ============================================================
 */
#pragma once
#include "common.h"

// Lua 状态机前置声明
struct lua_State;

// ============================================================
// Hook 模块公共 API
// ============================================================
namespace Il2CppHook
{
    /**
     * 安装（或替换）一个方法 Hook
     *
     * @param L          Lua 状态机
     * @param method      MethodInfo 指针
     * @param klass       声明类（Method userdata 中缓存）
     * @param callbackIdx Lua 栈上回调函数的位置
     * @return true 成功
     */
    bool HookMethod(lua_State* L, const Il2CppMethod* method, Il2CppClass* klass, int callbackIdx);

    /**
     * 卸载指定方法的 Hook
     * 只禁用 Hook 保留 trampoline 供再次安装复用
     */
    bool UnhookMethod(const Il2CppMethod* method);

    /**
     * 查询指定方法是否已 Hook
     */
    bool IsHooked(const Il2CppMethod* method);

    /**
     * 卸载全部 Hook（Lua 层 il2cpp.unhook_all 使用）
     */
    void UnhookAll();

    /**
     * 关闭模块
     * 必须在 LuaEngine::Shutdown 之前调用
     * 会禁用全部 Hook 并等待在途回调结束后释放资源
     */
    void Shutdown();
}
