/**
 * ============================================================
 * dll_main.cpp — Il2CppLua DLL 入口点
 * ============================================================
 * 本文件是注入到游戏进程中的 Il2CppLua.dll 的主入口 
 *
 * 工作流程
 * 
 * ·DllMain(DLL_PROCESS_ATTACH)：保存模块句柄 创建工作线程
 * ·工作线程执行 DllWorkerMain()：
 *   ·PipeChannel::Init()    — 连接管道
 *   ·PipeChannel::SendHello() — 版本握手
 *   ·Il2CppResolver::Init()  — 解析 IL2CPP 导出函数
 *   ·LuaEngine::Init()       — 创建 Lua 虚拟机
 *   ·PipeChannel::SendReady() — 通知 EXE 就绪
 *   ·消息循环：接收 CMD/FILE → 执行 Lua → 回送 OK/ERROR
 *   ·清理（逆序关闭各模块）
 * ·FreeLibraryAndExitThread()：自卸载 DLL
 *
 * 设计要点
 * 
 * ·DllMain 中不做耗时操作（Loader Lock 限制）
 * ·所有初始化在工作线程中完成
 * ·整个工作线程包裹在 SEH __try/__except 中
 * ·支持 EXE 发送 MSG_EXIT 主动断开
 * ·管道断开时自动退出并卸载
 * ·GameAssembly.dll 可能尚未加载 需重试等待
 *
 * 仅针对 Windows x64 
 * ============================================================
 */

#include "pipe_channel.h"
#include "il2cpp_resolver.h"
#include "il2cpp_hook.h"
#include "lua_engine.h"
#include "protocol.h"

// Windows API
#include <windows.h>
#include <cstdio>

// Lua C API（LUA_NOREF 等常量定义在 lauxlib.h 中）
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}


// ============================================================
// 全局变量
// ============================================================

// DLL 自身模块句柄（用于自卸载）
static HMODULE g_hSelfModule = nullptr;

// 工作线程句柄（用于资源管理）
static HANDLE g_hWorkerThread = nullptr;

// ============================================================
// 工作线程主函数声明
// ============================================================
// 实际的工作线程实现（包含 C++ 对象 不能放在 __try 中）
static void DllWorkerMain();

// 工作线程入口（SEH 包装 + 自卸载）
static DWORD WINAPI WorkerThreadProc(LPVOID lpParam);


// ============================================================
// DllMain — DLL 入口点
// ============================================================
// Windows 在 DLL 加载/卸载时调用此函数 
// 注意：DllMain 持有 Loader Lock 不能做耗时操作、不能调用
// 某些 API（如 CreateThread 以外的线程同步函数） 
BOOL APIENTRY DllMain(HMODULE hModule,            // DLL 模块句柄
                      DWORD   ul_reason_for_call, // 调用原因
                      LPVOID  lpReserved)         // 保留参数
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        // ---- DLL 加载到进程地址空间 ----

        // 保存模块句柄（供工作线程自卸载使用）
        g_hSelfModule = hModule;

        // 禁用线程附加/分离通知
        // 减少不必要的 DllMain 调用 提升性能
        DisableThreadLibraryCalls(hModule);

        // 创建工作线程
        // 不能在 DllMain 中直接做初始化工作（Loader Lock 限制）
        // CreateThread 是少数在 DllMain 中安全调用的 API
        g_hWorkerThread = CreateThread(
            nullptr,           // 默认安全属性
            0,                 // 默认栈大小（1MB）
            WorkerThreadProc,  // 线程函数
            nullptr,           // 无参数传递
            0,                 // 立即运行
            nullptr);          // 不需要线程 ID

        // 如果 CreateThread 失败 DllMain 返回 FALSE 表示加载失败
        // 但此时 DLL 已经被 LoadLibrary 加载 返回 FALSE 会导致
        // LoadLibrary 返回 NULL DLL 被卸载
        if (g_hWorkerThread == nullptr) return FALSE;
    }
    break;

    case DLL_PROCESS_DETACH:
    {
        // ---- DLL 从进程地址空间卸载 ----

        // 如果工作线程仍在运行（如进程退出时） 
        // 不做特殊处理——OS 会强制终止线程并回收资源
        // 正常情况下 工作线程已通过 FreeLibraryAndExitThread 自卸载

        // 关闭线程句柄（如果已创建）
        // 注意：如果线程仍在运行 CloseHandle 只是关闭句柄 不会终止线程
        if (g_hWorkerThread != nullptr)
        {
            CloseHandle(g_hWorkerThread);
            g_hWorkerThread = nullptr;
        }

        break;
    }

    // 已通过 DisableThreadLibraryCalls 禁用 不会收到这些通知
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH: break;
    }

    return TRUE;
}


