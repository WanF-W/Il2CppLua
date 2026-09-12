/**
 * lua_engine.h — Lua 虚拟机管理模块声明
 * 负责创建和管理 Lua 虚拟机 提供安全的代码执行入口
 *
 * 核心职责
 *
 * ·创建 Lua 状态机 (luaL_newstate) 并打开标准库
 * ·注册 IL2CPP 桥接函数 (调用 LuaBridge_Init)
 * ·重定向 print 函数到管道通信层
 * ·提供 SEH 安全的代码执行接口
 * ·自动回显：执行后如果有返回值 自动打印
 *
 * 线程安全模型
 *
 * ·Lua 状态机本身不是线程安全的
 * ·使用内置互斥锁保护所有 Lua 访问
 *
 * SEH 保护
 *
 * ·IL2CPP 函数调用可能引发访问违规 (0xC0000005)
 * ·所有 Lua 代码执行都包裹在 __try/__except 中
 * ·原生故障后隔离整个会话，不再访问或关闭受损 VM；嵌套故障传至最外层入口
 *
 * 仅针对 Windows x64
 */
#pragma once
#include "common.h"
#include "protocol.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

// Lua 状态机前置声明（避免在头文件中包含 Lua 头文件）
struct lua_State;
// 输出回调类型
// Lua 的 print 输出和执行返回值通过此回调发送到管道通信层
// 参数 text 为以 '\n' 结尾的文本（或不含换行的单行文本）
// 回调实现由 PipeChannel 提供 最终通过 MSG_LOG 帧发送给 EXE
using OutputCallback = std::function<void(const char* text)>;
// LuaEngine — Lua 虚拟机管理器（单例）
class LuaEngine
{
public:
    // 一次 Lua 执行边界的输出捕获器。捕获上下文按线程嵌套，
    // 因此普通命令、Hook 回调和 schedule 回调可以各自独立成批。
    class OutputCapture
    {
    public:
        OutputCapture() = default;
        OutputCapture(const OutputCapture&) = delete;
        OutputCapture& operator=(const OutputCapture&) = delete;

    private:
        OutputCapture* previous = nullptr;
        std::string text;
        bool active = false;
        bool truncated = false;
        friend class LuaEngine;
    };

    struct ExecutionError
    {
        protocol::ErrorCategory category = protocol::ErrorCategory::Lua;
        int32_t line = -1;
        std::string message;
    };

    // 获取单例实例
    static LuaEngine& Instance();

    /**
     * 初始化 Lua 引擎
     *
     * @param outputCb 输出回调（用于 print 重定向和返回值回显）
     * @return true 初始化成功 false 失败
     */
    bool Init(OutputCallback outputCb);

    /**
     * 关闭 Lua 引擎
     */
    void Shutdown();

    // 访问初始化状态
    bool IsInitialized() const { return !IsFaulted() && m_initialized.load(std::memory_order_acquire); }
    bool IsFaulted() const { return m_faultCode.load(std::memory_order_acquire) != 0; }
    // No Lua API or allocation in the native exception filter.
    void Quarantine(unsigned long exceptionCode);
    static bool EnterExecution(); // returns whether an outer execution exists
    static void LeaveExecution();
    static bool IsExecuting();
    void RequireHealthy() const;

    /**
     * 执行一段 Lua 代码字符串
     *
     * @param code Lua 源码（UTF-8 编码）
     * @return true 执行成功 false 失败
     */
    bool ExecuteString(const char* code);
    bool ExecuteString(const char* code, size_t length);

    /**
     * 执行一个 Lua 文件
     *
     * @param path 文件路径
     * @return true 执行成功 false 失败
     */
    bool ExecuteFile(const char* path);
    bool ExecuteFile(const char* path, size_t length);

    // 获取最近一次执行失败的结构化错误。错误文本不包含 Lua source name，
    // 行号单独传输给协议层，避免 CLI 显示 [string "<string>"]。
    ExecutionError GetLastError() const;

    // 供 IL2CPP/CSharp 桥接入口标记错误层级，再由 ExecuteBuffer 统一收集。
    static int RaiseError(lua_State* L, protocol::ErrorCategory category, const char* format, ...);

    // 将 runtime_invoke 返回的托管异常转换成可读的 ToString() 文本；
    // 如果当前运行时没有 il2cpp_object_to_string，则使用 fallback。
    static int RaiseManagedException(
        lua_State* L, Il2CppException* exception, const char* fallback);

    // 获取 Lua 状态机指针
    // 返回状态机指针。调用方若要使用 Lua C API，必须同时持有 GetMutex()；
    // 指针本身不拥有 Lua 状态机的生命周期。
    lua_State* GetState() const { return IsFaulted() ? nullptr : m_L.load(std::memory_order_acquire); }

    // 获取互斥锁（可重入）
    // Hook 回调可能在同一线程内递归触发（回调内调用被 Hook 的方法）
    // 因此使用 std::recursive_mutex 保证同线程可重复加锁
    std::recursive_mutex& GetMutex() { return m_luaMutex; }

    // 开始/结束当前线程上的一次输出批次。结束时只发送一次逻辑日志。
    void BeginOutputCapture(OutputCapture& capture);
    void EndOutputCapture(OutputCapture& capture);

    // Hook 发生 SEH 时 C++ 析构不会执行；异常路径必须清除线程本地捕获指针，
    // 避免后续输出继续写入已经离开作用域的捕获对象。
    void AbortOutputCapture();

private:
    LuaEngine()  = default;
    ~LuaEngine();
    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    // includeLine 只对 Lua 文件执行启用；交互字符串错误不向 Lune 暴露行号。
    bool ExecuteBuffer(const char* buff, size_t size, const char* name, bool includeLine);
    bool ExecuteBufferCore(const char* buff, size_t size, const char* name, bool includeLine);
    bool ExecuteBufferProtected(const char* buff, size_t size, const char* name, bool includeLine);

    void SetLastError(protocol::ErrorCategory category, int32_t line, const char* message);

    void EmitOutput(const char* text);

    static int LuaPrint(lua_State* L);

    static void PrintReturnValues(lua_State* L, int count);

    // 成员变量
    std::atomic<lua_State*> m_L{nullptr};
    std::atomic<bool>       m_initialized{false};
    std::atomic<unsigned long> m_faultCode{0}; // sticky: never reset/reuse this session
    OutputCallback m_outputCb;
    mutable std::recursive_mutex m_luaMutex;
    ExecutionError m_lastError;
};
