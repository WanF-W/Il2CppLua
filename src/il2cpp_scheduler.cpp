/**
 * il2cpp_scheduler.cpp — Unity 主线程调度器实现
 */
#include "il2cpp_scheduler.h"
#include "il2cpp_hook.h"
#include "il2cpp_resolver.h"
#include "lua_engine.h"
#include "pipe_channel.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    // 默认候选只负责提供一个高频、通常运行于主线程的调用时机。
    // 用户通过 set_tick 指定成功后，不再依赖这些名称进行二次查找。
    struct TickCandidate
    {
        const char* namespaze;
        const char* className;
        const char* methodName;
    };

    // 队列只保存 Lua registry 引用，不直接持有 Lua 栈位置。
    // schedule 可能从 CLI 线程或 Hook 回调线程进入，因此单独加锁。
    std::mutex g_queueMutex;
    std::deque<int> g_queue;

    // tick 元数据与安装过程分开保护：读取状态不需要阻塞队列操作，
    // 替换 Hook 则必须串行，防止两个调用者交叉禁用同一个 tick。
    std::mutex g_stateMutex;
    std::mutex g_installMutex;
    const Il2CppMethod* g_tickMethod = nullptr;
    Il2CppClass* g_tickClass = nullptr;
    bool g_failureLogged = false;
    bool g_draining = false; // 由 LuaEngine 锁串行保护，阻止任务内嵌套 tick。
    std::atomic<DWORD> g_mainThreadId{0};

    void LogInstallFailureOnce()
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_failureLogged) return;
        g_failureLogged = true;
        PipeChannel::Instance().SendLog(
            "[schedule] tick hook not installed; use il2cpp.set_tick(method) to specify an entry\n");
    }
}

bool Il2CppScheduler::Schedule(lua_State* L, int callbackIndex)
{
    if (L == nullptr || !lua_isfunction(L, callbackIndex)) return false;

    // registry 引用使回调在真正执行前不会被 Lua GC 回收。
    lua_pushvalue(L, callbackIndex);
    const int reference = luaL_ref(L, LUA_REGISTRYINDEX);
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.push_back(reference);
    }

    // 入队与安装解耦：安装失败时保留任务，用户随后 set_tick 成功后仍可执行。
    if (!EnsureInstalled()) LogInstallFailureOnce();
    return true;
}

bool Il2CppScheduler::SetTick(const Il2CppMethod* method, Il2CppClass* klass)
{
    // 串行化替换过程，防止多个调用者同时禁用并重建内部 Hook。
    std::lock_guard<std::mutex> installLock(g_installMutex);
    if (method == nullptr) return false;
    auto& resolver = Il2CppResolver::Instance();
    if (klass == nullptr) klass = resolver.GetMethodClass(method);
    if (klass == nullptr || resolver.GetMethodPointer(method) == nullptr) return false;

    // Hook 层只安装原生跳板；入口选择和公开状态由 Scheduler 持有。
    if (!Il2CppHook::InstallSchedulerTick(method, klass))
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_tickMethod = method;
        g_tickClass = klass;
        g_failureLogged = false;
    }
    // 不再根据线程创建时间猜测主线程。首次真正触发所选 tick 的线程
    // 才会被记录；这要求 set_tick 选择一个稳定地运行在目标线程上的方法。
    g_mainThreadId.store(0, std::memory_order_relaxed);
    return true;
}

bool Il2CppScheduler::GetTick(const Il2CppMethod*& method, Il2CppClass*& klass)
{
    // 先查询 Hook，随后读取元数据，锁顺序与 Shutdown 保持一致。
    if (!Il2CppHook::IsSchedulerTickInstalled()) return false;
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (g_tickMethod == nullptr) return false;
    method = g_tickMethod;
    klass = g_tickClass;
    return true;
}

bool Il2CppScheduler::IsReady()
{
    return Il2CppHook::IsSchedulerTickInstalled();
}

bool Il2CppScheduler::EnsureInstalled()
{
    if (IsReady()) return true;

    // 候选按稳定性排序。它们只是默认值，任何游戏都可以显式覆盖。
    static constexpr TickCandidate candidates[] = {
        {"UnityEngine", "Time", "get_deltaTime"},
        {"UnityEngine", "Time", "get_frameCount"},
        {"UnityEngine", "Object", "get_name"},
    };

    auto& resolver = Il2CppResolver::Instance();
    for (const TickCandidate& candidate : candidates)
    {
        Il2CppClass* klass = resolver.GetClass(candidate.namespaze, candidate.className);
        if (klass == nullptr) continue;
        const Il2CppMethod* method = resolver.GetMethod(klass, candidate.methodName);
        if (method != nullptr && SetTick(method, klass)) return true;
    }
    return false;
}

void Il2CppScheduler::Drain(lua_State* L)
{
    if (L == nullptr || g_draining) return;

    DWORD mainId = g_mainThreadId.load(std::memory_order_relaxed);
    if (mainId == 0)
    {
        mainId = GetCurrentThreadId();
        g_mainThreadId.store(mainId, std::memory_order_relaxed);
    }
    if (GetCurrentThreadId() != mainId) return;

    g_draining = true;
    struct DrainGuard { ~DrainGuard() { g_draining = false; } } guard;

    // 一次性换出当前队列；回调中再次 schedule 的任务留到下一次 tick。
    std::deque<int> references;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        references.swap(g_queue);
    }

    // 单个任务失败只记录日志，不阻断同一批次的其他主线程任务。
    for (int reference : references)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, reference);
        if (!lua_isfunction(L, -1))
        {
            lua_pop(L, 1);
            luaL_unref(L, LUA_REGISTRYINDEX, reference);
            continue;
        }

        LuaEngine::OutputCapture outputCapture;
        LuaEngine::Instance().BeginOutputCapture(outputCapture);
        const int status = lua_pcall(L, 0, 0, 0);
        LuaEngine::Instance().EndOutputCapture(outputCapture);

        if (status != LUA_OK)
        {
            const char* error = lua_tostring(L, -1);
            char message[512];
            std::snprintf(message, sizeof(message), "[schedule] callback error: %s",
                error != nullptr ? error : "(non-string error)");
            PipeChannel::Instance().SendLog(message);
            lua_pop(L, 1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, reference);
    }
}

void Il2CppScheduler::Shutdown(lua_State* L)
{
    // Shutdown 在 Lua VM 仍有效时调用，因此可以安全释放 registry 引用。
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (L != nullptr)
        {
            for (int reference : g_queue)
                luaL_unref(L, LUA_REGISTRYINDEX, reference);
        }
        g_queue.clear();
    }

    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_tickMethod = nullptr;
    g_tickClass = nullptr;
    g_failureLogged = false;
    g_mainThreadId.store(0, std::memory_order_relaxed);
}
