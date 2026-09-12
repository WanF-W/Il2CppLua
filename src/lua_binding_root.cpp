/**
 * lua_binding_root.cpp — il2cpp/lua 全局表绑定
 * 负责不隶属于某个 userdata 的顶层能力：运行时状态、程序集和类查找、
 * 裸地址包装、Hook 全局清理、主线程调度，以及纯 Lua table 辅助工具。
 * 本模块只组织公开入口，具体反射、Hook 和调度工作委托给对应模块。
 */
#include "lua_binding_internal.h"
#include "il2cpp_hook.h"
#include "il2cpp_scheduler.h"
#include "pipe_channel.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
// il2cpp 全局表函数
// il2cpp.get_status() → string 导出函数的解析状态
static int Il2Cpp_GetStatus(lua_State* L)
{
    auto& resolver = Il2CppResolver::Instance();

    HMODULE gameAssembly = GetModuleHandleW(L"GameAssembly.dll");
    char moduleBase[32]{};
    snprintf(moduleBase, sizeof(moduleBase), "0x%p", gameAssembly);

    std::string status;
    status += "Initialized: ";
    status += resolver.IsInitialized() ? "true\n" : "false\n";
    status += "Exports: " + resolver.GetResolveStatus() + "\n";
    status += "Assemblies: " + std::to_string(resolver.GetAssemblyCount()) + "\n";
    status += "Images: " + std::to_string(resolver.GetImageCount()) + "\n";
    status += "Main thread: ";
    status += Il2CppScheduler::IsReady() ? "ready\n" : "not ready\n";
    status += "GameAssembly: ";
    status += gameAssembly != nullptr ? moduleBase : "not loaded";
    status += "\nRejected log batches: " + std::to_string(PipeChannel::Instance().GetRejectedLogBatches());
    status += "\nDiscarded log frames: " + std::to_string(PipeChannel::Instance().GetDiscardedLogFrames());
    status += "\nDropped log bytes (lower bound): " + std::to_string(PipeChannel::Instance().GetDroppedLogBytes());

    lua_pushlstring(L, status.c_str(), status.size());

    return 1;
}
// il2cpp.get_class(namespace, name) → Class | nil
static int Il2Cpp_GetClass(lua_State* L)
{
    // 获取参数：命名空间和类名
    // 命名空间（可选 默认空）
    const char* ns = luaL_optstring(L, 1, "");
    // 类名（必须）
    const char* name = luaL_checkstring(L, 2);

    auto& resolver = Il2CppResolver::Instance();
    Il2CppClass* klass = resolver.GetClass(ns, name);

    LuaBridge_PushClass(L, klass);
    return 1;
}

// il2cpp.get_assemblies() → Assembly[]
static int Il2Cpp_GetAssemblies(lua_State* L)
{
    auto& resolver = Il2CppResolver::Instance();
    const int32_t count = resolver.GetAssemblyCount();
    lua_createtable(L, count, 0);
    for (int32_t i = 0; i < count; ++i)
    {
        LuaBridge_PushAssembly(L, resolver.GetAssemblyAt(i));
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// il2cpp.get_assembly(name) → Assembly | nil
static int Il2Cpp_GetAssembly(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    LuaBridge_PushAssembly(L, Il2CppResolver::Instance().GetAssembly(name));
    return 1;
}

// il2cpp.is_initialized() → boolean
static int Il2Cpp_IsInitialized(lua_State* L)
{
    auto& resolver = Il2CppResolver::Instance();
    lua_pushboolean(L, resolver.IsInitialized() ? 1 : 0);
    return 1;
}

// il2cpp.wrap(address) → Instance
// 将裸指针包装为 Instance userdata
// 通过读取对象头的 klass 指针来确定对象的类
static bool IsReadableMemory(const void* address, size_t size)
{
    if (address == nullptr || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;

    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
        | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readable) == 0) return false;

    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = start + size;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end >= start && end <= regionEnd;
}

static int Il2Cpp_Wrap(lua_State* L)
{
    // 接受整数地址或 lightuserdata
    void* address = nullptr;
    // lightuserdata
    if (lua_islightuserdata(L, 1)) address = lua_touserdata(L, 1);
    // 整数地址
    else if (lua_isinteger(L, 1)) address = reinterpret_cast<void*>(static_cast<uintptr_t>(lua_tointeger(L, 1)));
    else return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "expected integer address or lightuserdata");

    if (address == nullptr || !IsReadableMemory(address, sizeof(Il2CppClass*)))
    {
        lua_pushnil(L);
        lua_pushstring(L, "object address is null or unreadable");
        return 2;
    }

    Il2CppClass* klass = nullptr;
    __try
    {
        klass = *reinterpret_cast<Il2CppClass**>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        klass = nullptr;
    }

    if (klass == nullptr || !IsReadableMemory(klass, sizeof(void*)))
    {
        lua_pushnil(L);
        lua_pushstring(L, "address does not contain a readable IL2CPP class pointer");
        return 2;
    }

    // 基础可读性检查通过后包装对象；PushInstance 读取实际类并建立 GCHandle。
    LuaBridge_PushInstance(L, reinterpret_cast<Il2CppObject*>(address));
    return 1;
}
// lua.each / lua.dump（Lua table 工具）
// 支持 Lua 表（get_methods / get_fields 等返回的结果表）
// 回调签名统一为 function(value, index) 索引从 1 开始

