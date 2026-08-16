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
#include "il2cpp_hook.h"

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
void LuaBridge_PushString(lua_State* L, Il2CppString* str)
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

    // 泛型实例化类型（如 List<T> / 泛型 struct）先解析出实际类别
    // 值类型按值类型处理 其余按引用类型处理
    if (typeEnum == Il2CppTypeEnum::TYPE_GENERICINST)
    {
        Il2CppClass* gclass = resolver.GetClassFromType(type);
        const Il2CppType* gtype = gclass ? resolver.GetClassType(gclass) : nullptr;
        if (gtype != nullptr)
        {
            int32_t genEnum = resolver.GetTypeEnum(gtype);
            typeEnum = (genEnum == Il2CppTypeEnum::TYPE_VALUETYPE
                || genEnum == Il2CppTypeEnum::TYPE_ENUM)
                ? Il2CppTypeEnum::TYPE_VALUETYPE
                : Il2CppTypeEnum::TYPE_CLASS;
        }
    }

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

    // 枚举（底层一般为 int32 与旧版 hook 行为一致）
    case Il2CppTypeEnum::TYPE_ENUM:
        *static_cast<int32_t*>(storage) = static_cast<int32_t>(lua_tointeger(L, idx));
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
    case Il2CppTypeEnum::TYPE_ARRAY:
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

    // 原生指针类型 (T* / 函数指针)
    case Il2CppTypeEnum::TYPE_PTR:
    case Il2CppTypeEnum::TYPE_FNPTR:
        // 接受 Lua 整数或 lightuserdata
        if (lua_islightuserdata(L, idx))
        {
            *static_cast<void**>(storage) = lua_touserdata(L, idx);
        }
        else
        {
            *static_cast<void**>(storage) = reinterpret_cast<void*>(static_cast<intptr_t>(lua_tointeger(L, idx)));
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
            // lightuserdata 本身就是要传的值类型数据指针
            // runtime_invoke 对值类型参数要求 params[i] 指向值数据
            // 因此直接透传 而不是把指针值复制到 storage
            outParam = lua_touserdata(L, idx);
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

            // 直接使用拆箱后的数据指针作为参数
            // 不再固定复制 16 字节：超过 16 字节的结构体会溢出 storage
            // 调用期间装箱对象由 Lua userdata 持有 数据指针保持有效
            outParam = unboxed;
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

// 公开包装: 供 il2cpp_hook.cpp 的 original() 显式传参复用同一套编组
bool LuaBridge_MarshalArg(lua_State* L, int idx, const Il2CppType* type, void* storage, void*& outParam)
{
    return MarshalArg(L, idx, type, storage, outParam);
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
        LuaBridge_PushString(L, reinterpret_cast<Il2CppString*>(result));
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

    // 检查类型枚举是否为数组类型
    // SZARRAY = 一维零基数组 ARRAY = 多维数组
    int32_t typeEnum = resolver.GetTypeEnum(type);
    return typeEnum == Il2CppTypeEnum::TYPE_SZARRAY
        || typeEnum == Il2CppTypeEnum::TYPE_ARRAY;
}

uint64_t LuaBridge_GetArrayLength(Il2CppObject* arr)
{
    if (arr == nullptr) return 0;
    auto& resolver = Il2CppResolver::Instance();
    return resolver.ArrayLength(reinterpret_cast<Il2CppArray*>(arr));
}

// ============================================================
// 数组元素类型与大小辅助
// ============================================================

// 获取数组元素类与元素大小
// 引用类型元素固定为指针大小 值类型元素通过 class_value_size 获取实际大小
// 旧版 Unity 缺少导出时回退按引用类型指针处理（保持旧行为）
static bool LuaBridge_GetArrayElementInfo(Il2CppObject* arr, Il2CppClass*& outElemClass, int32_t& outElemSize)
{
    if (arr == nullptr) return false;

    auto& resolver = Il2CppResolver::Instance();

    // 从对象头读取数组类
    Il2CppClass* arrayClass = READ_OFFSET(arr, 0, Il2CppClass*)[0];
    if (arrayClass == nullptr) return false;

    // 通过官方 API 获取元素类（仅数组类有效）
    outElemClass = resolver.GetElementClass(arrayClass);
    if (outElemClass == nullptr)
    {
        // 无法确定元素类型 回退为指针大小（按引用类型数组处理）
        outElemSize = static_cast<int32_t>(sizeof(void*));
        return true;
    }

    // 元素类型枚举
    const Il2CppType* elemType = resolver.GetClassType(outElemClass);
    int32_t typeEnum = elemType ? resolver.GetTypeEnum(elemType) : 0;

    // 引用类型（含多维数组）元素固定为指针大小
    if (IsRefType(typeEnum) || typeEnum == Il2CppTypeEnum::TYPE_ARRAY)
    {
        outElemSize = static_cast<int32_t>(sizeof(void*));
        return true;
    }

    // 基本类型按固定大小映射
    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
    case Il2CppTypeEnum::TYPE_I1:
    case Il2CppTypeEnum::TYPE_U1:
        outElemSize = 1;
        return true;
    case Il2CppTypeEnum::TYPE_CHAR:
    case Il2CppTypeEnum::TYPE_I2:
    case Il2CppTypeEnum::TYPE_U2:
        outElemSize = 2;
        return true;
    case Il2CppTypeEnum::TYPE_I4:
    case Il2CppTypeEnum::TYPE_U4:
    case Il2CppTypeEnum::TYPE_R4:
        outElemSize = 4;
        return true;
    case Il2CppTypeEnum::TYPE_I8:
    case Il2CppTypeEnum::TYPE_U8:
    case Il2CppTypeEnum::TYPE_R8:
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
        outElemSize = 8;
        return true;
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    {
        // 结构体元素：通过 value_size 获取实际大小
        uint32_t align = 0;
        int32_t size = resolver.ClassValueSize(outElemClass, &align);
        if (size <= 0) return false;
        outElemSize = size;
        return true;
    }
    default:
        // 未知类型 回退为指针大小
        outElemSize = static_cast<int32_t>(sizeof(void*));
        return true;
    }
}

