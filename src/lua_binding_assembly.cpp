/**
 * lua_binding_assembly.cpp — Assembly userdata 绑定
 * Assembly 是程序集范围反射的入口：可以读取程序集名、精确查找类，
 * 或在导出支持时枚举程序集中的全部类。所有返回对象均通过统一的
 * userdata 创建函数压栈，空运行时指针统一映射为 Lua nil。
 */
#include "lua_binding_internal.h"
// Assembly 元表方法
// assembly:get_name() → string | nil
// 名称直接来自 Il2CppAssembly 对应的 Il2CppImage 元数据。
static int Assembly_GetName(lua_State* L)
{
    LuaAssemblyUD* ud = static_cast<LuaAssemblyUD*>(luaL_checkudata(L, 1, LuaBridgeMT::ASSEMBLY));
    const char* name = Il2CppResolver::Instance().GetAssemblyName(ud->assembly);
    if (name != nullptr) lua_pushstring(L, name);
    else lua_pushnil(L);
    return 1;
}
// assembly:get_class(namespace, name) → Class | nil
// 只在当前程序集内查找，不回退到全程序集搜索。
static int Assembly_GetClass(lua_State* L)
{
    LuaAssemblyUD* ud = static_cast<LuaAssemblyUD*>(luaL_checkudata(L, 1, LuaBridgeMT::ASSEMBLY));
    const char* namespaze = luaL_optstring(L, 2, "");
    const char* name = luaL_checkstring(L, 3);
    LuaBridge_PushClass(L, Il2CppResolver::Instance().GetClass(ud->assembly, namespaze, name));
    return 1;
}

// assembly:get_classes() → Class[]
// Lua 数组保持 1 基索引；运行时缺少 image_get_class 等导出时明确报错。
static int Assembly_GetClasses(lua_State* L)
{
    LuaAssemblyUD* ud = static_cast<LuaAssemblyUD*>(luaL_checkudata(L, 1, LuaBridgeMT::ASSEMBLY));
    auto& resolver = Il2CppResolver::Instance();
    const int32_t count = resolver.GetAssemblyClassCount(ud->assembly);
    if (count < 0) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "assembly class enumeration is unavailable");

    lua_createtable(L, count, 0);
    for (int32_t i = 0; i < count; ++i)
    {
        LuaBridge_PushClass(L, resolver.GetAssemblyClassAt(ud->assembly, i));
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// 诊断输出包含程序集逻辑名和原生指针，便于与 dump 结果交叉确认。
static int Assembly_ToString(lua_State* L)
{
    LuaAssemblyUD* ud = static_cast<LuaAssemblyUD*>(luaL_checkudata(L, 1, LuaBridgeMT::ASSEMBLY));
    const char* name = Il2CppResolver::Instance().GetAssemblyName(ud->assembly);
    lua_pushfstring(L, "Assembly: %s @ 0x%p", name ? name : "?", ud->assembly);
    return 1;
}

static const luaL_Reg assembly_methods[] = {
    {"get_name",    Assembly_GetName},
    {"get_class",   Assembly_GetClass},
    {"get_classes", Assembly_GetClasses},
    {"__tostring",  Assembly_ToString},
    {nullptr, nullptr}
};

// 初始化入口只读取注册表，不取得其所有权，因此使用静态生命周期。
const luaL_Reg* LuaBinding_GetAssemblyMethods()
{
    return assembly_methods;
}
