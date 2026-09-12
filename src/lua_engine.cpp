/**
 * lua_engine.cpp — Lua 虚拟机管理模块实现
 * 本文件实现 lua_engine.h 中声明的 LuaEngine 类
 *
 * 模块组成
 *
 * ·SEH 执行边界与不可恢复会话隔离（ExecuteBufferProtected）
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
 */

#include "lua_engine.h"
#include "lua_bridge.h"
#include "il2cpp_resolver.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace
{
    constexpr size_t MAX_OUTPUT_BYTES = 1024 * 1024;
    constexpr char OUTPUT_TRUNCATED[] = "\n[output truncated]\n";

    size_t OutputPrefix(const char* text, size_t length, size_t room)
    {
        size_t bytes = (std::min)(length, room);
        if (bytes < length)
            while (bytes > 0 && (static_cast<unsigned char>(text[bytes]) & 0xC0) == 0x80) --bytes;
        return bytes;
    }

    void AppendOutput(std::string& output, bool& truncated, const char* text, size_t length)
    {
        if (truncated || text == nullptr) return;
        const size_t room = MAX_OUTPUT_BYTES - (sizeof(OUTPUT_TRUNCATED) - 1) - output.size();
        const size_t bytes = OutputPrefix(text, length, room);
        output.append(text, bytes);
        if (bytes < length)
        {
            output += OUTPUT_TRUNCATED;
            truncated = true;
        }
    }

    // Lua 默认把 source name 和行号拼进错误字符串，例如：
    // [string "<string>"]:1: attempt to call a nil value。
    // source name 对 CLI 用户没有帮助，因此在 DLL 端拆出行号后丢弃它。
    bool StripLuaSourcePrefix(std::string& message, int32_t& line)
    {
        size_t lineStart = std::string::npos;
        if (message.rfind("[string ", 0) == 0)
        {
            const size_t marker = message.find("]:");
            if (marker != std::string::npos) lineStart = marker + 2;
        }
        else
        {
            const size_t marker = message.find(':');
            if (marker != std::string::npos) lineStart = marker + 1;
        }

        if (lineStart == std::string::npos) return false;
        const size_t digitsStart = lineStart;
        while (lineStart < message.size()
            && std::isdigit(static_cast<unsigned char>(message[lineStart])))
        {
            ++lineStart;
        }
        if (lineStart == digitsStart || lineStart >= message.size() || message[lineStart] != ':')
            return false;

        try
        {
            line = static_cast<int32_t>(std::stol(message.substr(digitsStart, lineStart - digitsStart)));
        }
        catch (...)
        {
            return false;
        }

        size_t textStart = lineStart + 1;
        if (textStart < message.size() && message[textStart] == ' ') ++textStart;
        message.erase(0, textStart);
        return true;
    }

    // 每个线程维护自己的输出捕获栈；Hook 和 schedule 可能在不同游戏线程并发执行。
    thread_local LuaEngine::OutputCapture* g_outputCapture = nullptr;
    thread_local unsigned g_executionDepth = 0;
}

bool LuaEngine::EnterExecution() { return g_executionDepth++ != 0; }
void LuaEngine::LeaveExecution() { --g_executionDepth; }
bool LuaEngine::IsExecuting() { return g_executionDepth != 0; }

void LuaEngine::Quarantine(unsigned long exceptionCode)
{
    unsigned long expected = 0;
    m_faultCode.compare_exchange_strong(expected, exceptionCode != 0 ? exceptionCode : 1,
        std::memory_order_acq_rel);
    // No access to the abandoned Lua stack or its capture objects.
    AbortOutputCapture();
}