// 将数组第 index 个元素转换为 Lua 值压栈
// 返回 false 表示元素类型不支持
static bool LuaBridge_PushArrayElement(lua_State* L, Il2CppObject* arr, int64_t index)
{
    Il2CppClass* elemClass = nullptr;
    int32_t elemSize = 0;
    if (!LuaBridge_GetArrayElementInfo(arr, elemClass, elemSize)) return false;

    auto& resolver = Il2CppResolver::Instance();
    uint8_t* elemPtr = reinterpret_cast<uint8_t*>(arr) + ARRAY_DATA_OFFSET + index * elemSize;

    // 元素类未知 按引用类型指针处理（保持旧行为）
    if (elemClass == nullptr)
    {
        Il2CppObject* elem = *reinterpret_cast<Il2CppObject**>(elemPtr);
        LuaBridge_PushInstance(L, elem, nullptr);
        return true;
    }

    const Il2CppType* elemType = resolver.GetClassType(elemClass);
    int32_t typeEnum = elemType ? resolver.GetTypeEnum(elemType) : 0;

    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        lua_pushboolean(L, *reinterpret_cast<bool*>(elemPtr));
        return true;
    case Il2CppTypeEnum::TYPE_CHAR:
    case Il2CppTypeEnum::TYPE_U2:
        lua_pushinteger(L, *reinterpret_cast<uint16_t*>(elemPtr));
        return true;
    case Il2CppTypeEnum::TYPE_I1:
        lua_pushinteger(L, *reinterpret_cast<int8_t*>(elemPtr));
        return true;
    case Il2CppTypeEnum::TYPE_U1:
        lua_pushinteger(L, *reinterpret_cast<uint8_t*>(elemPtr));
        return true;
    case Il2CppTypeEnum::TYPE_I2:
        lua_pushinteger(L, *reinterpret_cast<int16_t*>(elemPtr));
        return true;
    case Il2CppTypeEnum::TYPE_I4:
        lua_pushinteger(L, *reinterpret_cast<int32_t*>(elemPtr));
        return true;
    case Il2CppTypeEnum::TYPE_U4:
        lua_pushinteger(L, *reinterpret_cast<uint32_t*>(elemPtr));
        return true;
    case Il2CppTypeEnum::TYPE_I8:
        lua_pushinteger(L, *reinterpret_cast<int64_t*>(elemPtr));
        return true;
    case Il2CppTypeEnum::TYPE_U8:
        // lua_Integer 为有符号 64 位 超过 INT64_MAX 的值会回绕
        lua_pushinteger(L, static_cast<lua_Integer>(*reinterpret_cast<uint64_t*>(elemPtr)));
        return true;
    case Il2CppTypeEnum::TYPE_R4:
        lua_pushnumber(L, *reinterpret_cast<float*>(elemPtr));
        return true;
    case Il2CppTypeEnum::TYPE_R8:
        lua_pushnumber(L, *reinterpret_cast<double*>(elemPtr));
        return true;
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
        lua_pushinteger(L, static_cast<lua_Integer>(*reinterpret_cast<intptr_t*>(elemPtr)));
        return true;
    case Il2CppTypeEnum::TYPE_STRING:
    {
        Il2CppString* str = *reinterpret_cast<Il2CppString**>(elemPtr);
        if (str != nullptr) LuaBridge_PushString(L, str);
        else lua_pushnil(L);
        return true;
    }
    case Il2CppTypeEnum::TYPE_CLASS:
    case Il2CppTypeEnum::TYPE_OBJECT:
    case Il2CppTypeEnum::TYPE_SZARRAY:
    case Il2CppTypeEnum::TYPE_ARRAY:
    {
        Il2CppObject* elem = *reinterpret_cast<Il2CppObject**>(elemPtr);
        LuaBridge_PushInstance(L, elem, elemClass);
        return true;
    }
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    {
        // 结构体元素：装箱后包装为 Instance
        Il2CppObject* boxed = resolver.Box(elemClass, elemPtr);
        LuaBridge_PushInstance(L, boxed, elemClass);
        return true;
    }
    default:
        return false;
    }
}

// 将 Lua 栈上 valueIdx 位置的值写入数组第 index 个元素
// 返回 false 表示元素类型不支持或值类型不匹配
static bool LuaBridge_SetArrayElement(lua_State* L, Il2CppObject* arr, int64_t index, int valueIdx)
{
    Il2CppClass* elemClass = nullptr;
    int32_t elemSize = 0;
    if (!LuaBridge_GetArrayElementInfo(arr, elemClass, elemSize)) return false;

    auto& resolver = Il2CppResolver::Instance();
    uint8_t* elemPtr = reinterpret_cast<uint8_t*>(arr) + ARRAY_DATA_OFFSET + index * elemSize;

    // 元素类未知 按引用类型指针处理（保持旧行为）
    if (elemClass == nullptr)
    {
        if (lua_isnil(L, valueIdx)) *reinterpret_cast<Il2CppObject**>(elemPtr) = nullptr;
        else
        {
            LuaInstanceUD* valUD = LuaBridge_CheckInstance(L, valueIdx);
            if (valUD == nullptr) return false;
            *reinterpret_cast<Il2CppObject**>(elemPtr) = valUD->obj;
        }
        return true;
    }

    const Il2CppType* elemType = resolver.GetClassType(elemClass);
    int32_t typeEnum = elemType ? resolver.GetTypeEnum(elemType) : 0;

    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        *reinterpret_cast<bool*>(elemPtr) = lua_toboolean(L, valueIdx) != 0;
        return true;
    case Il2CppTypeEnum::TYPE_CHAR:
    case Il2CppTypeEnum::TYPE_U2:
        *reinterpret_cast<uint16_t*>(elemPtr) = static_cast<uint16_t>(lua_tointeger(L, valueIdx));
        return true;
    case Il2CppTypeEnum::TYPE_I1:
        *reinterpret_cast<int8_t*>(elemPtr) = static_cast<int8_t>(lua_tointeger(L, valueIdx));
        return true;
    case Il2CppTypeEnum::TYPE_U1:
        *reinterpret_cast<uint8_t*>(elemPtr) = static_cast<uint8_t>(lua_tointeger(L, valueIdx));
        return true;
    case Il2CppTypeEnum::TYPE_I2:
        *reinterpret_cast<int16_t*>(elemPtr) = static_cast<int16_t>(lua_tointeger(L, valueIdx));
        return true;
    case Il2CppTypeEnum::TYPE_I4:
        *reinterpret_cast<int32_t*>(elemPtr) = static_cast<int32_t>(lua_tointeger(L, valueIdx));
        return true;
    case Il2CppTypeEnum::TYPE_U4:
        *reinterpret_cast<uint32_t*>(elemPtr) = static_cast<uint32_t>(lua_tointeger(L, valueIdx));
        return true;
    case Il2CppTypeEnum::TYPE_I8:
        *reinterpret_cast<int64_t*>(elemPtr) = static_cast<int64_t>(lua_tointeger(L, valueIdx));
        return true;
    case Il2CppTypeEnum::TYPE_U8:
        *reinterpret_cast<uint64_t*>(elemPtr) = static_cast<uint64_t>(lua_tointeger(L, valueIdx));
        return true;
    case Il2CppTypeEnum::TYPE_R4:
        *reinterpret_cast<float*>(elemPtr) = static_cast<float>(lua_tonumber(L, valueIdx));
        return true;
    case Il2CppTypeEnum::TYPE_R8:
        *reinterpret_cast<double*>(elemPtr) = lua_tonumber(L, valueIdx);
        return true;
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
        *reinterpret_cast<intptr_t*>(elemPtr) = static_cast<intptr_t>(lua_tointeger(L, valueIdx));
        return true;
    case Il2CppTypeEnum::TYPE_STRING:
    {
        if (lua_isnil(L, valueIdx)) *reinterpret_cast<Il2CppString**>(elemPtr) = nullptr;
        else
        {
            const char* str = lua_tostring(L, valueIdx);
            *reinterpret_cast<Il2CppString**>(elemPtr) = resolver.StringNew(str ? str : "");
        }
        return true;
    }
    case Il2CppTypeEnum::TYPE_CLASS:
    case Il2CppTypeEnum::TYPE_OBJECT:
    case Il2CppTypeEnum::TYPE_SZARRAY:
    case Il2CppTypeEnum::TYPE_ARRAY:
    {
        if (lua_isnil(L, valueIdx)) *reinterpret_cast<Il2CppObject**>(elemPtr) = nullptr;
        else
        {
            LuaInstanceUD* valUD = LuaBridge_CheckInstance(L, valueIdx);
            if (valUD == nullptr) return false;
            *reinterpret_cast<Il2CppObject**>(elemPtr) = valUD->obj;
        }
        return true;
    }
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    {
        if (lua_islightuserdata(L, valueIdx))
        {
            // 直接拷贝用户提供的原始结构体内存
            memcpy(elemPtr, lua_touserdata(L, valueIdx), elemSize);
        }
        else if (lua_isnil(L, valueIdx))
        {
            memset(elemPtr, 0, elemSize);
        }
        else
        {
            // 已装箱的值类型 拆箱后拷贝
            LuaInstanceUD* valUD = LuaBridge_CheckInstance(L, valueIdx);
            if (valUD == nullptr || valUD->obj == nullptr) return false;
            void* unboxed = resolver.Unbox(valUD->obj);
            if (unboxed == nullptr) return false;
            memcpy(elemPtr, unboxed, elemSize);
        }
        return true;
    }
    default:
        return false;
    }
}

