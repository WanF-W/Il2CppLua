/**
 * ============================================================
 * lua_engine.cpp — Lua 虚拟机管理模块实现
 * ============================================================
 * 本文件实现 lua_engine.h 中声明的 LuaEngine 类 
 *
 * 模块组成
 * 
 * ·SEH 安全包装器（SEHSafeLoadAndCall）
 * ·单例获取（Instance）
 * ·初始化与关闭（Init / Shutdown）
 * ·代码执行（ExecuteBuffer / ExecuteString / ExecuteFile）
 * ·自定义 print 函数（LuaPrint）
 * ·返回值自动回显（PrintReturnValues）
 *
 * SEH 注意事项
 * 
 * ·MSVC 不允许在同一个函数中混用 C++ 异常处理 (try/catch)
 * ·和结构化异常处理 (__try/__except) 因此将可能崩溃的
 * ·Lua 调用放在独立的 __try/__except 函数中 该函数内
 * ·不包含任何带析构函数的 C++ 对象 
 * ============================================================
 */

#include "lua_engine.h"
#include "lua_bridge.h"

#include <windows.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// ============================================================
// SEH 安全的 Lua 代码加载与执行
// ============================================================
// 将 luaL_loadbuffer + lua_pcall 包裹在 __try/__except 中 
// 防止 IL2CPP 函数调用引发访问违规 (0xC0000005) 导致 DLL 崩溃 
//
// 返回值
// 
//   LUA_OK (0)  — 执行成功 返回值在栈顶
//   其他正值    — Lua 错误码 (LUA_ERRSYNTAX / LUA_ERRRUN 等) 错误信息在栈顶
//   -1          — 结构化异常 (SEH) 栈状态未知
//
// 重要
// 
// 此函数内不能有带析构函数的 C++ 对象（如 std::string） 
// 否则 MSVC 编译会报错 C2712 
static int SEHSafeLoadAndCall(lua_State* L, const char* buff, size_t size, const char* name)
{
    __try
    {
        // 加载 Lua 代码块
        // luaL_loadbuffer 将源码编译为 Lua 函数并压入栈顶
        // name 参数用于错误信息中的代码块标识（如文件名）
        int status = luaL_loadbuffer(L, buff, size, name);
        // 加载失败（语法错误等） 错误信息已压入栈顶
        if (status != LUA_OK) return status;

        // 执行已加载的代码块
        // nargs=0              — 该代码块不需要参数
        // nresults=LUA_MULTRET — 保留所有返回值在栈上
        // errfunc=0            — 不使用错误处理函数
        return lua_pcall(L, 0, LUA_MULTRET, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // 捕获到结构化异常（如访问违规、除零等）
        // 此时 Lua 栈状态未知 调用方需自行恢复
        return -1;
    }
}

// ============================================================
// 单例获取
// ============================================================
LuaEngine& LuaEngine::Instance()
{
    static LuaEngine instance;
    return instance;
}

// ============================================================
// 析构函数
// ============================================================
LuaEngine::~LuaEngine()
{
    // 析构时确保资源已释放
    // 如果用户忘记调用 Shutdown 这里兜底清理
    if (m_initialized) Shutdown();
}

// ============================================================
// 初始化
// ============================================================
bool LuaEngine::Init(OutputCallback outputCb)
{
    // 防止重复初始化
    if (m_initialized) return true;

    // 创建 Lua 状态机
    // luaL_newstate 创建一个新的 Lua 状态机 返回 lua_State* 指针
    m_L = luaL_newstate();
    //创建失败
    if (m_L == nullptr) return false;

    // luaL_openlibs 加载所有标准库（base, string, table, math, io, os 等）
    luaL_openlibs(m_L);

    // 保存输出回调
    // 必须在替换 print 之前保存 因为 LuaPrint 需要用到
    m_outputCb = outputCb;

    // 替换 print 函数
    // 获取全局表 _G 将全局表压入栈顶
    lua_getglobal(m_L, "_G");

    // 将自定义的 LuaPrint 函数注册为全局 "print"
    // 将 C 函数压入栈顶
    lua_pushcfunction(m_L, LuaEngine::LuaPrint);
    // _G.print = LuaPrint 弹出函数
    lua_setfield(m_L, -2, "print");
    // 弹出全局表 恢复栈
    lua_pop(m_L, 1);

    // 注册 IL2CPP 桥接函数
    // LuaBridge_Init 创建 4 张元表并注册 il2cpp 全局表
    // 这一步必须在 Lua VM 创建之后、执行用户代码之前完成
    if (!LuaBridge_Init(m_L))
    {
        // 桥接层初始化失败 关闭 Lua 状态机
        lua_close(m_L);
        m_L = nullptr;
        return false;
    }

    m_initialized = true;
    return true;
}

// ============================================================
// 关闭
// ============================================================
void LuaEngine::Shutdown()
{
    if (!m_initialized) return;

    // 加锁保护关闭过程
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);

    // 关闭 Lua 状态机
    if (m_L != nullptr)
    {
        lua_close(m_L);
        m_L = nullptr;
    }

    // 清空输出回调 设置状态
    m_outputCb = nullptr;
    m_initialized = false;
}

