/**
 * ============================================================
 * il2cpp_resolver.cpp — IL2CPP 运行时桥接层实现
 * ============================================================
 * 本文件实现 il2cpp_resolver.h 中声明的 Il2CppResolver 类 
 *
 * 核心职责
 * 
 * ·通过 GetProcAddress 动态解析 GameAssembly.dll 导出的 40+ 个 IL2CPP 运行时函数 获取函数指针
 * ·将这些 C 风格函数指针封装为类型安全的 C++ 方法
 * ·提供 Image 全量缓存和 Class 按需缓存 加速重复查找
 * ·线程安全：所有缓存操作通过 mutex 保护
 *
 * 技术要点
 * 
 * ·GameAssembly.dll 是 Unity IL2CPP 打包后的核心原生库
 * ·IL2CPP 导出函数名以  il2cpp_ 为前缀 可通过 dump 出的头文件 获得完整函数签名
 * ·Windows x64 下所有函数使用默认 __stdcall 或 __cdecl 调用约定
 * ·（实际上 IL2CPP 导出函数使用 C 调用约定 在 x64 下统一为 Microsoft x64 ABI）
 * 
 * 不做跨平台适配 仅 Windows x64
 * ============================================================
 */

#include "il2cpp_resolver.h"
#include "pipe_channel.h"

#include <cstdio>
#include <cstdarg>

// ============================================================
// 单例获取
// ============================================================
Il2CppResolver& Il2CppResolver::Instance()
{
    // C++11 魔法静态 第一次调用时构造 后续直接返回引用
    // 线程安全 无需额外锁
    static Il2CppResolver instance;
    return instance;
}

// ============================================================
// 初始化
// ============================================================
// 初始化流程
// 
// ·解析 GameAssembly.dll 的全部导出函数
// ·获取 IL2CPP 应用域 (Domain)
// ·将当前线程 attach 到 IL2CPP 运行时（IL2CPP 要求所有访问托管对象的线程必须先 attach）
// ·遍历所有程序集 缓存全部 Image（后续 GetClass 查找时使用）
BridgeResult Il2CppResolver::Init()
{
    // 防止重复初始化
    if (m_initialized) return BridgeResult::ERR_ALREADY_INITIALIZED;

    // 解析导出函数
    // 如果关键函数解析失败 整个桥接层无法工作
    if (!ResolveExports()) return BridgeResult::ERR_IL2CPP_RESOLVE_FAILED;

    // 调用 il2cpp_domain_get() 获取应用域
    // Domain 是 IL2CPP 的根对象 所有程序集都挂在 Domain 下
    if (m_domain_get != nullptr) m_domain = m_domain_get();

    if (m_domain == nullptr) return BridgeResult::ERR_IL2CPP_RESOLVE_FAILED;

    // 调用 il2cpp_thread_attach() 附加到当前 IL2CPP 线程
    // IL2CPP 维护一个线程表 只有 attach 过的线程才能安全调用
    // IL2CPP API 不 attach 会导致随机崩溃或返回无效数据 
    if (m_thread_attach != nullptr && m_domain != nullptr) m_thread = m_thread_attach(m_domain);

    // 缓存所有 Image
    // Image 是程序集的元数据容器 一个程序集对应一个 Image 
    // 我们在初始化时一次性获取所有 Image 避免后续每次 GetClass
    // 都重新枚举程序集（减少开销和临时对象） 
    CacheAllImages();

    // 标记初始化完成
    m_initialized = true;
    return BridgeResult::OK;
}

// ============================================================
// 关闭
// ============================================================
void Il2CppResolver::Shutdown()
{
    // 如果线程已 attach detach 它
    // 不 detach 会导致线程退出时 IL2CPP 内部状态不一致
    if (m_thread_detach != nullptr && m_thread != nullptr)
    {
        // 调用 il2cpp_thread_detach()
        m_thread_detach(m_thread);
        m_thread = nullptr;
    }

    // 清空缓存
    {
        // 清空缓存
        std::lock_guard<std::mutex> lock(m_mutex);
        m_imageCache.clear();
        m_assemblyCache.clear();
        m_classCache.clear();
    }

    // 清空所有函数指针（防止 Shutdown 后误调用）
    m_domain_get = nullptr;
    m_domain_get_assemblies = nullptr;
    m_assembly_get_image = nullptr;
    m_image_get_assembly = nullptr;
    m_image_get_name = nullptr;
    m_image_get_class_count = nullptr;
    m_image_get_class = nullptr;
    m_thread_attach = nullptr;
    m_thread_detach = nullptr;
    m_class_from_name = nullptr;
    m_class_get_image = nullptr;
    m_class_is_valuetype = nullptr;
    m_class_is_enum = nullptr;
    m_class_get_name = nullptr;
    m_class_get_namespace = nullptr;
    m_class_get_parent = nullptr;
    m_class_instance_size = nullptr;
    m_class_get_type = nullptr;
    m_class_get_element_class = nullptr;
    m_class_value_size = nullptr;
    m_method_get_class = nullptr;
    m_class_get_method_from_name = nullptr;
    m_class_get_methods = nullptr;
    m_method_get_name = nullptr;
    m_method_get_param_name = nullptr;
    m_method_get_param_count = nullptr;
    m_method_get_return_type = nullptr;
    m_method_get_param = nullptr;
    m_method_get_flags = nullptr;
    m_class_is_assignable_from = nullptr;
    m_class_get_field_from_name = nullptr;
    m_class_get_fields = nullptr;
    m_field_get_name = nullptr;
    m_field_get_parent = nullptr;
    m_field_get_type = nullptr;
    m_field_get_offset = nullptr;
    m_field_get_flags = nullptr;
    m_field_get_value = nullptr;
    m_field_set_value = nullptr;
    m_field_static_get_value = nullptr;
    m_field_static_set_value = nullptr;
    m_object_new = nullptr;
    m_runtime_invoke = nullptr;
    m_runtime_class_init = nullptr;
    m_string_new = nullptr;
    m_string_new_len = nullptr;
    m_string_chars = nullptr;
    m_string_length = nullptr;
    m_value_box = nullptr;
    m_object_unbox = nullptr;
    m_type_get_type = nullptr;
    m_type_get_name = nullptr;
    m_class_from_type = nullptr;
    m_type_get_object = nullptr;
    m_array_new = nullptr;

    // 重置状态
    m_domain = nullptr;
    m_initialized = false;
}

