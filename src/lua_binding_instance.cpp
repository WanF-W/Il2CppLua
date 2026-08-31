/**
 * ============================================================
 * lua_binding_instance.cpp — Instance userdata 与托管容器绑定
 * ============================================================
 * 负责实例方法调用、实例字段访问和对象 dump；当对象实际为 Array 或
 * List<T> 时，同时提供 1 基索引、长度、写入和 each 遍历语义。
 * ============================================================
 */
#include "lua_binding_internal.h"

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

    const Il2CppMethod* method = LuaBridge_ResolveMethodOverload(L, klass, name, 3);
    if (method == nullptr) return luaL_error(L, "method not found: %s", name);

    return LuaBridge_InvokeMethod(L, method, ud->obj, 3);
}

// obj:read_field(name) → value
// 读取实例字段值
static int Instance_ReadField(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    Il2CppClass* klass = ud->klass;
    if (klass == nullptr && ud->obj != nullptr) klass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];
    if (klass == nullptr) return luaL_error(L, "cannot determine class for instance");

    const Il2CppField* field = resolver.GetField(klass, name);
    if (field == nullptr) return luaL_error(L, "field not found: %s", name);
    if (resolver.IsStaticField(field))
        return luaL_error(L, "field is static; use Class:read_static_field: %s", name);

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

// obj:write_field(name, value)
// 写入实例字段值
static int Instance_WriteField(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    Il2CppClass* klass = ud->klass;
    if (klass == nullptr && ud->obj != nullptr) klass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];
    if (klass == nullptr) return luaL_error(L, "cannot determine class for instance");

    const Il2CppField* field = resolver.GetField(klass, name);
    if (field == nullptr) return luaL_error(L, "field not found: %s", name);
    if (resolver.IsStaticField(field))
        return luaL_error(L, "field is static; use Class:write_static_field: %s", name);

    const Il2CppType* fieldType = resolver.GetFieldType(field);
    uint8_t buffer[16] = {};
    void* param = nullptr;

    if (!LuaBridge_MarshalArg(L, 3, fieldType, buffer, param)) return luaL_error(L, "failed to marshal field value");

    // 与 Class:write_static_field 使用相同编组规则：
    // 引用类型传指针存储区，值类型和基本类型传实际数据地址。
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);
    if (LuaBridge_IsRefType(typeEnum)) resolver.WriteField(ud->obj, field, buffer);
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

// 将指定 Field 在当前实例上的值转换为可读文本并追加到 dump 缓冲。
// 通过 Field:read(instance) 读取可保证父子类存在同名字段时仍使用正确的元数据。
static void Instance_DumpFieldValue(
    lua_State* L, DumpBuffer* buffer, const Il2CppField* field, Il2CppClass* declaringClass)
{
    LuaBridge_PushField(L, field, declaringClass);
    const int fieldIndex = lua_gettop(L);

    lua_getfield(L, fieldIndex, "read");
    lua_pushvalue(L, fieldIndex);
    lua_pushvalue(L, 1);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK)
    {
        const char* error = lua_tostring(L, -1);
        LuaBridge_DumpAppend(buffer, "<read error: %s>", error != nullptr ? error : "unknown");
        lua_settop(L, fieldIndex - 1);
        return;
    }

    const bool quoteString = lua_type(L, -1) == LUA_TSTRING;
    size_t textLength = 0;
    const char* text = luaL_tolstring(L, -1, &textLength);
    LuaBridge_DumpAppend(buffer, quoteString ? "\"%.*s\"" : "%.*s",
        static_cast<int>(textLength), text != nullptr ? text : "?");

    // 移除 tostring 结果、原始值和临时 Field userdata。
    lua_settop(L, fieldIndex - 1);
}

// Array/List dump 的 each 回调：把当前元素追加为“[index] = value”。回调由
// LuaBridge_EachArray/List 通过 lua_pcall 调用，因此元素 __tostring 出错时会
// 转换为普通 Lua 错误，不会越过执行保护边界。
static int Instance_DumpContainerElement(lua_State* L)
{
    DumpBuffer* buffer = static_cast<DumpBuffer*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (buffer == nullptr) return 0;

    const lua_Integer index = luaL_checkinteger(L, 2);
    const bool quoteString = lua_type(L, 1) == LUA_TSTRING;
    size_t textLength = 0;
    const char* text = luaL_tolstring(L, 1, &textLength);
    LuaBridge_DumpAppend(buffer, quoteString ? "[%lld] = \"%.*s\"\n" : "[%lld] = %.*s\n",
        static_cast<long long>(index), static_cast<int>(textLength), text != nullptr ? text : "?");
    lua_pop(L, 1);
    return 0;
}

