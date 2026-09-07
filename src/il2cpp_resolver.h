// 通用 IL2CPP 运行时接口，仅 Windows x64。
// Init 检查必需导出，object_to_string 可选；Image 全量缓存，Class 和类型名按需缓存。
// 生命周期由 DLL 工作线程管理，Shutdown 前必须停止 Hook 并销毁 Lua VM。
#pragma once
#include "common.h"
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class Il2CppResolver
{
public:
    // 单例
    static Il2CppResolver& Instance();

    // 生命周期
    // 初始化：解析导出函数、获取 domain、attach 线程、缓存 image
    BridgeResult Init();

    // 关闭：清空缓存、detach 初始化线程、清空函数指针。
    // Hook 回调线程的 attach 状态按进程生命周期保留，避免错误 detach。
    void Shutdown();

    // 工作线程在等待管道命令时不应长期占用 IL2CPP 的附着状态。
    // Init 和 Lua 初始化完成后调用此函数释放 Init 期间的附着。
    void DetachInitializationThread();

    // 仅供 DLL 生命周期代码判断 Init 线程是否仍由 Resolver 持有。
    bool HasInitializationThread() const { return m_thread != nullptr; }

    // Detach 一次 AttachThread 返回的线程句柄。调用方必须保证该线程
    // 不再访问 IL2CPP；函数指针会在 Resolver::Shutdown 前保持有效。
    void DetachThread(Il2CppThread* thread) const;

    // 获取 Il2CppResolver 类的基础字段
    bool IsInitialized() const { return m_initialized; }
    int GetImageCount() const { return static_cast<int>(m_imageCache.size()); }
    int GetAssemblyCount() const { return static_cast<int>(m_assemblyCache.size()); }
    // 线程与原生方法
    // 将当前线程附加到 IL2CPP 运行时
    // Hook 回调可能发生在任意游戏线程 调用 IL2CPP API 前必须先附加
    Il2CppThread* AttachThread() const;
    // 读取 MethodInfo 中的原生函数指针（methodPointer，始终位于 offset 0x00）
    void* GetMethodPointer(const Il2CppMethod* method) const;
    // 解析导出函数的结果
    std::string GetResolveStatus() const;
    // 程序集与镜像
    Il2CppAssembly* GetAssemblyAt(int32_t index) const;
    Il2CppAssembly* GetAssembly(const std::string& name) const;
    const char* GetAssemblyName(Il2CppAssembly* assembly) const;
    Il2CppAssembly* GetClassAssembly(Il2CppClass* klass) const;
    Il2CppClass* GetClass(Il2CppAssembly* assembly, const std::string& namespaze,
        const std::string& className) const;
    int32_t GetAssemblyClassCount(Il2CppAssembly* assembly) const;
    Il2CppClass* GetAssemblyClassAt(Il2CppAssembly* assembly, int32_t index) const;
    // 类查找与信息
    // 按命名空间+类名查找 结果缓存
    Il2CppClass* GetClass(const std::string& namespaze, const std::string& className);
    const char* GetClassSimpleName(Il2CppClass* klass) const;
    const char* GetClassNamespace(Il2CppClass* klass) const;
    Il2CppClass* GetClassParent(Il2CppClass* klass) const;
    // 获取对象实例大小（含对象头）
    int32_t GetClassInstanceSize(Il2CppClass* klass) const;
    // 获取类的 Il2CppType*
    const Il2CppType* GetClassType(Il2CppClass* klass) const;
    // 判断类是否为值类型 / 枚举
    bool IsValueType(Il2CppClass* klass) const;
    bool IsEnum(Il2CppClass* klass) const;
    // 方法查找与信息
    // 按方法名查找（不考虑参数个数）返回第一个同名方法
    const Il2CppMethod* GetMethod(Il2CppClass* klass, const std::string& name) const;
    // 获取方法名
    const char* GetMethodName(const Il2CppMethod* method) const;
    const char* GetMethodParamName(const Il2CppMethod* method, int32_t index) const;
    // 参数个数
    int32_t GetMethodParamCount(const Il2CppMethod* method) const;
    // 获取返回值类型
    const Il2CppType* GetMethodReturnType(const Il2CppMethod* method) const;
    // 获取指定位置参数的类型
    const Il2CppType* GetMethodParamType(const Il2CppMethod* method, int32_t index) const;
    // 方法标志位（判断静态/虚方法等）
    uint32_t GetMethodFlags(const Il2CppMethod* method) const;
    // 判断是否静态方法
    bool IsGenericMethod(const Il2CppMethod* method) const;
    bool IsInflatedMethod(const Il2CppMethod* method) const;
    bool IsStaticMethod(const Il2CppMethod* method) const;
    // 获取方法所属的类
    Il2CppClass* GetMethodClass(const Il2CppMethod* method) const;
    bool IsAssignableFrom(Il2CppClass* target, Il2CppClass* source) const;
    // 字段查找与信息
    // 按字段名查找
    const Il2CppField* GetField(Il2CppClass* klass, const std::string& name) const;
    // 获取字段名
    const char* GetFieldName(const Il2CppField* field) const;
    Il2CppClass* GetFieldClass(const Il2CppField* field) const;
    // 获取字段类型
    const Il2CppType* GetFieldType(const Il2CppField* field) const;
    // 获取字段偏移（在对象内的字节偏移）
    int32_t GetFieldOffset(const Il2CppField* field) const;
    uint32_t GetFieldFlags(const Il2CppField* field) const;
    bool IsStaticField(const Il2CppField* field) const;
    // 字段读写
    // 读实例字段
    void ReadField(Il2CppObject* obj, const Il2CppField* field, void* outValue) const;
    // 写实例字段
    void WriteField(Il2CppObject* obj, const Il2CppField* field, void* value) const;
    // 读静态字段
    void ReadStaticField(const Il2CppField* field, void* outValue) const;
    // 写静态字段
    void WriteStaticField(const Il2CppField* field, void* value) const;
    // 运行时调用
    Il2CppObject* ObjectNew(Il2CppClass* klass) const;
    // 调用方法（通过 runtime_invoke）
    Il2CppObject* RuntimeInvoke(const Il2CppMethod* method, void* obj, void** params, Il2CppException** outExc) const;
    // 触发类的静态构造函数
    void RuntimeClassInit(Il2CppClass* klass) const;
    // 字符串
    // 从指定长度的 UTF-8 字节创建托管字符串（可包含嵌入 null）
    Il2CppString* StringNewLen(const char* str, uint32_t len) const;
    // 读取字符串的 UTF-16 字符数组
    const uint16_t* StringChars(Il2CppString* str) const;
    // 读取字符串长度
    int32_t StringLength(Il2CppString* str) const;
    // 将托管对象转换为其 ToString() 结果。该导出在部分 IL2CPP 版本中可能不存在，
    // 因此调用方必须允许返回 nullptr，并回退到通用错误文本。
    Il2CppString* ObjectToString(Il2CppObject* object) const;
    // 装箱 / 拆箱
    // 将值类型数据装箱为托管对象
    Il2CppObject* Box(Il2CppClass* klass, void* data) const;
    // 将托管对象拆箱 返回值类型数据指针
    void* Unbox(Il2CppObject* obj) const;
    // 类型信息
    // 获取 Il2CppType 的类型枚举值
    int32_t GetTypeEnum(const Il2CppType* type) const;
    bool IsByRef(const Il2CppType* type) const;
    bool HasReferences(Il2CppClass* klass) const;
    uint32_t RetainObject(Il2CppObject* obj) const;
    void ReleaseObject(uint32_t handle) const;
    // 获取类型的字符串名称
    const char* GetTypeName(const Il2CppType* type) const;
    // 从 Il2CppType 获取对应的 Il2CppClass
    Il2CppClass* GetClassFromType(const Il2CppType* type) const;
    // 获取数组类的元素类型（仅数组类有效）
    Il2CppClass* GetElementClass(Il2CppClass* klass) const;
    // 获取值类型的实际大小（字节）
    int32_t ClassValueSize(Il2CppClass* klass, uint32_t* align) const;
    // 从 Il2CppType 获取 System.Type 托管对象，供 Unity 适配层调用。
    Il2CppObject* GetTypeObject(const Il2CppType* type) const;
    // 数组
    // 创建一维零基数组
    Il2CppArray*  ArrayNew(Il2CppClass* elementClass, uint32_t length) const;
    // 读取数组长度（直接从内存布局读取）
    uint64_t ArrayLength(Il2CppArray* arr) const;
    // 通过 IL2CPP 写屏障写入引用类型数组元素。
    // 所需写屏障导出在初始化时检查。
    bool ArraySetReference(Il2CppArray* arr, uint64_t index, Il2CppObject* value) const;
    // 方法 / 字段枚举
    // 遍历类的所有方法 写入 outList 返回数量
    int32_t EnumerateMethods(Il2CppClass* klass, const Il2CppMethod** outList, int32_t maxCount) const;
    // 遍历类的所有字段
    int32_t EnumerateFields(Il2CppClass* klass, const Il2CppField** outList, int32_t maxCount) const;

private:
    Il2CppResolver()  = default;
    ~Il2CppResolver() = default;
    Il2CppResolver(const Il2CppResolver&) = delete;
    Il2CppResolver& operator=(const Il2CppResolver&) = delete;

    // 解析 GameAssembly.dll 的所有导出函数
    bool ResolveExports();
    // 缓存所有 Image
    void CacheAllImages();
    // IL2CPP 导出函数指针类型
    // --- 域 / 程序集 ---
    typedef Il2CppDomain* (*pfn_domain_get)();
    typedef Il2CppAssembly** (*pfn_domain_get_assemblies)(Il2CppDomain*, size_t*);
    typedef Il2CppImage* (*pfn_assembly_get_image)(const Il2CppAssembly*);
    typedef const Il2CppAssembly* (*pfn_image_get_assembly)(const Il2CppImage*);
    typedef const char* (*pfn_image_get_name)(const Il2CppImage*);
    typedef size_t (*pfn_image_get_class_count)(const Il2CppImage*);
    typedef Il2CppClass* (*pfn_image_get_class)(const Il2CppImage*, size_t);

    // --- 线程 ---
    typedef Il2CppThread* (*pfn_thread_attach)(Il2CppDomain*);
    typedef void (*pfn_thread_detach)(Il2CppThread*);

    // --- 类 ---
    typedef Il2CppClass* (*pfn_class_from_name)(const Il2CppImage*, const char*, const char*);
    typedef const Il2CppImage* (*pfn_class_get_image)(Il2CppClass*);
    typedef bool (*pfn_class_is_valuetype)(Il2CppClass*);
    typedef bool (*pfn_class_is_enum)(Il2CppClass*);
    typedef const char* (*pfn_class_get_name)(Il2CppClass*);
    typedef const char* (*pfn_class_get_namespace)(Il2CppClass*);
    typedef Il2CppClass* (*pfn_class_get_parent)(Il2CppClass*);
    typedef int32_t (*pfn_class_instance_size)(Il2CppClass*);
    typedef const Il2CppType* (*pfn_class_get_type)(Il2CppClass*);
    typedef Il2CppClass* (*pfn_class_get_element_class)(Il2CppClass*);
    typedef int32_t (*pfn_class_value_size)(Il2CppClass*, uint32_t*);

    typedef bool (*pfn_method_is_generic)(const Il2CppMethod*);

    typedef bool (*pfn_method_is_inflated)(const Il2CppMethod*);

    // --- 方法 ---
    typedef Il2CppClass* (*pfn_method_get_class)(const Il2CppMethod*);
    typedef const Il2CppMethod* (*pfn_class_get_methods)(Il2CppClass*, void**);
    typedef const char* (*pfn_method_get_name)(const Il2CppMethod*);
    typedef const char* (*pfn_method_get_param_name)(const Il2CppMethod*, int);
    typedef int32_t (*pfn_method_get_param_count)(const Il2CppMethod*);
    typedef const Il2CppType* (*pfn_method_get_return_type)(const Il2CppMethod*);
    typedef const Il2CppType* (*pfn_method_get_param)(const Il2CppMethod*, int);
    // 官方签名: uint32_t il2cpp_method_get_flags(const MethodInfo*, uint32_t* iflags)
    typedef uint32_t (*pfn_method_get_flags)(const Il2CppMethod*, uint32_t*);
    typedef bool (*pfn_class_is_assignable_from)(Il2CppClass*, Il2CppClass*);

    // --- 字段 ---
    typedef const Il2CppField* (*pfn_class_get_field_from_name)(Il2CppClass*, const char*);
    typedef const Il2CppField* (*pfn_class_get_fields)(Il2CppClass*, void**);
    typedef const char* (*pfn_field_get_name)(const Il2CppField*);
    typedef Il2CppClass* (*pfn_field_get_parent)(const Il2CppField*);
    typedef const Il2CppType* (*pfn_field_get_type)(const Il2CppField*);
    typedef int32_t (*pfn_field_get_offset)(const Il2CppField*);
    typedef uint32_t (*pfn_field_get_flags)(const Il2CppField*);
    typedef void (*pfn_field_get_value)(Il2CppObject*, const Il2CppField*, void*);
    typedef void (*pfn_field_set_value)(Il2CppObject*, const Il2CppField*, void*);
    typedef void (*pfn_field_static_get_value)(const Il2CppField*, void*);
    typedef void (*pfn_field_static_set_value)(const Il2CppField*, void*);

    // --- 运行时 ---
    typedef Il2CppObject* (*pfn_object_new)(Il2CppClass*);
    typedef Il2CppObject* (*pfn_runtime_invoke)(const Il2CppMethod*, void*, void**, Il2CppException**);
    typedef void (*pfn_runtime_class_init)(Il2CppClass*);

    // --- 字符串 ---
    typedef Il2CppString* (*pfn_string_new_len)(const char*, uint32_t);
    typedef uint16_t* (*pfn_string_chars)(Il2CppString*);
    typedef int32_t(*pfn_string_length)(Il2CppString*);
    typedef Il2CppString* (*pfn_object_to_string)(Il2CppObject*);

    // --- 装箱 ---
    typedef Il2CppObject* (*pfn_value_box)(Il2CppClass*, void*);
    typedef void* (*pfn_object_unbox)(Il2CppObject*);

    // --- 类型 ---
    typedef int32_t(*pfn_type_get_type)(const Il2CppType*);
    typedef char* (*pfn_type_get_name)(const Il2CppType*);
    typedef Il2CppClass* (*pfn_class_from_type)(const Il2CppType*);
    typedef Il2CppObject* (*pfn_type_get_object)(const Il2CppType*);

    typedef bool (*pfn_type_is_byref)(const Il2CppType*);

    typedef void (*pfn_free)(void*);

    typedef uint32_t (*pfn_gchandle_new)(Il2CppObject*, bool);

    typedef void (*pfn_gchandle_free)(uint32_t);

    typedef bool (*pfn_class_has_references)(Il2CppClass*);

    typedef void (*pfn_gc_wbarrier_set_field)(Il2CppObject*, void**, void*);

    // --- 数组 ---
    // 官方签名的长度参数类型为 il2cpp_array_size_t（即 uint32_t）
    typedef Il2CppArray* (*pfn_array_new)(Il2CppClass*, uint32_t);
    // 函数指针成员
    pfn_domain_get m_domain_get = nullptr;
    pfn_domain_get_assemblies m_domain_get_assemblies = nullptr;
    pfn_assembly_get_image m_assembly_get_image = nullptr;
    pfn_image_get_assembly m_image_get_assembly = nullptr;
    pfn_image_get_name m_image_get_name = nullptr;
    pfn_image_get_class_count m_image_get_class_count = nullptr;
    pfn_image_get_class m_image_get_class = nullptr;
    pfn_thread_attach m_thread_attach = nullptr;
    pfn_thread_detach m_thread_detach = nullptr;
    pfn_class_from_name m_class_from_name = nullptr;
    pfn_class_get_image m_class_get_image = nullptr;
    pfn_class_is_valuetype m_class_is_valuetype = nullptr;
    pfn_class_is_enum m_class_is_enum = nullptr;
    pfn_class_get_name m_class_get_name = nullptr;
    pfn_class_get_namespace m_class_get_namespace = nullptr;
    pfn_class_get_parent m_class_get_parent = nullptr;
    pfn_class_instance_size m_class_instance_size = nullptr;
    pfn_class_get_type m_class_get_type = nullptr;
    pfn_class_get_element_class m_class_get_element_class = nullptr;
    pfn_class_value_size m_class_value_size = nullptr;
    pfn_method_is_generic m_method_is_generic = nullptr;
    pfn_method_is_inflated m_method_is_inflated = nullptr;
    pfn_method_get_class m_method_get_class = nullptr;
    pfn_class_get_methods m_class_get_methods = nullptr;
    pfn_method_get_name m_method_get_name = nullptr;
    pfn_method_get_param_name m_method_get_param_name = nullptr;
    pfn_method_get_param_count m_method_get_param_count = nullptr;
    pfn_method_get_return_type m_method_get_return_type = nullptr;
    pfn_method_get_param m_method_get_param = nullptr;
    pfn_method_get_flags m_method_get_flags = nullptr;
    pfn_class_is_assignable_from m_class_is_assignable_from = nullptr;
    pfn_class_get_field_from_name m_class_get_field_from_name = nullptr;
    pfn_class_get_fields m_class_get_fields = nullptr;
    pfn_field_get_name m_field_get_name = nullptr;
    pfn_field_get_parent m_field_get_parent = nullptr;
    pfn_field_get_type  m_field_get_type = nullptr;
    pfn_field_get_offset  m_field_get_offset = nullptr;
    pfn_field_get_flags m_field_get_flags = nullptr;
    pfn_field_get_value m_field_get_value = nullptr;
    pfn_field_set_value m_field_set_value = nullptr;
    pfn_field_static_get_value m_field_static_get_value = nullptr;
    pfn_field_static_set_value m_field_static_set_value = nullptr;
    pfn_object_new m_object_new = nullptr;
    pfn_runtime_invoke m_runtime_invoke = nullptr;
    pfn_runtime_class_init m_runtime_class_init = nullptr;
    pfn_string_new_len m_string_new_len = nullptr;
    pfn_string_chars m_string_chars = nullptr;
    pfn_string_length m_string_length = nullptr;
    pfn_object_to_string m_object_to_string = nullptr;
    pfn_value_box m_value_box = nullptr;
    pfn_object_unbox m_object_unbox = nullptr;
    pfn_type_get_type m_type_get_type = nullptr;
    pfn_type_get_name m_type_get_name = nullptr;
    pfn_class_from_type m_class_from_type = nullptr;
    pfn_type_get_object m_type_get_object = nullptr;
    pfn_type_is_byref m_type_is_byref = nullptr;
    pfn_free m_free = nullptr;
    pfn_gchandle_new m_gchandle_new = nullptr;
    pfn_gchandle_free m_gchandle_free = nullptr;
    pfn_class_has_references m_class_has_references = nullptr;
    pfn_gc_wbarrier_set_field m_gc_wbarrier_set_field = nullptr;
    pfn_array_new m_array_new = nullptr;
    // 状态成员
    // 导出函数总数
    int m_totalFunctions = 0;
    // 查找失败的导出函数名称数组
    std::vector<std::string> m_failedFunctions;
    // IL2CPP 应用域
    Il2CppDomain* m_domain = nullptr;
    // 已 attach 的线程
    Il2CppThread* m_thread = nullptr;
    // Assembly 与 Image 按相同下标对应缓存
    std::vector<Il2CppAssembly*> m_assemblyCache;
    std::vector<Il2CppImage*> m_imageCache;
    // 类缓存：key = (命名空间, 类名)
    std::map<std::pair<std::string, std::string>, Il2CppClass*> m_classCache;

    mutable std::map<const Il2CppType*, std::string> m_typeNames;
    mutable std::mutex m_mutex;
    bool m_initialized = false;
};