// ============================================================
// 解析 GameAssembly.dll 的全部导出函数
// ============================================================
// 使用 GetModuleHandleW 获取已加载的 GameAssembly.dll 模块句柄
// 然后通过 GetProcAddress 逐个解析每个 IL2CPP 导出函数
//
// 返回 true 表示所有关键函数都已成功解析
// 非关键函数（如 type_get_object、array_new）可能不存在于旧版
// Unity 中 解析失败不会导致整体失败 但相关功能将不可用
bool Il2CppResolver::ResolveExports()
{
    // 获取 GameAssembly.dll 模块句柄
    // 使用 GetModuleHandleW 而非 LoadLibraryW：DLL 被注入时游戏
    // 已经加载了 GameAssembly.dll 无需再次加载
    HMODULE hGameAssembly = GetModuleHandleW(L"GameAssembly.dll");
    // 游戏可能还没加载 GameAssembly.dll 或者不是 IL2CPP 游戏
    if (hGameAssembly == nullptr) return false;

    // 辅助宏：解析单个导出函数
    // # 将 GetProcAddress 的返回值（FARPROC）转换为正确的函数指针类型
    // # 并赋值给成员变量 如果函数不存在 指针保持 nullptr 
    #define RESOLVE(name, type) ++m_totalFunctions; \
        m_##name = reinterpret_cast<type>(GetProcAddress(hGameAssembly, "il2cpp_" #name)); \
        if (m_##name == nullptr) m_failedFunctions.push_back("il2cpp_" #name)

    // 域 / 程序集 / 镜像
    RESOLVE(domain_get, pfn_domain_get);
    RESOLVE(domain_get_assemblies, pfn_domain_get_assemblies);
    RESOLVE(assembly_get_image, pfn_assembly_get_image);
    RESOLVE(image_get_assembly, pfn_image_get_assembly);
    RESOLVE(image_get_name, pfn_image_get_name);
    RESOLVE(image_get_class_count, pfn_image_get_class_count);
    RESOLVE(image_get_class, pfn_image_get_class);

    // 线程管理
    RESOLVE(thread_attach, pfn_thread_attach);
    RESOLVE(thread_detach, pfn_thread_detach);

    // 类操作
    RESOLVE(class_from_name, pfn_class_from_name);
    RESOLVE(class_get_image, pfn_class_get_image);
    RESOLVE(class_is_valuetype, pfn_class_is_valuetype);
    RESOLVE(class_is_enum, pfn_class_is_enum);
    RESOLVE(class_get_name, pfn_class_get_name);
    RESOLVE(class_get_namespace, pfn_class_get_namespace);
    RESOLVE(class_get_parent, pfn_class_get_parent);
    RESOLVE(class_instance_size, pfn_class_instance_size);
    RESOLVE(class_get_type, pfn_class_get_type);
    RESOLVE(class_get_element_class, pfn_class_get_element_class);
    RESOLVE(class_value_size, pfn_class_value_size);

    // 方法操作
    RESOLVE(method_get_class, pfn_method_get_class);
    RESOLVE(class_get_method_from_name, pfn_class_get_method_from_name);
    RESOLVE(class_get_methods, pfn_class_get_methods);
    RESOLVE(method_get_name, pfn_method_get_name);
    RESOLVE(method_get_param_name, pfn_method_get_param_name);
    RESOLVE(method_get_param_count, pfn_method_get_param_count);
    RESOLVE(method_get_return_type, pfn_method_get_return_type);
    RESOLVE(method_get_param, pfn_method_get_param);
    RESOLVE(method_get_flags, pfn_method_get_flags);
    RESOLVE(class_is_assignable_from, pfn_class_is_assignable_from);

    // 字段操作
    RESOLVE(class_get_field_from_name, pfn_class_get_field_from_name);
    RESOLVE(class_get_fields, pfn_class_get_fields);
    RESOLVE(field_get_name, pfn_field_get_name);
    RESOLVE(field_get_parent, pfn_field_get_parent);
    RESOLVE(field_get_type, pfn_field_get_type);
    RESOLVE(field_get_offset, pfn_field_get_offset);
    RESOLVE(field_get_flags, pfn_field_get_flags);
    RESOLVE(field_get_value, pfn_field_get_value);
    RESOLVE(field_set_value, pfn_field_set_value);
    RESOLVE(field_static_get_value, pfn_field_static_get_value);
    RESOLVE(field_static_set_value, pfn_field_static_set_value);

    // 运行时调用
    RESOLVE(object_new, pfn_object_new);
    RESOLVE(runtime_invoke, pfn_runtime_invoke);
    RESOLVE(runtime_class_init, pfn_runtime_class_init);

    // 字符串
    RESOLVE(string_new, pfn_string_new);
    RESOLVE(string_new_len, pfn_string_new_len);
    RESOLVE(string_new_utf16, pfn_string_new_utf16);
    RESOLVE(string_chars, pfn_string_chars);
    RESOLVE(string_length, pfn_string_length);

    //  装箱/拆箱
    RESOLVE(value_box, pfn_value_box);
    RESOLVE(object_unbox, pfn_object_unbox);

    // 类型信息
    RESOLVE(type_get_type, pfn_type_get_type);
    RESOLVE(type_get_name, pfn_type_get_name);
    RESOLVE(class_from_type, pfn_class_from_type);
    RESOLVE(type_get_object, pfn_type_get_object);

    // 数组
    RESOLVE(array_new, pfn_array_new);

    // 取消宏定义 避免污染后续代码
    #undef RESOLVE

    // 检查关键函数是否解析成功
    // 以下函数是桥接层正常工作的最低要求 任何一个缺失都意味着
    // IL2CPP 运行时无法正常交互
    if (m_domain_get == nullptr) return false;
    if (m_domain_get_assemblies == nullptr) return false;
    if (m_assembly_get_image == nullptr) return false;  
    if (m_thread_attach == nullptr) return false;
    if (m_class_from_name == nullptr) return false;
    if (m_class_get_methods == nullptr) return false;
    if (m_method_get_name == nullptr) return false;
    if (m_object_new == nullptr) return false;
    if (m_runtime_invoke == nullptr) return false;

    // 非关键函数允许为 null（旧版 Unity 可能缺少）
    // 使用相关功能时会检查并返回错误
    return true;
}

// ============================================================
// 获取解析导出函数的结果
// ============================================================
std::string Il2CppResolver::GetResolveStatus() const
{
    if (m_failedFunctions.empty()) return "All " + std::to_string(m_totalFunctions) + " functions resolved";

    std::string result = std::to_string(m_totalFunctions - m_failedFunctions.size())
        + "/" + std::to_string(m_totalFunctions) + " resolved, missing: ";
    for (size_t i = 0; i < m_failedFunctions.size(); ++i)
    {
        if (i > 0) result += ", ";
        result += m_failedFunctions[i];
    }

    return result;
}

// ============================================================
// 缓存所有 Image
// ============================================================
// 遍历 IL2CPP Domain 下的所有程序集 将每个程序集的 Image 指针
// 存入 m_imageCache 后续 GetClass 查找时遍历这个缓存即可 
void Il2CppResolver::CacheAllImages()
{
    // 安全检查：确保所需函数指针已就绪
    if (m_domain == nullptr || m_domain_get_assemblies == nullptr || m_assembly_get_image == nullptr) return;

    // 调用 il2cpp_domain_get_assemblies 获取程序集数组
    // 返回值是 Il2CppAssembly**（程序集指针数组） count 接收数量
    size_t count = 0;
    Il2CppAssembly** assemblies = m_domain_get_assemblies(m_domain, &count);

    if (assemblies == nullptr || count == 0) return;

    // 遍历每个程序集 获取其 Image 并加入缓存
    for (size_t i = 0; i < count; ++i)
    {
        // 跳过空指针（理论上不会发生 但防御性编程）
        if (assemblies[i] == nullptr) continue;

        // 获取程序集对应的 Image
        Il2CppImage* image = m_assembly_get_image(assemblies[i]);
        if (image != nullptr)
        {
            m_assemblyCache.push_back(assemblies[i]);
            m_imageCache.push_back(image);
        }
    }

    // 注意：assemblies 指针指向的内存由 IL2CPP 运行时管理 
    // 我们不需要释放它 但这个指针只在当前线程 attach 状态下有效 
}

// ============================================================
// 程序集与镜像查询
// ============================================================
Il2CppAssembly* Il2CppResolver::GetAssemblyAt(int32_t index) const
{
    if (index < 0 || static_cast<size_t>(index) >= m_assemblyCache.size()) return nullptr;
    return m_assemblyCache[static_cast<size_t>(index)];
}

static std::string NormalizeAssemblyName(const std::string& value)
{
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (result.size() > 4 && result.compare(result.size() - 4, 4, ".dll") == 0)
        result.resize(result.size() - 4);
    return result;
}

Il2CppAssembly* Il2CppResolver::GetAssembly(const std::string& name) const
{
    const std::string wanted = NormalizeAssemblyName(name);
    for (Il2CppAssembly* assembly : m_assemblyCache)
    {
        const char* current = GetAssemblyName(assembly);
        if (current != nullptr && NormalizeAssemblyName(current) == wanted) return assembly;
    }
    return nullptr;
}

const char* Il2CppResolver::GetAssemblyName(Il2CppAssembly* assembly) const
{
    if (assembly == nullptr || m_assembly_get_image == nullptr || m_image_get_name == nullptr) return nullptr;
    Il2CppImage* image = m_assembly_get_image(assembly);
    return image != nullptr ? m_image_get_name(image) : nullptr;
}

Il2CppAssembly* Il2CppResolver::GetClassAssembly(Il2CppClass* klass) const
{
    if (klass == nullptr || m_class_get_image == nullptr) return nullptr;
    const Il2CppImage* image = m_class_get_image(klass);
    if (image == nullptr) return nullptr;

    // 优先利用初始化时建立的 Assembly/Image 对应缓存，
    // 旧版 Unity 缺少 image_get_assembly 导出时仍可正常反查。
    for (size_t i = 0; i < m_imageCache.size() && i < m_assemblyCache.size(); ++i)
    {
        if (m_imageCache[i] == image) return m_assemblyCache[i];
    }

    return m_image_get_assembly != nullptr
        ? const_cast<Il2CppAssembly*>(m_image_get_assembly(image))
        : nullptr;
}

Il2CppClass* Il2CppResolver::GetClass(Il2CppAssembly* assembly, const std::string& namespaze,
    const std::string& className) const
{
    if (assembly == nullptr || m_assembly_get_image == nullptr || m_class_from_name == nullptr) return nullptr;
    Il2CppImage* image = m_assembly_get_image(assembly);
    return image != nullptr ? m_class_from_name(image, namespaze.c_str(), className.c_str()) : nullptr;
}

int32_t Il2CppResolver::GetAssemblyClassCount(Il2CppAssembly* assembly) const
{
    if (assembly == nullptr || m_assembly_get_image == nullptr || m_image_get_class_count == nullptr) return -1;
    Il2CppImage* image = m_assembly_get_image(assembly);
    if (image == nullptr) return -1;
    const size_t count = m_image_get_class_count(image);
    return count > static_cast<size_t>(INT32_MAX) ? INT32_MAX : static_cast<int32_t>(count);
}

Il2CppClass* Il2CppResolver::GetAssemblyClassAt(Il2CppAssembly* assembly, int32_t index) const
{
    if (assembly == nullptr || index < 0 || m_assembly_get_image == nullptr || m_image_get_class == nullptr)
        return nullptr;
    Il2CppImage* image = m_assembly_get_image(assembly);
    if (image == nullptr) return nullptr;
    const size_t count = m_image_get_class_count != nullptr ? m_image_get_class_count(image) : 0;
    if (static_cast<size_t>(index) >= count) return nullptr;
    return m_image_get_class(image, static_cast<size_t>(index));
}

// ============================================================
// 按命名空间 + 类名查找类
// ============================================================
// 查找策略
// 
// ·先查缓存（m_classCache） 命中则直接返回
// ·未命中则遍历所有缓存的 Image 对每个 Image 调用
// ·il2cpp_class_from_name 尝试查找
// ·找到后存入缓存 供下次查询使用
//
// 参数
// 
// ·namespaze — 命名空间（如 "UnityEngine"、"System"、"" 表示全局）
// ·className — 类名（如 "Object"、"Transform"）
// 
// 返回：Il2CppClass* 指针 未找到返回 nullptr
Il2CppClass* Il2CppResolver::GetClass(const std::string& namespaze, const std::string& className)
{
    // 构造缓存键：(命名空间, 类名)
    auto key = std::make_pair(namespaze, className);

    // 先查缓存
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_classCache.find(key);
        if (it != m_classCache.end()) return it->second;
    }

    // 遍历所有 Image 查找
    // il2cpp_class_from_name 接受 (Image*, namespace, name) 三个参数 
    // 在指定 Image 的范围内查找类 我们需要遍历所有 Image 才能找到 
    if (m_class_from_name == nullptr) return nullptr;

    Il2CppClass* result = nullptr;

    // 遍历缓存的 Image 列表
    // 注意：这里不需要加锁读取 m_imageCache 因为它在 Init 后只读不写
    for (Il2CppImage* image : m_imageCache)
    {
        if (image == nullptr) continue;

        // 尝试在当前 Image 中查找类
        Il2CppClass* klass = m_class_from_name(image, namespaze.c_str(), className.c_str());
        // 找到了 停止遍历
        if (klass != nullptr)
        {
            result = klass;
            break;
        }
    }

    // 存入缓存
    // 即使 result 为 nullptr 也缓存 避免重复查找不存在的类
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_classCache[key] = result;
    }

    return result;
}

