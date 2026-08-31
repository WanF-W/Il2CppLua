/**
 * ============================================================
 * lua_binding_field.cpp — Field userdata 绑定
 * ============================================================
 * 同一个 Field userdata 同时支持静态和实例字段，通过字段 flags 决定参数
 * 形式。读写前验证实例与声明类兼容性，并统一复用 Lua/C# 值编组规则。
 * ============================================================
 */
#include "lua_binding_internal.h"
#include <cstdio>

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

    // 按字段类型动态分配缓冲区 避免大结构体越界
    const Il2CppType* fieldType = resolver.GetFieldType(ud->field);
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);
    std::vector<uint8_t> fieldStorage(LuaBridge_GetFieldValueSize(fieldType, typeEnum), 0);
    uint8_t* buffer = fieldStorage.data();

    if (resolver.IsStaticField(ud->field))
    {
        if (lua_gettop(L) != 1) return luaL_error(L, "static field read takes no arguments");
        resolver.ReadStaticField(ud->field, buffer);
    }
    else
    {
        if (lua_gettop(L) != 2) return luaL_error(L, "instance field read requires an Instance");
        LuaInstanceUD* instUD = LuaBridge_CheckInstance(L, 2);
        if (instUD == nullptr) return luaL_error(L, "instance field read requires an Instance");
        Il2CppClass* declaringClass = resolver.GetFieldClass(ud->field);
        if (declaringClass == nullptr) declaringClass = ud->klass;
        Il2CppClass* instanceClass = instUD->klass;
        if (instanceClass == nullptr && instUD->obj != nullptr)
            instanceClass = READ_OFFSET(instUD->obj, 0, Il2CppClass*)[0];
        if (!resolver.IsAssignableFrom(declaringClass, instanceClass))
            return luaL_error(L, "instance type is not compatible with field declaring class");
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

// fld:write(...)
// 实例字段传 (Instance, value)，静态字段只传 (value)。
static int Field_Write(lua_State* L)
{
    LuaFieldUD* ud = static_cast<LuaFieldUD*>(luaL_checkudata(L, 1, LuaBridgeMT::FIELD));
    auto& resolver = Il2CppResolver::Instance();

    const Il2CppType* fieldType = resolver.GetFieldType(ud->field);
    constexpr uint32_t FIELD_ATTRIBUTE_LITERAL = 0x0040;
    if ((resolver.GetFieldFlags(ud->field) & FIELD_ATTRIBUTE_LITERAL) != 0)
        return luaL_error(L, "const field cannot be written");
    uint8_t buffer[16] = {};
    void* param = nullptr;

    const bool isStatic = resolver.IsStaticField(ud->field);
    const int valueIndex = isStatic ? 2 : 3;
    if (isStatic && lua_gettop(L) != 2)
        return luaL_error(L, "static field write requires one value");
    if (!isStatic && lua_gettop(L) != 3)
        return luaL_error(L, "instance field write requires an Instance and a value");

    if (!LuaBridge_MarshalArg(L, valueIndex, fieldType, buffer, param))
        return luaL_error(L, "failed to marshal field value");

    // 引用类型字段：buffer 中保存的是对象指针
    // 值类型/基本类型字段：param 直接指向值数据
    int32_t typeEnum = resolver.GetTypeEnum(fieldType);

    if (isStatic)
    {
        if (LuaBridge_IsRefType(typeEnum)) resolver.WriteStaticField(ud->field, buffer);
        else resolver.WriteStaticField(ud->field, param);
    }
    else
    {
        LuaInstanceUD* instUD = LuaBridge_CheckInstance(L, 2);
        if (instUD == nullptr) return luaL_error(L, "instance field write requires an Instance");
        Il2CppClass* declaringClass = resolver.GetFieldClass(ud->field);
        if (declaringClass == nullptr) declaringClass = ud->klass;
        Il2CppClass* instanceClass = instUD->klass;
        if (instanceClass == nullptr && instUD->obj != nullptr)
            instanceClass = READ_OFFSET(instUD->obj, 0, Il2CppClass*)[0];
        if (!resolver.IsAssignableFrom(declaringClass, instanceClass))
            return luaL_error(L, "instance type is not compatible with field declaring class");
        if (LuaBridge_IsRefType(typeEnum)) resolver.WriteField(instUD->obj, ud->field, buffer);
        else resolver.WriteField(instUD->obj, ud->field, param);
    }

    return 0;
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