// 容器只展示逻辑元素，不展开 System.Array / List<T> 的运行时实现字段。
static int Instance_DumpContainer(
    lua_State* L, LuaInstanceUD* ud, Il2CppClass* actualClass, bool isArray)
{
    auto& resolver = Il2CppResolver::Instance();
    const char* typeName = resolver.GetTypeName(resolver.GetClassType(actualClass));
    const int64_t count = isArray
        ? static_cast<int64_t>(LuaBridge_GetArrayLength(ud->obj))
        : LuaBridge_GetListCount(ud->obj, actualClass);
    if (count < 0) return luaL_error(L, "failed to get container length");

    DumpBuffer* buffer = static_cast<DumpBuffer*>(lua_newuserdata(L, sizeof(DumpBuffer)));
    buffer->len = 0;
    LuaBridge_DumpAppend(buffer, "%s: %s\n", isArray ? "Array" : "List",
        typeName != nullptr ? typeName : "?");
    LuaBridge_DumpAppend(buffer, "count: %lld\n", static_cast<long long>(count));

    lua_pushlightuserdata(L, buffer);
    lua_pushcclosure(L, Instance_DumpContainerElement, 1);
    const int callbackIndex = lua_gettop(L);
    int64_t traversed = 0;
    if (isArray) LuaBridge_EachArray(L, ud->obj, callbackIndex, traversed);
    else LuaBridge_EachList(L, ud->obj, actualClass, callbackIndex, traversed);

    LuaBridge_PrintDump(L, buffer);
    return 0;
}

// obj:dump([includeParents]) → 无返回值
// Array/List 输出逻辑元素；普通对象默认输出实际类型声明的字段，传 true 时
// 按继承顺序附加父类字段。
static int Instance_Dump(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    if (ud->obj == nullptr) return luaL_error(L, "instance is null");
    const bool includeParents = lua_toboolean(L, 2) != 0;

    auto& resolver = Il2CppResolver::Instance();
    Il2CppClass* actualClass = ud->klass;
    if (actualClass == nullptr) actualClass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];
    if (actualClass == nullptr) return luaL_error(L, "cannot determine class for instance");

    const bool isArray = LuaBridge_IsArray(ud->obj);
    if (isArray || LuaBridge_IsList(ud->obj, actualClass))
        return Instance_DumpContainer(L, ud, actualClass, isArray);

    const char* namespaze = resolver.GetClassNamespace(actualClass);
    const char* name = resolver.GetClassSimpleName(actualClass);
    Il2CppAssembly* assembly = resolver.GetClassAssembly(actualClass);
    const char* assemblyName = resolver.GetAssemblyName(assembly);

    DumpBuffer* buffer = static_cast<DumpBuffer*>(lua_newuserdata(L, sizeof(DumpBuffer)));
    buffer->len = 0;
    LuaBridge_DumpAppend(buffer, "Instance: %s%s%s\n",
        namespaze != nullptr && namespaze[0] != '\0' ? namespaze : "",
        namespaze != nullptr && namespaze[0] != '\0' ? "." : "",
        name != nullptr ? name : "?");
    LuaBridge_DumpAppend(buffer, "Assembly: %s\n", assemblyName != nullptr ? assemblyName : "?");
    LuaBridge_DumpAppend(buffer, "Address: 0x%p\n", ud->obj);

    // 默认只处理当前类；需要父类时先收集链条再反转为基类到派生类顺序。
    std::vector<Il2CppClass*> classes;
    classes.push_back(actualClass);
    if (includeParents)
    {
        Il2CppClass* parent = resolver.GetClassParent(actualClass);
        while (parent != nullptr)
        {
            classes.push_back(parent);
            parent = resolver.GetClassParent(parent);
        }
        std::reverse(classes.begin(), classes.end());
    }

    constexpr int32_t MAX_FIELDS = 1024;
    std::vector<const Il2CppField*> fields(MAX_FIELDS);
    for (Il2CppClass* klass : classes)
    {
        const char* classNamespace = resolver.GetClassNamespace(klass);
        const char* className = resolver.GetClassSimpleName(klass);
        const int32_t fieldCount = resolver.EnumerateFields(
            klass, fields.data(), static_cast<int32_t>(fields.size()));

        int32_t instanceFieldCount = 0;
        for (int32_t i = 0; i < fieldCount; ++i)
        {
            if (!resolver.IsStaticField(fields[i])) ++instanceFieldCount;
        }

        LuaBridge_DumpAppend(buffer, "\n%s%s%s (%d fields):\n",
            classNamespace != nullptr && classNamespace[0] != '\0' ? classNamespace : "",
            classNamespace != nullptr && classNamespace[0] != '\0' ? "." : "",
            className != nullptr ? className : "?", instanceFieldCount);

        for (int32_t i = 0; i < fieldCount; ++i)
        {
            // 静态字段不属于某个实例，只能通过 Class 的静态字段值接口读取。
            if (resolver.IsStaticField(fields[i])) continue;
            const char* fieldName = resolver.GetFieldName(fields[i]);
            const char* fieldType = resolver.GetTypeName(resolver.GetFieldType(fields[i]));
            LuaBridge_DumpAppend(buffer, "  %s %s = ",
                fieldType != nullptr ? fieldType : "?",
                fieldName != nullptr ? fieldName : "?");
            Instance_DumpFieldValue(L, buffer, fields[i], klass);
            LuaBridge_DumpAppend(buffer, "\n");
        }
    }

    LuaBridge_PrintDump(L, buffer);
    return 0;
}