// ============================================================
// 类信息获取方法
// ============================================================

// 获取类名（不含命名空间）
const char* Il2CppResolver::GetClassSimpleName(Il2CppClass* klass) const
{
    // 空指针检查
    if (klass == nullptr || m_class_get_name == nullptr) return nullptr;
    // 调用 il2cpp_class_get_name 返回内部静态字符串指针 无需释放
    return m_class_get_name(klass);
}

// 获取命名空间
const char* Il2CppResolver::GetClassNamespace(Il2CppClass* klass) const
{
    if (klass == nullptr || m_class_get_namespace == nullptr) return nullptr;
    return m_class_get_namespace(klass);
}

// 获取父类
Il2CppClass* Il2CppResolver::GetClassParent(Il2CppClass* klass) const
{
    if (klass == nullptr || m_class_get_parent == nullptr) return nullptr;
    return m_class_get_parent(klass);
}

// 获取对象实例大小（含对象头 16 字节）
int32_t Il2CppResolver::GetClassInstanceSize(Il2CppClass* klass) const
{
    if (klass == nullptr || m_class_instance_size == nullptr) return 0;
    return m_class_instance_size(klass);
}

// 获取类的 Il2CppType*
const Il2CppType* Il2CppResolver::GetClassType(Il2CppClass* klass) const
{
    if (klass == nullptr || m_class_get_type == nullptr) return nullptr;
    return m_class_get_type(klass);
}

