// Lua 与 IL2CPP 的统一值转换、兼容性评分及临时存储。
// 调用和重载选择位于 lua_method_call.cpp；原生 ABI 仅由 Hook 层处理。
#include "lua_binding_internal.h"

#include <windows.h>
#include <cstring>

// 内部辅助：类型枚举判断
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

// 获取值类型在运行时使用的实际字节数。枚举的底层类型可以是 byte、short、
// int 或 long，不能把所有枚举都假定为 4 字节。
static size_t GetManagedValueSize(const Il2CppType* type)
{
    if (type == nullptr) return 0;

    auto& resolver = Il2CppResolver::Instance();
    Il2CppClass* klass = resolver.GetClassFromType(type);
    if (klass == nullptr) return 0;

    uint32_t align = 0;
    const int32_t size = resolver.ClassValueSize(klass, &align);
    return size > 0 ? static_cast<size_t>(size) : 0;
}

// 读取枚举底层字段 value__ 的类型，用于保留有符号枚举的符号扩展。
// 如果目标运行时没有提供该字段，按无符号位模式读取，仍能保证内存宽度正确。
static bool IsSignedEnum(const Il2CppType* enumType)
{
    auto& resolver = Il2CppResolver::Instance();
    Il2CppClass* enumClass = resolver.GetClassFromType(enumType);
    if (enumClass == nullptr) return false;

    const Il2CppField* valueField = resolver.GetField(enumClass, "value__");
    const Il2CppType* underlyingType = resolver.GetFieldType(valueField);
    const int32_t underlyingEnum = LuaBridge_GetEffectiveTypeEnum(underlyingType);
    return underlyingEnum == Il2CppTypeEnum::TYPE_I1
        || underlyingEnum == Il2CppTypeEnum::TYPE_I2
        || underlyingEnum == Il2CppTypeEnum::TYPE_I4
        || underlyingEnum == Il2CppTypeEnum::TYPE_I8
        || underlyingEnum == Il2CppTypeEnum::TYPE_I;
}

static int PushEnumValue(lua_State* L, const Il2CppType* enumType, const void* value)
{
    const size_t size = GetManagedValueSize(enumType);
    if (value == nullptr || size == 0 || size > sizeof(uint64_t)) return 0;

    uint64_t bits = 0;
    memcpy(&bits, value, size);
    if (IsSignedEnum(enumType))
    {
        switch (size)
        {
        case 1:
        {
            int8_t signedValue = 0;
            memcpy(&signedValue, &bits, sizeof(signedValue));
            lua_pushinteger(L, signedValue);
            return 1;
        }
        case 2:
        {
            int16_t signedValue = 0;
            memcpy(&signedValue, &bits, sizeof(signedValue));
            lua_pushinteger(L, signedValue);
            return 1;
        }
        case 4:
        {
            int32_t signedValue = 0;
            memcpy(&signedValue, &bits, sizeof(signedValue));
            lua_pushinteger(L, signedValue);
            return 1;
        }
        case 8:
        {
            int64_t signedValue = 0;
            memcpy(&signedValue, &bits, sizeof(signedValue));
            lua_pushinteger(L, signedValue);
            return 1;
        }
        default: break;
        }
    }

    lua_pushinteger(L, static_cast<lua_Integer>(bits));
    return 1;
}

// 判断是否为引用类型
bool LuaBridge_IsRefType(int32_t typeEnum)
{
    return typeEnum == Il2CppTypeEnum::TYPE_STRING
        || typeEnum == Il2CppTypeEnum::TYPE_CLASS
        || typeEnum == Il2CppTypeEnum::TYPE_OBJECT
        || typeEnum == Il2CppTypeEnum::TYPE_SZARRAY
        || typeEnum == Il2CppTypeEnum::TYPE_ARRAY;
}

