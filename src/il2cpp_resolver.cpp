// 运行时导出解析、元数据查询和 IL2CPP 原语；只依赖 LuaEngine 的故障状态，不调用 Lua C API。
// 导出完整性在 Init 检查；缓存名称由本层拥有，元数据地址由运行时拥有。
#include "il2cpp_resolver.h"
#include "lua_engine.h" // lifecycle guard only; no Lua C API in this layer

#include <algorithm>
#include <cctype>
#include <windows.h>
// 单例获取
Il2CppResolver& Il2CppResolver::Instance()
{
    // C++11 魔法静态 第一次调用时构造 后续直接返回引用
    // 线程安全 无需额外锁
    static Il2CppResolver instance;
    return instance;
}
// 初始化
// 初始化流程
//
// ·解析 GameAssembly.dll 的全部导出函数
// ·获取 IL2CPP 应用域 (Domain)
// ·将当前线程 attach 到 IL2CPP 运行时（IL2CPP 要求所有访问托管对象的线程必须先 attach）
// ·遍历所有程序集 缓存全部 Image（后续 GetClass 查找时使用）
BridgeResult Il2CppResolver::Init()
{
    if (m_initialized) return BridgeResult::ERR_ALREADY_INITIALIZED;

    // Init 可能在 GameAssembly 尚未加载时被重复尝试。每次尝试都从干净的
    // 统计状态开始，否则失败导出名称和计数会跨重试累加，误导诊断信息。
    m_totalFunctions = 0;
    m_failedFunctions.clear();

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
    if (m_thread_attach == nullptr || m_domain == nullptr
        || (m_thread = m_thread_attach(m_domain)) == nullptr)
    {
        return BridgeResult::ERR_IL2CPP_RESOLVE_FAILED;
    }

    // 缓存所有 Image
    // Image 是程序集的元数据容器 一个程序集对应一个 Image
    // 我们在初始化时一次性获取所有 Image 避免后续每次 GetClass
    // 都重新枚举程序集（减少开销和临时对象）
    CacheAllImages();

    m_initialized = true;
    return BridgeResult::OK;
}
// 关闭
void Il2CppResolver::Shutdown()
{
    if (LuaEngine::Instance().IsFaulted()) return;
    // 这里只 detach Init 所在线程。Hook 回调线程的附加状态故意保持到进程
    // 结束，避免在未知回调生命周期中错误 detach；详见 README 的限制说明。
    if (m_thread_detach != nullptr && m_thread != nullptr)
    {
        // 调用 il2cpp_thread_detach()
        m_thread_detach(m_thread);
        m_thread = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_imageCache.clear();
        m_assemblyCache.clear();
        m_classCache.clear();
        m_typeNames.clear();
        m_closedClasses.clear();
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
    m_method_is_generic = nullptr;
    m_method_is_inflated = nullptr;
    m_method_get_class = nullptr;
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
    m_string_new_len = nullptr;
    m_string_chars = nullptr;
    m_string_length = nullptr;
    m_object_to_string = nullptr;
    m_value_box = nullptr;
    m_object_unbox = nullptr;
    m_type_get_type = nullptr;
    m_type_get_name = nullptr;
    m_class_from_type = nullptr;
    m_type_get_object = nullptr;
    m_type_is_byref = nullptr;
    m_free = nullptr;
    m_gchandle_new = nullptr;
    m_gchandle_free = nullptr;
    m_class_has_references = nullptr;
    m_gc_wbarrier_set_field = nullptr;
    m_array_new = nullptr;

    // 重置状态
    m_domain = nullptr;
    m_totalFunctions = 0;
    m_failedFunctions.clear();
    m_initialized = false;
}

void Il2CppResolver::DetachInitializationThread()
{
    if (m_thread_detach != nullptr && m_thread != nullptr)
    {
        m_thread_detach(m_thread);
        m_thread = nullptr;
    }
}

void Il2CppResolver::DetachThread(Il2CppThread* thread) const
{
    if (thread != nullptr && m_thread_detach != nullptr)
        m_thread_detach(thread);
}
// 解析 GameAssembly.dll 的全部导出函数
// 使用 GetModuleHandleW 获取已加载的 GameAssembly.dll 模块句柄
// 然后通过 GetProcAddress 逐个解析每个 IL2CPP 导出函数
//
// 返回 true 表示必需导出均可用；object_to_string 仅增强诊断，不参与硬性检查。
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
    RESOLVE(method_is_generic, pfn_method_is_generic);
    RESOLVE(method_is_inflated, pfn_method_is_inflated);
    RESOLVE(method_get_class, pfn_method_get_class);
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
    RESOLVE(string_new_len, pfn_string_new_len);
    RESOLVE(string_chars, pfn_string_chars);
    RESOLVE(string_length, pfn_string_length);

    // ToString 只用于增强异常诊断，不作为初始化硬依赖。老版本或裁剪过的
    // GameAssembly 可能没有这个导出，此时桥接层仍应正常工作并回退到通用文本。
    m_object_to_string = reinterpret_cast<pfn_object_to_string>(
        GetProcAddress(hGameAssembly, "il2cpp_object_to_string"));

    //  装箱/拆箱
    RESOLVE(value_box, pfn_value_box);
    RESOLVE(object_unbox, pfn_object_unbox);

    // 类型信息
    RESOLVE(type_get_type, pfn_type_get_type);
    RESOLVE(type_get_name, pfn_type_get_name);
    RESOLVE(class_from_type, pfn_class_from_type);
    RESOLVE(type_get_object, pfn_type_get_object);

    // 数组
    RESOLVE(type_is_byref, pfn_type_is_byref);
    RESOLVE(free, pfn_free);
    RESOLVE(gchandle_new, pfn_gchandle_new);
    RESOLVE(gchandle_free, pfn_gchandle_free);
    RESOLVE(class_has_references, pfn_class_has_references);
    RESOLVE(gc_wbarrier_set_field, pfn_gc_wbarrier_set_field);
    RESOLVE(array_new, pfn_array_new);

    // 取消宏定义 避免污染后续代码
    #undef RESOLVE

    // 元数据、值转换与 GC API 构成完整契约；缺失时不能假装读写成功。
    return m_failedFunctions.empty();
}
// 获取解析导出函数的结果
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
// 缓存所有 Image
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
// 程序集与镜像查询
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

    // 优先利用初始化时建立的 Assembly/Image 对应缓存；
    // 缓存之外的镜像通过 image_get_assembly 查询。
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
// 按命名空间 + 类名查找类
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
// 类信息获取方法
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
    return klass != nullptr && m_class_is_valuetype != nullptr && m_class_is_valuetype(klass);
}