bool Il2CppResolver::IsValueType(Il2CppClass* klass) const
{
    if (klass == nullptr) return false;
    if (m_class_is_valuetype != nullptr) return m_class_is_valuetype(klass);
    const int32_t type = GetTypeEnum(GetClassType(klass));
    return type == Il2CppTypeEnum::TYPE_VALUETYPE || type == Il2CppTypeEnum::TYPE_ENUM;
}

bool Il2CppResolver::IsEnum(Il2CppClass* klass) const
{
    if (klass == nullptr) return false;
    if (m_class_is_enum != nullptr) return m_class_is_enum(klass);
    return GetTypeEnum(GetClassType(klass)) == Il2CppTypeEnum::TYPE_ENUM;
}

// ============================================================
// 方法查找与信息
// ============================================================

// 按方法名查找（不限定参数个数） 返回第一个同名方法
// il2cpp_class_get_method_from_name 需要指定参数个数 
// 如果指定了错误的个数会返回 nullptr 因此我们通过遍历
// 所有方法来按名字查找 忽略参数个数 
const Il2CppMethod* Il2CppResolver::GetMethod(Il2CppClass* klass, const std::string& name) const
{
    if (klass == nullptr || m_class_get_methods == nullptr || m_method_get_name == nullptr) return nullptr;

    // 使用迭代器遍历类的所有方法
    // IL2CPP 的 class_get_methods 使用 void* 迭代器模式
    // 初始传入 iter = nullptr 每次调用返回下一个方法并更新 iter 
    // 返回 nullptr 表示遍历结束 
    void* iter = nullptr;
    const Il2CppMethod* method = nullptr;

    while ((method = m_class_get_methods(klass, &iter)) != nullptr)
    {
        // 获取当前方法名
        const char* methodName = m_method_get_name(method);
        // 名字匹配 返回该方法
        if (methodName != nullptr && name == methodName) return method;
    }

    // 未找到
    return nullptr;
}