// ============================================================
// 通用遍历辅助（数组 / List<T> / Lua 表）
// ============================================================
// 供 obj:each、il2cpp.each、il2cpp.dump 共用
// 回调签名统一为 function(value, index) 索引从 1 开始

// 判断 Instance 是否为 System.Collections.Generic.List<T>
static bool LuaBridge_IsList(Il2CppObject* obj, Il2CppClass* klass)
{
    if (obj == nullptr) return false;

    auto& resolver = Il2CppResolver::Instance();
    if (klass == nullptr) klass = READ_OFFSET(obj, 0, Il2CppClass*)[0];
    if (klass == nullptr) return false;

    const char* name = resolver.GetKlassName(klass);
    const char* ns = resolver.GetClassNamespace(klass);
    return name != nullptr && ns != nullptr
        && strcmp(name, "List`1") == 0
        && strcmp(ns, "System.Collections.Generic") == 0;
}

// 获取 List<T> 元素个数（失败返回 -1）
static int64_t LuaBridge_GetListCount(Il2CppObject* list, Il2CppClass* klass)
{
    if (list == nullptr) return -1;

    auto& resolver = Il2CppResolver::Instance();
    if (klass == nullptr) klass = READ_OFFSET(list, 0, Il2CppClass*)[0];
    if (klass == nullptr) return -1;

    const Il2CppMethod* getCount = resolver.GetMethod(klass, "get_Count");
    if (getCount == nullptr) return -1;

    Il2CppException* exc = nullptr;
    Il2CppObject* result = resolver.RuntimeInvoke(getCount, list, nullptr, &exc);
    if (exc != nullptr || result == nullptr) return -1;

    const Il2CppType* retType = resolver.GetMethodReturnType(getCount);
    int32_t enumv = retType ? resolver.GetTypeEnum(retType) : 0;
    void* data = resolver.Unbox(result);
    if (data == nullptr) return -1;

    switch (enumv)
    {
    case Il2CppTypeEnum::TYPE_I4: return *static_cast<int32_t*>(data);
    case Il2CppTypeEnum::TYPE_U4: return *static_cast<uint32_t*>(data);
    case Il2CppTypeEnum::TYPE_I8: return *static_cast<int64_t*>(data);
    case Il2CppTypeEnum::TYPE_U8: return static_cast<int64_t>(*static_cast<uint64_t*>(data));
    default: return -1;
    }
}

// 遍历 IL2CPP 数组: 对每个元素调用 fn(value, index)
static void LuaBridge_EachArray(lua_State* L, Il2CppObject* arr, int fnIdx, int64_t& outCount)
{
    uint64_t len = LuaBridge_GetArrayLength(arr);
    outCount = static_cast<int64_t>(len);

    for (uint64_t i = 0; i < len; ++i)
    {
        // 读取数组元素压栈（与 obj[i] 使用同一套编组）
        if (!LuaBridge_PushArrayElement(L, arr, static_cast<int64_t>(i)))
        {
            luaL_error(L, "unsupported array element type at index %llu", static_cast<unsigned long long>(i));
            return;
        }

        // 调用 fn(value, index)
        lua_pushvalue(L, fnIdx);
        lua_pushvalue(L, -2);
        lua_pushinteger(L, static_cast<lua_Integer>(i) + 1);
        int status = lua_pcall(L, 2, 0, 0);
        if (status != LUA_OK)
        {
            const char* err = lua_tostring(L, -1);
            char errBuf[512];
            snprintf(errBuf, sizeof(errBuf), "each callback error: %s", err ? err : "(non-string error)");
            lua_pop(L, 1);      // 错误
            lua_pop(L, 1);      // value
            luaL_error(L, "%s", errBuf);
            return;
        }
        lua_pop(L, 1);          // value
    }
}

// 遍历 List<T>: 通过 get_Count / get_Item 逐个取出元素调用 fn(value, index)
static void LuaBridge_EachList(lua_State* L, Il2CppObject* list, Il2CppClass* klass, int fnIdx, int64_t& outCount)
{
    auto& resolver = Il2CppResolver::Instance();
    if (klass == nullptr) klass = READ_OFFSET(list, 0, Il2CppClass*)[0];
    if (klass == nullptr)
    {
        luaL_error(L, "cannot determine class for List");
        return;
    }

    const Il2CppMethod* getCount = resolver.GetMethod(klass, "get_Count");
    const Il2CppMethod* getItem = resolver.GetMethod(klass, "get_Item");
    if (getCount == nullptr || getItem == nullptr)
    {
        luaL_error(L, "List get_Count/get_Item not found");
        return;
    }

    int64_t count = LuaBridge_GetListCount(list, klass);
    if (count < 0)
    {
        luaL_error(L, "failed to get List count");
        return;
    }
    outCount = count;

    for (int64_t i = 0; i < count; ++i)
    {
        // 调用 get_Item(i) 取出元素
        int32_t idx = static_cast<int32_t>(i);
        void* args[1] = { &idx };
        Il2CppException* exc = nullptr;
        Il2CppObject* elem = resolver.RuntimeInvoke(getItem, list, args, &exc);
        if (exc != nullptr)
        {
            luaL_error(L, "List get_Item(%lld) threw a C# exception", static_cast<long long>(i));
            return;
        }

        // 元素转 Lua 值（与 mth:call 使用同一套返回值转换）
        const Il2CppType* retType = resolver.GetMethodReturnType(getItem);
        LuaBridge_PushReturnValue(L, elem, retType);

        // 调用 fn(value, index)
        lua_pushvalue(L, fnIdx);
        lua_pushvalue(L, -2);
        lua_pushinteger(L, static_cast<lua_Integer>(i) + 1);
        int status = lua_pcall(L, 2, 0, 0);
        if (status != LUA_OK)
        {
            const char* err = lua_tostring(L, -1);
            char errBuf[512];
            snprintf(errBuf, sizeof(errBuf), "each callback error: %s", err ? err : "(non-string error)");
            lua_pop(L, 1);      // 错误
            lua_pop(L, 1);      // value
            luaL_error(L, "%s", errBuf);
            return;
        }
        lua_pop(L, 1);          // value
    }
}