// lua.each(table, fn) → 无返回值
// 只负责 Lua table 遍历，运行时容器由各自 userdata 处理
static int Lua_Each(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    return LuaBridge_EachTable(L, 1, 2);
}

// lua.dump 输出缓冲（固定大小 超出截断）
// 向 dump 缓冲追加格式化文本
void LuaBridge_DumpAppend(DumpBuffer* buf, const char* fmt, ...)
{
    if (buf == nullptr || buf->truncated) return;

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf->data + buf->len, sizeof(buf->data) - buf->len, fmt, args);
    va_end(args);
    if (n <= 0) return;

    size_t room = sizeof(buf->data) - buf->len;
    buf->len += static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room - 1;
    if (static_cast<size_t>(n) >= room)
    {
        buf->truncated = true;
        constexpr char marker[] = "\n[dump truncated]\n";
        size_t end = sizeof(buf->data) - sizeof(marker);
        while (end > 0 && (static_cast<unsigned char>(buf->data[end]) & 0xC0) == 0x80) --end;
        // Pad the reserved tail so len remains a stable full-buffer sentinel.
        memset(buf->data + end, ' ', sizeof(buf->data) - sizeof(marker) - end);
        memcpy(buf->data + sizeof(buf->data) - sizeof(marker), marker, sizeof(marker));
    }
}

void LuaBridge_PrintDump(lua_State* L, const DumpBuffer* buffer)
{
    if (L == nullptr || buffer == nullptr) return;

    // dump 构造过程以换行结束，而 LuaPrint 也会追加换行。只移除末尾的
    // CR/LF，保留内容内部的分行，使下一个 Lune 提示符只间隔一行。
    size_t length = buffer->len;
    while (length > 0 && (buffer->data[length - 1] == '\n' || buffer->data[length - 1] == '\r'))
        --length;

    lua_getglobal(L, "print");
    lua_pushlstring(L, buffer->data, length);
    lua_call(L, 1, 0);
}

// lua.dump 的格式化回调: function(value, index) → 追加一行 "[index] = value"
static int DumpFormatCallback(lua_State* L)
{
    DumpBuffer* buf = static_cast<DumpBuffer*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (buf == nullptr) return 0;

    // 两个 tostring 结果都留在 Lua 栈上；后一个元方法报错也没有 C++ 资源泄漏。
    const char* key = luaL_tolstring(L, 2, nullptr);
    size_t length = 0;
    const char* value = luaL_tolstring(L, 1, &length);
    LuaBridge_DumpAppend(buf, "[%s] = %.*s\n", key, static_cast<int>(length), value);
    lua_pop(L, 2);

    return 0;
}

// lua.dump(table) → 无返回值
// 输出 Lua table 的第一层键值
static int Lua_Dump(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    const int64_t count = LuaBridge_GetTableCount(L, 1);

    // 输出缓冲作为回调闭包的 upvalue
    DumpBuffer* buf = static_cast<DumpBuffer*>(lua_newuserdata(L, sizeof(DumpBuffer)));
    buf->len = 0;
    buf->truncated = false;
    lua_pushcclosure(L, DumpFormatCallback, 1);
    int fnIdx = lua_gettop(L);

    LuaBridge_DumpAppend(buf, "count: %lld\n", static_cast<long long>(count));

    // 逐元素追加
    LuaBridge_EachTable(L, 1, fnIdx);

    // 一次性输出（走 print 同一输出通道）
    LuaBridge_PrintDump(L, buf);

    return 0;
}