void LuaEngine::RequireHealthy() const
{
    if (IsFaulted()) RaiseException(0xE04C5541, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
// 单例获取
LuaEngine& LuaEngine::Instance()
{
    static LuaEngine instance;
    return instance;
}

void LuaEngine::BeginOutputCapture(OutputCapture& capture)
{
    if (capture.active) return;

    capture.previous = g_outputCapture;
    capture.text.clear();
    capture.truncated = false;
    capture.active = true;
    g_outputCapture = &capture;
}

void LuaEngine::EndOutputCapture(OutputCapture& capture)
{
    if (!capture.active) return;

    // 捕获上下文必须按后进先出结束。若异常路径已经清空线程指针，
    // 当前对象中的地址可能已不再属于活动栈，不能重新挂回 previous。
    if (g_outputCapture != &capture)
    {
        capture.active = false;
        capture.text.clear();
        capture.previous = nullptr;
        return;
    }

    g_outputCapture = capture.previous;
    capture.previous = nullptr;
    capture.active = false;

    if (!capture.text.empty() && m_outputCb)
    {
        // 先恢复父捕获上下文，再直接调用输出回调，保证本批次不会被父批次重新吸收。
        std::string output = std::move(capture.text);
        m_outputCb(output.c_str());
    }
    capture.text.clear();
}

void LuaEngine::AbortOutputCapture()
{
    // 仅用于 HookDispatch 的 SEH 异常路径；此时不能访问可能已经失效的栈对象。
    g_outputCapture = nullptr;
}

void LuaEngine::EmitOutput(const char* text)
{
    if (text == nullptr) return;

    if (g_outputCapture != nullptr)
    {
        AppendOutput(g_outputCapture->text, g_outputCapture->truncated,
            text, strnlen_s(text, MAX_OUTPUT_BYTES + 1));
        return;
    }

    if (m_outputCb) m_outputCb(text);
}

LuaEngine::ExecutionError LuaEngine::GetLastError() const
{
    if (IsFaulted())
    {
        char message[192];
        snprintf(message, sizeof(message), "native fault 0x%08lX; Lua session quarantined; restart the target process to recover",
            m_faultCode.load(std::memory_order_acquire));
        return {protocol::ErrorCategory::Il2Cpp, -1, message};
    }
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    return m_lastError;
}

void LuaEngine::SetLastError(protocol::ErrorCategory category, int32_t line, const char* message)
{
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    m_lastError.category = category;
    m_lastError.line = line;
    m_lastError.message = message != nullptr ? message : "unknown error";
}

int LuaEngine::RaiseError(lua_State* L, protocol::ErrorCategory category, const char* format, ...)
{
    LuaEngine& engine = Instance();
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_luaMutex);
        engine.m_lastError.category = category;
        engine.m_lastError.line = -1;
    }

    char message[4096]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format != nullptr ? format : "unknown error", args);
    va_end(args);
    return luaL_error(L, "%s", message);
}

