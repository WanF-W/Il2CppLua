/**
 * ============================================================
 * lua_bridge.h — Lua ↔ IL2CPP 桥接层声明
 * ============================================================
 * 本模块是整个项目的核心
 * 它将 IL2CPP 的类/对象/方法/字段映射为
 * Lua 的 userdata 并为每种类型创建一张元表 (metatable) 
 *
 * 用户在 Lua 中通过以下方式操作 IL2CPP 运行时
 *
 * ·local cls = il2cpp.get_class("UnityEngine", "Transform")
 * ·local method = cls:get_method("GetPosition")
 * ·local obj = cls:new()
 * ·obj:call("SetPosition", 1, 2, 3)
 * ·local x = obj:get("position_x")
 * ·obj:set("position_x", 10)
 *
 * 四种 userdata 类型
 * 
 * ·Class    — IL2CPP 类的引用 (Il2CppClass*)
 * ·Instance — IL2CPP 对象实例 (Il2CppObject*)
 * ·Method   — IL2CPP 方法的引用 (Il2CppMethod*)
 * ·Field    — IL2CPP 字段的引用 (Il2CppField*)
 *
 * 每种类型有独立的元表 提供该类型支持的操作 
 * ============================================================
 */
#pragma once
#include "common.h"
#include "il2cpp_resolver.h"

// Lua 状态机前置声明
struct lua_State;

// ============================================================
// 元表名称常量
// ============================================================
// 这些名称存储在 Lua registry 中 用于创建和识别 userdata 类型
namespace LuaBridgeMT
{
    constexpr const char* CLASS    = "Il2CppLua.Class";     // 类元表名
    constexpr const char* INSTANCE = "Il2CppLua.Instance";  // 实例元表名
    constexpr const char* METHOD   = "Il2CppLua.Method";    // 方法元表名
    constexpr const char* FIELD    = "Il2CppLua.Field";     // 字段元表名
}

// ============================================================
// Userdata 结构定义
// ============================================================
// 每种 userdata 都是一个固定大小的内存块 存储对应的 IL2CPP 指针 
// Lua 通过 lua_newuserdata 分配 并关联到对应的元表 

// Class userdata — 包装 Il2CppClass*
// method - IL2CPP 类指针
struct LuaClassUD
{
    Il2CppClass* klass;
};

// Instance userdata — 包装 Il2CppObject*
// obj - IL2CPP 对象指针（可能为 null）
// klass - 字段所属的类
struct LuaInstanceUD
{
    Il2CppObject* obj;
    Il2CppClass* klass;
};

// Method userdata — 包装 const Il2CppMethod*
// method - IL2CPP 方法指针
// klass - 字段所属的类
struct LuaMethodUD
{
    const Il2CppMethod* method;
    Il2CppClass* klass;
};

// Field userdata — 包装 const Il2CppField*
// field - IL2CPP 字段指针
// klass - 字段所属的类
struct LuaFieldUD
{
    const Il2CppField* field;
    Il2CppClass* klass;
};

// ============================================================
// 公共 API
// ============================================================

/**
 * 初始化桥接层
 * 创建 4 张元表并注册 il2cpp 全局表
 * @param L Lua 状态机
 * @return true 成功
 */
bool LuaBridge_Init(lua_State* L);

// ============================================================
// Userdata 创建辅助函数
// ============================================================
// 这些函数创建 userdata 并关联到正确的元表 然后压入 Lua 栈

// 创建 Class userdata 并压栈
void LuaBridge_PushClass(lua_State* L, Il2CppClass* klass);

// 创建 Instance userdata 并压栈
// klass 参数可选 为 nullptr 时从对象头读取
void LuaBridge_PushInstance(lua_State* L, Il2CppObject* obj, Il2CppClass* klass = nullptr);

// 创建 Method userdata 并压栈
void LuaBridge_PushMethod(lua_State* L, const Il2CppMethod* method, Il2CppClass* klass);

// 创建 Field userdata 并压栈
void LuaBridge_PushField(lua_State* L, const Il2CppField* field, Il2CppClass* klass);

// ============================================================
// Userdata 类型检查辅助函数
// ============================================================
// 检查指定位置的 Lua 值是否为对应类型的 userdata
// 返回指向 userdata 结构的指针 类型不匹配返回 nullptr

LuaClassUD* LuaBridge_CheckClass(lua_State* L, int idx);
LuaInstanceUD* LuaBridge_CheckInstance(lua_State* L, int idx);
LuaMethodUD* LuaBridge_CheckMethod(lua_State* L, int idx);
LuaFieldUD* LuaBridge_CheckField(lua_State* L, int idx);

// ============================================================
// 类型编组辅助函数
// ============================================================

/**
 * 将 C# 返回值压入 Lua 栈
 * @param L           Lua 状态机
 * @param result      runtime_invoke 返回的 Il2CppObject*
 * @param returnType  方法返回值类型（可为 nullptr 表示 void）
 */
void LuaBridge_PushReturnValue(lua_State* L, Il2CppObject* result, const Il2CppType* returnType);

/**
 * 检查一个 IL2CPP 对象是否为数组
 * @param obj 对象指针
 * @return true 如果是 SZARRAY（一维零基数组）
 */
bool LuaBridge_IsArray(Il2CppObject* obj);

/**
 * 获取数组长度
 * @param arr 数组对象指针
 * @return 元素个数
 */
uint64_t LuaBridge_GetArrayLength(Il2CppObject* arr);
