// 方法选择与调用共用实现；Class、Instance、Method 三种入口只负责解析接收者。
#include "lua_binding_internal.h"
#include <cstring>
#include <cstdint>

const Il2CppMethod* LuaBridge_ResolveMethodOverload(
    lua_State* L, Il2CppClass* klass, const char* name, int firstArgIdx, bool staticOnly)
{
    auto& resolver = Il2CppResolver::Instance();
    const int count = lua_gettop(L) - firstArgIdx + 1;
    const bool constructor = std::strcmp(name, ".ctor") == 0;
    for (auto* current = klass; current != nullptr; current = resolver.GetClassParent(current))
    {
        void* iterator = nullptr;
        const Il2CppMethod* best = nullptr;
        int bestScore = -1;
        while (const auto* method = resolver.NextMethod(current, iterator))
        {
            if (std::strcmp(resolver.GetMethodName(method), name) != 0
                || resolver.GetMethodParamCount(method) != count
                || (staticOnly && !resolver.IsStaticMethod(method))) continue;
            if (!resolver.CanInvokeMethod(method)) continue;
            int score = 0;
            for (int arg = 0; arg < count; ++arg)
            {
                const int local = LuaBridge_ScoreArg(L, firstArgIdx + arg, resolver.GetMethodParamType(method, arg));
                if (local < 0) { score = -1; break; }
                score += local;
            }
            if (score > bestScore) { best = method; bestScore = score; }
        }
        // 子类声明优先；只有没有兼容重载时查父类。构造函数不继承。
        if (best != nullptr || constructor) return best;
    }
    return nullptr;
}

int LuaBridge_InvokeMethod(lua_State* L, const Il2CppMethod* method, void* obj, int argStartIdx)
{
    auto& resolver = Il2CppResolver::Instance();
    if (!resolver.CanInvokeMethod(method))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp,
            "method requires closed, supported types (Nullable/ref/open or unavailable type information)");
    const int top = lua_gettop(L);
    const int count = resolver.GetMethodParamCount(method);
    if (!lua_checkstack(L, count * 2 + 8)) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "Lua stack capacity exceeded");
    if (top - argStartIdx + 1 != count)
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "argument count mismatch: expected %d, got %d", count, top - argStartIdx + 1);
    const Il2CppType* returnType = resolver.GetMethodReturnType(method);
    if (returnType == nullptr || resolver.IsByRef(returnType)
        || (resolver.GetTypeEnum(returnType) != Il2CppTypeEnum::TYPE_VOID
            && LuaBridge_GetValueStorageSize(returnType) == 0))
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported return type (ref returns are not supported)");
    auto** params = count > 0 ? static_cast<void**>(LuaBridge_NewBuffer(L, sizeof(void*) * count)) : nullptr;
    for (int i = 0; i < count; ++i)
    {
        const Il2CppType* type = resolver.GetMethodParamType(method, i);
        const size_t size = LuaBridge_GetValueStorageSize(type);
        if (size == 0 || resolver.IsByRef(type))
            return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "unsupported parameter %d (ref/out are not supported)", i + 1);
        void* storage = LuaBridge_NewBuffer(L, size);
        if (!LuaBridge_MarshalArg(L, argStartIdx + i, type, storage, params[i], size))
            return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "failed to marshal argument %d", i + 1);
        // 新建字符串必须跨后续参数分配保持存活。
        if (resolver.GetTypeEnum(type) == Il2CppTypeEnum::TYPE_STRING && params[i] != nullptr)
            LuaBridge_PushInstance(L, static_cast<Il2CppObject*>(params[i]));
    }
    // runtime_invoke 的值类型 this 是数据地址，Object 的继承方法仍用对象头。
    if (resolver.IsStaticMethod(method)) obj = nullptr;
    if (obj != nullptr && resolver.IsValueType(resolver.GetMethodClass(method)))
        obj = resolver.Unbox(static_cast<Il2CppObject*>(obj));
    Il2CppException* exception = nullptr;
    Il2CppObject* result = resolver.RuntimeInvoke(method, obj, params, &exception);
    if (exception != nullptr)
    {
        return LuaEngine::RaiseManagedException(
            L,
            exception,
            "CSharp exception thrown during method invocation (managed exception text unavailable)");
    }
    const int results = LuaBridge_PushReturnValue(L, result, returnType);
    // 返回值复制到调用前栈顶之后，再统一清理临时 userdata。
    // 无参时返回值已在 top + 1；不能用 lua_replace，否则会弹掉返回值。
    if (results != 0) lua_copy(L, -1, top + 1);
    lua_settop(L, top + results);
    return results;
}