// 遍历 Lua 表: 先数组部分（1..# 保持顺序）再键值部分
// 回调 fn(value, index_or_key)
static int LuaBridge_EachTable(lua_State* L, int tblIdx, int fnIdx)
{
    luaL_checktype(L, tblIdx, LUA_TTABLE);
    luaL_checktype(L, fnIdx, LUA_TFUNCTION);

    lua_Integer len = lua_rawlen(L, tblIdx);

    // 数组部分
    for (lua_Integer i = 1; i <= len; ++i)
    {
        lua_rawgeti(L, tblIdx, i);
        // 数组边界内出现空洞 跳过
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            continue;
        }

        lua_pushvalue(L, fnIdx);
        lua_pushvalue(L, -2);
        lua_pushinteger(L, i);
        if (lua_pcall(L, 2, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L, -1);
            char errBuf[512];
            snprintf(errBuf, sizeof(errBuf), "each callback error: %s", err ? err : "(non-string error)");
            lua_pop(L, 1);      // 错误
            lua_pop(L, 1);      // value
            return luaL_error(L, "%s", errBuf);
        }
        lua_pop(L, 1);          // value
    }

    // 键值部分（跳过数组部分已遍历的整数键）
    lua_pushnil(L);
    while (lua_next(L, tblIdx) != 0)
    {
        // 栈: ... key value
        bool skip = false;
        if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2))
        {
            lua_Integer k = lua_tointeger(L, -2);
            if (k >= 1 && k <= len) skip = true;
        }

        if (!skip)
        {
            lua_pushvalue(L, fnIdx);
            lua_pushvalue(L, -3);   // value
            lua_pushvalue(L, -4);   // key
            if (lua_pcall(L, 2, 0, 0) != LUA_OK)
            {
                const char* err = lua_tostring(L, -1);
                char errBuf[512];
                snprintf(errBuf, sizeof(errBuf), "each callback error: %s", err ? err : "(non-string error)");
                lua_pop(L, 1);      // 错误
                lua_pop(L, 1);      // value
                lua_pop(L, 1);      // key
                return luaL_error(L, "%s", errBuf);
            }
        }

        // 弹出 value 保留 key 供 lua_next 继续遍历
        lua_pop(L, 1);
    }
    return 0;
}

// ============================================================
// 字段值缓冲区大小辅助
// ============================================================

// 计算 il2cpp_field_get_value 所需的缓冲区大小
// 引用类型/基本类型按固定大小 结构体通过 class_value_size 获取实际大小
// 无法确定大小时回退 16 字节（保持旧行为）
static size_t LuaBridge_GetFieldValueSize(const Il2CppType* fieldType, int32_t typeEnum)
{
    auto& resolver = Il2CppResolver::Instance();

    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
    case Il2CppTypeEnum::TYPE_I1:
    case Il2CppTypeEnum::TYPE_U1:
        return 1;
    case Il2CppTypeEnum::TYPE_CHAR:
    case Il2CppTypeEnum::TYPE_I2:
    case Il2CppTypeEnum::TYPE_U2:
        return 2;
    case Il2CppTypeEnum::TYPE_I4:
    case Il2CppTypeEnum::TYPE_U4:
    case Il2CppTypeEnum::TYPE_R4:
        return 4;
    case Il2CppTypeEnum::TYPE_I8:
    case Il2CppTypeEnum::TYPE_U8:
    case Il2CppTypeEnum::TYPE_R8:
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
        return 8;
    case Il2CppTypeEnum::TYPE_STRING:
    case Il2CppTypeEnum::TYPE_CLASS:
    case Il2CppTypeEnum::TYPE_OBJECT:
    case Il2CppTypeEnum::TYPE_SZARRAY:
    case Il2CppTypeEnum::TYPE_ARRAY:
        return sizeof(void*);
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    {
        Il2CppClass* valueKlass = resolver.GetClassFromType(fieldType);
        if (valueKlass != nullptr)
        {
            uint32_t align = 0;
            int32_t size = resolver.ClassValueSize(valueKlass, &align);
            if (size > 0) return static_cast<size_t>(size);
        }
        // 无法获取大小时回退 16 字节
        return 16;
    }
    default:
        // 未知类型回退 16 字节（与旧固定缓冲区一致）
        return 16;
    }
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

// ============================================================
// il2cpp.each / il2cpp.dump（通用遍历）
// ============================================================
// 支持 Lua 表（get_methods / get_fields 等返回的结果表）
// 以及 IL2CPP 数组、System.Collections.Generic.List<T> 实例
// 回调签名统一为 function(value, index) 索引从 1 开始

// il2cpp.each(container, fn) → 无返回值
// 对容器中的每个元素调用 fn(value, index)
static int Il2Cpp_Each(lua_State* L)
{
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // Lua 表
    if (lua_istable(L, 1)) return LuaBridge_EachTable(L, 1, 2);

    // IL2CPP 数组 / List<T>
    LuaInstanceUD* ud = LuaBridge_CheckInstance(L, 1);
    if (ud != nullptr && ud->obj != nullptr)
    {
        if (LuaBridge_IsArray(ud->obj))
        {
            int64_t count = 0;
            LuaBridge_EachArray(L, ud->obj, 2, count);
            return 0;
        }
        if (LuaBridge_IsList(ud->obj, ud->klass))
        {
            int64_t count = 0;
            LuaBridge_EachList(L, ud->obj, ud->klass, 2, count);
            return 0;
        }
    }

    return luaL_error(L, "each: expected table, array instance or List instance");
}

// il2cpp.dump 输出缓冲（固定大小 超出截断）
struct DumpBuffer
{
    char data[262144];  // 256KB 足够打印绝大多数容器的全部元素
    size_t len;
};

// 向 dump 缓冲追加格式化文本
static void DumpAppend(DumpBuffer* buf, const char* fmt, ...)
{
    if (buf == nullptr || buf->len >= sizeof(buf->data)) return;

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf->data + buf->len, sizeof(buf->data) - buf->len, fmt, args);
    va_end(args);
    if (n <= 0) return;

    size_t room = sizeof(buf->data) - buf->len;
    buf->len += static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room - 1;
}

// il2cpp.dump 的格式化回调: function(value, index) → 追加一行 "[index] = value"
static int DumpFormatCallback(lua_State* L)
{
    DumpBuffer* buf = static_cast<DumpBuffer*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (buf == nullptr) return 0;

    // 索引/键（整数或字符串）
    std::string key;
    if (lua_type(L, 2) == LUA_TNUMBER && lua_isinteger(L, 2))
    {
        key = std::to_string(static_cast<long long>(lua_tointeger(L, 2)));
    }
    else
    {
        size_t keyLen = 0;
        const char* keyStr = luaL_tolstring(L, 2, &keyLen);
        key.assign(keyStr ? keyStr : "?", keyStr ? keyLen : 1);
        lua_pop(L, 1);  // tostring 结果
    }

    // 值（userdata 会走 __tostring 元方法）
    size_t valLen = 0;
    const char* valStr = luaL_tolstring(L, 1, &valLen);
    DumpAppend(buf, "[%s] = %.*s\n", key.c_str(),
        static_cast<int>(valLen), valStr ? valStr : "?");
    lua_pop(L, 1);  // tostring 结果

    return 0;
}