// ============================================================
// 工作线程主函数（实际实现）
// ============================================================
// 此函数包含 C++ 对象（std::string 等） 因此不能放在 __try 中 
// SEH 包装由 WorkerThreadProc 负责 
static void DllWorkerMain()
{
    // ========================================================
    // 初始化管道通信
    // ========================================================

    // PipeChannel::Init 从共享内存读取管道名并连接
    if (!PipeChannel::Instance().Init())
    {
        // 管道初始化失败：可能是共享内存不存在（DLL 不是通过注入器加载）
        // 直接退出 工作线程将自卸载
        return;
    }

    // 发送握手帧（版本字符串）
    // EXE 收到后检查版本是否匹配
    PipeChannel::Instance().SendHello();

    // ========================================================
    // 初始化 IL2CPP 运行时桥接
    // ========================================================

    // GameAssembly.dll 可能尚未加载（游戏启动早期）
    // 重试等待最多 30 秒（60 次 × 500ms）
    BridgeResult il2cppResult = BridgeResult::ERR_IL2CPP_RESOLVE_FAILED;
    for (int retry = 0; retry < 60; ++retry)
    {
        // 尝试初始化 IL2CPP 解析器
        il2cppResult = Il2CppResolver::Instance().Init();
        // 成功 退出重试循环
        if (il2cppResult == BridgeResult::OK) break;

        // 等待 500ms 后重试
        Sleep(500);
    }

    if (il2cppResult != BridgeResult::OK)
    {
        // IL2CPP 初始化失败：发送错误并退出
        PipeChannel::Instance().SendError("failed to initialize IL2CPP resolver (GameAssembly.dll not found or exports missing)");
        PipeChannel::Instance().Shutdown();
        return;
    }

    if (Il2CppResolver::Instance().HasMissingExports())
        PipeChannel::Instance().SendLog("[warning] some optional IL2CPP exports could not be resolved");

    // ========================================================
    // 初始化 Lua 引擎
    // ========================================================

    // 创建 Lua 虚拟机并注册 IL2CPP 桥接函数
    // 输出回调：Lua 的 print 输出和返回值回显通过管道发送给 EXE
    if (!LuaEngine::Instance().Init(
            [](const char* text) -> void
            {
                // Lambda 捕获 PipeChannel 单例的 SendLog 方法
                // 每次 Lua print 或返回值回显都会调用此回调
                PipeChannel::Instance().SendLog(text);
            }))
    {
        // Lua 初始化失败
        PipeChannel::Instance().SendError("failed to initialize Lua engine");
        Il2CppResolver::Instance().Shutdown();
        PipeChannel::Instance().Shutdown();
        return;
    }

    // ========================================================
    // 通知 EXE：就绪
    // ========================================================

    // 构造状态消息 包含 IL2CPP 镜像数量
    char statusMsg[256];
    sprintf_s(statusMsg, 256, "IL2CPP resolved: %d images, Lua ready", Il2CppResolver::Instance().GetImageCount());
    PipeChannel::Instance().SendReady(statusMsg);

    // ========================================================
    // 消息循环
    // ========================================================

    // 循环接收 EXE 发来的命令帧并执行
    while (true)
    {
        // 阻塞读取一个帧
        uint8_t frameType = 0;
        std::vector<uint8_t> payload;

        // 管道断开或读取失败 退出消息循环
        if (!PipeChannel::Instance().RecvFrame(frameType, payload)) break;

        // 根据帧类型分发处理
        switch (frameType)
        {
        case protocol::MSG_CMD:
        {
            // 执行 Lua 代码字符串
            // 将负载转换为以零结尾的字符串
            // payload 中不包含零终止符 需要手动添加
            std::string code(payload.begin(), payload.end());

            // 执行 Lua 代码
            // LuaEngine 内部会将 print 输出和返回值通过管道回传
            bool ok = LuaEngine::Instance().ExecuteString(code.c_str());

            // 根据执行结果发送 OK 或 ERROR
            if (ok) PipeChannel::Instance().SendOk();
            else PipeChannel::Instance().SendError("execution failed");
        }
        break;

        case protocol::MSG_FILE:
        {
            // 执行 Lua 文件
            // 负载为文件路径
            std::string path(payload.begin(), payload.end());

            // 执行 Lua 文件
            bool ok = LuaEngine::Instance().ExecuteFile(path.c_str());

            // 根据执行结果发送 OK 或 ERROR
            if (ok) PipeChannel::Instance().SendOk();
            else PipeChannel::Instance().SendError("file execution failed");
        }
        break;

        case protocol::MSG_EXIT:
        {
            // 退出指令
            // EXE 请求 DLL 断开连接并卸载
            // 发送 EXIT 确认后退出消息循环
            PipeChannel::Instance().SendExit();
            // 跳出 while 循环
            goto exit_loop;
        }
        // 不 break 直接退出 switch
        // 未知帧类型 忽略
        // 不回复 继续等待下一个帧
        default: break;
        }
    }
exit_loop:

    // ========================================================
    // 清理（逆序关闭各模块）
    // ========================================================

    // 卸载全部 Hook（必须在 LuaEngine::Shutdown 之前
    // 保证回调的 Lua 引用在释放时仍然有效）
    Il2CppHook::Shutdown();

    // 关闭 Lua 引擎
    LuaEngine::Instance().Shutdown();

    // 关闭 IL2CPP 解析器（detach 线程 清空缓存）
    Il2CppResolver::Instance().Shutdown();

    // 关闭管道通信
    PipeChannel::Instance().Shutdown();

}


