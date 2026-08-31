/**
 * ============================================================
 * lua_binding_method.cpp — Method userdata 绑定
 * ============================================================
 * 封装方法元数据、完整签名、显式调用和 Hook 生命周期。签名生成同时供
 * il2cpp.get_tick() 使用，保证 Method 显示与 Scheduler 状态格式一致。
 * ============================================================
 */
#include "lua_binding_internal.h"
#include "il2cpp_hook.h"

// ============================================================
// Method 元表方法
// ============================================================

// mth:get_name() → string
static int Method_GetName(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();
    const char* name = resolver.GetMethodName(ud->method);
    lua_pushstring(L, name ? name : "");
    return 1;
}

// 根据 MethodAttributes 中的成员访问位返回签名使用的访问修饰符。
static const char* GetMethodAccessModifier(uint32_t flags)
{
    constexpr uint32_t MEMBER_ACCESS_MASK = 0x0007;
    switch (flags & MEMBER_ACCESS_MASK)
    {
    case 0x0001: return "private";
    case 0x0002: return "private protected";
    case 0x0003: return "internal";
    case 0x0004: return "protected";
    case 0x0005: return "protected internal";
    case 0x0006: return "public";
    default:     return "private scope";
    }
}

// 生成稳定的完整方法签名，供 get_signature 和 __tostring 共用。
std::string LuaBridge_BuildMethodSignature(const Il2CppMethod* method, Il2CppClass* fallbackClass)
{
    auto& resolver = Il2CppResolver::Instance();
    const uint32_t flags = resolver.GetMethodFlags(method);
    constexpr uint32_t METHOD_STATIC = 0x0010;
    constexpr uint32_t METHOD_FINAL = 0x0020;
    constexpr uint32_t METHOD_VIRTUAL = 0x0040;
    constexpr uint32_t METHOD_ABSTRACT = 0x0400;

    std::string signature = GetMethodAccessModifier(flags);
    if ((flags & METHOD_STATIC) != 0) signature += " static";
    if ((flags & METHOD_ABSTRACT) != 0) signature += " abstract";
    else if ((flags & METHOD_VIRTUAL) != 0) signature += " virtual";
    if ((flags & METHOD_FINAL) != 0) signature += " final";

    const Il2CppType* returnType = resolver.GetMethodReturnType(method);
    const char* returnTypeName = resolver.GetTypeName(returnType);
    signature += ' ';
    signature += returnTypeName != nullptr ? returnTypeName : "System.Void";
    signature += ' ';

    Il2CppClass* klass = resolver.GetMethodClass(method);
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

    const char* methodName = resolver.GetMethodName(method);
    signature += methodName != nullptr ? methodName : "?";
    signature += '(';

    const int32_t paramCount = resolver.GetMethodParamCount(method);
    for (int32_t i = 0; i < paramCount; ++i)
    {
        if (i > 0) signature += ", ";
        const char* typeName = resolver.GetTypeName(resolver.GetMethodParamType(method, i));
        signature += typeName != nullptr ? typeName : "?";
        signature += ' ';

        const char* paramName = resolver.GetMethodParamName(method, i);
        if (paramName != nullptr && paramName[0] != '\0') signature += paramName;
        else signature += "arg" + std::to_string(i + 1);
    }
    signature += ')';
    return signature;
}

// mth:get_class() → Class | nil
static int Method_GetClass(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    Il2CppClass* klass = Il2CppResolver::Instance().GetMethodClass(ud->method);
    LuaBridge_PushClass(L, klass != nullptr ? klass : ud->klass);
    return 1;
}

// mth:get_signature() → string
static int Method_GetSignature(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    const std::string signature = LuaBridge_BuildMethodSignature(ud->method, ud->klass);
    lua_pushlstring(L, signature.c_str(), signature.size());
    return 1;
}