// il2cpp.dump(container) → 无返回值
// 输出容器长度与每个元素 支持 Lua 表 / IL2CPP 数组 / List<T>
// 其他值直接输出其字符串表示
static int Il2Cpp_Dump(lua_State* L)
{
    // 非容器: 直接打印单个值（Method / Field / Class / 普通 Instance 等）
    if (!lua_istable(L, 1))
    {
        LuaInstanceUD* ud = LuaBridge_CheckInstance(L, 1);
        bool isContainer = (ud != nullptr && ud->obj != nullptr)
            && (LuaBridge_IsArray(ud->obj) || LuaBridge_IsList(ud->obj, ud->klass));
        if (!isContainer)
        {
            lua_getglobal(L, "print");
            lua_pushvalue(L, 1);
            lua_call(L, 1, 0);
            return 0;
        }
    }

    // 容器长度（先输出 再逐元素）
    int64_t count = -1;
    bool isTable = lua_istable(L, 1);
    LuaInstanceUD* ud = nullptr;
    if (isTable)
    {
        count = static_cast<int64_t>(lua_rawlen(L, 1));
    }
    else
    {
        ud = LuaBridge_CheckInstance(L, 1);
        if (ud != nullptr && ud->obj != nullptr)
        {
            if (LuaBridge_IsArray(ud->obj))
                count = static_cast<int64_t>(LuaBridge_GetArrayLength(ud->obj));
            else if (LuaBridge_IsList(ud->obj, ud->klass))
                count = LuaBridge_GetListCount(ud->obj, ud->klass);
        }
    }
    if (count < 0)
    {
        return luaL_error(L, "dump: expected table, array instance or List instance");
    }

    // 输出缓冲作为回调闭包的 upvalue
    DumpBuffer* buf = static_cast<DumpBuffer*>(lua_newuserdata(L, sizeof(DumpBuffer)));
    buf->len = 0;
    lua_pushcclosure(L, DumpFormatCallback, 1);
    int fnIdx = lua_gettop(L);

    DumpAppend(buf, "length: %lld\n", static_cast<long long>(count));

    // 逐元素追加
    if (isTable)
    {
        LuaBridge_EachTable(L, 1, fnIdx);
    }
    else if (ud != nullptr && ud->obj != nullptr)
    {
        if (LuaBridge_IsArray(ud->obj))
        {
            int64_t c = 0;
            LuaBridge_EachArray(L, ud->obj, fnIdx, c);
        }
        else
        {
            int64_t c = 0;
            LuaBridge_EachList(L, ud->obj, ud->klass, fnIdx, c);
        }
    }

    // 一次性输出（走 print 同一输出通道）
    lua_getglobal(L, "print");
    lua_pushlstring(L, buf->data, buf->len);
    lua_call(L, 1, 0);

    return 0;
}

// il2cpp 全局表的函数注册表
// il2cpp.unhook_all()
// 卸载全部已安装的方法 Hook
static int Il2Cpp_UnhookAll(lua_State* L)
{
    Il2CppHook::UnhookAll();
    return 0;
}

// ============================================================
// il2cpp.mainThread 子表函数
// ============================================================
// 参考 frida-il2cpp-bridge 的 Il2Cpp.mainThread.schedule
// schedule 只负责入队 由内部 tick hook 在 Unity 主线程取出来执行

// il2cpp.mainThread.schedule(fn) → 无返回值
// 把一个 Lua 函数加入主线程执行队列
static int MainThread_Schedule(lua_State* L)
{
    if (!lua_isfunction(L, 1)) return luaL_error(L, "expected function as argument");

    Il2CppHook::MainThreadSchedule(L, 1);
    return 0;
}