bool Il2CppResolver::IsEnum(Il2CppClass* klass) const
{
    return klass != nullptr && m_class_is_enum != nullptr && m_class_is_enum(klass);
}
// 方法查找与信息
// 按方法名查找（不限定参数个数） 返回第一个同名方法
// il2cpp_class_get_method_from_name 需要指定参数个数
// 如果指定了错误的个数会返回 nullptr 因此我们通过遍历
// 所有方法来按名字查找 忽略参数个数
const Il2CppMethod* Il2CppResolver::GetMethod(Il2CppClass* klass, const std::string& name) const
{
    if (klass == nullptr || m_class_get_methods == nullptr || m_method_get_name == nullptr) return nullptr;

    for (auto* current = klass; current != nullptr; current = GetClassParent(current))
    {
        void* iter = nullptr;
        while (const auto* method = m_class_get_methods(current, &iter))
        {
            const char* methodName = m_method_get_name(method);
            if (methodName != nullptr && name == methodName) return method;
        }
        if (name == ".ctor" || name == ".cctor") break;
    }
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
    return target != nullptr && source != nullptr && m_class_is_assignable_from != nullptr
        && m_class_is_assignable_from(target, source);
}
// 线程与原生方法
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
// 字段查找与信息
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
// 字段读写
// 读取实例字段值
// 将 obj 对象中 field 对应的字段值复制到 outValue 指向的缓冲区
// outValue 缓冲区大小必须足够容纳字段的实际值，大小由调用方按类型确定。
void Il2CppResolver::ReadField(Il2CppObject* obj, const Il2CppField* field, void* outValue) const
{
    if (obj == nullptr || field == nullptr || outValue == nullptr || m_field_get_value == nullptr) return;
    // il2cpp_field_get_value 内部根据字段类型大小进行内存复制
    m_field_get_value(obj, field, outValue);
    LuaEngine::Instance().RequireHealthy();
}