// mth:call(obj, ...) → value
// 使用此 Method userdata 显式调用方法
static int Method_Call(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));

    void* obj = nullptr;
    int argStart = 2;

    // 如果不是静态方法 需要 this 对象
    auto& resolver = Il2CppResolver::Instance();
    if (!resolver.IsStaticMethod(ud->method))
    {
        // 实例方法：第一个参数必须是 Instance
        LuaInstanceUD* instUD = LuaBridge_CheckInstance(L, 2);
        if (instUD == nullptr) return luaL_error(L, "instance method requires Instance as first arg");

        Il2CppClass* declaringClass = resolver.GetMethodClass(ud->method);
        if (declaringClass == nullptr) declaringClass = ud->klass;
        Il2CppClass* instanceClass = instUD->klass;
        if (instanceClass == nullptr && instUD->obj != nullptr)
            instanceClass = READ_OFFSET(instUD->obj, 0, Il2CppClass*)[0];
        if (!resolver.IsAssignableFrom(declaringClass, instanceClass))
            return luaL_error(L, "instance type is not compatible with method declaring class");

        obj = instUD->obj;
        // 参数从第 3 个位置开始
        argStart = 3;
    }

    return LuaBridge_InvokeMethod(L, ud->method, obj, argStart);
}

// ============================================================
// Method Hook（参考 frida-il2cpp-bridge 的 implementation / revert）
// ============================================================

// mth:hook(function(this, original, ...) ... end)
// 替换方法实现 回调签名:
// ·实例方法: function(this, original, 参数1, ...) ... return 返回值 end
// ·静态方法: function(Class, original, 参数1, ...) ... end
// ·original() 调用原方法（可传替换参数）并返回原方法返回值
// ·不调用 original 时 回调返回值直接作为方法返回值
static int Method_Hook(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();

    if (lua_type(L, 2) != LUA_TFUNCTION)
    {
        return luaL_error(L, "hook requires a function");
    }

    if (!Il2CppHook::HookMethod(L, ud->method, ud->klass, 2))
    {
        const char* name = resolver.GetMethodName(ud->method);
        return luaL_error(L, "failed to hook method: %s", name ? name : "?");
    }
    return 0;
}

// mth:unhook()
// 恢复原始实现
static int Method_Unhook(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));

    if (!Il2CppHook::UnhookMethod(ud->method))
    {
        return luaL_error(L, "method is not hooked");
    }
    return 0;
}

// mth:is_hooked() -> boolean
// 查询是否已 Hook
static int Method_IsHooked(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));

    lua_pushboolean(L, Il2CppHook::IsHooked(ud->method) ? 1 : 0);
    return 1;
}

// mth:get_address() → number
// 返回方法原生代码地址（MethodInfo 首字段 methodPointer 即 GameAssembly.dll 中的函数入口）
// 与 frida-il2cpp-bridge 的 method.virtualAddress 对应
static int Method_GetAddress(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();
    lua_pushinteger(L, reinterpret_cast<int64_t>(resolver.GetMethodPointer(ud->method)));
    return 1;
}

// mth:__tostring() → string
static int Method_ToString(lua_State* L)
{
    LuaMethodUD* ud = static_cast<LuaMethodUD*>(luaL_checkudata(L, 1, LuaBridgeMT::METHOD));
    auto& resolver = Il2CppResolver::Instance();
    const std::string signature = LuaBridge_BuildMethodSignature(ud->method, ud->klass);
    void* codeAddr = resolver.GetMethodPointer(ud->method);
    lua_pushfstring(L, "Method: %s @ 0x%p", signature.c_str(), codeAddr);
    return 1;
}

// Method 元表方法注册表
static const luaL_Reg method_methods[] = {
    {"get_name",       Method_GetName},
    {"get_class",      Method_GetClass},
    {"get_signature",  Method_GetSignature},
    {"call",           Method_Call},
    {"hook",           Method_Hook},
    {"unhook",         Method_Unhook},
    {"is_hooked",      Method_IsHooked},
    {"get_address",    Method_GetAddress},
    {"__tostring",     Method_ToString},
    {nullptr, nullptr}
};

const luaL_Reg* LuaBinding_GetMethodMethods()
{
    return method_methods;
}
