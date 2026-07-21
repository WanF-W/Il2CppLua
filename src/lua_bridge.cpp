/**
 * ============================================================
 * lua_bridge.cpp — Lua ↔ IL2CPP 桥接层实现
 * ============================================================
 * 本文件实现 lua_bridge.h 中声明的全部函数 
 *
 * 模块组成
 * 
 * ·类型编组 (MarshalArg / PushReturnValue)
 * ·Userdata 创建与检查辅助函数
 * ·il2cpp 全局表函数 (get_class / wrap / find_objects 等)
 * ·Class 元表方法 (get_name / get_method / new / static_call 等)
 * ·Instance 元表方法 (call / get / set / 数组访问等)
 * ·Method 元表方法 (call / hook / ovload 等)
 * ·Field 元表方法 (get / set / get_offset 等)
 * ·元表创建与初始化
 *
 * 类型编组策略
 * 
 *   Lua → C#:
 *     nil        → nullptr (引用类型) / 0 (值类型)
 *     boolean    → bool (1 字节)
 *     number     → int/float/double (根据参数类型决定)
 *     string     → Il2CppString* (通过 StringNew 创建)
 *     Instance   → Il2CppObject* (直接取指针)
 *     lightuserdata → void* (原始指针透传)
 *
 *   C# → Lua:
 *     nullptr    → nil
 *     bool       → boolean
 *     int/float  → number/integer
 *     string     → string (UTF-16 → UTF-8 转换)
 *     object     → Instance userdata
 *     其他       → Instance userdata (兜底)
 * ============================================================
 */

#include "lua_bridge.h"
#include "il2cpp_resolver.h"
#include "lua_engine.h"
#include "pipe_channel.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// ============================================================
// 调试日志：通过管道发送到前端CLI显示
// ============================================================
static void DbgLog(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    PipeChannel::Instance().SendLog(buf);
}

// ============================================================
// 内部辅助：类型枚举判断
// ============================================================

// 判断是否为整数类型
static bool IsIntegerType(int32_t typeEnum)
{
    // TYPE_I1/I2/I4/I8 和 TYPE_U1/U2/U4/U8 都是整数
    return typeEnum == Il2CppTypeEnum::TYPE_I1
        || typeEnum == Il2CppTypeEnum::TYPE_U1
        || typeEnum == Il2CppTypeEnum::TYPE_I2
        || typeEnum == Il2CppTypeEnum::TYPE_U2
        || typeEnum == Il2CppTypeEnum::TYPE_I4
        || typeEnum == Il2CppTypeEnum::TYPE_U4
        || typeEnum == Il2CppTypeEnum::TYPE_I8
        || typeEnum == Il2CppTypeEnum::TYPE_U8
        || typeEnum == Il2CppTypeEnum::TYPE_I    // 原生 int
        || typeEnum == Il2CppTypeEnum::TYPE_U;   // 原生 uint
}

// 判断是否为浮点类型
static bool IsFloatType(int32_t typeEnum)
{
    return typeEnum == Il2CppTypeEnum::TYPE_R4   // float
        || typeEnum == Il2CppTypeEnum::TYPE_R8;  // double
}

// 判断是否为引用类型
static bool IsRefType(int32_t typeEnum)
{
    return typeEnum == Il2CppTypeEnum::TYPE_STRING
        || typeEnum == Il2CppTypeEnum::TYPE_CLASS
        || typeEnum == Il2CppTypeEnum::TYPE_OBJECT
        || typeEnum == Il2CppTypeEnum::TYPE_SZARRAY;
}

// ============================================================
// 内部辅助：IL2CPP 字符串 → Lua 字符串
// ============================================================
// IL2CPP 字符串内部以 UTF-16 存储 Lua 字符串以 UTF-8 存储
// 使用 WideCharToMultiByte 进行转换
static void PushIl2CppString(lua_State* L, Il2CppString* str)
{
    // 获取解析器引用
    auto& resolver = Il2CppResolver::Instance();

    // 获取字符串的 UTF-16 字符数据和长度
    const uint16_t* chars = resolver.StringChars(str);
    int32_t len = resolver.StringLength(str);

    // 空字符串处理
    if (len == 0 || chars == nullptr)
    {
        // 压入空字符串
        lua_pushstring(L, "");
        return;
    }

    // 计算转换后 UTF-8 所需的字节数
    int utf8Len = WideCharToMultiByte(
        CP_UTF8,                            // 目标编码：UTF-8
        0,                                  // 转换标志
        reinterpret_cast<LPCWCH>(chars),    // 源数据 (UTF-16)
        len,                                // 源字符数
        nullptr, 0,                         // 只计算长度 不实际转换
        nullptr, nullptr);                  // 默认替换字符

    if (utf8Len > 0)
    {
        // 使用 std::vector 分配缓冲区（自动释放 避免 alloca 栈溢出警告）
        std::vector<char> buf(utf8Len + 1);

        // 执行转换
        WideCharToMultiByte(
            CP_UTF8, 0,
            reinterpret_cast<LPCWCH>(chars), len,
            buf.data(), utf8Len,
            nullptr, nullptr);

        // 添加终止符
        buf[utf8Len] = '\0';
        // 压入 Lua 字符串
        lua_pushlstring(L, buf.data(), utf8Len);
    }
    else
    {
        // 转换失败 压入空字符串
        lua_pushstring(L, "");
    }
}