// 写入实例字段值
void Il2CppResolver::WriteField(Il2CppObject* obj, const Il2CppField* field, void* value) const
{
    if (obj == nullptr || field == nullptr || m_field_set_value == nullptr) return;
    m_field_set_value(obj, field, value);
    LuaEngine::Instance().RequireHealthy();
}

// 读取静态字段值
// 静态字段不绑定到实例 直接通过 FieldInfo 读取
void Il2CppResolver::ReadStaticField(const Il2CppField* field, void* outValue) const
{
    if (field == nullptr || outValue == nullptr || m_field_static_get_value == nullptr) return;
    m_field_static_get_value(field, outValue);
    LuaEngine::Instance().RequireHealthy();
}

// 写入静态字段值
void Il2CppResolver::WriteStaticField(const Il2CppField* field, void* value) const
{
    if (field == nullptr || m_field_static_set_value == nullptr) return;
    m_field_static_set_value(field, value);
    LuaEngine::Instance().RequireHealthy();
}
// 运行时调用
// 返回的指针对象已初始化 klass 头部 但字段值未初始化
// 如需完整构造 应通过 RuntimeInvoke 调用 .ctor 方法
Il2CppObject* Il2CppResolver::ObjectNew(Il2CppClass* klass) const
{
    if (m_object_new == nullptr || !CanUseClass(klass)) return nullptr;
    auto* result = m_object_new(klass);
    LuaEngine::Instance().RequireHealthy();
    return result;
}

// 通过 runtime_invoke 调用方法 这是 IL2CPP 提供的安全调用方式
//
// ·调用指定 MethodInfo；调用方负责选择虚方法和拆箱值类型 this
// ·值参数传存储地址，引用参数传对象本身
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
    // 确保 outExc 有初始值
    if (outExc != nullptr) *outExc = nullptr;
    if (m_runtime_invoke == nullptr || !CanInvokeMethod(method)) return nullptr;

    // runtime_invoke 自己处理静态初始化并捕获托管异常。
    auto* result = m_runtime_invoke(method, obj, params, outExc);
    LuaEngine::Instance().RequireHealthy();
    return result;
}