int LuaEngine::RaiseManagedException(
    lua_State* L, Il2CppException* exception, const char* fallback)
{
    const char* fallbackMessage = fallback != nullptr
        ? fallback
        : "CSharp exception thrown during method invocation (managed exception text unavailable)";
    char message[8192]{};

    // 使用固定缓冲区避免在随后 luaL_error 的 longjmp 路径上遗留需要
    // 析构的临时 C++ 对象。ObjectToString 内部同时兼容官方导出和
    // 直接查找 ToString() 的运行时调用兜底。
    if (exception != nullptr)
    {
        auto& resolver = Il2CppResolver::Instance();
        Il2CppString* managedText = resolver.ObjectToString(
            reinterpret_cast<Il2CppObject*>(exception));
        const int32_t length = resolver.StringLength(managedText);
        const uint16_t* chars = resolver.StringChars(managedText);
        if (managedText != nullptr && chars != nullptr && length > 0)
        {
            const int byteCount = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                reinterpret_cast<LPCWCH>(chars),
                length,
                nullptr,
                0,
                nullptr,
                nullptr);
            if (byteCount > 0 && byteCount < static_cast<int>(sizeof(message)))
            {
                if (WideCharToMultiByte(
                        CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        reinterpret_cast<LPCWCH>(chars),
                        length,
                        message,
                        static_cast<int>(sizeof(message) - 1),
                        nullptr,
                        nullptr) > 0)
                {
                    return RaiseError(L, protocol::ErrorCategory::CSharp, "%s", message);
                }
            }
        }

        // ToString 失败时至少报告异常的实际托管类型，避免所有问题都
        // 退化成相同的“method invocation”提示。
        Il2CppClass* exceptionClass = READ_OFFSET(
            reinterpret_cast<Il2CppObject*>(exception), 0, Il2CppClass*)[0];
        const char* name = resolver.GetClassSimpleName(exceptionClass);
        const char* namespaze = resolver.GetClassNamespace(exceptionClass);
        if (name != nullptr && *name != '\0')
        {
            if (namespaze != nullptr && *namespaze != '\0')
            {
                _snprintf_s(
                    message,
                    sizeof(message),
                    _TRUNCATE,
                    "CSharp exception during method invocation: %s.%s",
                    namespaze,
                    name);
            }
            else
            {
                _snprintf_s(
                    message,
                    sizeof(message),
                    _TRUNCATE,
                    "CSharp exception during method invocation: %s",
                    name);
            }
            return RaiseError(L, protocol::ErrorCategory::CSharp, "%s", message);
        }
    }

    return RaiseError(L, protocol::ErrorCategory::CSharp, "%s", fallbackMessage);
}

// 析构函数
LuaEngine::~LuaEngine()
{
    if (IsFaulted()) return;
    if (bridge_lifecycle::g_processTerminating.load(std::memory_order_acquire))
        return;

    // 析构时确保资源已释放
    // 如果用户忘记调用 Shutdown 这里兜底清理
    if (m_initialized.load(std::memory_order_acquire)) Shutdown();
}
// 初始化
bool LuaEngine::Init(OutputCallback outputCb)
{
    if (IsFaulted()) return false;
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    if (IsFaulted()) return false;

    if (m_initialized.load(std::memory_order_acquire)) return true;

    // 创建 Lua 状态机
    // luaL_newstate 创建一个新的 Lua 状态机 返回 lua_State* 指针
    lua_State* state = luaL_newstate();
    //创建失败
    if (state == nullptr) return false;
    m_L.store(state, std::memory_order_release);

    // luaL_openlibs 加载所有标准库（base, string, table, math, io, os 等）
    luaL_openlibs(state);

    // 保存输出回调
    // 必须在替换 print 之前保存 因为 LuaPrint 需要用到
    m_outputCb = outputCb;

    // 替换 print 函数
    // 获取全局表 _G 将全局表压入栈顶
    lua_getglobal(state, "_G");

    // 将自定义的 LuaPrint 函数注册为全局 "print"
    // 将 C 函数压入栈顶
    lua_pushcfunction(state, LuaEngine::LuaPrint);
    // _G.print = LuaPrint 弹出函数
    lua_setfield(state, -2, "print");
    // 弹出全局表 恢复栈
    lua_pop(state, 1);

    // 注册 IL2CPP 桥接函数
    // LuaBridge_Init 创建五张 userdata 元表并注册 il2cpp 与 lua 全局表
    // 这一步必须在 Lua VM 创建之后、执行用户代码之前完成
    if (!LuaBridge_Init(state))
    {
        // 桥接层初始化失败 关闭 Lua 状态机
        lua_close(state);
        m_L.store(nullptr, std::memory_order_release);
        return false;
    }

    m_initialized.store(true, std::memory_order_release);
    return true;
}
// 关闭
void LuaEngine::Shutdown()
{
    // A quarantined VM and its registry/GCHandles are retained until process exit.
    if (IsFaulted()) return;
    // 加锁保护关闭过程
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    if (IsFaulted()) return;
    if (!m_initialized.load(std::memory_order_acquire)) return;

    // 关闭 Lua 状态机
    lua_State* state = m_L.exchange(nullptr, std::memory_order_acq_rel);
    if (state != nullptr)
    {
        lua_close(state);
    }

    // 清空输出回调 设置状态
    m_outputCb = nullptr;
    m_initialized.store(false, std::memory_order_release);
}
// 执行 Lua 代码缓冲区（核心实现）
bool LuaEngine::ExecuteBuffer(const char* buff, size_t size, const char* name, bool includeLine)
{
    return !IsFaulted() && ExecuteBufferProtected(buff, size, name, includeLine) && !IsFaulted();
}