// 把泛型实例等“外层类型”归一化为实际运行时类型。
// IL2CPP 的 TYPE_GENERICINST 既可能表示 class，也可能表示 struct；只看
// 原始 enum 会让字段、数组和方法调用对同一个类型采取不同的传参方式。
int32_t LuaBridge_GetEffectiveTypeEnum(const Il2CppType* type)
{
    if (type == nullptr) return -1;

    auto& resolver = Il2CppResolver::Instance();
    const int32_t typeEnum = resolver.GetTypeEnum(type);
    if (typeEnum == Il2CppTypeEnum::TYPE_ARRAY
        || typeEnum == Il2CppTypeEnum::TYPE_SZARRAY
        || typeEnum == Il2CppTypeEnum::TYPE_STRING
        || typeEnum == Il2CppTypeEnum::TYPE_CLASS
        || typeEnum == Il2CppTypeEnum::TYPE_OBJECT
        || typeEnum == Il2CppTypeEnum::TYPE_ENUM)
    {
        return typeEnum;
    }

    if (typeEnum != Il2CppTypeEnum::TYPE_VALUETYPE
        && typeEnum != Il2CppTypeEnum::TYPE_GENERICINST)
    {
        return typeEnum;
    }

    Il2CppClass* klass = resolver.GetClassFromType(type);
    if (klass == nullptr) return typeEnum;
    if (resolver.IsEnum(klass)) return Il2CppTypeEnum::TYPE_ENUM;
    if (resolver.IsValueType(klass)) return Il2CppTypeEnum::TYPE_VALUETYPE;
    return Il2CppTypeEnum::TYPE_CLASS;
}