// obj:__tostring() → string
static int Instance_ToString(lua_State* L)
{
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
    auto& resolver = Il2CppResolver::Instance();

    Il2CppClass* klass = ud->klass;
    if (klass == nullptr && ud->obj != nullptr) klass = READ_OFFSET(ud->obj, 0, Il2CppClass*)[0];
    const char* typeName = klass ? resolver.GetTypeName(resolver.GetClassType(klass)) : nullptr;
    if (typeName == nullptr && klass != nullptr) typeName = resolver.GetClassSimpleName(klass);

    if (ud->obj != nullptr && LuaBridge_IsArray(ud->obj))
    {
        lua_pushfstring(L, "Instance: %s length=%I @ 0x%p",
            typeName ? typeName : "?",
            static_cast<lua_Integer>(LuaBridge_GetArrayLength(ud->obj)), ud->obj);
    }
    else if (ud->obj != nullptr && LuaBridge_IsList(ud->obj, klass))
    {
        const int64_t count = LuaBridge_GetListCount(ud->obj, klass);
        lua_pushfstring(L, "Instance: %s count=%I @ 0x%p",
            typeName ? typeName : "?", static_cast<lua_Integer>(count), ud->obj);
    }
    else lua_pushfstring(L, "Instance: %s @ 0x%p", typeName ? typeName : "?", ud->obj);
    return 1;
}

