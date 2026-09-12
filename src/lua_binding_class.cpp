/**
 * lua_binding_class.cpp — Class userdata 绑定
 * 提供类元数据查询、重载选择、对象/数组创建、静态调用、静态字段访问、
 * Unity 对象查找和类结构 dump。实例级行为留在 lua_binding_instance.cpp。
 */
#include "lua_binding_internal.h"
#include "unity_object_query.h"
#include <cstring>
#include <map>
#include <string>
// Class 元表方法
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
        LuaBridge_PushMethod(L, resolver.GetMethod(ud->klass, name));
        return 1;
    }

    for (int i = 0; i < typeCount; ++i) luaL_checkstring(L, 3 + i);
    for (auto* current = ud->klass; current != nullptr; current = resolver.GetClassParent(current))
    {
        void* iterator = nullptr;
        while (const auto* method = resolver.NextMethod(current, iterator))
        {
            if (strcmp(resolver.GetMethodName(method), name) != 0
                || resolver.GetMethodParamCount(method) != typeCount) continue;
            bool matched = true;
            for (int arg = 0; arg < typeCount; ++arg)
            {
                const auto* type = resolver.GetMethodParamType(method, arg);
                if (!MethodTypeNameMatches(resolver.GetTypeName(type), lua_tostring(L, 3 + arg)))
                { matched = false; break; }
            }
            if (matched) { LuaBridge_PushMethod(L, method); return 1; }
        }
        if (strcmp(name, ".ctor") == 0 || strcmp(name, ".cctor") == 0) break;
    }

    lua_pushnil(L);
    return 1;
}

// cls:get_methods() → table of Method
static int Class_GetMethods(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();

    void* iterator = nullptr;
    lua_Integer index = 0;
    lua_newtable(L);
    while (const auto* method = resolver.NextMethod(ud->klass, iterator))
    {
        LuaBridge_PushMethod(L, method);
        // Lua 索引从 1 开始
        lua_rawseti(L, -2, ++index);
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
    LuaBridge_PushField(L, field);
    return 1;
}

// cls:get_fields() → table of Field
static int Class_GetFields(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();

    void* iterator = nullptr;
    lua_Integer index = 0;
    lua_newtable(L);
    while (const auto* field = resolver.NextField(ud->klass, iterator))
    {
        LuaBridge_PushField(L, field);
        lua_rawseti(L, -2, ++index);
    }
    return 1;
}

// cls:new(...) → Instance
// 创建对象并调用构造函数
static int Class_New(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    if (!resolver.CanUseClass(ud->klass))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "cannot construct open/Nullable type or type with unavailable metadata");

    // 先解析构造函数，再分配对象。参数错误时不应留下一个永远不会使用的
    // 托管对象。
    const int argc = lua_gettop(L) - 1;
    const Il2CppMethod* matched = LuaBridge_ResolveMethodOverload(L, ud->klass, ".ctor", 2);

    if (matched == nullptr && (argc != 0 || !resolver.IsValueType(ud->klass)))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "no matching constructor found; use alloc() only for intentional uninitialized allocation");
    Il2CppObject* obj = resolver.ObjectNew(ud->klass);
    if (obj == nullptr) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "failed to allocate object");
    LuaBridge_PushInstance(L, obj);
    // 将结果放在参数之前，使通用调用仍只看到构造参数。
    lua_insert(L, 2);
    if (matched != nullptr) LuaBridge_InvokeMethod(L, matched, obj, 3);
    lua_pushvalue(L, 2);
    return 1;
}

// cls:alloc() → Instance
// 只分配托管对象并初始化类，不调用实例构造函数。
static int Class_Alloc(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();
    if (!resolver.CanUseClass(ud->klass))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "cannot allocate open/Nullable type or type with unavailable metadata");
    resolver.RuntimeClassInit(ud->klass);
    Il2CppObject* obj = resolver.ObjectNew(ud->klass);
    if (obj == nullptr) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "failed to allocate object");
    LuaBridge_PushInstance(L, obj);
    return 1;
}

// cls:new_array(length) → Instance
// 创建以当前 Class 为元素类型的一维零基托管数组。
static int Class_NewArray(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    if (!Il2CppResolver::Instance().CanUseClass(ud->klass))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported array element class (open/Nullable or unavailable metadata)");
    const lua_Integer requestedLength = luaL_checkinteger(L, 2);
    if (requestedLength < 0 || static_cast<uint64_t>(requestedLength) > UINT32_MAX)
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "array length must be between 0 and %u", UINT32_MAX);

    Il2CppArray* array = Il2CppResolver::Instance().ArrayNew(
        ud->klass, static_cast<uint32_t>(requestedLength));
    if (array == nullptr) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "failed to create managed array");
    LuaBridge_PushInstance(L, reinterpret_cast<Il2CppObject*>(array));
    return 1;
}
// 重载方法选择（obj:call / cls:static_call 使用）
// 按方法名 + 参数个数 + Lua 实参类型兼容性自动选择最佳重载
// 避免同名重载方法总是命中第一个的问题
//
// 选择策略
// ·同名且参数个数精确匹配的方法优先
// ·多个匹配时按 Lua 实参类型打分 完全匹配 > 可转换 > 不兼容
// ·没有任何参数个数匹配的重载时直接返回 nullptr，由调用层报告错误