bool LuaBridge_IsRefType(const Il2CppType* type)
{
    const int32_t rawEnum = type != nullptr
        ? Il2CppResolver::Instance().GetTypeEnum(type) : -1;
    if (LuaBridge_IsRefType(rawEnum)) return true;
    return LuaBridge_GetEffectiveTypeEnum(type) == Il2CppTypeEnum::TYPE_CLASS;
}
// 内部辅助：IL2CPP 字符串 → Lua 字符串
// IL2CPP 字符串内部以 UTF-16 存储 Lua 字符串以 UTF-8 存储
// 使用 WideCharToMultiByte 进行转换
void LuaBridge_PushString(lua_State* L, Il2CppString* str)
{
    auto& resolver = Il2CppResolver::Instance();

    // 获取字符串的 UTF-16 字符数据和长度
    const uint16_t* chars = resolver.StringChars(str);
    int32_t len = resolver.StringLength(str);

    // 空字符串处理
    if (len == 0 || chars == nullptr)
    {
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
        // Lua 持有临时 UTF-8 缓冲区。
        auto* buf = static_cast<char*>(LuaBridge_NewBuffer(L, static_cast<size_t>(utf8Len) + 1));

        WideCharToMultiByte(
            CP_UTF8, 0,
            reinterpret_cast<LPCWCH>(chars), len,
            buf, utf8Len,
            nullptr, nullptr);

        buf[utf8Len] = '\0';
        lua_pushlstring(L, buf, utf8Len);
        lua_remove(L, -2);
    }
    else
    {
        // 转换失败 压入空字符串
        lua_pushstring(L, "");
    }
}
// 内部辅助：Lua 值 → C# 参数编组
// 将 Lua 栈上指定位置的值转换为 C# 参数
// 转换后的数据存储在 storage 指向的调用方缓冲区中；缓冲区大小由
// storageSize 明确传入，值类型不再被固定截断为 16 字节。
// outParam 被设置为指向参数数据的指针（供 runtime_invoke 使用）
//
// 返回 true 表示编组成功 false 表示类型不匹配
// 无分配的参数预检，同时用于重载评分和实际编组；负数表示不支持。
int LuaBridge_ScoreArg(lua_State* L, int idx, const Il2CppType* type)
{
    auto& resolver = Il2CppResolver::Instance();
    if (!resolver.CanMarshalType(type)) return -1;
    const int kind = LuaBridge_GetEffectiveTypeEnum(type);
    const int luaKind = lua_type(L, idx);
    if (kind == Il2CppTypeEnum::TYPE_BOOLEAN) return luaKind == LUA_TBOOLEAN ? 3 : -1;
    if (kind == Il2CppTypeEnum::TYPE_STRING)
        return luaKind == LUA_TSTRING ? 3 : luaKind == LUA_TNIL ? 2 : -1;
    if (LuaBridge_IsRefType(kind))
    {
        if (luaKind == LUA_TNIL) return 2;
        auto* instance = LuaBridge_CheckInstance(L, idx);
        auto* expected = resolver.GetClassFromType(type);
        if (instance == nullptr || !resolver.IsAssignableFrom(expected, instance->klass)) return -1;
        return expected == instance->klass ? 3 : 1;
    }
    if (kind == Il2CppTypeEnum::TYPE_VALUETYPE)
    {
        if (luaKind == LUA_TNIL) return 1;
        if (luaKind == LUA_TLIGHTUSERDATA) return lua_touserdata(L, idx) != nullptr ? 1 : -1;
        auto* instance = LuaBridge_CheckInstance(L, idx);
        return instance != nullptr && instance->klass == resolver.GetClassFromType(type) ? 3 : -1;
    }
    const bool pointer = kind == Il2CppTypeEnum::TYPE_PTR || kind == Il2CppTypeEnum::TYPE_FNPTR;
    if (pointer && luaKind == LUA_TNIL) return 1;
    if ((pointer || kind == Il2CppTypeEnum::TYPE_I || kind == Il2CppTypeEnum::TYPE_U)
        && luaKind == LUA_TLIGHTUSERDATA) return 2;
    if (luaKind != LUA_TNUMBER) return -1;
    if (IsFloatType(kind)) return lua_isinteger(L, idx) ? 1 : 3;
    if (!IsIntegerType(kind) && kind != Il2CppTypeEnum::TYPE_CHAR
        && kind != Il2CppTypeEnum::TYPE_ENUM && !pointer) return -1;
    int exact = 0;
    const lua_Integer value = lua_tointegerx(L, idx, &exact);
    if (!exact) return -1;
    int rangeKind = kind;
    if (kind == Il2CppTypeEnum::TYPE_ENUM)
    {
        const auto* field = resolver.GetField(resolver.GetClassFromType(type), "value__");
        const auto* underlying = resolver.GetFieldType(field);
        if (underlying == nullptr) return -1;
        rangeKind = resolver.GetTypeEnum(underlying);
        if (!IsIntegerType(rangeKind)) return -1;
    }
    // UInt64/UIntPtr use the complete Lua integer bit pattern on Windows x64.
    // Narrow types instead require an exactly representable target value.
    switch (rangeKind)
    {
    case Il2CppTypeEnum::TYPE_I1: if (value < INT8_MIN || value > INT8_MAX) return -1; break;
    case Il2CppTypeEnum::TYPE_I2: if (value < INT16_MIN || value > INT16_MAX) return -1; break;
    case Il2CppTypeEnum::TYPE_I4: if (value < INT32_MIN || value > INT32_MAX) return -1; break;
    case Il2CppTypeEnum::TYPE_U1: if (value < 0 || value > UINT8_MAX) return -1; break;
    case Il2CppTypeEnum::TYPE_CHAR:
    case Il2CppTypeEnum::TYPE_U2: if (value < 0 || value > UINT16_MAX) return -1; break;
    case Il2CppTypeEnum::TYPE_U4: if (value < 0 || static_cast<uint64_t>(value) > UINT32_MAX) return -1; break;
    default: break;
    }
    if (!lua_isinteger(L, idx)) return 1; // 1.0 可精确转整数，但优先匹配浮点重载。
    return kind == Il2CppTypeEnum::TYPE_I4 || kind == Il2CppTypeEnum::TYPE_U4 ? 3 : 2;
}