// lua.hex(integer | lightuserdata) → string
// 只负责显示格式转换，不改变地址/偏移 API 使用 integer 的数值语义。
static int Lua_Hex(lua_State* L)
{
    uint64_t value = 0;
    if (lua_isinteger(L, 1))
    {
        // 转为无符号值可保留负整数的完整 64 位位模式。
        value = static_cast<uint64_t>(lua_tointeger(L, 1));
    }
    else if (lua_islightuserdata(L, 1))
    {
        value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(lua_touserdata(L, 1)));
    }
    else
    {
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "hex expects an integer or lightuserdata");
    }

    char text[32]{};
    snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    lua_pushstring(L, text);
    return 1;
}

// il2cpp 全局表的函数注册表
// il2cpp.unhook_all()
// 卸载全部已安装的方法 Hook
static int Il2Cpp_UnhookAll(lua_State* L)
{
    (void)L;
    Il2CppHook::UnhookAll();
    return 0;
}
// il2cpp 主线程调度函数
// schedule 只负责入队，由内部 tick hook 在选定的目标线程取出执行；通常应是 Unity 主线程。

// il2cpp.schedule(fn) → 无返回值
// 把一个 Lua 函数加入主线程执行队列
static int Il2Cpp_Schedule(lua_State* L)
{
    if (!lua_isfunction(L, 1)) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "expected function as argument");

    if (!Il2CppScheduler::Schedule(L, 1))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp,
            "schedule queue full (max 1024 pending tasks) or allocation failed");
    return 0;
}

// il2cpp.set_tick(method) → true | false, error
// 使用精确 MethodInfo 替换内部 tick，避免全局字符串查找的类和重载歧义。
static int Il2Cpp_SetTick(lua_State* L)
{
    LuaMethodUD* ud = LuaBridge_CheckMethod(L, 1);
    if (ud == nullptr) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "set_tick requires a Method");

    auto& resolver = Il2CppResolver::Instance();
    if (resolver.GetMethodPointer(ud->method) == nullptr)
    {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "method has no native address");
        return 2;
    }

    if (!Il2CppScheduler::SetTick(ud->method, ud->klass))
    {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "failed to install tick hook");
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

// il2cpp.get_tick() → signature | nil
// 返回当前 tick 的完整方法签名，未安装时返回 nil。
static int Il2Cpp_GetTick(lua_State* L)
{
    const Il2CppMethod* method = nullptr;
    Il2CppClass* klass = nullptr;

    if (!Il2CppScheduler::GetTick(method, klass))
    {
        lua_pushnil(L);
        return 1;
    }

    const std::string signature = LuaBridge_BuildMethodSignature(method, klass);
    lua_pushlstring(L, signature.c_str(), signature.size());
    return 1;
}

// il2cpp.is_tick_ready() → boolean
// 查询内部 tick hook 是否已安装
static int Il2Cpp_IsTickReady(lua_State* L)
{
    lua_pushboolean(L, Il2CppScheduler::IsReady() ? 1 : 0);
    return 1;
}

// il2cpp 全局表的函数注册表
static const luaL_Reg il2cpp_funcs[] = {
    {"get_status", Il2Cpp_GetStatus},          // 查看解析导出函数状态
    {"get_class", Il2Cpp_GetClass},            // 查找类
    {"get_assemblies", Il2Cpp_GetAssemblies},  // 获取 Assembly 表
    {"get_assembly", Il2Cpp_GetAssembly},      // 按名称查找 Assembly
    {"unhook_all", Il2Cpp_UnhookAll},          // 卸载全部 Hook
    {"is_initialized", Il2Cpp_IsInitialized},  // 检查初始化状态
    {"wrap", Il2Cpp_Wrap},                     // 裸指针包装
    {"schedule", Il2Cpp_Schedule},             // 投递 Lua 函数到 Unity 主线程
    {"set_tick", Il2Cpp_SetTick},              // 设置精确 tick Method
    {"get_tick", Il2Cpp_GetTick},              // 获取 tick 方法签名
    {"is_tick_ready", Il2Cpp_IsTickReady},     // 查询 tick hook 状态
    {nullptr, nullptr}                         // 结束标记
};

void LuaBinding_RegisterGlobals(lua_State* L)
{
    lua_newtable(L);
    luaL_setfuncs(L, il2cpp_funcs, 0);
    lua_setglobal(L, "il2cpp");

    static const luaL_Reg luaFunctions[] = {
        {"each", Lua_Each},
        {"dump", Lua_Dump},
        {"hex", Lua_Hex},
        {nullptr, nullptr}
    };
    lua_newtable(L);
    luaL_setfuncs(L, luaFunctions, 0);
    lua_setglobal(L, "lua");
}