// ============================================================
// 执行 Lua 代码缓冲区（核心实现）
// ============================================================
bool LuaEngine::ExecuteBuffer(const char* buff, size_t size, const char* name)
{
    // 前置检查 确保已初始化
    if (!m_initialized || m_L == nullptr) return false;

    // 前置检查：确保代码缓冲区有效
    if (buff == nullptr || size == 0) return false;

    // 记录栈基线
    // 在执行前记录栈顶位置 执行后用于计算返回值数量和恢复栈
    int baseline = lua_gettop(m_L);

    // 锁定 Lua 状态机（RAII 执行完自动释放）
    std::unique_lock<std::recursive_mutex> luaLock(m_luaMutex);

    // SEH 安全执行
    int status = SEHSafeLoadAndCall(m_L, buff, size, name);

    // 锁在 luaLock 析构时自动释放

    // 处理执行结果
    if (status == -1)
    {
        // 结构化异常：栈状态未知 强制恢复到基线
        lua_settop(m_L, baseline);

        // 通过输出回调发送错误信息
        if (m_outputCb) m_outputCb("[SEH exception] access violation during execution\n");
        return false;
    }
    else if (status != LUA_OK)
    {
        // Lua 错误（语法错误或运行时错误）
        // 错误信息在栈顶（baseline + 1 的位置）
        // 获取错误信息字符串
        const char* err = lua_tostring(m_L, -1);
        // 错误对象不是字符串
        if (err == nullptr) err = "(non-string error object)";

        // 通过输出回调发送错误信息
        if (m_outputCb)
        {
            // 发送错误文本
            m_outputCb(err);
            // 添加换行
            m_outputCb("\n");
        }

        // 弹出错误信息 恢复栈到基线
        lua_settop(m_L, baseline);
        return false;
    }

    // 执行成功 处理返回值
    // 计算返回值数量 当前栈顶 - 基线
    int nresults = lua_gettop(m_L) - baseline;

    if (nresults > 0 && m_outputCb)
    {
        // 有返回值 自动回显
        PrintReturnValues(m_L, nresults, m_outputCb);
    }

    // 恢复栈到基线（弹出所有返回值）
    // 这确保每次执行后栈都回到初始状态 防止栈无限增长
    lua_settop(m_L, baseline);

    return true;
}

// ============================================================
// 执行 Lua 代码字符串
// ============================================================
bool LuaEngine::ExecuteString(const char* code)
{
    // 委托给 ExecuteBuffer
    // 使用 strlen 计算长度 名称为 "<string>" 用于错误信息
    if (code == nullptr) return false;

    return ExecuteBuffer(code, strlen(code), "<string>");
}

// ============================================================
// 执行 Lua 文件
// ============================================================
bool LuaEngine::ExecuteFile(const char* path)
{
    // 前置检查
    if (!m_initialized || path == nullptr) return false;

    // 将路径转换为宽字符（支持 Unicode 路径）
    // Lua 代码路径来自管道协议 可能是 UTF-8 或 ANSI 编码
    // 使用 CP_UTF8 转换为 UTF-16 支持中文等非 ASCII 路径
    wchar_t wpath[MAX_PATH];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    // 转换失败
    if (wlen == 0)
    {
        // 可能是路径过长
        if (m_outputCb) m_outputCb("[error] failed to convert file path to wide string\n");
        return false;
    }

    // 打开文件
    // 使用 CreateFileW 打开文件 支持 Unicode 路径
    HANDLE hFile = CreateFileW(wpath,                    // 文件路径
                               GENERIC_READ,             // 只读访问
                               FILE_SHARE_READ,          // 允许其他进程读取
                               nullptr,                  // 默认安全属性
                               OPEN_EXISTING,            // 文件必须存在
                               FILE_ATTRIBUTE_NORMAL,    // 普通文件
                               nullptr);                 // 无模板文件

    // 文件打开失败
    if (hFile == INVALID_HANDLE_VALUE)
    {
        if (m_outputCb)
            m_outputCb("[error] cannot open file\n");
        return false;
    }

    // 获取文件大小
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0)
    {
        // 无法获取文件大小或文件为空
        CloseHandle(hFile);
        if (m_outputCb) m_outputCb("[error] file is empty or cannot get size\n");
        return false;
    }

    // 文件大小安全检查 限制为 4MB 防止读取过大文件
    if (fileSize.QuadPart > static_cast<long long>(4 * 1024) * 1024)
    {
        CloseHandle(hFile);
        if (m_outputCb) m_outputCb("[error] file too large (max 4MB)\n");
        return false;
    }

    size_t srcSize = static_cast<size_t>(fileSize.QuadPart);

    // 分配缓冲区并读取文件内容
    // 多分配 1 字节用于零终止符（虽然 luaL_loadbuffer 不需要零终止 
    // 但多分配一个字节更安全）
    char* buffer = new (std::nothrow) char[srcSize + 1];
    if (buffer == nullptr)
    {
        // 内存分配失败
        CloseHandle(hFile);
        if (m_outputCb) m_outputCb("[error] out of memory\n");
        return false;
    }

    // 分块读取文件内容（ReadFile 可能不会一次读完大文件）
    DWORD totalRead = 0;
    while (totalRead < srcSize)
    {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hFile,
                           buffer + totalRead,                      // 写入位置
                           static_cast<DWORD>(srcSize - totalRead), // 读取量
                           &bytesRead,                              // 实际读取量
                           nullptr);                                // 同步 IO

        // 读取结束或出错
        if (!ok || bytesRead == 0) break;  

        // 循环读取
        totalRead += bytesRead;
    }

    // 关闭文件句柄（已读完内容）
    CloseHandle(hFile);

    // 零终止符（安全措施）
    buffer[totalRead] = '\0';

    // 执行文件内容
    // 从路径中提取文件名作为代码块名称（用于错误信息）
    // 例如 "C:\scripts\test.lua" → "test.lua"
    const char* name = path;
    // Windows 路径分隔符
    const char* lastSlash = strrchr(path, '\\');
    if (lastSlash != nullptr) name = lastSlash + 1;
    else
    {
        // 也检查正斜杠（兼容混合路径）
        lastSlash = strrchr(path, '/');
        if (lastSlash != nullptr) name = lastSlash + 1;
    }

    // 调用 ExecuteBuffer 执行文件内容
    bool result = ExecuteBuffer(buffer, totalRead, name);

    // 释放缓冲区
    delete[] buffer;

    return result;
}