// 按 Lua 实参类型给单个方法打分（分数越高越匹配）
// cls:static_call(name, ...) → value
// 按名称调用静态方法（同名重载自动按参数个数与类型匹配）
static int Class_StaticCall(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);

    const Il2CppMethod* method = LuaBridge_ResolveMethodOverload(
        L, ud->klass, name, 3, true);
    if (method == nullptr) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "method not found: %s", name);

    // ResolveMethodOverload 已过滤静态方法，这里保留一次明确的边界检查，
    // 防止以后新增选择路径时把实例方法传入 nullptr this。
    if (!Il2CppResolver::Instance().IsStaticMethod(method))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "method is not static: %s", name);

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
    if (field == nullptr) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "field not found: %s", name);
    if (!resolver.IsStaticField(field))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "field is not static; use Instance:read_field: %s", name);

    return LuaBridge_ReadField(L, nullptr, field);
}

// cls:write_static_field(name, value)
// 写入静态字段值
static int Class_WriteStaticField(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    const char* name = luaL_checkstring(L, 2);
    auto& resolver = Il2CppResolver::Instance();

    const Il2CppField* field = resolver.GetField(ud->klass, name);
    if (field == nullptr) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "field not found: %s", name);
    if (!resolver.IsStaticField(field))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "field is not static; use Instance:write_field: %s", name);
    constexpr uint32_t FIELD_ATTRIBUTE_LITERAL = 0x0040;
    if ((resolver.GetFieldFlags(field) & FIELD_ATTRIBUTE_LITERAL) != 0)
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "const field cannot be written: %s", name);

    // 根据字段类型编组 Lua 值
    return LuaBridge_WriteField(L, nullptr, field, 3);
}

// cls:find_unity_objects() → table of Instance | nil
// 通过 UnityEngine.Object.FindObjectsOfType/FindObjectsByType 查找活跃对象。
// 此能力只适用于继承 UnityEngine.Object 的类，不是通用托管堆遍历。
static int Class_FindUnityObjects(lua_State* L)
{
    LuaClassUD* ud = static_cast<LuaClassUD*>(luaL_checkudata(L, 1, LuaBridgeMT::CLASS));
    auto& resolver = Il2CppResolver::Instance();

    Il2CppArray* arr = UnityObjectQuery::FindObjectsOfType(ud->klass);
    if (arr == nullptr)
    {
        lua_pushnil(L);
        return 1;
    }

    uint64_t count = resolver.ArrayLength(arr);
    lua_newtable(L);

    for (uint64_t i = 0; i < count; ++i)
    {
        if (!LuaBridge_PushArrayElement(
                L, reinterpret_cast<Il2CppObject*>(arr), static_cast<int64_t>(i)))
        {
            lua_pop(L, 1); // table
            return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "FindObjectsOfType returned an unsupported array");
        }
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
    buffer->truncated = false;
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

    // 展示最多 1024 项，多取一项检测截断；查询接口不受展示预算限制。
    constexpr int32_t MAX_FIELDS = 1024;
    auto** fields = static_cast<const Il2CppField**>(LuaBridge_NewBuffer(L, sizeof(Il2CppField*) * (MAX_FIELDS + 1)));
    const int32_t fieldCount = resolver.EnumerateFields(
        ud->klass, fields, MAX_FIELDS + 1);
    LuaBridge_DumpAppend(buffer, "Fields (first %d%s):\n", fieldCount > MAX_FIELDS ? MAX_FIELDS : fieldCount, fieldCount > MAX_FIELDS ? "; truncated" : "");
    for (int32_t i = 0; i < fieldCount && i < MAX_FIELDS; ++i)
    {
        const char* fieldName = resolver.GetFieldName(fields[i]);
        const char* fieldType = resolver.GetTypeName(resolver.GetFieldType(fields[i]));
        LuaBridge_DumpAppend(buffer, "  %s%s %s\n",
            resolver.IsStaticField(fields[i]) ? "static " : "",
            fieldType != nullptr ? fieldType : "?",
            fieldName != nullptr ? fieldName : "?");
    }

    constexpr int32_t MAX_METHODS = 1024;
    auto** methods = static_cast<const Il2CppMethod**>(LuaBridge_NewBuffer(L, sizeof(Il2CppMethod*) * (MAX_METHODS + 1)));
    const int32_t methodCount = resolver.EnumerateMethods(
        ud->klass, methods, MAX_METHODS + 1);
    LuaBridge_DumpAppend(buffer, "\nMethods (first %d%s):\n", methodCount > MAX_METHODS ? MAX_METHODS : methodCount, methodCount > MAX_METHODS ? "; truncated" : "");
    for (int32_t i = 0; i < methodCount && i < MAX_METHODS; ++i)
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
