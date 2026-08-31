/**
 * ============================================================
 * lua_binding_class.cpp — Class userdata 绑定
 * ============================================================
 * 提供类元数据查询、重载选择、对象/数组创建、静态调用、静态字段访问、
 * Unity 对象查找和类结构 dump。实例级行为留在 lua_binding_instance.cpp。
 * ============================================================
 */
#include "lua_binding_internal.h"
#include <climits>
#include <vector>

// ============================================================
// Class 元表方法
// ============================================================

// cls:get_name() → string
static int Class_GetName(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    const char* name = resolver.GetClassSimpleName(ud->klass);
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

// cls:get_full_name() → string
// 全局命名空间的类只返回类名，其他类返回 Namespace.ClassName。
static int Class_GetFullName(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    const char* namespaze = resolver.GetClassNamespace(ud->klass);
    const char* name = resolver.GetClassSimpleName(ud->klass);

    std::string fullName;
    if (namespaze != nullptr && namespaze[0] != '\0')
    {
        fullName += namespaze;
        fullName += '.';
    }
    fullName += name != nullptr ? name : "";
    lua_pushlstring(L, fullName.c_str(), fullName.size());
    return 1;
}

// cls:get_assembly() → Assembly | nil
static int Class_GetAssembly(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    LuaBridge_PushAssembly(L, Il2CppResolver::Instance().GetClassAssembly(ud->klass));
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

// cls:is_value_type() → boolean
static int Class_IsValueType(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    lua_pushboolean(L, Il2CppResolver::Instance().IsValueType(ud->klass));
    return 1;
}

// cls:is_enum() → boolean
static int Class_IsEnum(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    lua_pushboolean(L, Il2CppResolver::Instance().IsEnum(ud->klass));
    return 1;
}

// 将 Lua API 中的类型简写转换为 IL2CPP 反射使用的完整类型名。
static std::string NormalizeMethodTypeName(const std::string& input)
{
    static const std::map<std::string, std::string> aliases = {
        {"bool", "System.Boolean"}, {"byte", "System.Byte"},
        {"sbyte", "System.SByte"}, {"char", "System.Char"},
        {"short", "System.Int16"}, {"ushort", "System.UInt16"},
        {"int", "System.Int32"}, {"uint", "System.UInt32"},
        {"long", "System.Int64"}, {"ulong", "System.UInt64"},
        {"float", "System.Single"}, {"double", "System.Double"},
        {"string", "System.String"}, {"object", "System.Object"},
        {"void", "System.Void"}
    };

    // 数组类型保留 [] 后缀，仅规范化元素类型。
    if (input.size() > 2 && input.compare(input.size() - 2, 2, "[]") == 0)
        return NormalizeMethodTypeName(input.substr(0, input.size() - 2)) + "[]";

    auto it = aliases.find(input);
    return it != aliases.end() ? it->second : input;
}

// 完整类型名精确比较；用户只传类名时也允许匹配命名空间后的末级名称。
static bool MethodTypeNameMatches(const char* actualTypeName, const std::string& requestedTypeName)
{
    if (actualTypeName == nullptr) return false;
    const std::string actual(actualTypeName);
    const std::string requested = NormalizeMethodTypeName(requestedTypeName);
    if (actual == requested) return true;
    if (requested.find('.') != std::string::npos) return false;

    const size_t separator = actual.find_last_of("./+");
    return separator == std::string::npos
        ? actual == requested
        : actual.substr(separator + 1) == requested;
}

// cls:get_method(name [, paramType1, ...]) → Method | nil
// 不传类型时保持原有行为；传入类型后按完整参数签名精确查找重载。
static int Class_GetMethod(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    const int32_t typeCount = lua_gettop(L) - 2;
    if (typeCount == 0)
    {
        LuaBridge_PushMethod(L, resolver.GetMethod(ud->klass, name), ud->klass);
        return 1;
    }

    constexpr int32_t MAX_METHODS = 1024;
    std::vector<const Il2CppMethod*> methods(MAX_METHODS);
    const int32_t methodCount = resolver.EnumerateMethods(
        ud->klass, methods.data(), static_cast<int32_t>(methods.size()));
    for (int32_t i = 0; i < methodCount; ++i)
    {
        const Il2CppMethod* method = methods[i];
        const char* methodName = resolver.GetMethodName(method);
        if (methodName == nullptr || strcmp(methodName, name) != 0) continue;
        if (resolver.GetMethodParamCount(method) != typeCount) continue;

        bool matched = true;
        for (int32_t paramIndex = 0; paramIndex < typeCount; ++paramIndex)
        {
            const char* requested = luaL_checkstring(L, 3 + paramIndex);
            const Il2CppType* paramType = resolver.GetMethodParamType(method, paramIndex);
            if (!MethodTypeNameMatches(resolver.GetTypeName(paramType), requested))
            {
                matched = false;
                break;
            }
        }

        if (matched)
        {
            LuaBridge_PushMethod(L, method, ud->klass);
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

// cls:get_methods() → table of Method
static int Class_GetMethods(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();

    // 枚举所有方法（最多 1024 个）
    constexpr int32_t MAX_METHODS = 1024;
    std::vector<const Il2CppMethod*> methods(MAX_METHODS);
    int32_t count = resolver.EnumerateMethods(
        ud->klass, methods.data(), static_cast<int32_t>(methods.size()));

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

    constexpr int32_t MAX_FIELDS = 1024;
    std::vector<const Il2CppField*> fields(MAX_FIELDS);
    int32_t count = resolver.EnumerateFields(
        ud->klass, fields.data(), static_cast<int32_t>(fields.size()));

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

    // 按 Lua 实参类型选择最佳构造函数重载，不再只比较参数数量。
    const int argc = lua_gettop(L) - 1;
    const Il2CppMethod* matched = LuaBridge_ResolveMethodOverload(L, ud->klass, ".ctor", 2);
    if (matched != nullptr && resolver.GetMethodParamCount(matched) != argc) matched = nullptr;

    // 无参类型可能没有显式 .ctor，此时保留 ObjectNew 产生的默认对象。
    if (matched == nullptr && argc > 0)
        return luaL_error(L, "no matching constructor found");

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
                if (!LuaBridge_MarshalArg(L, 2 + i, pt, storages[i].data, params[i]))
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

// cls:alloc() → Instance
// 只分配托管对象并初始化类，不调用实例构造函数。
static int Class_Alloc(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    resolver.RuntimeClassInit(ud->klass);
    Il2CppObject* obj = resolver.ObjectNew(ud->klass);
    if (obj == nullptr) return luaL_error(L, "failed to allocate object");
    LuaBridge_PushInstance(L, obj, ud->klass);
    return 1;
}

// cls:new_array(length) → Instance
// 创建以当前 Class 为元素类型的一维零基托管数组。
static int Class_NewArray(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const lua_Integer requestedLength = luaL_checkinteger(L, 2);
    if (requestedLength < 0 || static_cast<uint64_t>(requestedLength) > UINT32_MAX)
        return luaL_error(L, "array length must be between 0 and %u", UINT32_MAX);

    Il2CppArray* array = Il2CppResolver::Instance().ArrayNew(
        ud->klass, static_cast<uint32_t>(requestedLength));
    if (array == nullptr) return luaL_error(L, "failed to create managed array");
    LuaBridge_PushInstance(L, reinterpret_cast<Il2CppObject*>(array), nullptr);
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
            if (LuaBridge_IsRefType(paramEnum)
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
const Il2CppMethod* LuaBridge_ResolveMethodOverload(lua_State* L, Il2CppClass* klass, const char* name, int firstArgIdx)
{
    auto& resolver = Il2CppResolver::Instance();

    constexpr int32_t MAX_METHODS = 1024;
    std::vector<const Il2CppMethod*> methods(MAX_METHODS);
    int32_t methodCount = resolver.EnumerateMethods(
        klass, methods.data(), static_cast<int32_t>(methods.size()));

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

    const Il2CppMethod* method = LuaBridge_ResolveMethodOverload(L, ud->klass, name, 3);
    if (method == nullptr) return luaL_error(L, "method not found: %s", name);

    // 静态方法 obj 传 nullptr
    return LuaBridge_InvokeMethod(L, method, nullptr, 3);
}

// cls:read_static_field(name) → value
// 读取静态字段值
static int Class_ReadStaticField(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    const Il2CppField* field = resolver.GetField(ud->klass, name);
    if (field == nullptr) return luaL_error(L, "field not found: %s", name);
    if (!resolver.IsStaticField(field))
        return luaL_error(L, "field is not static; use Instance:read_field: %s", name);

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

// cls:write_static_field(name, value)
// 写入静态字段值
static int Class_WriteStaticField(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    const Il2CppField* field = resolver.GetField(ud->klass, name);
    if (field == nullptr) return luaL_error(L, "field not found: %s", name);
    if (!resolver.IsStaticField(field))
        return luaL_error(L, "field is not static; use Instance:write_field: %s", name);
    constexpr uint32_t FIELD_ATTRIBUTE_LITERAL = 0x0040;
    if ((resolver.GetFieldFlags(field) & FIELD_ATTRIBUTE_LITERAL) != 0)
        return luaL_error(L, "const field cannot be written: %s", name);

    // 根据字段类型编组 Lua 值
    const Il2CppType* fieldType = resolver.GetFieldType(field);
    uint8_t buffer[16] = {};
    void* param = nullptr;

    if (!LuaBridge_MarshalArg(L, 3, fieldType, buffer, param)) return luaL_error(L, "failed to marshal field value");

    // 引用类型字段：buffer 中保存的是对象指针
    // field_set 系列需要“指向指针的指针”（即 buffer）
    // 值类型/基本类型字段：param 直接指向值数据 结构体可能超过 16 字节
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);
    if (LuaBridge_IsRefType(typeEnum)) resolver.WriteStaticField(field, buffer);
    else resolver.WriteStaticField(field, param);
    return 0;
}

// cls:find_unity_objects() → table of Instance | nil
// 通过 UnityEngine.Object.FindObjectsOfType 查找活跃对象。
// 此能力只适用于继承 UnityEngine.Object 的类，不是通用托管堆遍历。
static int Class_FindUnityObjects(lua_State* L)
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

// cls:dump() → 无返回值
// 输出当前类声明的字段、方法和基础反射信息，不递归展开父类或字段类型。
static int Class_Dump(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();

    const char* namespaze = resolver.GetClassNamespace(ud->klass);
    const char* name = resolver.GetClassSimpleName(ud->klass);
    const char* kind = resolver.IsEnum(ud->klass)
        ? "enum"
        : (resolver.IsValueType(ud->klass) ? "struct" : "class");

    Il2CppAssembly* assembly = resolver.GetClassAssembly(ud->klass);
    const char* assemblyName = resolver.GetAssemblyName(assembly);
    Il2CppClass* parent = resolver.GetClassParent(ud->klass);
    const char* parentNamespace = resolver.GetClassNamespace(parent);
    const char* parentName = resolver.GetClassSimpleName(parent);

    // DumpBuffer 较大，使用 Lua userdata 存放，避免占用 256KB 线程栈。
    DumpBuffer* buffer = static_cast<DumpBuffer*>(lua_newuserdata(L, sizeof(DumpBuffer)));
    buffer->len = 0;
    LuaBridge_DumpAppend(buffer, "Class: %s%s%s\n",
        namespaze != nullptr && namespaze[0] != '\0' ? namespaze : "",
        namespaze != nullptr && namespaze[0] != '\0' ? "." : "",
        name != nullptr ? name : "?");
    LuaBridge_DumpAppend(buffer, "Assembly: %s\n", assemblyName != nullptr ? assemblyName : "?");
    LuaBridge_DumpAppend(buffer, "Kind: %s\n", kind);
    if (parent != nullptr)
    {
        LuaBridge_DumpAppend(buffer, "Parent: %s%s%s\n",
            parentNamespace != nullptr && parentNamespace[0] != '\0' ? parentNamespace : "",
            parentNamespace != nullptr && parentNamespace[0] != '\0' ? "." : "",
            parentName != nullptr ? parentName : "?");
    }
    else LuaBridge_DumpAppend(buffer, "Parent: nil\n");
    LuaBridge_DumpAppend(buffer, "Address: 0x%p\n", ud->klass);
    LuaBridge_DumpAppend(buffer, "Instance Size: %d\n\n", resolver.GetClassInstanceSize(ud->klass));

    // 反射项上限沿用 resolver 的固定枚举上限，但存储放在堆上。
    // 避免字段和方法各 1024 个指针同时占用约 16KB 线程栈（C6262）。
    constexpr int32_t MAX_FIELDS = 1024;
    std::vector<const Il2CppField*> fields(MAX_FIELDS);
    const int32_t fieldCount = resolver.EnumerateFields(
        ud->klass, fields.data(), static_cast<int32_t>(fields.size()));
    LuaBridge_DumpAppend(buffer, "Fields (%d):\n", fieldCount);
    for (int32_t i = 0; i < fieldCount; ++i)
    {
        const char* fieldName = resolver.GetFieldName(fields[i]);
        const char* fieldType = resolver.GetTypeName(resolver.GetFieldType(fields[i]));
        LuaBridge_DumpAppend(buffer, "  %s%s %s\n",
            resolver.IsStaticField(fields[i]) ? "static " : "",
            fieldType != nullptr ? fieldType : "?",
            fieldName != nullptr ? fieldName : "?");
    }

    constexpr int32_t MAX_METHODS = 1024;
    std::vector<const Il2CppMethod*> methods(MAX_METHODS);
    const int32_t methodCount = resolver.EnumerateMethods(
        ud->klass, methods.data(), static_cast<int32_t>(methods.size()));
    LuaBridge_DumpAppend(buffer, "\nMethods (%d):\n", methodCount);
    for (int32_t i = 0; i < methodCount; ++i)
    {
        const Il2CppMethod* method = methods[i];
        const char* methodName = resolver.GetMethodName(method);
        const char* returnType = resolver.GetTypeName(resolver.GetMethodReturnType(method));
        LuaBridge_DumpAppend(buffer, "  %s%s %s(",
            resolver.IsStaticMethod(method) ? "static " : "",
            returnType != nullptr ? returnType : "?",
            methodName != nullptr ? methodName : "?");

        const int32_t paramCount = resolver.GetMethodParamCount(method);
        for (int32_t paramIndex = 0; paramIndex < paramCount; ++paramIndex)
        {
            if (paramIndex > 0) LuaBridge_DumpAppend(buffer, ", ");
            const char* paramType = resolver.GetTypeName(
                resolver.GetMethodParamType(method, paramIndex));
            LuaBridge_DumpAppend(buffer, "%s", paramType != nullptr ? paramType : "?");
        }
        LuaBridge_DumpAppend(buffer, ")\n");
    }

    LuaBridge_PrintDump(L, buffer);
    return 0;
}

// cls:__tostring() → string
static int Class_ToString(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    const char* ns = resolver.GetClassNamespace(ud->klass);
    const char* name = resolver.GetClassSimpleName(ud->klass);
    lua_pushfstring(L, "Class: %s.%s @ 0x%p", ns ? ns : "", name ? name : "?", ud->klass);
    return 1;
}

// Class 元表方法注册表
static const luaL_Reg class_methods[] = {
    {"get_name",          Class_GetName},
    {"get_namespace",     Class_GetNamespace},
    {"get_full_name",     Class_GetFullName},
    {"get_assembly",      Class_GetAssembly},
    {"get_parent",        Class_GetParent},
    {"is_value_type",     Class_IsValueType},
    {"is_enum",           Class_IsEnum},
    {"get_method",        Class_GetMethod},
    {"get_methods",       Class_GetMethods},
    {"get_field",         Class_GetField},
    {"get_fields",        Class_GetFields},
    {"new",               Class_New},
    {"alloc",             Class_Alloc},
    {"new_array",         Class_NewArray},
    {"static_call",       Class_StaticCall},
    {"read_static_field", Class_ReadStaticField},
    {"write_static_field", Class_WriteStaticField},
    {"find_unity_objects", Class_FindUnityObjects},
    {"get_instance_size", Class_GetInstanceSize},
    {"get_address",       Class_GetAddress},
    {"dump",              Class_Dump},
    {"__tostring",        Class_ToString},
    {nullptr, nullptr}
};

const luaL_Reg* LuaBinding_GetClassMethods()
{
    return class_methods;
}
