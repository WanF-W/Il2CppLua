/**
 * lua_userdata.cpp — IL2CPP userdata 创建与类型检查
 * 集中维护五种 userdata 的内存布局和元表绑定规则，避免各 API 模块
 * 自行构造 userdata 后产生类型标识不一致。Push 系列将空指针映射为
 * nil；Check 系列使用 luaL_testudata，仅在类型准确匹配时返回包装结构。
 */
#include "lua_binding_internal.h"
// Userdata 创建辅助函数实现
// 创建 Assembly userdata。程序集生命周期由 IL2CPP Runtime 管理。
void LuaBridge_PushAssembly(lua_State* L, Il2CppAssembly* assembly)
{
    if (assembly == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    LuaAssemblyUD* ud = static_cast<LuaAssemblyUD*>(lua_newuserdata(L, sizeof(LuaAssemblyUD)));
    ud->assembly = assembly;
    luaL_getmetatable(L, LuaBridgeMT::ASSEMBLY);
    lua_setmetatable(L, -2);
}
void LuaBridge_PushClass(lua_State* L, Il2CppClass* klass)
{
    if (klass == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    LuaClassUD* ud = static_cast<LuaClassUD*>(lua_newuserdata(L, sizeof(LuaClassUD)));
    ud->klass = klass;

    luaL_getmetatable(L, LuaBridgeMT::CLASS);
    lua_setmetatable(L, -2);
}

// 创建 Instance userdata，并为对象建立强 GCHandle。
void LuaBridge_PushInstance(lua_State* L, Il2CppObject* obj)
{
    if (obj == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    // 实际类决定可调用成员，不能用参数的声明类型替代。
    auto* klass = READ_OFFSET(obj, 0, Il2CppClass*)[0];
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(lua_newuserdata(L, sizeof(LuaInstanceUD)));
    ud->obj = obj;
    ud->klass = klass;
    ud->gcHandle = 0;

    luaL_getmetatable(L, LuaBridgeMT::INSTANCE);
    lua_setmetatable(L, -2);
    ud->gcHandle = Il2CppResolver::Instance().RetainObject(obj);
    if (ud->gcHandle == 0) LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "failed to retain managed object");
}

// 创建 Method userdata，缓存实际声明类。
void LuaBridge_PushMethod(lua_State* L, const Il2CppMethod* method)
{
    if (method == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    LuaMethodUD* ud = static_cast<LuaMethodUD*>(lua_newuserdata(L, sizeof(LuaMethodUD)));
    ud->method = method;
    ud->klass = Il2CppResolver::Instance().GetMethodClass(method);

    luaL_getmetatable(L, LuaBridgeMT::METHOD);
    lua_setmetatable(L, -2);
}

// 创建 Field userdata，缓存字段声明类。
void LuaBridge_PushField(lua_State* L, const Il2CppField* field)
{
    if (field == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    LuaFieldUD* ud = static_cast<LuaFieldUD*>(lua_newuserdata(L, sizeof(LuaFieldUD)));
    ud->field = field;
    ud->klass = Il2CppResolver::Instance().GetFieldClass(field);

    luaL_getmetatable(L, LuaBridgeMT::FIELD);
    lua_setmetatable(L, -2);
}
// Userdata 类型检查辅助函数实现
// testudata 不抛出 Lua 错误，调用方可据此实现多类型参数或自定义错误信息。

LuaInstanceUD* LuaBridge_CheckInstance(lua_State* L, int idx)
{
    return static_cast<LuaInstanceUD*>(luaL_testudata(L, idx, LuaBridgeMT::INSTANCE));
}

LuaMethodUD* LuaBridge_CheckMethod(lua_State* L, int idx)
{
    return static_cast<LuaMethodUD*>(luaL_testudata(L, idx, LuaBridgeMT::METHOD));
}