// 触发类的静态构造函数
// IL2CPP 的静态字段在首次访问前可能未初始化（.cctor 未执行）
// 调用此方法确保静态构造函数已执行
void Il2CppResolver::RuntimeClassInit(Il2CppClass* klass) const
{
    if (m_runtime_class_init == nullptr || !CanUseClass(klass)) return;
    m_runtime_class_init(klass);
    LuaEngine::Instance().RequireHealthy();
}
// 字符串操作
// 从指定长度的字节创建托管字符串（可包含嵌入的 null）
Il2CppString* Il2CppResolver::StringNewLen(const char* str, uint32_t len) const
{
    if (str == nullptr) return nullptr;

    auto* result = m_string_new_len != nullptr ? m_string_new_len(str, len) : nullptr;
    LuaEngine::Instance().RequireHealthy();
    return result;
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

Il2CppString* Il2CppResolver::ObjectToString(Il2CppObject* object) const
{
    if (object == nullptr) return nullptr;

    // 优先使用官方导出；部分 Unity 版本虽然能正常 runtime_invoke，
    // 但不会导出 il2cpp_object_to_string，因此这里保留方法调用兜底。
    if (m_object_to_string != nullptr)
    {
        Il2CppString* result = m_object_to_string(object);
        LuaEngine::Instance().RequireHealthy();
        if (result != nullptr) return result;
    }

    // 异常对象和普通托管对象一样，首字段是实际运行时类指针。直接查找
    // ToString() 并通过 runtime_invoke 调用，兼容缺少 object_to_string 导出的版本。
    Il2CppClass* klass = READ_OFFSET(object, 0, Il2CppClass*)[0];
    const Il2CppMethod* method = GetMethod(klass, "ToString");
    if (method == nullptr || GetMethodParamCount(method) != 0) return nullptr;

    Il2CppException* exception = nullptr;
    Il2CppObject* result = RuntimeInvoke(method, object, nullptr, &exception);
    if (exception != nullptr || result == nullptr) return nullptr;
    return reinterpret_cast<Il2CppString*>(result);
}
// 装箱 / 拆箱
// 装箱 将值类型数据包装为托管对象
// 参数
//
// ·klass — 值类型的 Il2CppClass
// ·data  — 指向值类型数据的指针
// 返回装箱后的 Il2CppObject*
Il2CppObject* Il2CppResolver::Box(Il2CppClass* klass, void* data) const
{
    if (data == nullptr || m_value_box == nullptr || !CanUseClass(klass)) return nullptr;
    auto* result = m_value_box(klass, data);
    LuaEngine::Instance().RequireHealthy();
    return result;
}

// 拆箱：获取托管对象内部的值类型数据指针
// 返回的指针指向对象头之后的值类型数据区域
void* Il2CppResolver::Unbox(Il2CppObject* obj) const
{
    if (obj == nullptr || m_object_unbox == nullptr) return nullptr;
    return m_object_unbox(obj);
}
// 类型信息
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
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_typeNames.find(type);
    if (it != m_typeNames.end()) return it->second.c_str();
    char* allocated = m_type_get_name(type);
    if (allocated == nullptr) return nullptr;
    // 导出返回 il2cpp_alloc 内存；缓存拥有副本，调用方借用到 Shutdown。
    try { it = m_typeNames.emplace(type, allocated).first; }
    catch (...) { m_free(allocated); throw; }
    m_free(allocated);
    return it->second.c_str();
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
// UnityEngine 的反射 API 需要 System.Type；该通用转换由 Unity 适配层复用。
Il2CppObject* Il2CppResolver::GetTypeObject(const Il2CppType* type) const
{
    if (type == nullptr || m_type_get_object == nullptr) return nullptr;
    return m_type_get_object(type);
}
// 数组操作
// 创建一维零基数组
// 参数
//
// ·elementClass — 数组元素的类型
// ·length       — 数组长度（官方类型为 il2cpp_array_size_t 即 uint32_t）
// 返回 Il2CppArray* 指针
Il2CppArray* Il2CppResolver::ArrayNew(Il2CppClass* elementClass, uint32_t length) const
{
    if (m_array_new == nullptr || !CanUseClass(elementClass)) return nullptr;
    auto* result = m_array_new(elementClass, length);
    LuaEngine::Instance().RequireHealthy();
    return result;
}

// 读取数组长度
// 直接从一维数组布局读取，不依赖额外导出函数。
uint64_t Il2CppResolver::ArrayLength(Il2CppArray* arr) const
{
    if (arr == nullptr) return 0;
    // 直接读取数组对象 offset 0x18 处的 uint64_t 值
    return READ_OFFSET(arr, 0x18, uint64_t)[0];
}

bool Il2CppResolver::ArraySetReference(
    Il2CppArray* arr, uint64_t index, Il2CppObject* value) const
{
    if (arr == nullptr || m_gc_wbarrier_set_field == nullptr || index >= ArrayLength(arr))
        return false;
    auto slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20) + index;
    m_gc_wbarrier_set_field(reinterpret_cast<Il2CppObject*>(arr), slot, value);
    return true;
}
// 方法/字段枚举
// 遍历类的所有方法 写入 outList 数组
// 返回实际写入的方法数量
// 使用 class_get_methods 的迭代器模式逐个获取
const Il2CppMethod* Il2CppResolver::NextMethod(Il2CppClass* klass, void*& iterator) const
{
    return klass != nullptr && m_class_get_methods != nullptr
        ? m_class_get_methods(klass, &iterator) : nullptr;
}

const Il2CppField* Il2CppResolver::NextField(Il2CppClass* klass, void*& iterator) const
{
    return klass != nullptr && m_class_get_fields != nullptr
        ? m_class_get_fields(klass, &iterator) : nullptr;
}

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

thread_local bool Il2CppResolver::s_typeQueryActive = false;

bool Il2CppResolver::IsNullableClass(Il2CppClass* klass) const
{
    const char* name = GetClassSimpleName(klass);
    const char* ns = GetClassNamespace(klass);
    return name != nullptr && ns != nullptr && IsValueType(klass)
        && strcmp(name, "Nullable`1") == 0 && strcmp(ns, "System") == 0;
}

