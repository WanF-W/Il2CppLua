/**
 * ============================================================
 * lua_value.cpp — Lua 与 IL2CPP 的值转换
 * ============================================================
 * 负责参数编组、返回值转换、字符串转换与通用方法调用。
 * ============================================================
 */

#include "lua_binding_internal.h"
#include "il2cpp_resolver.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
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
bool LuaBridge_IsRefType(int32_t typeEnum)
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
int LuaBridge_InvokeMethod(lua_State* L, const Il2CppMethod* method, void* obj, int argStartIdx)
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

    // void 不产生 Lua 返回值；其他类型（包括 null 引用）产生一个返回值。
    return LuaBridge_PushReturnValue(L, result, retType);
}
// ============================================================
// C# 返回值 → Lua 值
// ============================================================
int LuaBridge_PushReturnValue(lua_State* L, Il2CppObject* result, const Il2CppType* returnType)
{
    auto& resolver = Il2CppResolver::Instance();

    // 无返回类型或显式 void 都不向 Lua 栈压入占位 nil。
    if (returnType == nullptr)
    {
        return 0;
    }

    int32_t typeEnum = resolver.GetTypeEnum(returnType);

    // void 类型
    if (typeEnum == Il2CppTypeEnum::TYPE_VOID)
    {
        return 0;
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
        return 1;
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
    return 1;
}
// ============================================================
// 字段值缓冲区大小辅助
// ============================================================

// 计算 il2cpp_field_get_value 所需的缓冲区大小
// 引用类型/基本类型按固定大小 结构体通过 class_value_size 获取实际大小
// 无法确定大小时回退 16 字节（保持旧行为）
size_t LuaBridge_GetFieldValueSize(const Il2CppType* fieldType, int32_t typeEnum)
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