// il2cpp.mainThread.set_tick(namespace, class, method) → boolean
// 指定内部 tick 入口方法（游戏不调用默认入口时使用）
static int MainThread_SetTick(lua_State* L)
{
    const char* ns = luaL_optstring(L, 1, "");
    const char* klass = luaL_checkstring(L, 2);
    const char* method = luaL_checkstring(L, 3);

    bool ok = Il2CppHook::SetMainThreadTickTarget(ns, klass, method);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// il2cpp.mainThread.get_tick() → namespace, class, method | nil
// 查询当前内部 tick 入口方法
static int MainThread_GetTick(lua_State* L)
{
    std::string ns;
    std::string klass;
    std::string method;

    if (!Il2CppHook::GetMainThreadTickTarget(ns, klass, method))
    {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, ns.c_str());
    lua_pushstring(L, klass.c_str());
    lua_pushstring(L, method.c_str());
    return 3;
}

// il2cpp.mainThread.is_ready() → boolean
// 查询内部 tick hook 是否已安装
static int MainThread_IsReady(lua_State* L)
{
    lua_pushboolean(L, Il2CppHook::IsMainThreadTickInstalled() ? 1 : 0);
    return 1;
}

// il2cpp 全局表的函数注册表
static const luaL_Reg il2cpp_funcs[] = {
    {"get_status", Il2Cpp_GetStatus},          // 产看解析导出函数状态
    {"get_class", Il2Cpp_GetClass},            // 查找类
    {"get_assemblies", Il2Cpp_GetAssemblies},  // 获取程序集数量
    {"get_image_count", Il2Cpp_GetImageCount}, // 获取镜像数量
    {"unhook_all", Il2Cpp_UnhookAll},          // 卸载全部 Hook
    {"is_initialized", Il2Cpp_IsInitialized},  // 检查初始化状态
    {"wrap", Il2Cpp_Wrap},                     // 裸指针包装
    {"find_objects", Il2Cpp_FindObjects},      // 查找对象
    {"each", Il2Cpp_Each},                     // 通用遍历（表/数组/List）
    {"dump", Il2Cpp_Dump},                     // 输出容器长度与全部元素
    {nullptr, nullptr}                         // 结束标记
};

// il2cpp.mainThread 子表的函数注册表
static const luaL_Reg main_thread_funcs[] = {
    {"schedule", MainThread_Schedule}, // 排队到主线程执行
    {"set_tick", MainThread_SetTick},  // 指定内部 tick 入口方法
    {"get_tick", MainThread_GetTick},  // 查询内部 tick 入口方法
    {"is_ready", MainThread_IsReady},  // 查询内部 tick 是否已安装
    {nullptr, nullptr}                 // 结束标记
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

// ============================================================
// 重载方法选择（obj:call / cls:static_call 使用）
// ============================================================
// 按方法名 + 参数个数 + Lua 实参类型兼容性自动选择最佳重载
// 避免同名重载方法总是命中第一个的问题
//
// 选择策略
// ·同名且参数个数精确匹配的方法优先
// ·多个匹配时按 Lua 实参类型打分 完全匹配 > 可转换 > 不兼容
// ·没有任何参数个数匹配的重载时 回退到第一个同名方法（保持旧行为 由调用层报错）

// 按 Lua 实参类型给单个方法打分（分数越高越匹配）
static int ScoreMethodAgainstArgs(lua_State* L, const Il2CppMethod* method, int firstArgIdx)
{
    auto& resolver = Il2CppResolver::Instance();
    int32_t paramCount = resolver.GetMethodParamCount(method);
    int score = 0;

    for (int32_t i = 0; i < paramCount; ++i)
    {
        const Il2CppType* paramType = resolver.GetMethodParamType(method, i);
        if (paramType == nullptr) return INT32_MIN;

        // ref/out 参数取目标类型
        int32_t paramEnum = resolver.GetTypeEnum(paramType);
        if (paramEnum == Il2CppTypeEnum::TYPE_BYREF)
        {
            Il2CppClass* targetClass = resolver.GetClassFromType(paramType);
            const Il2CppType* targetType = targetClass ? resolver.GetClassType(targetClass) : nullptr;
            if (targetType != nullptr) paramEnum = resolver.GetTypeEnum(targetType);
        }

        int argType = lua_type(L, firstArgIdx + i);
        int local = -100;

        switch (argType)
        {
        case LUA_TNIL:
            // nil 可编组为引用类型 nullptr 或值类型 0 引用类型更匹配
            if (IsRefType(paramEnum)
                || paramEnum == Il2CppTypeEnum::TYPE_OBJECT
                || paramEnum == Il2CppTypeEnum::TYPE_ARRAY)
                local = 2;
            else local = 1;
            break;

        case LUA_TBOOLEAN:
            if (paramEnum == Il2CppTypeEnum::TYPE_BOOLEAN) local = 3;
            break;

        case LUA_TNUMBER:
            if (lua_isinteger(L, firstArgIdx + i))
            {
                switch (paramEnum)
                {
                case Il2CppTypeEnum::TYPE_I4:
                case Il2CppTypeEnum::TYPE_U4: local = 3; break;
                case Il2CppTypeEnum::TYPE_I1:
                case Il2CppTypeEnum::TYPE_I2:
                case Il2CppTypeEnum::TYPE_I8:
                case Il2CppTypeEnum::TYPE_U1:
                case Il2CppTypeEnum::TYPE_U2:
                case Il2CppTypeEnum::TYPE_U8:
                case Il2CppTypeEnum::TYPE_I:
                case Il2CppTypeEnum::TYPE_U:
                case Il2CppTypeEnum::TYPE_CHAR: local = 2; break;
                case Il2CppTypeEnum::TYPE_R4:
                case Il2CppTypeEnum::TYPE_R8: local = 1; break;
                default: break;
                }
            }
            else
            {
                switch (paramEnum)
                {
                case Il2CppTypeEnum::TYPE_R4:
                case Il2CppTypeEnum::TYPE_R8: local = 3; break;
                case Il2CppTypeEnum::TYPE_I4:
                case Il2CppTypeEnum::TYPE_I8:
                case Il2CppTypeEnum::TYPE_U4:
                case Il2CppTypeEnum::TYPE_U8:
                case Il2CppTypeEnum::TYPE_I:
                case Il2CppTypeEnum::TYPE_U: local = 1; break;
                default: break;
                }
            }
            break;

        case LUA_TSTRING:
            if (paramEnum == Il2CppTypeEnum::TYPE_STRING) local = 3;
            else if (paramEnum == Il2CppTypeEnum::TYPE_OBJECT
                  || paramEnum == Il2CppTypeEnum::TYPE_CLASS) local = 1;
            break;

        case LUA_TLIGHTUSERDATA:
            if (paramEnum == Il2CppTypeEnum::TYPE_I
                || paramEnum == Il2CppTypeEnum::TYPE_U
                || paramEnum == Il2CppTypeEnum::TYPE_OBJECT
                || paramEnum == Il2CppTypeEnum::TYPE_CLASS)
                local = 2;
            break;

        case LUA_TTABLE:
        case LUA_TFUNCTION:
            if (paramEnum == Il2CppTypeEnum::TYPE_OBJECT) local = 1;
            break;

        case LUA_TUSERDATA:
        {
            LuaInstanceUD* instUD = LuaBridge_CheckInstance(L, firstArgIdx + i);
            if (instUD == nullptr || instUD->obj == nullptr)
            {
                // Method / Field / Class 等其他 userdata 按 object 处理
                if (paramEnum == Il2CppTypeEnum::TYPE_OBJECT) local = 1;
            }
            else if (paramEnum == Il2CppTypeEnum::TYPE_STRING)
            {
                // 字符串参数需要 Il2CppString* 普通对象不能直接传
                local = -100;
            }
            else if (paramEnum == Il2CppTypeEnum::TYPE_OBJECT)
            {
                local = 1;
            }
            else if (paramEnum == Il2CppTypeEnum::TYPE_CLASS
                  || paramEnum == Il2CppTypeEnum::TYPE_SZARRAY
                  || paramEnum == Il2CppTypeEnum::TYPE_ARRAY)
            {
                Il2CppClass* instClass = instUD->klass;
                if (instClass == nullptr) instClass = READ_OFFSET(instUD->obj, 0, Il2CppClass*)[0];
                Il2CppClass* paramClass = resolver.GetClassFromType(paramType);
                if (instClass != nullptr && paramClass != nullptr)
                {
                    if (instClass == paramClass) local = 3;
                    else
                    {
                        // 沿父类链查找 子类实例可匹配父类参数
                        local = 2;
                        Il2CppClass* parent = resolver.GetClassParent(instClass);
                        while (parent != nullptr)
                        {
                            if (parent == paramClass) break;
                            parent = resolver.GetClassParent(parent);
                        }
                        if (parent == nullptr) local = -100;
                    }
                }
                else local = 1;
            }
            else if (paramEnum == Il2CppTypeEnum::TYPE_VALUETYPE
                  || paramEnum == Il2CppTypeEnum::TYPE_ENUM
                  || paramEnum == Il2CppTypeEnum::TYPE_GENERICINST)
            {
                // 值类型 / 枚举 / 泛型实例参数: 仅类完全一致时匹配
                Il2CppClass* instClass = instUD->klass;
                if (instClass == nullptr) instClass = READ_OFFSET(instUD->obj, 0, Il2CppClass*)[0];
                Il2CppClass* paramClass = resolver.GetClassFromType(paramType);
                if (instClass != nullptr && paramClass != nullptr && instClass == paramClass) local = 3;
            }
            break;
        }

        default:
            break;
        }

        score += local;
        // 某个参数完全不兼容 直接放弃该候选
        if (local < 0) return INT32_MIN;
    }

    return score;
}

// 选择最佳重载方法（找不到返回 nullptr）
static const Il2CppMethod* ResolveMethodOverload(lua_State* L, Il2CppClass* klass, const char* name, int firstArgIdx)
{
    auto& resolver = Il2CppResolver::Instance();

    const int32_t MAX_METHODS = 1024;
    const Il2CppMethod* methods[MAX_METHODS];
    int32_t methodCount = resolver.EnumerateMethods(klass, methods, MAX_METHODS);

    // Lua 实参个数
    int luaArgCount = lua_gettop(L) - firstArgIdx + 1;

    // 第一遍: 同名且参数个数匹配的候选
    std::vector<const Il2CppMethod*> candidates;
    for (int32_t i = 0; i < methodCount; ++i)
    {
        const Il2CppMethod* m = methods[i];
        const char* mname = resolver.GetMethodName(m);
        if (mname == nullptr || strcmp(mname, name) != 0) continue;
        if (resolver.GetMethodParamCount(m) != luaArgCount) continue;
        candidates.push_back(m);
    }

    // 恰好一个 → 直接使用
    if (candidates.size() == 1) return candidates[0];

    // 多个 → 按类型兼容性打分 取最高分（并列取第一个）
    if (candidates.size() > 1)
    {
        const Il2CppMethod* best = nullptr;
        int bestScore = INT32_MIN;
        for (const Il2CppMethod* m : candidates)
        {
            int score = ScoreMethodAgainstArgs(L, m, firstArgIdx);
            if (score > bestScore)
            {
                bestScore = score;
                best = m;
            }
        }
        if (best != nullptr) return best;
    }

    // 没有参数个数匹配的重载 → 回退第一个同名方法（保持旧行为）
    for (int32_t i = 0; i < methodCount; ++i)
    {
        const Il2CppMethod* m = methods[i];
        const char* mname = resolver.GetMethodName(m);
        if (mname != nullptr && strcmp(mname, name) == 0) return m;
    }

    return nullptr;
}

// cls:static_call(name, ...) → value
// 按名称调用静态方法（同名重载自动按参数个数与类型匹配）
static int Class_StaticCall(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);

    const Il2CppMethod* method = ResolveMethodOverload(L, ud->klass, name, 3);
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

    // 根据字段类型推送 Lua 值
    const Il2CppType* fieldType = resolver.GetFieldType(field);
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);

    // 按字段类型动态分配缓冲区
    // 避免 Matrix4x4（64 字节）等大结构体超过固定 16 字节导致越界
    std::vector<uint8_t> fieldStorage(LuaBridge_GetFieldValueSize(fieldType, typeEnum), 0);
    uint8_t* buffer = fieldStorage.data();

    // 读取静态字段
    resolver.ReadStaticField(field, buffer);

    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        lua_pushboolean(L, *reinterpret_cast<bool*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_CHAR:
        lua_pushinteger(L, *reinterpret_cast<uint16_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I1:
        lua_pushinteger(L, *reinterpret_cast<int8_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I2:
        lua_pushinteger(L, *reinterpret_cast<int16_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U1:
        lua_pushinteger(L, *reinterpret_cast<uint8_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U2:
        lua_pushinteger(L, *reinterpret_cast<uint16_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U4:
        lua_pushinteger(L, *reinterpret_cast<uint32_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U8:
        // lua_Integer 为有符号 64 位 超过 INT64_MAX 的值会回绕
        lua_pushinteger(L, static_cast<lua_Integer>(*reinterpret_cast<uint64_t*>(buffer)));
        break;
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
        lua_pushinteger(L, static_cast<lua_Integer>(*reinterpret_cast<intptr_t*>(buffer)));
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
        if (str != nullptr) LuaBridge_PushString(L, str);
        else lua_pushnil(L);
        break;
    }
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    {
        // 值类型字段：buffer 中是原始值字节 需要先装箱再包装为 Instance
        // 缓冲区已按 value_size 动态分配 可容纳任意大小结构体
        Il2CppClass* valueKlass = resolver.GetClassFromType(fieldType);
        Il2CppObject* boxed = (valueKlass != nullptr) ? resolver.Box(valueKlass, buffer) : nullptr;
        LuaBridge_PushInstance(L, boxed, valueKlass);
        break;
    }
    default:       
    {
        // 引用类型字段（class / object / array）
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

    // 引用类型字段：buffer 中保存的是对象指针
    // field_set 系列需要“指向指针的指针”（即 buffer）
    // 值类型/基本类型字段：param 直接指向值数据 结构体可能超过 16 字节
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);
    if (IsRefType(typeEnum)) resolver.WriteStaticField(field, buffer);
    else resolver.WriteStaticField(field, param);
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

// cls:get_address() → number
// 返回 Il2CppClass* 的原始地址
static int Class_GetAddress(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    lua_pushinteger(L, reinterpret_cast<int64_t>(ud->klass));
    return 1;
}

// cls:__tostring() → string
static int Class_ToString(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    const char* ns = resolver.GetClassNamespace(ud->klass);
    const char* name = resolver.GetKlassName(ud->klass);
    lua_pushfstring(L, "Class: %s.%s @ 0x%p", ns ? ns : "", name ? name : "?", ud->klass);
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
    {"get_address",       Class_GetAddress},
    {"__tostring",        Class_ToString},
    {nullptr, nullptr}
};


// ============================================================
// Instance 元表方法
// ============================================================

// obj:call(name, ...) → value
// 按名称调用实例方法（同名重载自动按参数个数与类型匹配）
static int Instance_Call(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    const char* name = luaL_checkstring(L, 2);

    // 确保 klass 已知
    Il2CppClass* klass = ud->klass;
    if (klass == nullptr && ud->obj != nullptr) klass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];
    if (klass == nullptr) return luaL_error(L, "cannot determine class for instance");

    const Il2CppMethod* method = ResolveMethodOverload(L, klass, name, 3);
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

    // 根据字段类型推送 Lua 值
    const Il2CppType* fieldType = resolver.GetFieldType(field);
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);

    // 按字段类型动态分配缓冲区 避免大结构体越界
    std::vector<uint8_t> fieldStorage(LuaBridge_GetFieldValueSize(fieldType, typeEnum), 0);
    uint8_t* buffer = fieldStorage.data();

    // 读取字段值
    resolver.ReadField(ud->obj, field, buffer);

    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        lua_pushboolean(L, *reinterpret_cast<bool*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_CHAR:
        lua_pushinteger(L, *reinterpret_cast<uint16_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I1:
        lua_pushinteger(L, *reinterpret_cast<int8_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I2:
        lua_pushinteger(L, *reinterpret_cast<int16_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U1:
        lua_pushinteger(L, *reinterpret_cast<uint8_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U2:
        lua_pushinteger(L, *reinterpret_cast<uint16_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U4:
        lua_pushinteger(L, *reinterpret_cast<uint32_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U8:
        // lua_Integer 为有符号 64 位 超过 INT64_MAX 的值会回绕
        lua_pushinteger(L, static_cast<lua_Integer>(*reinterpret_cast<uint64_t*>(buffer)));
        break;
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
        lua_pushinteger(L, static_cast<lua_Integer>(*reinterpret_cast<intptr_t*>(buffer)));
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
        if (str != nullptr) LuaBridge_PushString(L, str);
        else lua_pushnil(L);
        break;
    }
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    {
        // 值类型字段：buffer 中是原始值字节 需要先装箱再包装为 Instance
        // 缓冲区已按 value_size 动态分配 可容纳任意大小结构体
        Il2CppClass* valueKlass = resolver.GetClassFromType(fieldType);
        Il2CppObject* boxed = (valueKlass != nullptr) ? resolver.Box(valueKlass, buffer) : nullptr;
        LuaBridge_PushInstance(L, boxed, valueKlass);
        break;
    }
    default:
    {
        // 引用类型字段（class / object / array）
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

    // 同 static_set：引用类型字段传 buffer 值类型/基本类型字段传 param
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);
    if (IsRefType(typeEnum)) resolver.WriteField(ud->obj, field, buffer);
    else resolver.WriteField(ud->obj, field, param);
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

        // 读取数组元素（按元素实际类型与大小处理）
        if (!LuaBridge_PushArrayElement(L, ud->obj, idx))
        {
            return luaL_error(L, "unsupported array element type");
        }
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

        // 写入数组元素（按元素实际类型与大小处理）
        if (!LuaBridge_SetArrayElement(L, ud->obj, idx, 3))
        {
            return luaL_error(L, "failed to set array element");
        }
        return 0;
    }

    return luaL_error(L, "cannot set arbitrary fields on instance");
}

// obj:__len() → number
// 返回数组 / List<T> 长度（非容器返回 0）
static int Instance_Len(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    if (ud->obj == nullptr)
    {
        lua_pushinteger(L, 0);
        return 1;
    }

    if (LuaBridge_IsArray(ud->obj))
    {
        lua_pushinteger(L, static_cast<int64_t>(LuaBridge_GetArrayLength(ud->obj)));
    }
    else if (LuaBridge_IsList(ud->obj, ud->klass))
    {
        int64_t count = LuaBridge_GetListCount(ud->obj, ud->klass);
        lua_pushinteger(L, count >= 0 ? count : 0);
    }
    // 非容器返回 0
    else lua_pushinteger(L, 0);
    return 1;
}

// obj:each(function(value, index) ... end)
// 数组 / List<T> 遍历: 从 1 到长度逐个取出元素调用回调
// 回调参数与 arr[i] 语义一致 索引从 1 开始（C# 下标 = index - 1）
static int Instance_Each(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    if (ud->obj == nullptr) return luaL_error(L, "instance is null");

    // 回调必须是函数
    luaL_checktype(L, 2, LUA_TFUNCTION);

    if (LuaBridge_IsArray(ud->obj))
    {
        int64_t count = 0;
        LuaBridge_EachArray(L, ud->obj, 2, count);
        return 0;
    }
    if (LuaBridge_IsList(ud->obj, ud->klass))
    {
        int64_t count = 0;
        LuaBridge_EachList(L, ud->obj, ud->klass, 2, count);
        return 0;
    }

    return luaL_error(L, "object is not an array or List");
}

// Instance 元表方法注册表
static const luaL_Reg instance_methods[] = {
    {"call",         Instance_Call},
    {"get",          Instance_Get},
    {"set",          Instance_Set},
    {"get_class",    Instance_GetClass},
    {"get_address",  Instance_GetAddress},
    {"each",         Instance_Each},
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

// ============================================================
// Method Hook（参考 frida-il2cpp-bridge 的 implementation / revert）
// ============================================================

// mth:hook(function(this, original, ...) ... end)
// 替换方法实现 回调签名:
// ·实例方法: function(this, original, 参数1, ...) ... return 返回值 end
// ·静态方法: function(Class, original, 参数1, ...) ... end
// ·original() 调用原方法（可传替换参数）并返回原方法返回值
// ·不调用 original 时 回调返回值直接作为方法返回值
static int Method_Hook(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();

    if (lua_type(L, 2) != LUA_TFUNCTION)
    {
        return luaL_error(L, "hook requires a function");
    }

    if (!Il2CppHook::HookMethod(L, ud->method, ud->klass, 2))
    {
        const char* name = resolver.GetMethodName(ud->method);
        return luaL_error(L, "failed to hook method: %s", name ? name : "?");
    }
    return 0;
}

// mth:unhook()
// 恢复原始实现
static int Method_Unhook(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));

    if (!Il2CppHook::UnhookMethod(ud->method))
    {
        return luaL_error(L, "method is not hooked");
    }
    return 0;
}

// mth:hooked() -> boolean
// 查询是否已 Hook
static int Method_Hooked(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));

    lua_pushboolean(L, Il2CppHook::IsHooked(ud->method) ? 1 : 0);
    return 1;
}

// mth:get_address() → number
// 返回方法原生代码地址（MethodInfo 首字段 methodPointer 即 GameAssembly.dll 中的函数入口）
// 与 frida-il2cpp-bridge 的 method.virtualAddress 对应
static int Method_GetAddress(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();
    lua_pushinteger(L, reinterpret_cast<int64_t>(resolver.GetMethodPointer(ud->method)));
    return 1;
}

// mth:__tostring() → string
static int Method_ToString(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();
    const char* name = resolver.GetMethodName(ud->method);
    int32_t pc = resolver.GetMethodParamCount(ud->method);
    bool isStatic = resolver.IsStaticMethod(ud->method);
    void* codeAddr = resolver.GetMethodPointer(ud->method);
    lua_pushfstring(L, "Method: %s%s(%d params) @ 0x%p", isStatic ? "static " : "", name ? name : "?", pc, codeAddr);
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
    {"hook",           Method_Hook},
    {"unhook",         Method_Unhook},
    {"hooked",         Method_Hooked},
    {"get_address",    Method_GetAddress},
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

    // 按字段类型动态分配缓冲区 避免大结构体越界
    const Il2CppType* fieldType = resolver.GetFieldType(ud->field);
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);
    std::vector<uint8_t> fieldStorage(LuaBridge_GetFieldValueSize(fieldType, typeEnum), 0);
    uint8_t* buffer = fieldStorage.data();

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

    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        lua_pushboolean(L, *reinterpret_cast<bool*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_CHAR:
        lua_pushinteger(L, *reinterpret_cast<uint16_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I1:
        lua_pushinteger(L, *reinterpret_cast<int8_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_I2:
        lua_pushinteger(L, *reinterpret_cast<int16_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U1:
        lua_pushinteger(L, *reinterpret_cast<uint8_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U2:
        lua_pushinteger(L, *reinterpret_cast<uint16_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U4:
        lua_pushinteger(L, *reinterpret_cast<uint32_t*>(buffer));
        break;
    case Il2CppTypeEnum::TYPE_U8:
        // lua_Integer 为有符号 64 位 超过 INT64_MAX 的值会回绕
        lua_pushinteger(L, static_cast<lua_Integer>(*reinterpret_cast<uint64_t*>(buffer)));
        break;
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
        lua_pushinteger(L, static_cast<lua_Integer>(*reinterpret_cast<intptr_t*>(buffer)));
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
        if (str != nullptr) LuaBridge_PushString(L, str);
        else lua_pushnil(L);
        break;
    }
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    {
        // 值类型字段：buffer 中是原始值字节 需要先装箱再包装为 Instance
        // 缓冲区已按 value_size 动态分配 可容纳任意大小结构体
        Il2CppClass* valueKlass = resolver.GetClassFromType(fieldType);
        Il2CppObject* boxed = (valueKlass != nullptr) ? resolver.Box(valueKlass, buffer) : nullptr;
        LuaBridge_PushInstance(L, boxed, valueKlass);
        break;
    }
    default:
    {
        // 引用类型字段（class / object / array）
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

    // 引用类型字段：buffer 中保存的是对象指针
    // 值类型/基本类型字段：param 直接指向值数据
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);

    // 判断是实例字段还是静态字段
    if (lua_isnil(L, 2))
    {
        if (IsRefType(typeEnum)) resolver.WriteStaticField(ud->field, buffer);
        else resolver.WriteStaticField(ud->field, param);
    }
    else
    {
        LuaInstanceUD* instUD = LuaBridge_CheckInstance(L, 2);
        if (instUD == nullptr) return luaL_error(L, "expected Instance or nil");
        if (IsRefType(typeEnum)) resolver.WriteField(instUD->obj, ud->field, buffer);
        else resolver.WriteField(instUD->obj, ud->field, param);
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

    // ---- 注册 il2cpp.mainThread 子表 ----
    // 主线程调度（参考 frida-il2cpp-bridge 的 Il2Cpp.mainThread.schedule）
    lua_newtable(L);                    // 创建 mainThread 表
    luaL_setfuncs(L, main_thread_funcs, 0); // 注册函数
    lua_setfield(L, -2, "mainThread");  // il2cpp.mainThread = 表

    lua_setglobal(L, "il2cpp");        // 设置为全局变量

    // ---- 预装内部主线程 tick hook ----
    // 让 il2cpp.mainThread.is_ready() 初始即为 true
    // 预装失败不阻塞初始化 首次 schedule 时会自动重试
    Il2CppHook::EnsureMainThreadTickInstalled();

    return true;
}