bool LuaEngine::ExecuteBufferProtected(const char* buff, size_t size, const char* name, bool includeLine)
{
    m_luaMutex.lock();
    const bool nested = EnterExecution();
    __try
    {
        __try { return ExecuteBufferCore(buff, size, name, includeLine); }
        __except ((Quarantine(GetExceptionCode()), nested ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER))
        {
            return false;
        }
    }
    __finally
    {
        LeaveExecution();
        m_luaMutex.unlock();
    }
}

bool LuaEngine::ExecuteBufferCore(const char* buff, size_t size, const char* name, bool includeLine)
{
    // 状态检查、栈基线和整个执行过程必须使用同一把锁。否则 Shutdown
    // 可能在检查之后关闭 m_L，留下一个已经失效的栈指针。
    RequireHealthy();
    m_lastError = {};
    m_lastError.category = protocol::ErrorCategory::Lua;
    if (buff == nullptr)
    {
        m_lastError.message = "empty Lua input";
        return false;
    }
    if (!m_initialized.load(std::memory_order_acquire))
    {
        m_lastError.category = protocol::ErrorCategory::Il2Cpp;
        m_lastError.message = "Lua engine is not initialized";
        return false;
    }
    lua_State* state = m_L.load(std::memory_order_acquire);
    if (state == nullptr)
    {
        m_lastError.category = protocol::ErrorCategory::Il2Cpp;
        m_lastError.message = "Lua state is unavailable";
        return false;
    }

    // 记录栈基线
    // 在执行前记录栈顶位置 执行后用于计算返回值数量和恢复栈
    int baseline = lua_gettop(state);

    // 普通 Lua 命令也按一次执行统一收集输出；Hook / schedule 的嵌套执行
    // 会建立自己的子捕获上下文，不会混入这条命令的日志批次。
    OutputCapture outputCapture;
    BeginOutputCapture(outputCapture);

    int status = luaL_loadbuffer(state, buff, size, name);
    if (status == LUA_OK) status = lua_pcall(state, 0, LUA_MULTRET, 0);
    RequireHealthy();
    if (status != LUA_OK)
    {
        // Lua 错误（语法错误或运行时错误）
        // 错误信息在栈顶（baseline + 1 的位置）
        // 获取错误信息字符串
        const char* err = lua_tostring(state, -1);
        // 错误对象不是字符串
        if (err == nullptr) err = "(non-string error object)";

        std::string message(err);
        int32_t line = -1;
        StripLuaSourcePrefix(message, line);
        // 只有 Lua 文件自身的 Lua 错误带源码行号。桥接层产生的 Il2Cpp / CSharp
        // 错误即使发生在文件中，也只返回错误类别和文本，避免混淆协议语义。
        m_lastError.line = includeLine
            && m_lastError.category == protocol::ErrorCategory::Lua
            ? line
            : -1;
        if (message.empty()) message = "Lua execution failed";
        m_lastError.message = std::move(message);

        // 弹出错误信息 恢复栈到基线
        lua_settop(state, baseline);
        EndOutputCapture(outputCapture);
        return false;
    }

    // 执行成功 处理返回值
    // 计算返回值数量 当前栈顶 - 基线
    int nresults = lua_gettop(state) - baseline;

    if (nresults > 0 && m_outputCb)
    {
        // 有返回值 自动回显
        PrintReturnValues(state, nresults);
    }

    EndOutputCapture(outputCapture);

    // 恢复栈到基线（弹出所有返回值）
    // 这确保每次执行后栈都回到初始状态 防止栈无限增长
    lua_settop(state, baseline);

    return true;
}
// 执行 Lua 代码字符串
bool LuaEngine::ExecuteString(const char* code)
{
    // 以固定的 CLI 名称执行；错误中的 source name 会在 ExecuteBuffer 内移除。
    if (code == nullptr) return false;

    return ExecuteString(code, strlen(code));
}
bool LuaEngine::ExecuteString(const char* code, size_t length)
{
    return ExecuteBuffer(code, length, "=lune", false);
}
// 执行 Lua 文件
bool LuaEngine::ExecuteFile(const char* path)
{
    return ExecuteFile(path, path != nullptr ? strlen(path) : 0);
}

