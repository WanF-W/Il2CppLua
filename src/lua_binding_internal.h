/**
 * lua_binding_internal.h — Lua 绑定实现共享接口
 *
 * 仅供 lua_binding_*.cpp 和 Hook 编组代码使用，不属于公开 API。
 */
#pragma once
#include "lua_bridge.h"
#include "il2cpp_resolver.h"
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

struct DumpBuffer
{
    // dump 可能输出大量反射项，缓冲区由 Lua userdata 分配在堆上。
    char data[262144];
    size_t len = 0;
};

// ---- Lua/C# 值转换与调用 ----
// 这些函数同时服务普通绑定和 Hook，确保两条调用路径使用相同编组规则。
bool LuaBridge_IsRefType(int32_t typeEnum);
size_t LuaBridge_GetFieldValueSize(const Il2CppType* type, int32_t typeEnum);
int LuaBridge_InvokeMethod(lua_State* L, const Il2CppMethod* method, void* object, int firstArgIndex);
const Il2CppMethod* LuaBridge_ResolveMethodOverload(
    lua_State* L, Il2CppClass* klass, const char* name, int firstArgIndex);
std::string LuaBridge_BuildMethodSignature(
    const Il2CppMethod* method, Il2CppClass* fallbackClass);
void LuaBridge_DumpAppend(DumpBuffer* buffer, const char* format, ...);
// 通过统一 print 通道输出 dump，并去掉缓冲末尾换行，避免 print 再追加空行。
void LuaBridge_PrintDump(lua_State* L, const DumpBuffer* buffer);

// ---- 托管容器访问 ----
// Lua 层统一使用 1 基索引；这里的 index 参数已经由调用方转换为 0 基。
bool LuaBridge_PushArrayElement(lua_State* L, Il2CppObject* array, int64_t index);
bool LuaBridge_SetArrayElement(lua_State* L, Il2CppObject* array, int64_t index, int valueIndex);
bool LuaBridge_IsList(Il2CppObject* object, Il2CppClass* klass);
void LuaBridge_EachArray(lua_State* L, Il2CppObject* array, int functionIndex, int64_t& count);
void LuaBridge_EachList(lua_State* L, Il2CppObject* list, Il2CppClass* klass, int functionIndex, int64_t& count);
int64_t LuaBridge_GetListCount(Il2CppObject* list, Il2CppClass* klass);
int LuaBridge_EachTable(lua_State* L, int tableIndex, int functionIndex);
int64_t LuaBridge_GetTableCount(lua_State* L, int tableIndex);

// ---- 绑定模块注册 ----
// 各实现文件返回静态 luaL_Reg 表，初始化入口负责统一创建元表。
void LuaBinding_RegisterGlobals(lua_State* L);
const luaL_Reg* LuaBinding_GetAssemblyMethods();
const luaL_Reg* LuaBinding_GetClassMethods();
const luaL_Reg* LuaBinding_GetInstanceMethods();
const luaL_Reg* LuaBinding_GetMethodMethods();
const luaL_Reg* LuaBinding_GetFieldMethods();
