/**
 * lua_container.cpp — 托管 Array、List<T> 与 Lua table 访问
 * Array 根据元素类型直接读写连续内存；List<T> 通过 Count、get_Item 和
 * set_Item 方法访问，避免依赖不同 Unity 版本的私有字段布局。Lua table
 * 遍历则完全使用 Lua C API，与 IL2CPP 容器逻辑保持隔离。
 */
#include "lua_binding_internal.h"
#include <cstring>
// 数组辅助函数实现
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

    // 这里的内存访问只实现了一维零基数组。多维数组的 bounds 和索引
    // 规则不同，不能静默地套用同一套偏移计算。
    int32_t typeEnum = resolver.GetTypeEnum(type);
    return typeEnum == Il2CppTypeEnum::TYPE_SZARRAY;
}

uint64_t LuaBridge_GetArrayLength(Il2CppObject* arr)
{
    if (arr == nullptr) return 0;
    auto& resolver = Il2CppResolver::Instance();
    return resolver.ArrayLength(reinterpret_cast<Il2CppArray*>(arr));
}
// 数组元素类型与大小辅助
// 获取数组元素类与元素大小。
// 引用类型元素固定为指针大小，值类型元素必须从运行时取得真实大小；
// 不再对未知类型猜测布局。
static bool LuaBridge_GetArrayElementInfo(Il2CppObject* arr, Il2CppClass*& outElemClass, int32_t& outElemSize)
{
    if (arr == nullptr) return false;

    auto& resolver = Il2CppResolver::Instance();

    // 从对象头读取数组类
    Il2CppClass* arrayClass = READ_OFFSET(arr, 0, Il2CppClass*)[0];
    if (arrayClass == nullptr) return false;
    const Il2CppType* arrayType = resolver.GetClassType(arrayClass);
    if (arrayType == nullptr
        || resolver.GetTypeEnum(arrayType) != Il2CppTypeEnum::TYPE_SZARRAY)
        return false;

    // 通过官方 API 获取元素类。元素类型未知时不能猜测为指针，否则会
    // 把值类型数组按引用数组读写，造成数据损坏。
    outElemClass = resolver.GetElementClass(arrayClass);
    if (outElemClass == nullptr) return false;

    const auto* type = resolver.GetClassType(outElemClass);
    outElemSize = static_cast<int32_t>(LuaBridge_GetValueStorageSize(type));
    return outElemSize > 0;
}

// 计算一维数组元素地址，同时检查乘法和加法溢出。
static uint8_t* LuaBridge_GetArrayElementPointer(
    Il2CppArray* arr, uint64_t index, int32_t elemSize)
{
    if (arr == nullptr || elemSize <= 0) return nullptr;
    const uint64_t maxSize = static_cast<uint64_t>(SIZE_MAX);
    if (index > (maxSize - ARRAY_DATA_OFFSET) / static_cast<uint64_t>(elemSize))
        return nullptr;

    const size_t offset = static_cast<size_t>(ARRAY_DATA_OFFSET
        + index * static_cast<uint64_t>(elemSize));
    const uintptr_t base = reinterpret_cast<uintptr_t>(arr);
    if (offset > UINTPTR_MAX - base) return nullptr;
    return reinterpret_cast<uint8_t*>(base + offset);
}

// 将数组第 index 个元素转换为 Lua 值压栈
// 返回 false 表示元素类型不支持
bool LuaBridge_PushArrayElement(lua_State* L, Il2CppObject* arr, int64_t index)
{
    Il2CppClass* elemClass = nullptr;
    int32_t elemSize = 0;
    if (!LuaBridge_GetArrayElementInfo(arr, elemClass, elemSize)) return false;

    auto& resolver = Il2CppResolver::Instance();
    const uint64_t length = resolver.ArrayLength(reinterpret_cast<Il2CppArray*>(arr));
    if (index < 0 || static_cast<uint64_t>(index) >= length) return false;
    uint8_t* elemPtr = LuaBridge_GetArrayElementPointer(
        reinterpret_cast<Il2CppArray*>(arr), static_cast<uint64_t>(index), elemSize);
    if (elemPtr == nullptr) return false;

    const Il2CppType* elemType = resolver.GetClassType(elemClass);
    return LuaBridge_PushFieldValue(L, elemType, elemPtr) == 1;
}

// 将 Lua 栈上 valueIdx 位置的值写入数组第 index 个元素
// 返回 false 表示元素类型不支持或值类型不匹配
bool LuaBridge_SetArrayElement(lua_State* L, Il2CppObject* arr, int64_t index, int valueIdx)
{
    Il2CppClass* elemClass = nullptr;
    int32_t elemSize = 0;
    if (!LuaBridge_GetArrayElementInfo(arr, elemClass, elemSize)) return false;

    auto& resolver = Il2CppResolver::Instance();
    const uint64_t length = resolver.ArrayLength(reinterpret_cast<Il2CppArray*>(arr));
    if (index < 0 || static_cast<uint64_t>(index) >= length) return false;
    uint8_t* elemPtr = LuaBridge_GetArrayElementPointer(
        reinterpret_cast<Il2CppArray*>(arr), static_cast<uint64_t>(index), elemSize);
    if (elemPtr == nullptr) return false;

    const Il2CppType* elemType = resolver.GetClassType(elemClass);
    if (elemType == nullptr) return false;

    // 含引用的结构体需要逐字段写屏障；当前 API 明确拒绝裸 memcpy。
    if (resolver.IsValueType(elemClass) && resolver.HasReferences(elemClass)) return false;
    valueIdx = lua_absindex(L, valueIdx);
    void* storage = LuaBridge_NewBuffer(L, static_cast<size_t>(elemSize));
    void* value = nullptr;
    bool ok = LuaBridge_MarshalArg(L, valueIdx, elemType, storage, value, elemSize);
    if (ok)
    {
        if (LuaBridge_IsRefType(elemType))
            ok = resolver.ArraySetReference(reinterpret_cast<Il2CppArray*>(arr), index, static_cast<Il2CppObject*>(value));
        else if (value != nullptr) memcpy(elemPtr, value, static_cast<size_t>(elemSize));
        else ok = false;
    }
    lua_pop(L, 1);
    return ok;
}
// 通用遍历辅助（数组 / List<T> / Lua 表）
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