bool LuaEngine::ExecuteFile(const char* path, size_t pathLength)
{
    if (IsFaulted()) return false;
    if (path == nullptr || pathLength == 0)
    {
        SetLastError(protocol::ErrorCategory::Lua, -1, "Lua file path is empty");
        return false;
    }
    if (pathLength > INT_MAX || memchr(path, '\0', pathLength) != nullptr)
    {
        SetLastError(protocol::ErrorCategory::Lua, -1, "invalid file path (embedded NUL or excessive length)");
        return false;
    }
    // Own the terminator; callers only promise pathLength readable bytes.
    const std::string ownedPath(path, pathLength);
    path = ownedPath.c_str();
    if (!IsInitialized())
    {
        SetLastError(protocol::ErrorCategory::Il2Cpp, -1, "Lua engine is not initialized");
        return false;
    }
    auto error = [this](const char* message)
    {
        SetLastError(protocol::ErrorCategory::Lua, -1, message);
        return false;
    };
    // 磁盘 I/O 不持有 Lua 锁；执行时由 ExecuteBuffer 重新确认 VM 生命周期。
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (length <= 0) return error("invalid UTF-8 file path");
    std::vector<wchar_t> widePath(static_cast<size_t>(length));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, widePath.data(), length) <= 0)
        return error("failed to convert file path");
    std::vector<char> source;
    {
        struct FileHandle
        {
            HANDLE value;
            ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        } file{CreateFileW(widePath.data(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) return error("cannot open file");
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0)
            return error("cannot get file size");
        if (size.QuadPart > 4 * 1024 * 1024) return error("file too large (max 4MB)");
        source.resize(static_cast<size_t>(size.QuadPart));
        size_t offset = 0;
        while (offset < source.size())
        {
            DWORD read = 0;
            if (!ReadFile(file.value, source.data() + offset, static_cast<DWORD>(source.size() - offset), &read, nullptr)
                || read == 0) return error("failed to read complete file");
            offset += read;
        }
    }
    const char* name = path;
    for (const char* current = path; *current != '\0'; ++current)
        if (*current == '/' || *current == '\\') name = current + 1;
    return ExecuteBuffer(source.empty() ? "" : source.data(), source.size(), name, true);
}
// 自定义 print 函数
// 替换 Lua 原生的 print 将输出重定向到管道通信层
// 行为与原生 print 一致：
// print("hello", 42, true) → 输出 "hello\t42\ttrue\n"
int LuaEngine::LuaPrint(lua_State* L)
{
    int n = lua_gettop(L);

    // luaL_tolstring 可运行用户元方法；缓冲区由 Lua 管理，不跨 longjmp 持有 std::string。
    luaL_Buffer output;
    luaL_buffinit(L, &output);
    size_t used = 0;
    bool truncated = false;
    for (int i = 1; i <= n; ++i)
    {
        size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length);
        Instance().RequireHealthy();
        if (!truncated)
        {
            const size_t room = MAX_OUTPUT_BYTES - sizeof(OUTPUT_TRUNCATED) - used;
            const size_t bytes = OutputPrefix(text, length, room);
            if (bytes < length)
            {
                lua_pushlstring(L, text, bytes);
                lua_replace(L, -2);
            }
            luaL_addvalue(&output);
            used += bytes;
            if (bytes < length || (i < n && used == MAX_OUTPUT_BYTES - sizeof(OUTPUT_TRUNCATED)))
            {
                luaL_addstring(&output, OUTPUT_TRUNCATED);
                truncated = true;
            }
            else if (i < n) { luaL_addchar(&output, '\t'); ++used; }
        }
        else lua_pop(L, 1);
    }
    luaL_addchar(&output, '\n');
    luaL_pushresult(&output);
    Instance().EmitOutput(lua_tostring(L, -1));
    return 0;
}
// 打印返回值（自动回显）
// 在 lua_pcall 内执行 luaL_tolstring，使 userdata 的 __tostring 错误保持为普通
// Lua 错误。直接在 ExecuteBuffer 的 pcall 结束后调用 luaL_tolstring，会让错误
// 越过保护边界并终止承载 IPC 的工作线程。
static int ProtectedToString(lua_State* L)
{
    luaL_checkany(L, 1);
    luaL_tolstring(L, 1, nullptr);
    return 1;
}

