/**
 * ============================================================
 * lua_engine.h — Lua 虚拟机管理模块声明
 * ============================================================
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
 * ·捕获异常后恢复栈并报告错误 避免 DLL 崩溃
 *
 * 仅针对 Windows x64 
 * ============================================================
 */
#pragma once
#include "common.h"

// Lua 状态机前置声明（避免在头文件中包含 Lua 头文件）
struct lua_State;

// ============================================================
// 输出回调类型
// ============================================================
// Lua 的 print 输出和执行返回值通过此回调发送到管道通信层 
// 参数 text 为以 '\n' 结尾的文本（或不含换行的单行文本） 
// 回调实现由 PipeChannel 提供 最终通过 MSG_LOG 帧发送给 EXE 
using OutputCallback = std::function<void(const char* text)>;

// ============================================================
// LuaEngine — Lua 虚拟机管理器（单例）
// ============================================================
class LuaEngine
{
public:
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
    bool IsInitialized() const { return m_initialized; }

    /**
     * 执行一段 Lua 代码字符串
     *
     * @param code Lua 源码（UTF-8 编码）
     * @return true 执行成功 false 失败
     */
    bool ExecuteString(const char* code);

    /**
     * 执行一个 Lua 文件
     *
     * @param path 文件路径
     * @return true 执行成功 false 失败
     */
    bool ExecuteFile(const char* path);

    // 获取 Lua 状态机指针
    lua_State* GetState() const { return m_L; }

    // 获取输出回调
    const OutputCallback& GetOutputCallback() const { return m_outputCb; }

    // 获取互斥锁（可重入）
    // Hook 回调可能在同一线程内递归触发（回调内调用被 Hook 的方法）
    // 因此使用 std::recursive_mutex 保证同线程可重复加锁
    std::recursive_mutex& GetMutex() { return m_luaMutex; }

private:
    LuaEngine()  = default;
    ~LuaEngine();
    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    bool ExecuteBuffer(const char* buff, size_t size, const char* name);

    static int LuaPrint(lua_State* L);

    static void PrintReturnValues(lua_State* L, int count, const OutputCallback& outputCb);

    // 成员变量
    lua_State*     m_L           = nullptr;
    bool           m_initialized = false;
    OutputCallback m_outputCb;
    std::recursive_mutex m_luaMutex;
};