// 方法名
const char* Il2CppResolver::GetMethodName(const Il2CppMethod* method) const
{
    if (method == nullptr || m_method_get_name == nullptr) return nullptr;
    return m_method_get_name(method);
}

// 指定位置参数的元数据名称
const char* Il2CppResolver::GetMethodParamName(const Il2CppMethod* method, int32_t index) const
{
    if (method == nullptr || index < 0 || m_method_get_param_name == nullptr) return nullptr;
    return m_method_get_param_name(method, index);
}

// 参数个数
int32_t Il2CppResolver::GetMethodParamCount(const Il2CppMethod* method) const
{
    if (method == nullptr || m_method_get_param_count == nullptr) return 0;
    return m_method_get_param_count(method);
}

// 返回值类型
const Il2CppType* Il2CppResolver::GetMethodReturnType(const Il2CppMethod* method) const
{
    if (method == nullptr || m_method_get_return_type == nullptr) return nullptr;
    return m_method_get_return_type(method);
}

// 指定位置参数的类型
const Il2CppType* Il2CppResolver::GetMethodParamType(const Il2CppMethod* method, int32_t index) const
{
    if (method == nullptr || m_method_get_param == nullptr) return nullptr;
    return m_method_get_param(method, index);
}

// 方法标志位
uint32_t Il2CppResolver::GetMethodFlags(const Il2CppMethod* method) const
{
    if (method == nullptr || m_method_get_flags == nullptr) return 0;
    // il2cpp_method_get_flags 签名为 (method, uint32_t* iflags)
    // 第二个参数输出方法实现标志 传 nullptr 表示不关心
    return m_method_get_flags(method, nullptr);
}

// 判断是否静态方法
bool Il2CppResolver::IsStaticMethod(const Il2CppMethod* method) const
{
    uint32_t flags = GetMethodFlags(method);
    // 检查 METHOD_FLAG_STATIC (0x0010) 位
    return (flags & METHOD_FLAG_STATIC) != 0;
}

// 获取方法所属的类
Il2CppClass* Il2CppResolver::GetMethodClass(const Il2CppMethod* method) const
{
    if (method == nullptr) return nullptr;
    if (m_method_get_class != nullptr) return m_method_get_class(method);
    // 注意: MethodInfo 的第一个字段是 methodPointer（offset 0x00）
    // 不是 klass 因此不能直接按偏移 0 读取
    // 导出缺失时返回 nullptr 由调用方决定是否可用
    return nullptr;
}

// 判断 source 实例是否可以作为 target 类型使用
bool Il2CppResolver::IsAssignableFrom(Il2CppClass* target, Il2CppClass* source) const
{
    if (target == nullptr || source == nullptr) return false;
    if (target == source) return true;
    if (m_class_is_assignable_from != nullptr) return m_class_is_assignable_from(target, source);

    // 旧版 Unity 缺少导出时，至少沿父类链检查普通类继承关系。
    Il2CppClass* current = source;
    while (current != nullptr)
    {
        if (current == target) return true;
        current = GetClassParent(current);
    }
    return false;
}

// ========================================================
// 线程与原生方法
// ========================================================

// 将当前线程附加到 IL2CPP 运行时
// Hook 回调可能发生在任意游戏线程 调用 IL2CPP API 前必须先附加
// 官方签名: Il2CppThread* il2cpp_thread_attach(Il2CppDomain* domain)
// 已附加的线程重复调用会返回已有的 Il2CppThread* 因此可以每次调用
Il2CppThread* Il2CppResolver::AttachThread() const
{
    if (!m_initialized || m_thread_attach == nullptr || m_domain == nullptr) return nullptr;
    return m_thread_attach(m_domain);
}

