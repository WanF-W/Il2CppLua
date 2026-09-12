/**
 * unity_object_query.cpp — UnityEngine.Object 查询适配实现
 */
#include "unity_object_query.h"
#include "il2cpp_resolver.h"

#include <cstring>

namespace UnityObjectQuery
{
namespace
{
    const Il2CppMethod* FindStaticMethod(
        Il2CppResolver& resolver, Il2CppClass* klass,
        const char* name, int32_t paramCount)
    {
        void* iterator = nullptr;
        while (const auto* method = resolver.NextMethod(klass, iterator))
        {
            const char* methodName = resolver.GetMethodName(method);
            if (methodName != nullptr
                && std::strcmp(methodName, name) == 0
                && resolver.GetMethodParamCount(method) == paramCount
                && resolver.IsStaticMethod(method))
            {
                return method;
            }
        }
        return nullptr;
    }
}

Il2CppArray* FindObjectsOfType(Il2CppClass* klass)
{
    if (klass == nullptr) return nullptr;

    auto& resolver = Il2CppResolver::Instance();
    const Il2CppType* type = resolver.GetClassType(klass);
    Il2CppClass* unityObject = resolver.GetClass("UnityEngine", "Object");
    if (type == nullptr || unityObject == nullptr)
        return nullptr;
    if (!resolver.IsAssignableFrom(unityObject, klass))
        return nullptr;

    Il2CppObject* typeObject = resolver.GetTypeObject(type);
    if (typeObject == nullptr) return nullptr;

    const Il2CppMethod* method = FindStaticMethod(
        resolver, unityObject, "FindObjectsOfType", 1);
    if (method == nullptr)
    {
        method = FindStaticMethod(resolver, unityObject, "FindObjectsByType", 2);
    }
    if (method == nullptr) return nullptr;

    int32_t sortMode = 0; // FindObjectsSortMode.None
    void* params[2] = { typeObject, &sortMode };
    Il2CppException* exception = nullptr;
    Il2CppObject* result = resolver.RuntimeInvoke(
        method, nullptr, params, &exception);
    if (exception != nullptr || result == nullptr) return nullptr;

    return reinterpret_cast<Il2CppArray*>(result);
}
}