// ============================================================
// 自定义 print 函数
// ============================================================
// 替换 Lua 原生的 print 将输出重定向到管道通信层 
// 行为与原生 print 一致：
// print("hello", 42, true) → 输出 "hello\t42\ttrue\n"
int LuaEngine::LuaPrint(lua_State* L)
{
    // 获取参数个数
    int n = lua_gettop(L);

    // 获取 LuaEngine 单例的输出回调
    // 通过单例获取 因为 LuaPrint 是静态函数 无法直接访问成员变量
    const OutputCallback& cb = Instance().m_outputCb;

    // 如果没有设置输出回调 直接返回
    if (!cb) return 0;

    // 逐个处理参数 先拼接为完整字符串再一次性发送
    // 避免多次调用 cb 导致多个 MSG_LOG 帧交错
    std::string output;
    for (int i = 1; i <= n; ++i)
    {
        // 获取参数的字符串表示
        // luaL_tolstring 对所有类型都能生成字符串描述
        // 
        // ·字符串/数字：直接返回值
        // ·userdata：调用 __tostring 元方法
        // ·table/无 __tostring 的类型：返回 "type: address" 格式
        // 结果会推入栈顶 使用后需弹出
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);

        if (s != nullptr)
        {
            output.append(s, len);
        }
        else
        {
            const char* typeName = luaL_typename(L, i);
            output += (typeName ? typeName : "(unknown)");
        }

        // 弹出 luaL_tolstring 推入的结果
        lua_pop(L, 1);

        // 参数之间用制表符分隔（与原生 print 一致）
        if (i < n) output += '\t';
    }

    // 末尾添加换行符
    output += '\n';

    // 一次性发送完整输出（单个 MSG_LOG 帧）
    cb(output.c_str());

    // print 不返回值
    return 0;
}


// ============================================================
// 打印返回值（自动回显）
// ============================================================
// 执行完 Lua 代码后 如果栈上有返回值 逐个打印 
// 每个返回值占一行 格式：值 (类型名)
void LuaEngine::PrintReturnValues(lua_State* L, int count, const OutputCallback& outputCb)
{
    // 先拼接所有返回值为完整字符串 再一次性发送
    // 避免多次调用 outputCb 导致多个 MSG_LOG 帧交错
    std::string output;

    // 使用绝对索引：luaL_tolstring 会向栈顶推入结果
    // 负索引会因推入操作而偏移 必须用绝对索引
    int base = lua_gettop(L) - count + 1;

    for (int i = 0; i < count; ++i)
    {
        int idx = base + i;
        int type = lua_type(L, idx);

        if (type == LUA_TNIL)
        {
            output += "nil\n";
        }
        else if (type == LUA_TBOOLEAN)
        {
            output += (lua_toboolean(L, idx) ? "true\n" : "false\n");
        }
        else
        {
            // luaL_tolstring 对所有类型都能生成字符串描述
            // 
            // ·字符串/数字：直接返回值
            // ·userdata：调用 __tostring 元方法
            // ·table/无 __tostring 的类型：返回 "type: address" 格式
            // 
            // 结果会推入栈顶 使用后需弹出
            size_t len = 0;
            const char* s = luaL_tolstring(L, idx, &len);
            if (s != nullptr)
            {
                output.append(s, len);
                output += '\n';
            }
            else
            {
                const char* typeName = lua_typename(L, type);
                output += '(';
                output += (typeName ? typeName : "unknown");
                output += ")\n";
            }

            // 弹出 luaL_tolstring 推入的结果
            lua_pop(L, 1);
        }
    }

    // 一次性发送所有返回值（单个 MSG_LOG 帧）
    if (!output.empty()) outputCb(output.c_str());
}