// 读取 MethodInfo 中的原生函数指针
// MethodInfo 布局: methodPointer 始终位于 offset 0x00（见 common.h）
void* Il2CppResolver::GetMethodPointer(const Il2CppMethod* method) const
{
    if (method == nullptr) return nullptr;
    return READ_OFFSET(method, METHODINFO_METHODPOINTER_OFFSET, void*)[0];
}

// ============================================================
// 字段查找与信息
// ============================================================

// 按字段名查找
const Il2CppField* Il2CppResolver::GetField(Il2CppClass* klass, const std::string& name) const
{
    if (klass == nullptr || m_class_get_field_from_name == nullptr) return nullptr;
    return m_class_get_field_from_name(klass, name.c_str());
}

// 字段名
const char* Il2CppResolver::GetFieldName(const Il2CppField* field) const
{
    if (field == nullptr || m_field_get_name == nullptr) return nullptr;
    return m_field_get_name(field);
}

// 字段声明所属的类
Il2CppClass* Il2CppResolver::GetFieldClass(const Il2CppField* field) const
{
    if (field == nullptr || m_field_get_parent == nullptr) return nullptr;
    return m_field_get_parent(field);
}

// 字段类型
const Il2CppType* Il2CppResolver::GetFieldType(const Il2CppField* field) const
{
    if (field == nullptr || m_field_get_type == nullptr) return nullptr;
    return m_field_get_type(field);
}

// 字段偏移（在对象内的字节偏移量）
// 对于实例字段 偏移从对象头（16 字节）之后开始
// 对于静态字段 偏移是相对于类的静态数据区
int32_t Il2CppResolver::GetFieldOffset(const Il2CppField* field) const
{
    if (field == nullptr || m_field_get_offset == nullptr) return 0;
    return m_field_get_offset(field);
}

uint32_t Il2CppResolver::GetFieldFlags(const Il2CppField* field) const
{
    return field != nullptr && m_field_get_flags != nullptr ? m_field_get_flags(field) : 0;
}

bool Il2CppResolver::IsStaticField(const Il2CppField* field) const
{
    // System.Reflection.FieldAttributes.Static = 0x0010。
    constexpr uint32_t FIELD_ATTRIBUTE_STATIC = 0x0010;
    return (GetFieldFlags(field) & FIELD_ATTRIBUTE_STATIC) != 0;
}

// ============================================================
// 字段读写
// ============================================================

// 读取实例字段值
// 将 obj 对象中 field 对应的字段值复制到 outValue 指向的缓冲区
// outValue 缓冲区大小必须足够容纳字段值（至少 sizeof(void*) 字节）
void Il2CppResolver::ReadField(Il2CppObject* obj, const Il2CppField* field, void* outValue) const
{
    if (obj == nullptr || field == nullptr || outValue == nullptr || m_field_get_value == nullptr) return;
    // il2cpp_field_get_value 内部根据字段类型大小进行内存复制
    m_field_get_value(obj, field, outValue);
}

// 写入实例字段值
void Il2CppResolver::WriteField(Il2CppObject* obj, const Il2CppField* field, void* value) const
{
    if (obj == nullptr || field == nullptr || value == nullptr || m_field_set_value == nullptr) return;
    m_field_set_value(obj, field, value);
}

// 读取静态字段值
// 静态字段不绑定到实例 直接通过 FieldInfo 读取
void Il2CppResolver::ReadStaticField(const Il2CppField* field, void* outValue) const
{
    if (field == nullptr || outValue == nullptr || m_field_static_get_value == nullptr) return;
    m_field_static_get_value(field, outValue);
}

// 写入静态字段值
void Il2CppResolver::WriteStaticField(const Il2CppField* field, void* value) const
{
    if (field == nullptr || value == nullptr || m_field_static_set_value == nullptr) return;
    m_field_static_set_value(field, value);
}

// ============================================================
// 运行时调用
// ============================================================

// 创建对象（分配内存 不调用构造函数）
// 返回的指针对象已初始化 klass 头部 但字段值未初始化
// 如需完整构造 应通过 RuntimeInvoke 调用 .ctor 方法
Il2CppObject* Il2CppResolver::ObjectNew(Il2CppClass* klass) const
{
    if (klass == nullptr || m_object_new == nullptr) return nullptr;
    return m_object_new(klass);
}

// 通过 runtime_invoke 调用方法 这是 IL2CPP 提供的安全调用方式
//
// ·自动处理虚方法分派
// ·自动装箱/拆箱参数
// ·捕获 C# 异常到 outExc
//
// 参数
// 
// ·method  — 要调用的方法
// ·obj     — this 指针（静态方法传 nullptr）
// ·params  — 参数指针数组 每个元素指向一个参数值
// ·outExc  — [out] 接收异常对象指针 无异常时为 nullptr
// 
// 返回：方法的返回值（装箱后的托管对象）
// void 方法返回 nullptr
Il2CppObject* Il2CppResolver::RuntimeInvoke(const Il2CppMethod* method, void* obj, void** params, Il2CppException** outExc) const
{
    if (method == nullptr || m_runtime_invoke == nullptr) return nullptr;

    // 确保 outExc 有初始值
    if (outExc != nullptr) *outExc = nullptr;

    // 关键：在调用方法前 确保类的静态构造函数(.cctor)已执行
    // 未初始化的类在第一次调用方法时可能导致访问违规崩溃
    Il2CppClass* klass = GetMethodClass(method);
    if (klass != nullptr && m_runtime_class_init != nullptr) m_runtime_class_init(klass);

    return m_runtime_invoke(method, obj, params, outExc);
}