// 执行完 Lua 代码后 如果栈上有返回值 逐个打印
// 每个返回值占一行 格式：值 (类型名)
void LuaEngine::PrintReturnValues(lua_State* L, int count)
{
    // 先拼接所有返回值为完整字符串 再一次性发送
    // 避免多次调用 outputCb 导致多个 MSG_LOG 帧交错
    std::string output;
    bool truncated = false;

    // 使用绝对索引：luaL_tolstring 会向栈顶推入结果
    // 负索引会因推入操作而偏移 必须用绝对索引
    int base = lua_gettop(L) - count + 1;

    for (int i = 0; i < count; ++i)
    {
        int idx = base + i;
        int type = lua_type(L, idx);

        if (type == LUA_TNIL)
        {
            AppendOutput(output, truncated, "nil\n", 4);
        }
        else if (type == LUA_TBOOLEAN)
        {
            const char* text = lua_toboolean(L, idx) ? "true\n" : "false\n";
            AppendOutput(output, truncated, text, strlen(text));
        }
        else
        {
            // tostring 可能调用用户数据的 __tostring 元方法，必须放在独立 pcall 中。
            lua_pushcfunction(L, ProtectedToString);
            lua_pushvalue(L, idx);
            const int stringifyStatus = lua_pcall(L, 1, 1, 0);
            Instance().RequireHealthy();
            if (stringifyStatus == LUA_OK)
            {
                size_t len = 0;
                const char* text = lua_tolstring(L, -1, &len);
                if (text != nullptr) AppendOutput(output, truncated, text, len);
                else
                {
                    const char* typeName = lua_typename(L, type);
                    AppendOutput(output, truncated, "(", 1);
                    AppendOutput(output, truncated, typeName, strlen(typeName));
                    AppendOutput(output, truncated, ")", 1);
                }
            }
            else
            {
                size_t errorLength = 0;
                const char* error = lua_tolstring(L, -1, &errorLength);
                AppendOutput(output, truncated, "<tostring error: ", 16);
                if (error != nullptr) AppendOutput(output, truncated, error, errorLength);
                else AppendOutput(output, truncated, "unknown error", 13);
                AppendOutput(output, truncated, ">", 1);
            }
            AppendOutput(output, truncated, "\n", 1);

            // 弹出 tostring 结果或 pcall 错误，原始返回值仍留在基线区域。
            lua_pop(L, 1);
        }
    }

    // 一次性发送所有返回值（单个 MSG_LOG 帧）
    if (!output.empty()) Instance().EmitOutput(output.c_str());
}
