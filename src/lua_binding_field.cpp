/**
 * lua_binding_field.cpp — Field userdata 绑定
 * 同一个 Field userdata 同时支持静态和实例字段，通过字段 flags 决定参数
 * 形式。读写前验证实例与声明类兼容性，并统一复用 Lua/C# 值编组规则。
 */
#include "lua_binding_internal.h"
#include <cstdio>
// Field 元表方法
// fld:get_name() → string
static int Field_GetName(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();
    const char* name = resolver.GetFieldName(ud->field);
    lua_pushstring(L, name ? name : "");
    return 1;
}
// fld:get_class() → Class | nil
static int Field_GetClass(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    Il2CppClass* klass = Il2CppResolver::Instance().GetFieldClass(ud->field);
    LuaBridge_PushClass(L, klass != nullptr ? klass : ud->klass);
    return 1;
}

// 生成字段完整签名，供 get_signature 和 __tostring 共用。
static std::string BuildFieldSignature(const Il2CppField* field, Il2CppClass* fallbackClass)
{
    auto& resolver = Il2CppResolver::Instance();
    const uint32_t flags = resolver.GetFieldFlags(field);
    constexpr uint32_t MEMBER_ACCESS_MASK = 0x0007;
    constexpr uint32_t FIELD_STATIC = 0x0010;
    constexpr uint32_t FIELD_INIT_ONLY = 0x0020;
    constexpr uint32_t FIELD_LITERAL = 0x0040;

    const char* access = "private scope";
    switch (flags & MEMBER_ACCESS_MASK)
    {
    case 0x0001: access = "private"; break;
    case 0x0002: access = "private protected"; break;
    case 0x0003: access = "internal"; break;
    case 0x0004: access = "protected"; break;
    case 0x0005: access = "protected internal"; break;
    case 0x0006: access = "public"; break;
    }

    std::string signature = access;
    if ((flags & FIELD_STATIC) != 0) signature += " static";
    if ((flags & FIELD_LITERAL) != 0) signature += " const";
    else if ((flags & FIELD_INIT_ONLY) != 0) signature += " readonly";

    const char* typeName = resolver.GetTypeName(resolver.GetFieldType(field));
    signature += ' ';
    signature += typeName != nullptr ? typeName : "?";
    signature += ' ';

    Il2CppClass* klass = resolver.GetFieldClass(field);
    if (klass == nullptr) klass = fallbackClass;
    const char* namespaze = resolver.GetClassNamespace(klass);
    const char* className = resolver.GetClassSimpleName(klass);
    if (namespaze != nullptr && namespaze[0] != '\0')
    {
        signature += namespaze;
        signature += '.';
    }
    signature += className != nullptr ? className : "?";
    signature += '.';

    const char* fieldName = resolver.GetFieldName(field);
    signature += fieldName != nullptr ? fieldName : "?";
    return signature;
}

// fld:get_signature() → string
static int Field_GetSignature(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    const std::string signature = BuildFieldSignature(ud->field, ud->klass);
    lua_pushlstring(L, signature.c_str(), signature.size());
    return 1;
}

// fld:get_offset() → number
static int Field_GetOffset(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();
    if (resolver.IsStaticField(ud->field))
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, resolver.GetFieldOffset(ud->field));
    return 1;
}

// fld:read(...) → value
// 实例字段传 Instance，静态字段不传参数。
static int Field_Read(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();

    if (resolver.IsStaticField(ud->field))
    {
        if (lua_gettop(L) != 1) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "static field read takes no arguments");
        return LuaBridge_ReadField(L, nullptr, ud->field);
    }
    if (lua_gettop(L) != 2) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "instance field read requires an Instance");
    auto* instance = LuaBridge_CheckInstance(L, 2);
    if (instance == nullptr || !resolver.IsAssignableFrom(resolver.GetFieldClass(ud->field), instance->klass))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "instance type is not compatible with field declaring class");
    return LuaBridge_ReadField(L, instance->obj, ud->field);
}