static int PushListElement(lua_State* L, Il2CppObject* list, const Il2CppMethod* method, int64_t index)
{
    auto& resolver = Il2CppResolver::Instance();
    if (!resolver.CanInvokeMethod(method))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported List element getter (open/Nullable or unavailable metadata)");
    int32_t managedIndex = static_cast<int32_t>(index);
    void* args[] = { &managedIndex };
    Il2CppException* exception = nullptr;
    auto* result = resolver.RuntimeInvoke(method, list, args, &exception);
    if (exception != nullptr)
    {
        return LuaEngine::RaiseManagedException(
            L,
            exception,
            "List get_Item threw a CSharp exception (managed exception text unavailable)");
    }
    if (LuaBridge_PushReturnValue(L, result, resolver.GetMethodReturnType(method)) != 1)
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported List element return type");
    return 1;
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
            LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported array element type at index %I", static_cast<lua_Integer>(i + 1));
            return;
        }

        // 调用 fn(value, index)
        lua_pushvalue(L, fnIdx);
        lua_pushvalue(L, -2);
        lua_pushinteger(L, static_cast<lua_Integer>(i) + 1);
        int status = lua_pcall(L, 2, 0, 0);
        if (status != LUA_OK)
        {
            // The error object stays on top; Lua unwinds this C frame.
            lua_error(L);
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
        LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "cannot determine class for List");
        return;
    }

    int64_t count = LuaBridge_GetListCount(list, klass);
    if (count < 0)
    {
        LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "failed to get List count");
        return;
    }
    const auto* getItem = resolver.GetMethod(klass, "get_Item");
    if (getItem == nullptr) { LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "List get_Item not found"); return; }
    outCount = count;

    for (int64_t i = 0; i < count; ++i)
    {
        PushListElement(L, list, getItem, i);

        // 调用 fn(value, index)
        lua_pushvalue(L, fnIdx);
        lua_pushvalue(L, -2);
        lua_pushinteger(L, static_cast<lua_Integer>(i) + 1);
        int status = lua_pcall(L, 2, 0, 0);
        if (status != LUA_OK)
        {
            // The error object stays on top; Lua unwinds this C frame.
            lua_error(L);
            return;
        }
        lua_pop(L, 1);          // value
    }
}

// 遍历 Lua 表: 先数组部分（1..# 保持顺序）再键值部分
// 回调 fn(value, index_or_key)
int LuaBridge_EachTable(lua_State* L, int tblIdx, int fnIdx)
{
    // 回调执行会改变 Lua 栈，先把两个参数固定为绝对索引，避免负索引
    // 在压入 value、函数和错误对象后指向错误的位置。
    tblIdx = lua_absindex(L, tblIdx);
    fnIdx = lua_absindex(L, fnIdx);
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
            // The error object stays on top; Lua unwinds this C frame.
            lua_error(L);
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
            // ... key value fn → ... key value fn value key
            lua_pushvalue(L, -2);   // value
            lua_pushvalue(L, -4);   // key
            if (lua_pcall(L, 2, 0, 0) != LUA_OK)
            {
                return lua_error(L);
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

int LuaBridge_GetListElement(lua_State* L, Il2CppObject* list, Il2CppClass* klass, int64_t index)
{
    auto& resolver = Il2CppResolver::Instance();
    const int64_t count = LuaBridge_GetListCount(list, klass);
    if (count < 0) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "failed to get List count");
    if (index < 0 || index >= count) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "List index out of bounds");
    const auto* method = resolver.GetMethod(klass, "get_Item");
    if (method == nullptr) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "List get_Item not found");
    return PushListElement(L, list, method, index);
}

int LuaBridge_SetListElement(lua_State* L, Il2CppObject* list, Il2CppClass* klass, int64_t index, int valueIndex)
{
    auto& resolver = Il2CppResolver::Instance();
    const int64_t count = LuaBridge_GetListCount(list, klass);
    if (count < 0) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "failed to get List count");
    if (index < 0 || index >= count) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "List index out of bounds");
    const auto* method = resolver.GetMethod(klass, "set_Item");
    if (method == nullptr) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "List set_Item not found");
    valueIndex = lua_absindex(L, valueIndex);
    const int top = lua_gettop(L);
    lua_pushinteger(L, static_cast<lua_Integer>(index));
    lua_pushvalue(L, valueIndex);
    LuaBridge_InvokeMethod(L, method, list, top + 1);
    lua_settop(L, top);
    return 0;
}