bool Il2CppResolver::IsClosedClass(Il2CppClass* klass) const
{
    if (klass == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_closedClasses.find(klass);
        if (found != m_closedClasses.end()) return found->second;
    }
    // Do not hold a cache lock across managed reflection. A user Hook on a
    // reflection getter must not recursively start the same classification.
    if (s_typeQueryActive) return false;
    s_typeQueryActive = true;
    struct QueryGuard { bool& flag; ~QueryGuard() { flag = false; } } queryGuard{s_typeQueryActive};
    Il2CppObject* typeObject = GetTypeObject(GetClassType(klass));
    if (typeObject == nullptr) return false;
    const uint32_t handle = RetainObject(typeObject);
    if (handle == 0) return false;
    struct HandleGuard
    {
        const Il2CppResolver* resolver;
        uint32_t handle;
        ~HandleGuard() { resolver->ReleaseObject(handle); }
    } handleGuard{this, handle};
    auto* reflectionClass = READ_OFFSET(typeObject, 0, Il2CppClass*)[0];
    const auto* getter = GetMethod(reflectionClass, "get_ContainsGenericParameters");
    if (getter == nullptr || m_runtime_invoke == nullptr || GetMethodParamCount(getter) != 0
        || IsStaticMethod(getter) || GetTypeEnum(GetMethodReturnType(getter)) != Il2CppTypeEnum::TYPE_BOOLEAN)
        return false;
    Il2CppException* exception = nullptr;
    // Bootstrap classification cannot use the guarded RuntimeInvoke wrapper.
    Il2CppObject* result = m_runtime_invoke(getter, typeObject, nullptr, &exception);
    LuaEngine::Instance().RequireHealthy();
    if (exception != nullptr || result == nullptr) return false;
    const auto* value = static_cast<const bool*>(Unbox(result));
    if (value == nullptr) return false;
    const bool closed = !*value;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closedClasses.emplace(klass, closed);
    }
    return closed;
}

bool Il2CppResolver::CanUseClass(Il2CppClass* klass) const
{
    return klass != nullptr && !IsNullableClass(klass) && IsClosedClass(klass);
}

bool Il2CppResolver::CanMarshalType(const Il2CppType* type) const
{
    if (type == nullptr || IsByRef(type)) return false;
    switch (GetTypeEnum(type))
    {
    case Il2CppTypeEnum::TYPE_VAR:
    case Il2CppTypeEnum::TYPE_MVAR:
        return false;
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    case Il2CppTypeEnum::TYPE_GENERICINST:
    case Il2CppTypeEnum::TYPE_CLASS:
    case Il2CppTypeEnum::TYPE_OBJECT:
    case Il2CppTypeEnum::TYPE_ARRAY:
    case Il2CppTypeEnum::TYPE_SZARRAY:
    case Il2CppTypeEnum::TYPE_PTR:
        return CanUseClass(GetClassFromType(type));
    default:
        return true; // Actual storage/ABI support is checked by the value layer.
    }
}

bool Il2CppResolver::CanInvokeMethod(const Il2CppMethod* method) const
{
    if (method == nullptr || !CanUseClass(GetMethodClass(method))
        || (IsGenericMethod(method) && !IsInflatedMethod(method))
        || !CanMarshalType(GetMethodReturnType(method))) return false;
    const int count = GetMethodParamCount(method);
    if (count < 0) return false;
    for (int i = 0; i < count; ++i)
        if (!CanMarshalType(GetMethodParamType(method, i))) return false;
    return true;
}

bool Il2CppResolver::IsByRef(const Il2CppType* type) const
{
    return type != nullptr && m_type_is_byref != nullptr && m_type_is_byref(type);
}

bool Il2CppResolver::HasReferences(Il2CppClass* klass) const
{
    return klass != nullptr && m_class_has_references != nullptr && m_class_has_references(klass);
}

uint32_t Il2CppResolver::RetainObject(Il2CppObject* obj) const
{
    // 固定句柄同时保证 userdata 缓存的对象地址稳定。
    return obj != nullptr && m_gchandle_new != nullptr ? m_gchandle_new(obj, true) : 0;
}

void Il2CppResolver::ReleaseObject(uint32_t handle) const
{
    if (handle != 0 && m_gchandle_free != nullptr) m_gchandle_free(handle);
}

bool Il2CppResolver::IsGenericMethod(const Il2CppMethod* method) const
{
    return method != nullptr && m_method_is_generic != nullptr && m_method_is_generic(method);
}

bool Il2CppResolver::IsInflatedMethod(const Il2CppMethod* method) const
{
    return method != nullptr && m_method_is_inflated != nullptr && m_method_is_inflated(method);
}
