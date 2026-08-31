/**
 * ============================================================
 * lua_container.cpp — 托管 Array、List<T> 与 Lua table 访问
 * ============================================================
 * Array 根据元素类型直接读写连续内存；List<T> 通过 Count、get_Item 和
 * set_Item 方法访问，避免依赖不同 Unity 版本的私有字段布局。Lua table
 * 遍历则完全使用 Lua C API，与 IL2CPP 容器逻辑保持隔离。
 * ============================================================
 */
#include "lua_binding_internal.h"
#include <vector>

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
    if (LuaBridge_IsRefType(typeEnum) || typeEnum == Il2CppTypeEnum::TYPE_ARRAY)
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
bool LuaBridge_PushArrayElement(lua_State* L, Il2CppObject* arr, int64_t index)
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
bool LuaBridge_SetArrayElement(lua_State* L, Il2CppObject* arr, int64_t index, int valueIdx)
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
// 供 obj:each 与 Lua table 辅助工具共用
// 回调签名统一为 function(value, index) 索引从 1 开始

// 判断 Instance 是否为 System.Collections.Generic.List<T>
bool LuaBridge_IsList(Il2CppObject* obj, Il2CppClass* klass)
{
    if (obj == nullptr) return false;

    auto& resolver = Il2CppResolver::Instance();
    if (klass == nullptr) klass = READ_OFFSET(obj, 0, Il2CppClass*)[0];
    if (klass == nullptr) return false;

    const char* name = resolver.GetClassSimpleName(klass);
    const char* ns = resolver.GetClassNamespace(klass);
    return name != nullptr && ns != nullptr
        && strcmp(name, "List`1") == 0
        && strcmp(ns, "System.Collections.Generic") == 0;
}

// 获取 List<T> 元素个数（失败返回 -1）
int64_t LuaBridge_GetListCount(Il2CppObject* list, Il2CppClass* klass)
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
void LuaBridge_EachArray(lua_State* L, Il2CppObject* arr, int fnIdx, int64_t& outCount)
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
void LuaBridge_EachList(lua_State* L, Il2CppObject* list, Il2CppClass* klass, int fnIdx, int64_t& outCount)
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
int LuaBridge_EachTable(lua_State* L, int tblIdx, int fnIdx)
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

int64_t LuaBridge_GetTableCount(lua_State* L, int tblIdx)
{
    tblIdx = lua_absindex(L, tblIdx);
    int64_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, tblIdx) != 0)
    {
        ++count;
        lua_pop(L, 1);
    }
    return count;
}