bool LuaBridge_MarshalArg(
    lua_State* L, int idx, const Il2CppType* type, void* storage,
    void*& outParam, size_t storageSize)
{
    if (L == nullptr || LuaBridge_ScoreArg(L, idx, type) < 0) return false;

    auto& resolver = Il2CppResolver::Instance();

    // 获取参数的 IL2CPP 类型枚举
    const int32_t typeEnum = LuaBridge_GetEffectiveTypeEnum(type);
    if (typeEnum < 0) return false;

    // 根据类型枚举进行编组
    switch (typeEnum)
    {
    // 布尔类型
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        if (lua_type(L, idx) != LUA_TBOOLEAN) return false;
        if (storage == nullptr || storageSize < sizeof(bool)) return false;
        // Lua boolean → C# bool (1 字节)
        *static_cast<bool*>(storage) = lua_toboolean(L, idx) != 0;
        outParam = storage;
        return true;

    // 字符类型
    case Il2CppTypeEnum::TYPE_CHAR:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(uint16_t)) return false;
        // Lua integer → C# char (2 字节 UTF-16)
        *static_cast<uint16_t*>(storage) = static_cast<uint16_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;

    // 有符号整数类型
    case Il2CppTypeEnum::TYPE_I1:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(int8_t)) return false;
        *static_cast<int8_t*>(storage) = static_cast<int8_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_I2:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(int16_t)) return false;
        *static_cast<int16_t*>(storage) = static_cast<int16_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_I4:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(int32_t)) return false;
        *static_cast<int32_t*>(storage) = static_cast<int32_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_I8:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(int64_t)) return false;
        *static_cast<int64_t*>(storage) = static_cast<int64_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;

    // 无符号整数类型
    case Il2CppTypeEnum::TYPE_U1:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(uint8_t)) return false;
        *static_cast<uint8_t*>(storage) = static_cast<uint8_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_U2:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(uint16_t)) return false;
        *static_cast<uint16_t*>(storage) = static_cast<uint16_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_U4:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(uint32_t)) return false;
        *static_cast<uint32_t*>(storage) = static_cast<uint32_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_U8:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(uint64_t)) return false;
        *static_cast<uint64_t*>(storage) = static_cast<uint64_t>(lua_tointeger(L, idx));
        outParam = storage;
        return true;

    // 枚举按运行时 value__ 的真实宽度写入，兼容 byte/short/int/long 底层类型。
    case Il2CppTypeEnum::TYPE_ENUM:
    {
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        const size_t enumSize = GetManagedValueSize(type);
        if (storage == nullptr || enumSize == 0 || enumSize > sizeof(uint64_t)
            || storageSize < enumSize) return false;
        const uint64_t bits = static_cast<uint64_t>(lua_tointeger(L, idx));
        memcpy(storage, &bits, enumSize);
        outParam = storage;
        return true;
    }

    // 浮点类型
    case Il2CppTypeEnum::TYPE_R4:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(float)) return false;
        *static_cast<float*>(storage) = static_cast<float>(lua_tonumber(L, idx));
        outParam = storage;
        return true;
    case Il2CppTypeEnum::TYPE_R8:
        if (lua_type(L, idx) != LUA_TNUMBER) return false;
        if (storage == nullptr || storageSize < sizeof(double)) return false;
        *static_cast<double*>(storage) = lua_tonumber(L, idx);
        outParam = storage;
        return true;

    // 字符串类型（引用类型）
    case Il2CppTypeEnum::TYPE_STRING:
    {
        if (!lua_isnil(L, idx) && lua_type(L, idx) != LUA_TSTRING) return false;
        // Lua string → Il2CppString*
        // 关键：il2cpp_runtime_invoke 对引用类型直接使用 params[i] 作为对象指针
        // 不是指针的指针！所以 outParam 直接设为 Il2CppString* 本身
        Il2CppString* s = nullptr;
        if (!lua_isnil(L, idx))
        {
            size_t length = 0;
            const char* str = lua_tolstring(L, idx, &length);
            if (length > UINT32_MAX) return false;
            s = resolver.StringNewLen(str, static_cast<uint32_t>(length));
            if (s == nullptr) return false;
        }
        // 存储区保存指针值；跨分配的保活由调用路径使用 GCHandle 完成。
        if (storage == nullptr || storageSize < sizeof(Il2CppString*)) return false;
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
        if (!lua_isnil(L, idx) && LuaBridge_CheckInstance(L, idx) == nullptr)
            return false;
        // Instance userdata → Il2CppObject*
        // 同样 引用类型直接传对象指针
        Il2CppObject* obj = nullptr;
        if (!lua_isnil(L, idx))
        {
            LuaInstanceUD* ud = LuaBridge_CheckInstance(L, idx);
            if (ud == nullptr) return false;
            obj = ud->obj;

            // 即使调用方显式拿到了某个 Method，也不能把不兼容的对象
            // 静默传给 runtime_invoke；这一步是重载选择之外的最终边界。
            Il2CppClass* actualClass = ud->klass;
            if (actualClass == nullptr && obj != nullptr)
                actualClass = READ_OFFSET(obj, 0, Il2CppClass*)[0];
            Il2CppClass* expectedClass = resolver.GetClassFromType(type);
            if (obj != nullptr && expectedClass != nullptr
                && !resolver.IsAssignableFrom(expectedClass, actualClass))
                return false;
        }
        if (storage == nullptr || storageSize < sizeof(Il2CppObject*)) return false;
        *static_cast<Il2CppObject**>(storage) = obj;
        outParam = obj;
        return true;
    }

    // 原生整数类型 (IntPtr/UIntPtr)
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
        if (lua_type(L, idx) != LUA_TNUMBER && !lua_islightuserdata(L, idx))
            return false;
        if (storage == nullptr || storageSize < sizeof(uintptr_t)) return false;
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
        if (!lua_isnil(L, idx) && lua_type(L, idx) != LUA_TNUMBER
            && !lua_islightuserdata(L, idx)) return false;
        if (storage == nullptr || storageSize < sizeof(void*)) return false;
        if (lua_islightuserdata(L, idx))
        {
            *static_cast<void**>(storage) = lua_touserdata(L, idx);
        }
        else
        {
            *static_cast<void**>(storage) = reinterpret_cast<void*>(
                static_cast<intptr_t>(lua_tointeger(L, idx)));
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
            return outParam != nullptr;
        }
        else if (lua_isnil(L, idx))
        {
            Il2CppClass* valueClass = resolver.GetClassFromType(type);
            uint32_t align = 0;
            const int32_t valueSize = valueClass != nullptr
                ? resolver.ClassValueSize(valueClass, &align) : 0;
            // 没有实际大小时不能猜测结构体布局；固定清零 16 字节会越界或
            // 产生半初始化对象，因此宁可明确拒绝。
            if (valueSize <= 0 || storage == nullptr
                || storageSize < static_cast<size_t>(valueSize)) return false;
            memset(storage, 0, static_cast<size_t>(valueSize));
            outParam = storage;
            return true;
        }
        else
        {
            // 尝试作为已装箱的值类型
            LuaInstanceUD* ud = LuaBridge_CheckInstance(L, idx);
            if (ud == nullptr || ud->obj == nullptr) return false;

            Il2CppClass* expectedClass = resolver.GetClassFromType(type);
            Il2CppClass* actualClass = ud->klass;
            if (actualClass == nullptr)
                actualClass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];
            if (expectedClass != nullptr && actualClass != expectedClass)
                return false;

            // 拆箱：获取对象内部的值类型数据指针
            void* unboxed = resolver.Unbox(ud->obj);
            if (unboxed == nullptr) return false;

            // 直接使用拆箱后的数据指针作为参数，避免把大结构体复制到
            // 可能过小的临时区。调用期间装箱对象由 Lua userdata 持有。
            outParam = unboxed;
            return true;
        }
    }

    default:
        return false;
    }
}