// 触发类的静态构造函数
// IL2CPP 的静态字段在首次访问前可能未初始化（.cctor 未执行）
// 调用此方法确保静态构造函数已执行
void Il2CppResolver::RuntimeClassInit(Il2CppClass* klass) const
{
    if (klass == nullptr || m_runtime_class_init == nullptr) return;
    m_runtime_class_init(klass);
}

// ============================================================
// 字符串操作
// ============================================================

// 从 C 字符串（UTF-8）创建 IL2CPP 托管字符串
//
// 参考 frida-il2cpp-bridge 实现 il2cpp_string_new 直接接受 UTF-8 编码字符串
// IL2CPP 运行时内部完成 UTF-8→UTF-16 转换
Il2CppString* Il2CppResolver::StringNew(const char* str) const
{
    if (str == nullptr) return nullptr;

    if (m_string_new == nullptr) return nullptr;

    return m_string_new(str);
}

// 从 UTF-16 字符串创建托管字符串
Il2CppString* Il2CppResolver::StringNewUtf16(const uint16_t* utf16, int32_t len) const
{
    if (utf16 == nullptr || m_string_new_utf16 == nullptr) return nullptr;
    return m_string_new_utf16(utf16, len);
}

// 从指定长度的字节创建托管字符串（可包含嵌入的 null）
Il2CppString* Il2CppResolver::StringNewLen(const char* str, uint32_t len) const
{
    if (str == nullptr) return nullptr;

    if (m_string_new_len != nullptr) return m_string_new_len(str, len);

    // 回退：创建临时null结尾字符串
    std::string tmp(str, len);
    return StringNew(tmp.c_str());
}

// 获取字符串的 UTF-16 字符数组指针
// 返回的指针直接指向托管字符串内部的字符数据 无需释放
const uint16_t* Il2CppResolver::StringChars(Il2CppString* str) const
{
    if (str == nullptr || m_string_chars == nullptr) return nullptr;
    return m_string_chars(str);
}

// 获取字符串长度（UTF-16 码元数 不是字节数）
int32_t Il2CppResolver::StringLength(Il2CppString* str) const
{
    if (str == nullptr || m_string_length == nullptr) return 0;
    return m_string_length(str);
}

// ============================================================
// 装箱 / 拆箱
// ============================================================
// 装箱 将值类型数据包装为托管对象
// 参数
// 
// ·klass — 值类型的 Il2CppClass
// ·data  — 指向值类型数据的指针
// 返回装箱后的 Il2CppObject*
Il2CppObject* Il2CppResolver::Box(Il2CppClass* klass, void* data) const
{
    if (klass == nullptr || data == nullptr || m_value_box == nullptr) return nullptr;
    return m_value_box(klass, data);
}

// 拆箱：获取托管对象内部的值类型数据指针
// 返回的指针指向对象头之后的值类型数据区域
void* Il2CppResolver::Unbox(Il2CppObject* obj) const
{
    if (obj == nullptr || m_object_unbox == nullptr) return nullptr;
    return m_object_unbox(obj);
}

// ============================================================
// 类型信息
// ============================================================
// 获取 Il2CppType 的类型枚举值（Il2CppTypeEnum）
// 返回值可用于判断是值类型、引用类型、基本类型等
int32_t Il2CppResolver::GetTypeEnum(const Il2CppType* type) const
{
    if (type == nullptr || m_type_get_type == nullptr) return 0;
    return m_type_get_type(type);
}

// 获取类型的字符串名称
const char* Il2CppResolver::GetTypeName(const Il2CppType* type) const
{
    if (type == nullptr || m_type_get_name == nullptr) return nullptr;
    return m_type_get_name(type);
}

// 从 Il2CppType 获取对应的 Il2CppClass
Il2CppClass* Il2CppResolver::GetClassFromType(const Il2CppType* type) const
{
    if (type == nullptr || m_class_from_type == nullptr) return nullptr;
    return m_class_from_type(type);
}

// 获取数组类的元素类型
Il2CppClass* Il2CppResolver::GetElementClass(Il2CppClass* klass) const
{
    if (klass == nullptr || m_class_get_element_class == nullptr) return nullptr;
    return m_class_get_element_class(klass);
}

// 获取值类型的实际大小
// 官方签名: int32_t il2cpp_class_value_size(Il2CppClass* klass, uint32_t* align)
int32_t Il2CppResolver::ClassValueSize(Il2CppClass* klass, uint32_t* align) const
{
    if (klass == nullptr || m_class_value_size == nullptr) return 0;
    return m_class_value_size(klass, align);
}

// 从 Il2CppType 获取 System.Type 托管对象
// 这是 FindObjectsOfType 的前置步骤
// 
// ·IL2CPP 的 FindObjectsOfType 需要一个 System.Type 参数 
// ·而我们手里只有 Il2CppType* 需要通过此函数转换为
// ·System.Type 的托管对象 
Il2CppObject* Il2CppResolver::GetTypeObject(const Il2CppType* type) const
{
    if (type == nullptr || m_type_get_object == nullptr) return nullptr;
    return m_type_get_object(type);
}

// ============================================================
// 数组操作
// ============================================================
// 创建一维零基数组
// 参数
// 
// ·elementClass — 数组元素的类型
// ·length       — 数组长度（官方类型为 il2cpp_array_size_t 即 uint32_t）
// 返回 Il2CppArray* 指针
Il2CppArray* Il2CppResolver::ArrayNew(Il2CppClass* elementClass, uint32_t length) const
{
    if (elementClass == nullptr || m_array_new == nullptr) return nullptr;
    return m_array_new(elementClass, length);
}

