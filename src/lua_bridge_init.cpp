/**
 * ============================================================
 * lua_bridge_init.cpp — Lua 桥接层注册入口
 * ============================================================
 * 本文件只负责组装各独立绑定模块，不实现任何具体运行时操作。
 * 初始化过程依次创建五种 userdata 元表、发布全局表，并尝试预装
 * Scheduler 的默认 tick。预装失败不会导致 Lua VM 初始化失败。
 * ============================================================
 */
#include "lua_binding_internal.h"
#include "il2cpp_scheduler.h"
#include <cstring>

static void CreateMetatable(lua_State* L, const char* name, const luaL_Reg* methods)
{
    // luaL_newmetatable 将类型表放入 registry，后续 userdata 只保存该表引用。
    luaL_newmetatable(L, name);
    luaL_setfuncs(L, methods, 0);

    // Instance 自定义 __index/__newindex，其余 userdata 的方法从自身元表读取。
    if (std::strcmp(name, LuaBridgeMT::INSTANCE) != 0)
    {
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

bool LuaBridge_Init(lua_State* L)
{
    if (L == nullptr) return false;

    // 注册顺序没有运行时依赖，但保持与公开 userdata 层级一致，便于审查。
    CreateMetatable(L, LuaBridgeMT::ASSEMBLY, LuaBinding_GetAssemblyMethods());
    CreateMetatable(L, LuaBridgeMT::CLASS, LuaBinding_GetClassMethods());
    CreateMetatable(L, LuaBridgeMT::INSTANCE, LuaBinding_GetInstanceMethods());
    CreateMetatable(L, LuaBridgeMT::METHOD, LuaBinding_GetMethodMethods());
    CreateMetatable(L, LuaBridgeMT::FIELD, LuaBinding_GetFieldMethods());
    // userdata 完成后再公开全局表，避免初始化中途暴露不完整 API。
    LuaBinding_RegisterGlobals(L);

    // 默认 tick 预装失败不阻塞初始化，首次 schedule 时仍会重试。
    Il2CppScheduler::EnsureInstalled();
    return true;
}