// 临时区留在 Lua 栈上，脚本错误的 longjmp 和正常 GC 都能释放。
void* LuaBridge_NewBuffer(lua_State* L, size_t size)
{
    // MS x64 的间接 struct 参数要求 16 字节对齐；不依赖 Lua userdata 头的布局。
    const auto raw = reinterpret_cast<uintptr_t>(lua_newuserdata(L, size + 15));
    void* data = reinterpret_cast<void*>((raw + 15) & ~uintptr_t(15));
    memset(data, 0, size);
    return data;
}
// C# 返回值 → Lua 值
int LuaBridge_PushReturnValue(lua_State* L, Il2CppObject* result, const Il2CppType* returnType)
{
    if (!Il2CppResolver::Instance().CanMarshalType(returnType))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported return type (open type or Nullable)");
    const int kind = LuaBridge_GetEffectiveTypeEnum(returnType);
    if (returnType == nullptr || kind == Il2CppTypeEnum::TYPE_VOID) return 0;
    if (result == nullptr) { lua_pushnil(L); return 1; }
    if (kind == Il2CppTypeEnum::TYPE_VALUETYPE)
    {
        // runtime_invoke 已经装箱，直接保留对象，无须拆箱后重新装箱。
        LuaBridge_PushInstance(L, result);
        return 1;
    }
    void* value = LuaBridge_IsRefType(returnType) ? static_cast<void*>(&result)
        : Il2CppResolver::Instance().Unbox(result);
    return LuaBridge_PushFieldValue(L, returnType, value);
}

