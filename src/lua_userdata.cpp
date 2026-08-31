/**
 * ============================================================
 * lua_userdata.cpp — IL2CPP userdata 创建与类型检查
 * ============================================================
 * 集中维护五种 userdata 的内存布局和元表绑定规则，避免各 API 模块
 * 自行构造 userdata 后产生类型标识不一致。Push 系列将空指针映射为
 * nil；Check 系列使用 luaL_testudata，仅在类型准确匹配时返回包装结构。
 * ============================================================
 */
#include "lua_binding_internal.h"

// ============================================================
// Userdata 创建辅助函数实现
// ============================================================

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
    // 空类压入 nil
    if (klass == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    // 分配 userdata
    LuaClassUD* ud = static_cast<LuaClassUD*>(lua_newuserdata(L, sizeof(LuaClassUD)));
    // 设置类指针
    ud->klass = klass;

    // 设置元表
    luaL_getmetatable(L, LuaBridgeMT::CLASS);
    lua_setmetatable(L, -2);
}

// 创建 Instance userdata。未显式给出类时从标准 Il2CppObject 头部读取。
void LuaBridge_PushInstance(lua_State* L, Il2CppObject* obj, Il2CppClass* klass)
{
    if (obj == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    // 如果未提供 klass 从对象头读取
    if (klass == nullptr)
    {
        // Il2CppObject 布局: offset 0x00 = Il2CppClass* klass
        klass = READ_OFFSET(obj, 0, Il2CppClass*)[0];
    }

    // 分配 userdata
    LuaInstanceUD* ud = static_cast<LuaInstanceUD*>(lua_newuserdata(L, sizeof(LuaInstanceUD)));
    ud->obj = obj;
    ud->klass = klass;

    // 设置元表
    luaL_getmetatable(L, LuaBridgeMT::INSTANCE);
    lua_setmetatable(L, -2);
}

// 创建 Method userdata，同时缓存声明类作为缺少 method_get_class 时的回退。
void LuaBridge_PushMethod(lua_State* L, const Il2CppMethod* method, Il2CppClass* klass)
{
    if (method == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    LuaMethodUD* ud = static_cast<LuaMethodUD*>(lua_newuserdata(L, sizeof(LuaMethodUD)));
    ud->method = method;
    ud->klass = klass;

    luaL_getmetatable(L, LuaBridgeMT::METHOD);
    lua_setmetatable(L, -2);
}

// 创建 Field userdata，同时缓存声明类用于兼容部分缺少导出的运行时。
void LuaBridge_PushField(lua_State* L, const Il2CppField* field, Il2CppClass* klass)
{
    if (field == nullptr)
    {
        lua_pushnil(L);
        return;
    }

    LuaFieldUD* ud = static_cast<LuaFieldUD*>(lua_newuserdata(L, sizeof(LuaFieldUD)));
    ud->field = field;
    ud->klass = klass;

    luaL_getmetatable(L, LuaBridgeMT::FIELD);
    lua_setmetatable(L, -2);
}

// ============================================================
// Userdata 类型检查辅助函数实现
// ============================================================
// testudata 不抛出 Lua 错误，调用方可据此实现多类型参数或自定义错误信息。

LuaAssemblyUD* LuaBridge_CheckAssembly(lua_State* L, int idx)
{
    return static_cast<LuaAssemblyUD*>(luaL_testudata(L, idx, LuaBridgeMT::ASSEMBLY));
}

LuaClassUD* LuaBridge_CheckClass(lua_State* L, int idx)
{
    return static_cast<LuaClassUD*>(luaL_testudata(L, idx, LuaBridgeMT::CLASS));
}

LuaInstanceUD* LuaBridge_CheckInstance(lua_State* L, int idx)
{
    return static_cast<LuaInstanceUD*>(luaL_testudata(L, idx, LuaBridgeMT::INSTANCE));
}

LuaMethodUD* LuaBridge_CheckMethod(lua_State* L, int idx)
{
    return static_cast<LuaMethodUD*>(luaL_testudata(L, idx, LuaBridgeMT::METHOD));
}

LuaFieldUD* LuaBridge_CheckField(lua_State* L, int idx)
{
    return static_cast<LuaFieldUD*>(luaL_testudata(L, idx, LuaBridgeMT::FIELD));
}