// ============================================================
// 工作线程入口（SEH 包装 + 自卸载）
// ============================================================
// 此函数是 CreateThread 的回调 不能包含带析构函数的 C++ 对象
// （因为使用了 __try/__except） 
// 它将实际工作委托给 DllWorkerMain 并在完成后自卸载 DLL 
static DWORD WINAPI WorkerThreadProc(LPVOID lpParam)
{
    // 消除未使用参数警告
    BRIDGE_UNUSED(lpParam);

    // SEH 安全包装
    // 将整个工作流程包裹在结构化异常处理中
    // 防止任何未捕获的访问违规导致游戏进程崩溃
    __try
    {
        // 调用实际的工作线程实现
        DllWorkerMain();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // 捕获到致命的结构化异常
        // 尝试通知 EXE（管道可能已断开 忽略失败）
        PipeChannel::Instance().SendError("fatal exception in worker thread");

        // 确保资源被清理
        Il2CppHook::Shutdown();
        LuaEngine::Instance().Shutdown();
        Il2CppResolver::Instance().Shutdown();
        PipeChannel::Instance().Shutdown();
    }

    // 自卸载
    // 工作线程结束后 DLL 不再需要驻留在进程地址空间中
    // FreeLibraryAndExitThread 会
    // 
    // ·减少 DLL 的引用计数
    // ·终止当前线程
    // ·如果引用计数降为 0 DLL 被卸载（触发 DLL_PROCESS_DETACH）
    //
    // 注意：此调用不会返回 之后的代码不会执行
    if (g_hSelfModule != nullptr) FreeLibraryAndExitThread(g_hSelfModule, 0);

    return 0;
}