// 将字段缓冲区中的原始值转换为 Lua 值。
// 字段读取、静态字段读取和 Instance:read_field 共用此函数，避免三处
// 独立维护相同的基础类型分支。
int LuaBridge_PushFieldValue(lua_State* L, const Il2CppType* type, void* value)
{
    if (L == nullptr || type == nullptr || value == nullptr) return 0;
    if (!Il2CppResolver::Instance().CanMarshalType(type)) return 0;

    auto& resolver = Il2CppResolver::Instance();
    const int32_t typeEnum = LuaBridge_GetEffectiveTypeEnum(type);
    switch (typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        lua_pushboolean(L, *static_cast<bool*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_CHAR:
        lua_pushinteger(L, *static_cast<uint16_t*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_I1:
        lua_pushinteger(L, *static_cast<int8_t*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_U1:
        lua_pushinteger(L, *static_cast<uint8_t*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_I2:
        lua_pushinteger(L, *static_cast<int16_t*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_U2:
        lua_pushinteger(L, *static_cast<uint16_t*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_I4:
        lua_pushinteger(L, *static_cast<int32_t*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_U4:
        lua_pushinteger(L, *static_cast<uint32_t*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_I8:
        lua_pushinteger(L, *static_cast<int64_t*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_U8:
        lua_pushinteger(L, static_cast<lua_Integer>(*static_cast<uint64_t*>(value)));
        return 1;
    case Il2CppTypeEnum::TYPE_I:
        lua_pushinteger(L, *static_cast<intptr_t*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_U:
        lua_pushinteger(L, static_cast<lua_Integer>(*static_cast<uintptr_t*>(value)));
        return 1;
    case Il2CppTypeEnum::TYPE_R4:
        lua_pushnumber(L, *static_cast<float*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_R8:
        lua_pushnumber(L, *static_cast<double*>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_ENUM:
        return PushEnumValue(L, type, value);
    case Il2CppTypeEnum::TYPE_PTR:
    case Il2CppTypeEnum::TYPE_FNPTR:
        lua_pushlightuserdata(L, *static_cast<void**>(value));
        return 1;
    case Il2CppTypeEnum::TYPE_STRING:
    {
        Il2CppString* str = *static_cast<Il2CppString**>(value);
        if (str != nullptr) LuaBridge_PushString(L, str);
        else lua_pushnil(L);
        return 1;
    }
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    {
        Il2CppClass* valueClass = resolver.GetClassFromType(type);
        Il2CppObject* boxed = valueClass != nullptr ? resolver.Box(valueClass, value) : nullptr;
        LuaBridge_PushInstance(L, boxed);
        return 1;
    }
    default:
    {
        Il2CppObject* object = *static_cast<Il2CppObject**>(value);
        LuaBridge_PushInstance(L, object);
        return 1;
    }
    }
}
// 字段值缓冲区大小辅助
// 计算字段读取、方法参数编组等场景所需的临时存储区大小。
// 引用类型/基本类型按 ABI 固定大小，结构体通过 class_value_size 获取实际大小。
// 无法确定大小时返回 0，由调用方报告不支持，而不是猜测布局。
size_t LuaBridge_GetValueStorageSize(const Il2CppType* fieldType)
{
    auto& resolver = Il2CppResolver::Instance();

    if (!resolver.CanMarshalType(fieldType)) return 0;
    const int32_t typeEnum = LuaBridge_GetEffectiveTypeEnum(fieldType);

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
    case Il2CppTypeEnum::TYPE_PTR:
    case Il2CppTypeEnum::TYPE_FNPTR:
        return 8;
    case Il2CppTypeEnum::TYPE_STRING:
    case Il2CppTypeEnum::TYPE_CLASS:
    case Il2CppTypeEnum::TYPE_OBJECT:
    case Il2CppTypeEnum::TYPE_SZARRAY:
    case Il2CppTypeEnum::TYPE_ARRAY:
        return sizeof(void*);
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    case Il2CppTypeEnum::TYPE_ENUM:
    {
        Il2CppClass* valueKlass = resolver.GetClassFromType(fieldType);
        if (valueKlass != nullptr)
        {
            uint32_t align = 0;
            int32_t size = resolver.ClassValueSize(valueKlass, &align);
            if (size > 0) return static_cast<size_t>(size);
        }
        return 0;
    }
    default:
        // 未知类型不猜测大小，调用方应把它作为不支持的类型报告。
        return 0;
    }
}