// obj:__index(key) → value
// 当 key 为数字时执行数组 / List<T> 元素访问
// 当 key 为字符串时在元表中查找方法
static int Instance_Index(lua_State* L)
{
    // 数字 key → 托管容器元素读取
    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
        if (ud->obj == nullptr) return luaL_error(L, "instance is null");

        // Lua 索引从 1 开始 C# 从 0 开始
        int64_t idx = lua_tointeger(L, 2) - 1;
        if (LuaBridge_IsArray(ud->obj))
        {
            const uint64_t len = LuaBridge_GetArrayLength(ud->obj);
            if (idx < 0 || static_cast<uint64_t>(idx) >= len)
                return luaL_error(L, "array index out of bounds: %d", static_cast<int>(idx + 1));

            // 数组按元素实际类型和大小从托管内存中读取。
            if (!LuaBridge_PushArrayElement(L, ud->obj, idx))
                return luaL_error(L, "unsupported array element type");
            return 1;
        }

        if (LuaBridge_IsList(ud->obj, ud->klass))
        {
            auto& resolver = Il2CppResolver::Instance();
            const int64_t count = LuaBridge_GetListCount(ud->obj, ud->klass);
            if (idx < 0 || idx >= count)
                return luaL_error(L, "List index out of bounds: %d", static_cast<int>(idx + 1));

            const Il2CppMethod* getItem = resolver.GetMethod(ud->klass, "get_Item");
            if (getItem == nullptr) return luaL_error(L, "List get_Item method not found");

            int32_t managedIndex = static_cast<int32_t>(idx);
            void* args[1] = { &managedIndex };
            Il2CppException* exc = nullptr;
            Il2CppObject* result = resolver.RuntimeInvoke(getItem, ud->obj, args, &exc);
            if (exc != nullptr) return luaL_error(L, "List get_Item threw a C# exception");
            return LuaBridge_PushReturnValue(L, result, resolver.GetMethodReturnType(getItem));
        }

        return luaL_error(L, "object is not an array or List");
    }

    // 字符串 key → 在元表中查找方法
    lua_getmetatable(L, 1);                     // 压入元表
    lua_pushvalue(L, 2);                        // 压入 key
    lua_rawget(L, -2);                          // 在元表中查找
    lua_remove(L, -2);                          // 移除元表
    return 1;
}

// obj:__newindex(key, value)
// 当 key 为数字时执行数组 / List<T> 元素写入
static int Instance_NewIndex(lua_State* L)
{
    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(luaL_checkudata(L, 1, LuaBridgeMT::INSTANCE));
        if (ud->obj == nullptr) return luaL_error(L, "instance is null");

        int64_t idx = lua_tointeger(L, 2) - 1;
        if (LuaBridge_IsArray(ud->obj))
        {
            const uint64_t len = LuaBridge_GetArrayLength(ud->obj);
            if (idx < 0 || static_cast<uint64_t>(idx) >= len)
                return luaL_error(L, "array index out of bounds: %d", static_cast<int>(idx + 1));

            if (!LuaBridge_SetArrayElement(L, ud->obj, idx, 3))
                return luaL_error(L, "failed to set array element");
            return 0;
        }

        if (LuaBridge_IsList(ud->obj, ud->klass))
        {
            auto& resolver = Il2CppResolver::Instance();
            const int64_t count = LuaBridge_GetListCount(ud->obj, ud->klass);
            if (idx < 0 || idx >= count)
                return luaL_error(L, "List index out of bounds: %d", static_cast<int>(idx + 1));

            const Il2CppMethod* setItem = resolver.GetMethod(ud->klass, "set_Item");
            if (setItem == nullptr) return luaL_error(L, "List set_Item method not found");

            int32_t managedIndex = static_cast<int32_t>(idx);
            alignas(16) uint8_t valueStorage[16] = {};
            void* valueArg = nullptr;
            const Il2CppType* valueType = resolver.GetMethodParamType(setItem, 1);
            if (!LuaBridge_MarshalArg(L, 3, valueType, valueStorage, valueArg))
                return luaL_error(L, "failed to marshal List element value");

            void* args[2] = { &managedIndex, valueArg };
            Il2CppException* exc = nullptr;
            resolver.RuntimeInvoke(setItem, ud->obj, args, &exc);
            if (exc != nullptr) return luaL_error(L, "List set_Item threw a C# exception");
            return 0;
        }

        return luaL_error(L, "object is not an array or List");
    }

    return luaL_error(L, "cannot set arbitrary fields on instance");
}

// obj:__len() → number
// 返回数组 / List<T> 长度，普通实例调用时明确报错。
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
    else return luaL_error(L, "object is not an array or List");
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
    {"read_field",   Instance_ReadField},
    {"write_field",  Instance_WriteField},
    {"get_class",    Instance_GetClass},
    {"get_address",  Instance_GetAddress},
    {"dump",         Instance_Dump},
    {"each",         Instance_Each},
    {"__tostring",   Instance_ToString},
    {"__index",      Instance_Index},
    {"__newindex",   Instance_NewIndex},
    {"__len",        Instance_Len},
    {nullptr, nullptr}
};

const luaL_Reg* LuaBinding_GetInstanceMethods()
{
    return instance_methods;
}