// ============================================================
// 内部辅助：Lua 值 → C# 参数编组
// ============================================================
// 将 Lua 栈上指定位置的值转换为 C# 参数
// 转换后的数据存储在 storage 指向的 16 字节缓冲区中
// outParam 被设置为指向参数数据的指针（供 runtime_invoke 使用）
//
// 返回 true 表示编组成功 false 表示类型不匹配
static bool MarshalArg(lua_State* L, int idx, const Il2CppType* type, void* storage, void*& outParam)
{
    // 获取解析器引用
    auto& resolver = Il2CppResolver::Instance();

    // 获取参数的 IL2CPP 类型枚举
    int32_t typeEnum = resolver.GetTypeEnum(type);

    // 根据类型枚举进行编组
    switch (typeEnum)
    {
    // 布尔类型
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        // Lua boolean → C# bool (1 字节)
        *static_cast<bool*>(storage) = lua_toboolean(L, idx) != 0;
        outParam = storage;
        return true;

    // 字符类型
    case Il2CppTypeEnum::TYPE_CHAR:
        // Lua integer → C# char (2 字节 UTF-16)
        *static_cast<uint16_t*>(storage) = static_cast<uint16_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;

    // 有符号整数类型
    case Il2CppTypeEnum::TYPE_I1:
        *static_cast<int8_t*>(storage) = static_cast<int8_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_I2: 
        *static_cast<int16_t*>(storage) = static_cast<int16_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_I4:
        *static_cast<int32_t*>(storage) = static_cast<int32_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_I8:
        *static_cast<int64_t*>(storage) = static_cast<int64_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;

    // 无符号整数类型
    case Il2CppTypeEnum::TYPE_U1:
        *static_cast<uint8_t*>(storage) = static_cast<uint8_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_U2:
        *static_cast<uint16_t*>(storage) = static_cast<uint16_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_U4:
        *static_cast<uint32_t*>(storage) = static_cast<uint32_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_U8:
        *static_cast<uint64_t*>(storage) = static_cast<uint64_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;

    // 浮点类型
    case Il2CppTypeEnum::TYPE_R4:
        *static_cast<float*>(storage) = static_cast<float>(lua_tonumber(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_R8:
        *static_cast<double*>(storage) = lua_tonumber(L, idx);
        outParam = storage;
        return true;

    // 字符串类型（引用类型）
    case Il2CppTypeEnum::TYPE_STRING:
    {
        // Lua string → Il2CppString*
        // 关键：il2cpp_runtime_invoke 对引用类型直接使用 params[i] 作为对象指针
        // 不是指针的指针！所以 outParam 直接设为 Il2CppString* 本身
        Il2CppString* s = nullptr;
        if (!lua_isnil(L, idx))
        {
            const char* str = lua_tostring(L, idx);
            s = resolver.StringNew(str ? str : "");
        }
        // 把指针存到 storage（保证生命周期 防止编译器优化掉）
        *static_cast<Il2CppString**>(storage) = s;
        // outParam 直接是对象指针（不是 storage 的地址）
        outParam = s;
        return true;
    }

    // 引用类型 (class, object, array)
    case Il2CppTypeEnum::TYPE_CLASS:
    case Il2CppTypeEnum::TYPE_OBJECT:
    case Il2CppTypeEnum::TYPE_SZARRAY:
    {
        // Instance userdata → Il2CppObject*
        // 同样 引用类型直接传对象指针
        Il2CppObject* obj = nullptr;
        if (!lua_isnil(L, idx))
        {
            LuaInstanceUD* ud = LuaBridge_CheckInstance(L, idx);
            if (ud == nullptr) return false;
            obj = ud->obj;
        }
        *static_cast<Il2CppObject**>(storage) = obj;
        outParam = obj;
        return true;
    }

    // 原生整数类型 (IntPtr/UIntPtr)
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
        // 接受 Lua 整数或 lightuserdata
        if (lua_islightuserdata(L, idx))
        {
            *static_cast<void**>(storage) = lua_touserdata(L, idx);
        }
        else
        {
            *static_cast<intptr_t*>(storage) = static_cast<intptr_t>(lua_tointeger(L, idx));
        }
        outParam = storage;
        return true;

    // 值类型 (struct)
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    {
        // 对于值类型 尝试接受 lightuserdata（原始内存指针）
        // 或 Instance userdata（已装箱的值类型）
        if (lua_islightuserdata(L, idx))
        {
            // 直接使用原始指针作为值类型数据
            void* ptr = lua_touserdata(L, idx);
            memcpy(storage, &ptr, sizeof(void*));
            outParam = storage;
            return true;
        }
        else if (lua_isnil(L, idx))
        {
            // nil → 全零值
            memset(storage, 0, 16);
            outParam = storage;
            return true;
        }
        else
        {
            // 尝试作为已装箱的值类型
            LuaInstanceUD* ud = LuaBridge_CheckInstance(L, idx);
            if (ud == nullptr || ud->obj == nullptr) return false;

            // 拆箱：获取对象内部的值类型数据指针
            void* unboxed = resolver.Unbox(ud->obj);
            if (unboxed == nullptr) return false;

            // 复制值类型数据到 storage
            // 注意：我们不知道确切大小 复制 16 字节作为兜底
            // 对于大多数值类型（int, float, Vector3 等）足够
            memcpy(storage, unboxed, 16);
            outParam = storage;
            return true;
        }
    }

    // 未支持的类型
    default:
        // 尝试将 lightuserdata 作为原始指针传递
        if (lua_islightuserdata(L, idx))
        {
            *static_cast<void**>(storage) = lua_touserdata(L, idx);
            outParam = storage;
            return true;
        }
        // 编组失败
        return false;
    }
}

// ============================================================
// 内部辅助：通用方法调用
// ============================================================
// 调用 IL2CPP 方法 将 Lua 参数编组为 C# 参数
// 调用结果转换为 Lua 值压栈
//
// 参数：
//   L           — Lua 状态机
//   method      — 要调用的方法
//   obj         — this 指针（静态方法传 nullptr）
//   argStartIdx — Lua 栈上第一个参数的位置
// 返回：压入 Lua 栈的返回值数量（1 或 0）
static int InvokeMethod(lua_State* L, const Il2CppMethod* method, void* obj, int argStartIdx)
{
    auto& resolver = Il2CppResolver::Instance();

    // 检查参数个数
    int32_t paramCount = resolver.GetMethodParamCount(method);
    int luaArgCount = lua_gettop(L) - argStartIdx + 1;

    if (luaArgCount != paramCount)
    {
        return luaL_error(L, "argument count mismatch: expected %d, got %d", paramCount, luaArgCount);
    }

    // 编组参数
    // 每个参数需要独立的存储空间（16 字节 够存任何基本类型或指针）
    // 使用 std::vector 分配 避免 alloca 栈溢出警告
    struct ArgStorage { alignas(16) uint8_t data[16]; };

    std::vector<ArgStorage> storages(paramCount);
    std::vector<void*> paramsArr(paramCount);
    void** params = paramCount > 0 ? paramsArr.data() : nullptr;

    // 逐个编组参数
    for (int32_t i = 0; i < paramCount; ++i)
    {
        // 获取第 i 个参数的类型
        const Il2CppType* paramType = resolver.GetMethodParamType(method, i);
        if (paramType == nullptr) return luaL_error(L, "failed to get parameter %d type", i);

        // 编组 Lua 值到 C# 参数
        if (!MarshalArg(L, argStartIdx + i, paramType, storages[i].data, params[i]))
        {
            return luaL_error(L, "failed to marshal argument %d", i + 1);
        }
    }

    // 用来接收 C# 异常
    Il2CppException* exc = nullptr;
    // 返回值类型
    const Il2CppType* retType = resolver.GetMethodReturnType(method);
    // 调用方法
    Il2CppObject* result = resolver.RuntimeInvoke(method, obj, params, &exc);

    // 异常处理
    if (exc != nullptr)
    {
        // 尝试获取异常消息
        // C# 异常对象也是 Il2CppObject 可以通过反射获取 Message 属性
        // 这里简化处理 直接报告异常发生
        return luaL_error(L, "C# exception thrown during method invocation");
    }

    // 压入返回值
    LuaBridge_PushReturnValue(L, result, retType);
    // 返回 1 个值
    return 1;
}

// ============================================================
// Userdata 创建辅助函数实现
// ============================================================

// 创建 Class userdata
void LuaBridge_PushClass(lua_State* L, Il2CppClass* klass)
{
    // 空类压入 nil
    if (klass == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    // 分配 userdata
    LuaClassUD* ud = static_cast<LuaClassUD*>(lua_newuserdata(L, sizeof(LuaClassUD)));
    // 设置类指针
    ud->klass = klass;

    // 设置元表
    luaL_getmetatable(L, LuaBridgeMT::CLASS);
    lua_setmetatable(L, -2);
}

// 创建 Instance userdata
void LuaBridge_PushInstance(lua_State* L, Il2CppObject* obj, Il2CppClass* klass)
{
    if (obj == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    // 如果未提供 klass 从对象头读取
    if (klass == nullptr)
    {
        // Il2CppObject 布局: offset 0x00 = Il2CppClass* klass
        klass = READ_OFFSET(obj, 0, Il2CppClass*)[0];
    }

    // 分配 userdata
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(lua_newuserdata(L, sizeof(LuaInstanceUD)));
    ud->obj = obj;
    ud->klass = klass;

    // 设置元表
    luaL_getmetatable(L, LuaBridgeMT::INSTANCE);
    lua_setmetatable(L, -2);
}

// 创建 Method userdata
void LuaBridge_PushMethod(lua_State* L, const Il2CppMethod* method, Il2CppClass* klass)
{
    if (method == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    LuaMethodUD* ud = static_cast<LuaMethodUD*>(lua_newuserdata(L, sizeof(LuaMethodUD)));
    ud->method = method;
    ud->klass = klass;

    luaL_getmetatable(L, LuaBridgeMT::METHOD);
    lua_setmetatable(L, -2);
}

// 创建 Field userdata
void LuaBridge_PushField(lua_State* L, const Il2CppField* field, Il2CppClass* klass)
{
    if (field == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    LuaFieldUD* ud = static_cast<LuaFieldUD*>(lua_newuserdata(L, sizeof(LuaFieldUD)));
    ud->field = field;
    ud->klass = klass;

    luaL_getmetatable(L, LuaBridgeMT::FIELD);
    lua_setmetatable(L, -2);
}

// ============================================================
// Userdata 类型检查辅助函数实现
// ============================================================

LuaClassUD* LuaBridge_CheckClass(lua_State* L, int idx)
{
    return static_cast<LuaClassUD*>(luaL_testudata(L, idx, LuaBridgeMT::CLASS));
}

LuaInstanceUD* LuaBridge_CheckInstance(lua_State* L, int idx)
{
    return static_cast<LuaInstanceUD*>(luaL_testudata(L, idx, LuaBridgeMT::INSTANCE));
}

LuaMethodUD* LuaBridge_CheckMethod(lua_State* L, int idx)
{
    return static_cast<LuaMethodUD*>(luaL_testudata(L, idx, LuaBridgeMT::METHOD));
}

LuaFieldUD* LuaBridge_CheckField(lua_State* L, int idx)
{
    return static_cast<LuaFieldUD*>(luaL_testudata(L, idx, LuaBridgeMT::FIELD));
}

// ============================================================
// C# 返回值 → Lua 值
// ============================================================
void LuaBridge_PushReturnValue(lua_State* L, Il2CppObject* result, const Il2CppType* returnType)
{
    auto& resolver = Il2CppResolver::Instance();

    // 无返回类型 → void
    if (returnType == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    int32_t typeEnum = resolver.GetTypeEnum(returnType);

    // void 类型
    if (typeEnum == Il2CppTypeEnum::TYPE_VOID)
    {
        lua_pushnil(L);
        return;
    }

    // null 引用
    if (result == nullptr)
    {
        // 值类型的 null → 0
        if (IsIntegerType(typeEnum)) lua_pushinteger(L, 0);
        else if (IsFloatType(typeEnum)) lua_pushnumber(L, 0.0);
        else if (typeEnum == Il2CppTypeEnum::TYPE_BOOLEAN) lua_pushboolean(L, 0);
        // 引用类型 → nil
        else lua_pushnil(L);
        return;
    }

    // 根据类型枚举转换返回值
    switch (typeEnum)
    {
    // 布尔
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        // 值类型需要 unbox 获取数据
        lua_pushboolean(L, *static_cast<bool*>(resolver.Unbox(result)));
        break;

    // 字符
    case Il2CppTypeEnum::TYPE_CHAR:
        lua_pushinteger(L, *static_cast<uint16_t*>(resolver.Unbox(result)));
        break;

    // 有符号整数
    case Il2CppTypeEnum::TYPE_I1:
        lua_pushinteger(L, *static_cast<int8_t*>(resolver.Unbox(result)));
        break;
    case Il2CppTypeEnum::TYPE_I2:
        lua_pushinteger(L, *static_cast<int16_t*>(resolver.Unbox(result)));
        break;
    case Il2CppTypeEnum::TYPE_I4:
        lua_pushinteger(L, *static_cast<int32_t*>(resolver.Unbox(result)));
        break;
    case Il2CppTypeEnum::TYPE_I8:
        lua_pushinteger(L, *static_cast<int64_t*>(resolver.Unbox(result)));
        break;

    // 无符号整数
    case Il2CppTypeEnum::TYPE_U1:
        lua_pushinteger(L, *static_cast<uint8_t*>(resolver.Unbox(result)));
        break;
    case Il2CppTypeEnum::TYPE_U2:
        lua_pushinteger(L, *static_cast<uint16_t*>(resolver.Unbox(result)));
        break;
    case Il2CppTypeEnum::TYPE_U4:
        lua_pushinteger(L, *static_cast<uint32_t*>(resolver.Unbox(result)));
        break;
    case Il2CppTypeEnum::TYPE_U8:
        lua_pushinteger(L, *static_cast<uint64_t*>(resolver.Unbox(result)));
        break;

    // 浮点
    case Il2CppTypeEnum::TYPE_R4:
        lua_pushnumber(L, *static_cast<float*>(resolver.Unbox(result)));
        break;
    case Il2CppTypeEnum::TYPE_R8:
        lua_pushnumber(L, *static_cast<double*>(resolver.Unbox(result)));
        break;

    // 原生整数
    case Il2CppTypeEnum::TYPE_I:
        lua_pushinteger(L, *static_cast<intptr_t*>(resolver.Unbox(result)));
        break;
    case Il2CppTypeEnum::TYPE_U:
        lua_pushinteger(L, *static_cast<uintptr_t*>(resolver.Unbox(result)));
        break;

    // 字符串
    case Il2CppTypeEnum::TYPE_STRING:
        PushIl2CppString(L, reinterpret_cast<Il2CppString*>(result));
        break;

    // 引用类型、数组、值类型、其他 → Instance userdata
    default:
        LuaBridge_PushInstance(L, result, nullptr);
        break;
    }
}

// ============================================================
// 数组辅助函数实现
// ============================================================

bool LuaBridge_IsArray(Il2CppObject* obj)
{
    if (obj == nullptr) return false;

    auto& resolver = Il2CppResolver::Instance();

    // 从对象头读取类指针
    Il2CppClass* klass = READ_OFFSET(obj, 0, Il2CppClass*)[0];
    if (klass == nullptr) return false;

    // 获取类的 Il2CppType
    const Il2CppType* type = resolver.GetClassType(klass);
    if (type == nullptr) return false;

    // 检查类型枚举是否为 SZARRAY (一维零基数组)
    int32_t typeEnum = resolver.GetTypeEnum(type);
    return typeEnum == Il2CppTypeEnum::TYPE_SZARRAY;
}

uint64_t LuaBridge_GetArrayLength(Il2CppObject* arr)
{
    if (arr == nullptr) return 0;
    auto& resolver = Il2CppResolver::Instance();
    return resolver.ArrayLength(reinterpret_cast<Il2CppArray*>(arr));
}

// ============================================================
// il2cpp 全局表函数
// ============================================================
// il2cpp.get_status() → string 导出函数的解析状态
static int Il2Cpp_GetStatus(lua_State* L)
{
    auto& resolver = Il2CppResolver::Instance();

    lua_pushstring(L, resolver.GetResolveStatus().c_str());

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

// il2cpp.get_assemblies() → number (程序集数量)
static int Il2Cpp_GetAssemblies(lua_State* L)
{
    auto& resolver = Il2CppResolver::Instance();
    // 返回缓存的 Image 数量（等于程序集数量）
    lua_pushinteger(L, resolver.GetImageCount());
    return 1;
}

// il2cpp.get_image_count() → number
static int Il2Cpp_GetImageCount(lua_State* L)
{
    auto& resolver = Il2CppResolver::Instance();
    lua_pushinteger(L, resolver.GetImageCount());
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
static int Il2Cpp_Wrap(lua_State* L)
{
    // 接受整数地址或 lightuserdata
    void* address = nullptr;
    // lightuserdata
    if (lua_islightuserdata(L, 1)) address = lua_touserdata(L, 1);
    // 整数地址
    else if (lua_isinteger(L, 1)) address = reinterpret_cast<void*>(static_cast<uintptr_t>(lua_tointeger(L, 1)));
    else return luaL_error(L, "expected integer address or lightuserdata");

    if (address == nullptr) return luaL_error(L, "cannot wrap null address");

    // 直接创建 Instance userdata
    // PushInstance 会从对象头读取 klass 指针
    LuaBridge_PushInstance(L, reinterpret_cast<Il2CppObject*>(address), nullptr);
    return 1;
}

// il2cpp.find_objects(klass) → table of Instance | nil
// 通过 Unity 的 FindObjectsOfType 查找堆上的活跃对象
static int Il2Cpp_FindObjects(lua_State* L)
{
    LuaClassUD* ud = LuaBridge_CheckClass(L, 1);
    if (ud == nullptr) return luaL_error(L, "expected Class as argument");

    auto& resolver = Il2CppResolver::Instance();
    Il2CppArray* arr = resolver.FindObjectsOfType(ud->klass);

    // 查找失败
    if (arr == nullptr)
    {
        lua_pushnil(L);
        return 1;
    }

    // 将数组转换为 Lua 表
    uint64_t count = resolver.ArrayLength(arr);
    // 创建结果表
    lua_newtable(L);

    // 数组数据起始地址 (offset 0x20)
    // 对于引用类型数组 每个元素是 8 字节指针
    uint8_t* dataBase = reinterpret_cast<uint8_t*>(arr) + ARRAY_DATA_OFFSET;

    for (uint64_t i = 0; i < count; ++i)
    {
        // 读取第 i 个元素（指针）
        Il2CppObject* elem = READ_OFFSET(dataBase, i * sizeof(void*), Il2CppObject*)[0];
        // 压入结果表 (Lua 索引从 1 开始)
        LuaBridge_PushInstance(L, elem, nullptr);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }

    return 1;
}

// il2cpp 全局表的函数注册表
static const luaL_Reg il2cpp_funcs[] = {
    {"get_status", Il2Cpp_GetStatus},          // 产看解析导出函数状态
    {"get_class", Il2Cpp_GetClass},            // 查找类
    {"get_assemblies", Il2Cpp_GetAssemblies},  // 获取程序集数量
    {"get_image_count", Il2Cpp_GetImageCount}, // 获取镜像数量
    {"is_initialized", Il2Cpp_IsInitialized},  // 检查初始化状态
    {"wrap", Il2Cpp_Wrap},                     // 裸指针包装
    {"find_objects", Il2Cpp_FindObjects},      // 查找对象
    {nullptr, nullptr}                         // 结束标记
};

// ============================================================
// Class 元表方法
// ============================================================

// cls:get_name() → string
static int Class_GetName(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    const char* name = resolver.GetKlassName(ud->klass);
    lua_pushstring(L, name ? name : "");
    return 1;
}

// cls:get_namespace() → string
static int Class_GetNamespace(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    const char* ns = resolver.GetClassNamespace(ud->klass);
    lua_pushstring(L, ns ? ns : "");
    return 1;
}

// cls:get_parent() → Class | nil
static int Class_GetParent(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    Il2CppClass* parent = resolver.GetClassParent(ud->klass);
    LuaBridge_PushClass(L, parent);
    return 1;
}

// cls:get_method(name) → Method | nil
// 按名称查找方法（返回第一个同名方法）
static int Class_GetMethod(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();
    const Il2CppMethod* method = resolver.GetMethod(ud->klass, name);
    LuaBridge_PushMethod(L, method, ud->klass);
    return 1;
}

// cls:get_methods() → table of Method
static int Class_GetMethods(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();

    // 枚举所有方法（最多 1024 个）
    const int32_t MAX_METHODS = 1024;
    const Il2CppMethod* methods[MAX_METHODS];
    int32_t count = resolver.EnumerateMethods(ud->klass, methods, MAX_METHODS);

    // 创建结果表
    lua_newtable(L);
    for (int32_t i = 0; i < count; ++i)
    {
        LuaBridge_PushMethod(L, methods[i], ud->klass);
        // Lua 索引从 1 开始
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

// cls:get_field(name) → Field | nil
static int Class_GetField(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();
    const Il2CppField* field = resolver.GetField(ud->klass, name);
    LuaBridge_PushField(L, field, ud->klass);
    return 1;
}

// cls:get_fields() → table of Field
static int Class_GetFields(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();

    const int32_t MAX_FIELDS = 1024;
    const Il2CppField* fields[MAX_FIELDS];
    int32_t count = resolver.EnumerateFields(ud->klass, fields, MAX_FIELDS);

    lua_newtable(L);
    for (int32_t i = 0; i < count; ++i)
    {
        LuaBridge_PushField(L, fields[i], ud->klass);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

// cls:new(...) → Instance
// 创建对象并调用构造函数
static int Class_New(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();

    // 分配对象（不调用构造函数）
    Il2CppObject* obj = resolver.ObjectNew(ud->klass);
    if (obj == nullptr) return luaL_error(L, "failed to allocate object");

    // 触发静态构造函数（确保类已初始化）
    resolver.RuntimeClassInit(ud->klass);

    // 查找 .ctor 方法
    // 使用参数个数来匹配构造函数重载
    // 减去 self 参数
    int argc = lua_gettop(L) - 1;

    // 枚举类的所有方法 从中筛选名为 ".ctor" 且参数个数匹配的方法
    // 最多枚举 1024 个方法（覆盖绝大多数类）
    const int32_t MAX_METHODS = 1024;
    const Il2CppMethod* methods[MAX_METHODS];
    int32_t methodCount = resolver.EnumerateMethods(ud->klass, methods, MAX_METHODS);

    // 遍历所有方法 查找参数个数匹配的构造函数
    // 最终匹配的构造函数
    const Il2CppMethod* matched = nullptr;
    // 第一个 .ctor（兜底用）
    const Il2CppMethod* firstCtor = nullptr;
    for (int32_t i = 0; i < methodCount; ++i)
    {
        const char* mname = resolver.GetMethodName(methods[i]);
        if (mname != nullptr && strcmp(mname, ".ctor") == 0)
        {
            // 记录第一个 .ctor
            if (firstCtor == nullptr) firstCtor = methods[i];

            // 检查参数个数是否匹配
            int32_t pc = resolver.GetMethodParamCount(methods[i]);
            if (pc == argc)
            {
                // 参数个数匹配 使用此构造函数
                matched = methods[i];
                // 找到即停止搜索
                break;
            }
        }
    }

    // 兜底：如果没找到参数个数完全匹配的 .ctor 使用第一个 .ctor
    if (matched == nullptr) matched = firstCtor;

    // 调用构造函数
    if (matched != nullptr)
    {
        Il2CppException* exc = nullptr;
        int32_t paramCount = resolver.GetMethodParamCount(matched);

        if (paramCount > 0)
        {
            // 编组参数
            struct ArgStorage { alignas(16) uint8_t data[16]; };
            std::vector<ArgStorage> storages(paramCount);
            std::vector<void*> paramsArr(paramCount);
            void** params = paramsArr.data();

            for (int32_t i = 0; i < paramCount; ++i)
            {
                const Il2CppType* pt = resolver.GetMethodParamType(matched, i);
                if (!MarshalArg(L, 2 + i, pt, storages[i].data, params[i]))
                {
                    return luaL_error(L, "failed to marshal ctor arg %d", i + 1);
                }
            }

            resolver.RuntimeInvoke(matched, obj, params, &exc);
        }
        else
        {
            resolver.RuntimeInvoke(matched, obj, nullptr, &exc);
        }

        if (exc != nullptr)
        {
            return luaL_error(L, "C# exception in constructor");
        }
    }

    // 压入新创建的对象
    LuaBridge_PushInstance(L, obj, ud->klass);
    return 1;
}

// cls:static_call(name, ...) → value
// 按名称调用静态方法
static int Class_StaticCall(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    const Il2CppMethod* method = resolver.GetMethod(ud->klass, name);
    if (method == nullptr) return luaL_error(L, "method not found: %s", name);

    // 静态方法 obj 传 nullptr
    return InvokeMethod(L, method, nullptr, 3);
}

// cls:static_get(name) → value
// 读取静态字段值
static int Class_StaticGet(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    const Il2CppField* field = resolver.GetField(ud->klass, name);
    if (field == nullptr) return luaL_error(L, "field not found: %s", name);

    // 读取静态字段
    uint8_t buffer[16] = {};
    resolver.ReadStaticField(field, buffer);

    // 根据字段类型推送 Lua 值
    const Il2CppType* fieldType = resolver.GetFieldType(field);
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);

    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        lua_pushboolean(L, *reinterpret_cast<bool*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I4:
        lua_pushinteger(L, *reinterpret_cast<int32_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I8:
        lua_pushinteger(L, *reinterpret_cast<int64_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_R4:
        lua_pushnumber(L, *reinterpret_cast<float*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_R8:
        lua_pushnumber(L, *reinterpret_cast<double*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_STRING:
    {
        Il2CppString* str = *reinterpret_cast<Il2CppString**>(buffer);
        if (str != nullptr) PushIl2CppString(L, str);
        else lua_pushnil(L);
        break;
    }
    default:       
    {
        // 引用类型或值类型
        Il2CppObject* obj = *reinterpret_cast<Il2CppObject**>(buffer);
        LuaBridge_PushInstance(L, obj, nullptr);
        break;
    }
    }
    return 1;
}

// cls:static_set(name, value)
// 写入静态字段值
static int Class_StaticSet(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    const Il2CppField* field = resolver.GetField(ud->klass, name);
    if (field == nullptr) return luaL_error(L, "field not found: %s", name);

    // 根据字段类型编组 Lua 值
    const Il2CppType* fieldType = resolver.GetFieldType(field);
    uint8_t buffer[16] = {};
    void* param = nullptr;

    if (!MarshalArg(L, 3, fieldType, buffer, param)) return luaL_error(L, "failed to marshal field value");

    resolver.WriteStaticField(field, buffer);
    return 0;
}

// cls:find_objects() → table of Instance | nil
// 查找此类型的所有活跃对象
static int Class_FindObjects(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();

    Il2CppArray* arr = resolver.FindObjectsOfType(ud->klass);
    if (arr == nullptr)
    {
        lua_pushnil(L);
        return 1;
    }

    uint64_t count = resolver.ArrayLength(arr);
    lua_newtable(L);
    uint8_t* dataBase = reinterpret_cast<uint8_t*>(arr) + ARRAY_DATA_OFFSET;

    for (uint64_t i = 0; i < count; ++i)
    {
        Il2CppObject* elem = READ_OFFSET(dataBase, i * sizeof(void*), Il2CppObject*)[0];
        LuaBridge_PushInstance(L, elem, nullptr);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

// cls:get_instance_size() → number
static int Class_GetInstanceSize(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    lua_pushinteger(L, resolver.GetClassInstanceSize(ud->klass));
    return 1;
}

// cls:__tostring() → string
static int Class_ToString(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    const char* ns = resolver.GetClassNamespace(ud->klass);
    const char* name = resolver.GetKlassName(ud->klass);
    lua_pushfstring(L, "Class: %s.%s", ns ? ns : "", name ? name : "?");
    return 1;
}

// Class 元表方法注册表
static const luaL_Reg class_methods[] = {
    {"get_name",          Class_GetName},
    {"get_namespace",     Class_GetNamespace},
    {"get_parent",        Class_GetParent},
    {"get_method",        Class_GetMethod},
    {"get_methods",       Class_GetMethods},
    {"get_field",         Class_GetField},
    {"get_fields",        Class_GetFields},
    {"new",               Class_New},
    {"static_call",       Class_StaticCall},
    {"static_get",        Class_StaticGet},
    {"static_set",        Class_StaticSet},
    {"find_objects",      Class_FindObjects},
    {"get_instance_size", Class_GetInstanceSize},
    {"__tostring",        Class_ToString},
    {nullptr, nullptr}
};


// ============================================================
// Instance 元表方法
// ============================================================

// obj:call(name, ...) → value
// 按名称调用实例方法
static int Instance_Call(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    // 确保 klass 已知
    Il2CppClass* klass = ud->klass;
    if (klass == nullptr && ud->obj != nullptr) klass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];
    if (klass == nullptr) return luaL_error(L, "cannot determine class for instance");

    const Il2CppMethod* method = resolver.GetMethod(klass, name);
    if (method == nullptr) return luaL_error(L, "method not found: %s", name);

    return InvokeMethod(L, method, ud->obj, 3);
}

// obj:get(name) → value
// 读取实例字段值
static int Instance_Get(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    Il2CppClass* klass = ud->klass;
    if (klass == nullptr && ud->obj != nullptr) klass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];
    if (klass == nullptr) return luaL_error(L, "cannot determine class for instance");

    const Il2CppField* field = resolver.GetField(klass, name);
    if (field == nullptr) return luaL_error(L, "field not found: %s", name);

    // 读取字段值
    uint8_t buffer[16] = {};
    resolver.ReadField(ud->obj, field, buffer);

    // 根据字段类型推送 Lua 值
    const Il2CppType* fieldType = resolver.GetFieldType(field);
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);

    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        lua_pushboolean(L, *reinterpret_cast<bool*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I4:
        lua_pushinteger(L, *reinterpret_cast<int32_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I8:
        lua_pushinteger(L, *reinterpret_cast<int64_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_R4:
        lua_pushnumber(L, *reinterpret_cast<float*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_R8:
        lua_pushnumber(L, *reinterpret_cast<double*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_STRING:
    {
        Il2CppString* str = *reinterpret_cast<Il2CppString**>(buffer);
        if (str != nullptr) PushIl2CppString(L, str);
        else lua_pushnil(L);
        break;
    }
    default:
    {
        Il2CppObject* obj = *reinterpret_cast<Il2CppObject**>(buffer);
        LuaBridge_PushInstance(L, obj, nullptr);
        break;
    }
    }
    return 1;
}

// obj:set(name, value)
// 写入实例字段值
static int Instance_Set(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    Il2CppClass* klass = ud->klass;
    if (klass == nullptr && ud->obj != nullptr) klass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];
    if (klass == nullptr) return luaL_error(L, "cannot determine class for instance");

    const Il2CppField* field = resolver.GetField(klass, name);
    if (field == nullptr) return luaL_error(L, "field not found: %s", name);

    const Il2CppType* fieldType = resolver.GetFieldType(field);
    uint8_t buffer[16] = {};
    void* param = nullptr;

    if (!MarshalArg(L, 3, fieldType, buffer, param)) return luaL_error(L, "failed to marshal field value");

    resolver.WriteField(ud->obj, field, buffer);
    return 0;
}

// obj:get_class() → Class
static int Instance_GetClass(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));

    Il2CppClass* klass = ud->klass;
    if (klass == nullptr && ud->obj != nullptr) klass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];

    LuaBridge_PushClass(L, klass);
    return 1;
}

// obj:get_address() → number
static int Instance_GetAddress(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    lua_pushinteger(L, reinterpret_cast<int64_t>(ud->obj));
    return 1;
}

// obj:__tostring() → string
static int Instance_ToString(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    auto& resolver = Il2CppResolver::Instance();

    Il2CppClass* klass = ud->klass;
    if (klass == nullptr && ud->obj != nullptr) klass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];
    const char* name = klass ? resolver.GetKlassName(klass) : "?";

    lua_pushfstring(L, "Instance: %s @ 0x%p", name ? name : "?", ud->obj);
    return 1;
}

// obj:__index(key) → value
// 当 key 为数字时执行数组元素访问
// 当 key 为字符串时在元表中查找方法
static int Instance_Index(lua_State* L)
{
    // 数字 key → 数组元素读取
    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
        if (ud->obj == nullptr) return luaL_error(L, "instance is null");

        // 检查是否为数组
        if (!LuaBridge_IsArray(ud->obj)) return luaL_error(L, "object is not an array");

        // Lua 索引从 1 开始 C# 从 0 开始
        int64_t idx = lua_tointeger(L, 2) - 1;
        uint64_t len = LuaBridge_GetArrayLength(ud->obj);
        if (idx < 0 || static_cast<uint64_t>(idx) >= len) return luaL_error(L, "array index out of bounds: %d", static_cast<int>(idx + 1));

        // 读取数组元素
        // 对于引用类型数组 元素是指针 (8 字节)
        // 数组数据起始地址 = arr + 0x20
        uint8_t* dataBase = reinterpret_cast<uint8_t*>(ud->obj) + ARRAY_DATA_OFFSET;
        Il2CppObject* elem = READ_OFFSET(dataBase, idx * sizeof(void*), Il2CppObject*)[0];
        LuaBridge_PushInstance(L, elem, nullptr);
        return 1;
    }

    // 字符串 key → 在元表中查找方法
    lua_getmetatable(L, 1);                     // 压入元表
    lua_pushvalue(L, 2);                        // 压入 key
    lua_rawget(L, -2);                          // 在元表中查找
    lua_remove(L, -2);                          // 移除元表
    return 1;
}

// obj:__newindex(key, value)
// 当 key 为数字时执行数组元素写入
static int Instance_NewIndex(lua_State* L)
{
    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
        if (ud->obj == nullptr) return luaL_error(L, "instance is null");

        if (!LuaBridge_IsArray(ud->obj)) return luaL_error(L, "object is not an array");

        int64_t idx = lua_tointeger(L, 2) - 1;
        uint64_t len = LuaBridge_GetArrayLength(ud->obj);
        if (idx < 0 || static_cast<uint64_t>(idx) >= len) return luaL_error(L, "array index out of bounds: %d", static_cast<int>(idx + 1));

        // 计算元素地址
        uint8_t* dataBase = reinterpret_cast<uint8_t*>(ud->obj) + ARRAY_DATA_OFFSET;
        void* elemPtr = dataBase + idx * sizeof(void*);

        // 将 Lua 值写入数组元素
        // 对于引用类型数组 直接写入指针
        if (lua_isnil(L, 3)) *static_cast<Il2CppObject**>(elemPtr) = nullptr;
        else
        {
            LuaInstanceUD* valUD = LuaBridge_CheckInstance(L, 3);
            if (valUD != nullptr) *static_cast<Il2CppObject**>(elemPtr) = valUD->obj;
            else return luaL_error(L, "can only assign Instance or nil to array element");
        }
        return 0;
    }

    return luaL_error(L, "cannot set arbitrary fields on instance");
}

// obj:__len() → number
// 返回数组长度（如果是数组的话）
static int Instance_Len(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    if (ud->obj == nullptr)
    {
        lua_pushinteger(L, 0);
        return 1;
    }

    if (LuaBridge_IsArray(ud->obj)) lua_pushinteger(L, static_cast<int64_t>(LuaBridge_GetArrayLength(ud->obj)));
    // 非数组返回 0
    else lua_pushinteger(L, 0);
    return 1;
}

// Instance 元表方法注册表
static const luaL_Reg instance_methods[] = {
    {"call",         Instance_Call},
    {"get",          Instance_Get},
    {"set",          Instance_Set},
    {"get_class",    Instance_GetClass},
    {"get_address",  Instance_GetAddress},
    {"__tostring",   Instance_ToString},
    {"__index",      Instance_Index},
    {"__newindex",   Instance_NewIndex},
    {"__len",        Instance_Len},
    {nullptr, nullptr}
};


// ============================================================
// Method 元表方法
// ============================================================

// mth:get_name() → string
static int Method_GetName(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();
    const char* name = resolver.GetMethodName(ud->method);
    lua_pushstring(L, name ? name : "");
    return 1;
}

// mth:get_param_count() → number
static int Method_GetParamCount(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();
    lua_pushinteger(L, resolver.GetMethodParamCount(ud->method));
    return 1;
}

// mth:get_return_type() → string
static int Method_GetReturnType(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();
    const Il2CppType* retType = resolver.GetMethodReturnType(ud->method);
    if (retType == nullptr)
    {
        lua_pushstring(L, "void");
        return 1;
    }
    const char* name = resolver.GetTypeName(retType);
    lua_pushstring(L, name ? name : "unknown");
    return 1;
}

// mth:get_params() → table of strings
static int Method_GetParams(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();

    int32_t count = resolver.GetMethodParamCount(ud->method);
    lua_newtable(L);
    for (int32_t i = 0; i < count; ++i)
    {
        const Il2CppType* pt = resolver.GetMethodParamType(ud->method, i);
        const char* name = pt ? resolver.GetTypeName(pt) : "unknown";
        lua_pushstring(L, name ? name : "unknown");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

// mth:is_static() → boolean
static int Method_IsStatic(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();
    lua_pushboolean(L, resolver.IsStaticMethod(ud->method) ? 1 : 0);
    return 1;
}

// mth:call(obj, ...) → value
// 使用此 Method userdata 显式调用方法
static int Method_Call(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));

    void* obj = nullptr;
    int argStart = 2;

    // 如果不是静态方法 需要 this 对象
    auto& resolver = Il2CppResolver::Instance();
    if (!resolver.IsStaticMethod(ud->method))
    {
        // 实例方法：第一个参数必须是 Instance
        LuaInstanceUD* instUD = LuaBridge_CheckInstance(L, 2);
        if (instUD == nullptr) return luaL_error(L, "instance method requires Instance as first arg");
        obj = instUD->obj;
        // 参数从第 3 个位置开始
        argStart = 3;
    }

    return InvokeMethod(L, ud->method, obj, argStart);
}

// ---- ovload 实现 ----
// mth:ovload(type1, type2, ...) → Method
// 按参数类型签名查找重载方法
//
// 类型字符串映射：
//   "bool"    → TYPE_BOOLEAN
//   "byte"    → TYPE_U1
//   "sbyte"   → TYPE_I1
//   "short"   → TYPE_I2
//   "ushort"  → TYPE_U2
//   "int"     → TYPE_I4
//   "uint"    → TYPE_U4
//   "long"    → TYPE_I8
//   "ulong"   → TYPE_U8
//   "float"   → TYPE_R4
//   "double"  → TYPE_R8
//   "string"  → TYPE_STRING
//   "object"  → TYPE_OBJECT
//   其他字符串 → 按类名搜索
static int Method_Ovload(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();

    // 获取方法名
    const char* methodName = resolver.GetMethodName(ud->method);
    if (methodName == nullptr) return luaL_error(L, "cannot get method name");

    // 收集类型参数
    int typeArgCount = lua_gettop(L) - 1;
    if (typeArgCount <= 0) return luaL_error(L, "ovload requires at least one type argument");

    // 解析类型字符串
    // 基本类型名 → 类型枚举
    // 类名 → Il2CppClass*（通过 GetClass 搜索）
    struct ParsedType
    {
        int32_t typeEnum;   // 类型枚举（基本类型）
        Il2CppClass* klass; // 类指针（类类型 nullptr 表示基本类型）
        std::string name;   // 原始类型字符串（用于类名比较）

        // 默认构造函数 初始化所有成员（修复 C26495 警告）
        ParsedType() : typeEnum(0), klass(nullptr), name() {}
    };

    std::vector<ParsedType> parsedTypes;
    parsedTypes.reserve(typeArgCount);

    // 基本类型名 → 类型枚举的映射表
    static const std::map<std::string, int32_t> basicTypeMap = {
        {"bool",   Il2CppTypeEnum::TYPE_BOOLEAN},
        {"byte",   Il2CppTypeEnum::TYPE_U1},
        {"sbyte",  Il2CppTypeEnum::TYPE_I1},
        {"char",   Il2CppTypeEnum::TYPE_CHAR},
        {"short",  Il2CppTypeEnum::TYPE_I2},
        {"ushort", Il2CppTypeEnum::TYPE_U2},
        {"int",    Il2CppTypeEnum::TYPE_I4},
        {"uint",   Il2CppTypeEnum::TYPE_U4},
        {"long",   Il2CppTypeEnum::TYPE_I8},
        {"ulong",  Il2CppTypeEnum::TYPE_U8},
        {"float",  Il2CppTypeEnum::TYPE_R4},
        {"double", Il2CppTypeEnum::TYPE_R8},
        {"string", Il2CppTypeEnum::TYPE_STRING},
        {"object", Il2CppTypeEnum::TYPE_OBJECT},
        {"void",   Il2CppTypeEnum::TYPE_VOID},
    };

    for (int i = 0; i < typeArgCount; ++i)
    {
        const char* typeStr = luaL_checkstring(L, 2 + i);
        ParsedType pt;
        pt.typeEnum = 0;
        pt.klass = nullptr;
        pt.name = typeStr;

        // 先查找基本类型映射
        auto it = basicTypeMap.find(typeStr);
        // 基本类型
        if (it != basicTypeMap.end()) pt.typeEnum = it->second;               
        else
        {
            // 不是基本类型 按类名搜索
            // 支持 "Namespace.ClassName" 格式
            std::string ns;
            std::string cls;
            const char* dot = strchr(typeStr, '.');
            if (dot != nullptr)
            {
                ns = std::string(typeStr, dot - typeStr);
                cls = std::string(dot + 1);
            }
            // 无命名空间
            else cls = typeStr;

            pt.klass = resolver.GetClass(ns, cls);
            if (pt.klass != nullptr)
            {
                // 获取类的类型枚举
                const Il2CppType* type = resolver.GetClassType(pt.klass);
                if (type != nullptr) pt.typeEnum = resolver.GetTypeEnum(type);
            }
        }

        parsedTypes.push_back(std::move(pt));
    }

    // 枚举类上的所有方法 查找匹配的重载
    const int32_t MAX_METHODS = 1024;
    const Il2CppMethod* methods[MAX_METHODS];
    int32_t methodCount = resolver.EnumerateMethods(ud->klass, methods, MAX_METHODS);

    for (int32_t i = 0; i < methodCount; ++i)
    {
        const Il2CppMethod* m = methods[i];

        // 方法名必须匹配
        const char* mname = resolver.GetMethodName(m);
        if (mname == nullptr || strcmp(mname, methodName) != 0) continue;

        // 参数个数必须匹配
        int32_t paramCount = resolver.GetMethodParamCount(m);
        if (paramCount != typeArgCount) continue;

        // 逐个比较参数类型
        bool matched = true;
        for (int32_t j = 0; j < paramCount; ++j)
        {
            const Il2CppType* paramType = resolver.GetMethodParamType(m, j);
            if (paramType == nullptr)
            {
                matched = false;
                break;
            }

            int32_t paramEnum = resolver.GetTypeEnum(paramType);
            const ParsedType& pt = parsedTypes[j];

            if (pt.klass != nullptr)
            {
                // 用户指定的是类类型
                // 比较类的类型枚举和类名
                if (paramEnum != pt.typeEnum)
                {
                    // 类型枚举不匹配
                    // 但可能是父类/子类关系 这里只做精确匹配
                    // 获取参数的类 比较类指针
                    Il2CppClass* paramClass = resolver.GetClassFromType(paramType);
                    if (paramClass != pt.klass)
                    {
                        matched = false;
                        break;
                    }
                }
            }
            else
            {
                // 用户指定的是基本类型
                if (paramEnum != pt.typeEnum)
                {
                    matched = false;
                    break;
                }
            }
        }

        if (matched)
        {
            // 找到匹配的重载
            LuaBridge_PushMethod(L, m, ud->klass);
            return 1;
        }
    }

    return luaL_error(L, "no matching overload found for %s with %d args", methodName, typeArgCount);
}

// mth:__tostring() → string
static int Method_ToString(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();
    const char* name = resolver.GetMethodName(ud->method);
    int32_t pc = resolver.GetMethodParamCount(ud->method);
    bool isStatic = resolver.IsStaticMethod(ud->method);
    lua_pushfstring(L, "Method: %s%s(%d params)", isStatic ? "static " : "", name ? name : "?", pc);
    return 1;
}

// Method 元表方法注册表
static const luaL_Reg method_methods[] = {
    {"get_name",       Method_GetName},
    {"get_param_count",Method_GetParamCount},
    {"get_return_type",Method_GetReturnType},
    {"get_params",     Method_GetParams},
    {"is_static",      Method_IsStatic},
    {"call",           Method_Call},
    {"ovload",         Method_Ovload},
    {"__tostring",     Method_ToString},
    {nullptr, nullptr}
};


// ============================================================
// Field 元表方法
// ============================================================

// fld:get_name() → string
static int Field_GetName(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();
    const char* name = resolver.GetFieldName(ud->field);
    lua_pushstring(L, name ? name : "");
    return 1;
}

// fld:get_type() → string
static int Field_GetType(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();
    const Il2CppType* type = resolver.GetFieldType(ud->field);
    const char* name = type ? resolver.GetTypeName(type) : "unknown";
    lua_pushstring(L, name ? name : "unknown");
    return 1;
}

// fld:get_offset() → number
static int Field_GetOffset(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();
    lua_pushinteger(L, resolver.GetFieldOffset(ud->field));
    return 1;
}

// fld:get(obj) → value
// 读取字段值 obj 为 Instance 或 nil（静态字段）
static int Field_Get(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();

    uint8_t buffer[16] = {};

    // 判断是实例字段还是静态字段
    if (lua_isnil(L, 2))
    {
        // 静态字段
        resolver.ReadStaticField(ud->field, buffer);
    }
    else
    {
        // 实例字段
        LuaInstanceUD* instUD = LuaBridge_CheckInstance(L, 2);
        if (instUD == nullptr) return luaL_error(L, "expected Instance or nil");
        resolver.ReadField(instUD->obj, ud->field, buffer);
    }

    // 根据字段类型推送 Lua 值
    const Il2CppType* fieldType = resolver.GetFieldType(ud->field);
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);

    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        lua_pushboolean(L, *reinterpret_cast<bool*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I4:
        lua_pushinteger(L, *reinterpret_cast<int32_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I8:
        lua_pushinteger(L, *reinterpret_cast<int64_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_R4:
        lua_pushnumber(L, *reinterpret_cast<float*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_R8:
        lua_pushnumber(L, *reinterpret_cast<double*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_STRING:
    {
        Il2CppString* str = *reinterpret_cast<Il2CppString**>(buffer);
        if (str != nullptr) PushIl2CppString(L, str);
        else lua_pushnil(L);
        break;
    }
    default:
    {
        Il2CppObject* obj = *reinterpret_cast<Il2CppObject**>(buffer);
        LuaBridge_PushInstance(L, obj, nullptr);
        break;
    }
    }
    return 1;
}

// fld:set(obj, value)
// 写入字段值
static int Field_Set(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();

    const Il2CppType* fieldType = resolver.GetFieldType(ud->field);
    uint8_t buffer[16] = {};
    void* param = nullptr;

    // 编组值
    if (!MarshalArg(L, 3, fieldType, buffer, param)) return luaL_error(L, "failed to marshal field value");

    // 判断是实例字段还是静态字段
    if (lua_isnil(L, 2))
    {
        resolver.WriteStaticField(ud->field, buffer);
    }
    else
    {
        LuaInstanceUD* instUD = LuaBridge_CheckInstance(L, 2);
        if (instUD == nullptr) return luaL_error(L, "expected Instance or nil");
        resolver.WriteField(instUD->obj, ud->field, buffer);
    }

    return 0;
}

// fld:__tostring() → string
static int Field_ToString(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();
    const char* name = resolver.GetFieldName(ud->field);
    int32_t off = resolver.GetFieldOffset(ud->field);
    char buf[128];
    snprintf(buf, sizeof(buf), "Field: %s @ offset 0x%X", name ? name : "?", off);
    lua_pushstring(L, buf);
    return 1;
}

// Field 元表方法注册表
static const luaL_Reg field_methods[] = {
    {"get_name",   Field_GetName},
    {"get_type",   Field_GetType},
    {"get_offset", Field_GetOffset},
    {"get",        Field_Get},
    {"set",        Field_Set},
    {"__tostring", Field_ToString},
    {nullptr, nullptr}
};


// ============================================================
// 元表创建
// ============================================================

// 创建一张元表并注册方法
// 元表存储在 Lua registry 中 以 mtName 为键
static void CreateMetatable(lua_State* L, const char* mtName, const luaL_Reg* methods)
{
    // 创建元表（存储在 registry 中）
    luaL_newmetatable(L, mtName);

    // 注册所有方法到元表中
    // luaL_setfuncs 会将数组中的每个函数注册到栈顶表中
    luaL_setfuncs(L, methods, 0);

    // 对于 Instance 元表 __index 和 __newindex 已经作为方法注册
    // 对于其他元表 设置 __index = 元表自身（使方法可通过 : 语法访问）
    if (strcmp(mtName, LuaBridgeMT::INSTANCE) != 0)
    {
        // __index 指向元表自身
        lua_pushvalue(L, -1);                    // 复制元表
        lua_setfield(L, -2, "__index");          // 设置 __index = 自身
    }

    // 弹出元表
    lua_pop(L, 1);
}


// ============================================================
// 初始化与关闭
// ============================================================

bool LuaBridge_Init(lua_State* L)
{
    if (L == nullptr) return false;

    // ---- 创建 4 张元表 ----
    CreateMetatable(L, LuaBridgeMT::CLASS,    class_methods);
    CreateMetatable(L, LuaBridgeMT::INSTANCE, instance_methods);
    CreateMetatable(L, LuaBridgeMT::METHOD,   method_methods);
    CreateMetatable(L, LuaBridgeMT::FIELD,    field_methods);

    // ---- 注册 il2cpp 全局表 ----
    lua_newtable(L);                   // 创建 il2cpp 表
    luaL_setfuncs(L, il2cpp_funcs, 0); // 注册函数
    lua_setglobal(L, "il2cpp");        // 设置为全局变量

    return true;
}
