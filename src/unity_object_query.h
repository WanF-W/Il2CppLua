/**
 * unity_object_query.h — UnityEngine.Object 查询适配
 * 这里集中放置 Unity 层能力。Resolver 只负责通用 IL2CPP 反射和调用，
 * 不直接依赖 UnityEngine 的业务类名。
 */
#pragma once

#include "common.h"

namespace UnityObjectQuery
{
    // 通过 UnityEngine.Object.FindObjectsOfType/FindObjectsByType 查询对象。
    // 仅适用于 UnityEngine.Object 派生类型；找不到对应 API 时返回 nullptr。
    Il2CppArray* FindObjectsOfType(Il2CppClass* klass);
}