// 读取数组长度
// 直接从内存布局读取 不依赖导出函数
// Il2CppArrayLayout 的 max_length 字段位于 offset 0x18
uint64_t Il2CppResolver::ArrayLength(Il2CppArray* arr) const
{
    if (arr == nullptr) return 0;
    // 直接读取数组对象 offset 0x18 处的 uint64_t 值
    return READ_OFFSET(arr, 0x18, uint64_t)[0];
}

// ============================================================
// GC 对象查找
// ============================================================
// 通过 Unity 的 UnityEngine.Object.FindObjectsOfType(Type) 查找
// 堆上存活的指定类型对象 
//
// 实现步骤
// 
// ·从 klass 获取 Il2CppType*
// ·从 Il2CppType* 获取 System.Type 托管对象
// ·查找 UnityEngine.Object 类
// ·在 UnityEngine.Object 上查找 FindObjectsOfType 方法
// ·调用该方法 传入 System.Type 作为参数
// ·返回结果数组
//
// 限制
// 
// ·仅对继承自 UnityEngine.Object 的类型有效
// ·旧版 Unity 使用 FindObjectsOfType 新版可能需要 FindObjectsByType
// ·需要 type_get_object 导出函数存在
Il2CppArray* Il2CppResolver::FindObjectsOfType(Il2CppClass* klass)
{
    // 前置检查
    if (klass == nullptr) return nullptr;

    // 需要 type_get_object 导出函数
    if (m_type_get_object == nullptr || m_class_get_type == nullptr) return nullptr;

    // 获取 Il2CppType*
    const Il2CppType* type = m_class_get_type(klass);
    if (type == nullptr) return nullptr;

    // 转换为 System.Type 托管对象
    Il2CppObject* typeObject = m_type_get_object(type);
    if (typeObject == nullptr) return nullptr;

    // 查找 UnityEngine.Object 类
    Il2CppClass* unityObject = GetClass("UnityEngine", "Object");
    if (unityObject == nullptr) return nullptr;

    // 查找 FindObjectsOfType 方法
    // 该方法签名为: static Object[] FindObjectsOfType(Type type)
    // 参数个数为 1
    const Il2CppMethod* findMethod = nullptr;

    // 优先尝试 FindObjectsOfType
    if (m_class_get_method_from_name != nullptr) findMethod = m_class_get_method_from_name(unityObject, "FindObjectsOfType", 1);

    // 如果没找到 尝试备选名称 FindObjectsByType（Unity 2023+）
    // 该方法签名为: static Object[] FindObjectsByType(Type type, FindObjectsSortMode sortMode)
    // 参数个数为 2 需要额外传入排序模式（0 = None）
    bool useFindObjectsByType = false;
    if (findMethod == nullptr && m_class_get_method_from_name != nullptr)
    {
        findMethod = m_class_get_method_from_name(unityObject, "FindObjectsByType", 2);
        if (findMethod != nullptr) useFindObjectsByType = true;
    }

    if (findMethod == nullptr) return nullptr;

    // 准备参数并调用
    // RuntimeInvoke 的 params 是 void** 数组 每个元素指向一个参数值
    // 对于 FindObjectsOfType(Type type) params[0] = typeObject
    void* params[2] = {};
    // 第一个参数：System.Type 对象的地址
    params[0] = typeObject;

    // 如果是 FindObjectsByType 还需要第二个参数 FindObjectsSortMode
    // FindObjectsSortMode.None = 0
    int32_t sortMode = 0;
    // 第二个参数：排序模式
    if (useFindObjectsByType) params[1] = &sortMode;

    // 调用方法（静态方法 obj 传 nullptr）
    Il2CppException* exc = nullptr;
    Il2CppObject* result = RuntimeInvoke(findMethod, nullptr, params, &exc);

    // 如果发生异常 返回 nullptr
    if (exc != nullptr) return nullptr;

    // 返回值就是 Object[] 数组
    return reinterpret_cast<Il2CppArray*>(result);
}

// ============================================================
// 方法/字段枚举
// ============================================================
// 遍历类的所有方法 写入 outList 数组
// 返回实际写入的方法数量
// 使用 class_get_methods 的迭代器模式逐个获取
int32_t Il2CppResolver::EnumerateMethods(Il2CppClass* klass, const Il2CppMethod** outList, int32_t maxCount) const
{
    if (klass == nullptr || outList == nullptr || maxCount <= 0 || m_class_get_methods == nullptr) return 0;

    // 已找到的方法数
    int32_t count = 0;
    // 迭代器 初始必须为 nullptr
    void* iter = nullptr;
    const Il2CppMethod* method = nullptr;

    // 逐个遍历方法
    while ((method = m_class_get_methods(klass, &iter)) != nullptr)
    {
        // 达到最大数量 停止
        if (count >= maxCount) break;
        // 写入输出数组
        outList[count] = method;  
        // 计数
        ++count;
    }

    return count;
}

// 遍历类的所有字段 写入 outList 数组
int32_t Il2CppResolver::EnumerateFields(Il2CppClass* klass, const Il2CppField** outList, int32_t maxCount) const
{
    if (klass == nullptr || outList == nullptr || maxCount <= 0 || m_class_get_fields == nullptr) return 0;

    int32_t count = 0;
    void* iter = nullptr;
    const Il2CppField* field = nullptr;

    while ((field = m_class_get_fields(klass, &iter)) != nullptr)
    {
        if (count >= maxCount) break;
        outList[count] = field;
        ++count;
    }

    return count;
}
