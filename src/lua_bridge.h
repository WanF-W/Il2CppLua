/**
 * ============================================================
 * lua_bridge.h — Lua ↔ IL2CPP 桥接层公共声明
 * ============================================================
 * 定义桥接层生命周期、userdata 布局以及 Hook 所需的公共值转换接口。
 * 具体 Lua API 实现在 lua_binding_*.cpp 中。
 *
 * 用户在 Lua 中通过以下方式操作 IL2CPP 运行时
 *
 * ·local cls = il2cpp.get_class("UnityEngine", "Transform")
 * ·local method = cls:get_method("GetPosition")
 * ·local obj = cls:new()
 * ·obj:call("SetPosition", 1, 2, 3)
 * ·local x = obj:read_field("position_x")
 * ·obj:write_field("position_x", 10)
 * ·il2cpp.schedule(function() ... end) -- 排队到 Unity 主线程执行
 * ·lua.each(table, fn)         -- 遍历 Lua table
 * ·lua.dump(table)             -- 输出 Lua table 的第一层键值
 * ·obj:call(name, ...)         -- 按名称调用 同名重载自动匹配
 *
 * 五种 userdata 类型
 * 
 * ·Assembly — IL2CPP 程序集的引用 (Il2CppAssembly*)
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

// Lua 状态机前置声明
struct lua_State;

// ============================================================
// 元表名称常量
// ============================================================
// 这些名称存储在 Lua registry 中 用于创建和识别 userdata 类型
namespace LuaBridgeMT
{
    constexpr const char* ASSEMBLY = "Il2CppLua.Assembly";  // 程序集元表名
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

struct LuaAssemblyUD
{
    Il2CppAssembly* assembly;
};

// Class userdata — 包装 Il2CppClass*
// klass - IL2CPP 类指针
struct LuaClassUD
{
    Il2CppClass* klass;
};

// Instance userdata — 包装 Il2CppObject*
// obj - IL2CPP 对象指针（可能为 null）
// klass - 对象实际类或创建 userdata 时已知的类
struct LuaInstanceUD
{
    Il2CppObject* obj;
    Il2CppClass* klass;
};

// Method userdata — 包装 const Il2CppMethod*
// method - IL2CPP 方法指针
// klass - 方法声明类
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
 * 创建五张 userdata 元表并注册 il2cpp 与 lua 全局表
 * @param L Lua 状态机
 * @return true 成功
 */
bool LuaBridge_Init(lua_State* L);

// ============================================================
// Userdata 创建辅助函数
// ============================================================
// 这些函数创建 userdata 并关联到正确的元表 然后压入 Lua 栈

// 创建 Assembly userdata 并压栈
void LuaBridge_PushAssembly(lua_State* L, Il2CppAssembly* assembly);

// 创建 Class userdata 并压栈
void LuaBridge_PushClass(lua_State* L, Il2CppClass* klass);

// 创建 Instance userdata 并压栈
// klass 参数可选 为 nullptr 时从对象头读取
void LuaBridge_PushInstance(lua_State* L, Il2CppObject* obj, Il2CppClass* klass = nullptr);

// 创建 Method userdata 并压栈
void LuaBridge_PushMethod(lua_State* L, const Il2CppMethod* method, Il2CppClass* klass);

// 创建 Field userdata 并压栈
void LuaBridge_PushField(lua_State* L, const Il2CppField* field, Il2CppClass* klass);

// 创建 IL2CPP 字符串对应的 Lua 字符串并压栈
// 内部将 UTF-16 转换为 UTF-8（Hook 回调参数编组等场景使用）
void LuaBridge_PushString(lua_State* L, Il2CppString* str);

// ============================================================
// Userdata 类型检查辅助函数
// ============================================================
// 检查指定位置的 Lua 值是否为对应类型的 userdata
// 返回指向 userdata 结构的指针 类型不匹配返回 nullptr

LuaAssemblyUD* LuaBridge_CheckAssembly(lua_State* L, int idx);
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
 * @return 实际压入的 Lua 值数量；void 为 0，其他类型为 1
 */
int LuaBridge_PushReturnValue(lua_State* L, Il2CppObject* result, const Il2CppType* returnType);

/**
 * 将 Lua 栈上的值编组为 C# 参数（与 mth:call 使用同一套逻辑）
 * @param L           Lua 状态机
 * @param idx         Lua 栈上参数位置
 * @param type        IL2CPP 参数类型
 * W@param storage     至少 16 字节的存储区（基本类型/值类型写入）
 * @param outParam    输出: 供 runtime_invoke 使用的参数指针
 * @return true 编组成功
 */
bool LuaBridge_MarshalArg(lua_State* L, int idx, const Il2CppType* type, void* storage, void*& outParam);

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