int LuaBridge_ReadField(lua_State* L, Il2CppObject* obj, const Il2CppField* field)
{
    auto& resolver = Il2CppResolver::Instance();
    if (!resolver.CanUseClass(resolver.GetFieldClass(field)))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported field declaring class (open/Nullable or unavailable metadata)");
    const auto* type = resolver.GetFieldType(field);
    const size_t size = LuaBridge_GetValueStorageSize(type);
    if (size == 0) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported field type");
    void* storage = LuaBridge_NewBuffer(L, size);
    if (resolver.IsStaticField(field)) resolver.ReadStaticField(field, storage);
    else resolver.ReadField(obj, field, storage);
    const int result = LuaBridge_PushFieldValue(L, type, storage);
    if (result != 1) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported field value");
    lua_remove(L, -2);
    return 1;
}

// fld:write(...)
// 实例字段传 (Instance, value)，静态字段只传 (value)。
int LuaBridge_WriteField(lua_State* L, Il2CppObject* obj, const Il2CppField* field, int valueIndex)
{
    auto& resolver = Il2CppResolver::Instance();
    if (!resolver.CanUseClass(resolver.GetFieldClass(field)))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported field declaring class (open/Nullable or unavailable metadata)");
    valueIndex = lua_absindex(L, valueIndex);
    if ((resolver.GetFieldFlags(field) & 0x0040) != 0)
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "const field cannot be written");
    const Il2CppType* type = resolver.GetFieldType(field);
    const size_t size = LuaBridge_GetValueStorageSize(type);
    if (size == 0) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported field type");
    void* storage = LuaBridge_NewBuffer(L, size);
    void* value = nullptr;
    if (!LuaBridge_MarshalArg(L, valueIndex, type, storage, value, size))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "failed to marshal field value");
    const int kind = resolver.GetTypeEnum(type);
    if (kind == Il2CppTypeEnum::TYPE_PTR || kind == Il2CppTypeEnum::TYPE_FNPTR)
        value = *static_cast<void**>(storage);
    if (resolver.IsStaticField(field)) resolver.WriteStaticField(field, value);
    else resolver.WriteField(obj, field, value);
    lua_pop(L, 1);
    return 0;
}

static int Field_Write(lua_State* L)
{
    auto* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();
    if (resolver.IsStaticField(ud->field))
    {
        if (lua_gettop(L) != 2) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "static field write requires one value");
        return LuaBridge_WriteField(L, nullptr, ud->field, 2);
    }
    if (lua_gettop(L) != 3) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "instance field write requires an Instance and a value");
    auto* instance = LuaBridge_CheckInstance(L, 2);
    if (instance == nullptr || !resolver.IsAssignableFrom(resolver.GetFieldClass(ud->field), instance->klass))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "instance type is not compatible with field declaring class");
    return LuaBridge_WriteField(L, instance->obj, ud->field, 3);
}

// fld:__tostring() → string
static int Field_ToString(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();
    const std::string signature = BuildFieldSignature(ud->field, ud->klass);
    if (resolver.IsStaticField(ud->field))
        lua_pushfstring(L, "Field: %s", signature.c_str());
    else
    {
        // lua_pushfstring 只支持 Lua 自己定义的有限格式集，不支持十六进制 %x。
        // 先用 C++ 格式化偏移，再作为普通字符串压栈，避免触发 Lua 格式错误。
        char offset[32]{};
        snprintf(offset, sizeof(offset), "0x%X",
            static_cast<unsigned int>(resolver.GetFieldOffset(ud->field)));
        const std::string text = "Field: " + signature + " offset=" + offset;
        lua_pushlstring(L, text.c_str(), text.size());
    }
    return 1;
}

// Field 元表方法注册表
static const luaL_Reg field_methods[] = {
    {"get_name",      Field_GetName},
    {"get_class",     Field_GetClass},
    {"get_signature", Field_GetSignature},
    {"get_offset",    Field_GetOffset},
    {"read",          Field_Read},
    {"write",         Field_Write},
    {"__tostring",    Field_ToString},
    {nullptr, nullptr}
};

const luaL_Reg* LuaBinding_GetFieldMethods()
{
    return field_methods;
}
