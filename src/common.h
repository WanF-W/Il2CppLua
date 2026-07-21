/**
 * ============================================================
 * common.h — 公共基础设施
 * ============================================================
 * 本文件定义整个项目共享的基础类型
 * 
 * ·统一错误码枚举 BridgeResult
 * ·IL2CPP 原生结构体的前置声明
 * ·IL2CPP 类型枚举 Il2CppTypeEnum（与运行时完全一致）
 * ·IL2CPP 关键结构体的内存布局（x64 专用）
 * ·通用辅助宏
 *
 * 仅针对 Windows x64 + Unity IL2CPP 未做任何跨平台适配
 * ============================================================
 */
#pragma once

// ---- 标准库 ----
#include <cstdint>    // 固定宽度整数类型
#include <cstddef>    // size_t, nullptr
#include <cstring>    // memcpy, strlen
#include <string>     // std::string, std::wstring
#include <vector>     // std::vector
#include <map>        // std::map（类缓存）
#include <mutex>      // std::mutex, std::lock_guard
#include <functional> // std::function（输出回调）
#include <utility>    // std::pair, std::move

// ============================================================
// 统一错误码
// ============================================================
// 所有模块的公共返回值
// 正值或 0 表示成功
// 负值表示失败
enum class BridgeResult : int32_t
{
    OK                        =  0,   // 成功
    ERR_UNKNOWN               = -1,   // 未知错误
    ERR_INVALID_PARAM         = -2,   // 参数无效
    ERR_NOT_INITIALIZED       = -3,   // 模块未初始化
    ERR_ALREADY_INITIALIZED   = -4,   // 模块已初始化（重复调用 Init）
    ERR_IL2CPP_RESOLVE_FAILED = -5,   // 无法解析 GameAssembly.dll 导出函数
    ERR_LUA_INIT_FAILED       = -6,   // Lua 虚拟机创建失败
    ERR_CONSOLE_INIT_FAILED   = -7,   // 控制台/管道初始化失败
    ERR_CLASS_NOT_FOUND       = -8,   // 找不到指定的 IL2CPP 类
    ERR_LUA_EXECUTE_FAILED    = -9,   // Lua 代码执行失败
    ERR_METHOD_NOT_FOUND      = -10,  // 找不到指定的方法
    ERR_FIELD_NOT_FOUND       = -11,  // 找不到指定的字段
    ERR_INVOKE_FAILED         = -12,  // 方法调用（runtime_invoke）失败
    ERR_TYPE_MISMATCH         = -13,  // 类型不匹配
    ERR_NULL_OBJECT           = -14,  // 操作了空对象
    ERR_PIPE_CONNECT_FAILED   = -15,  // 管道连接失败
    ERR_HOOK_FAILED           = -16,  // Hook 安装失败
};

// ============================================================
// IL2CPP 原生结构体前置声明
// ============================================================
// 这些结构体的真实布局由 IL2CPP 运行时定义 我们只需要指针
// 操作它们 因此用不透明指针（opaque pointer）即可
struct Il2CppDomain;          // 应用域 IL2CPP 的根对象
struct Il2CppThread;          // 托管线程
struct Il2CppAssembly;        // 程序集（DLL）
struct Il2CppImage;           // 程序集镜像（元数据容器）
struct Il2CppClass;           // 类/类型定义
struct Il2CppObject;          // 托管对象实例（所有对象的基类布局）
struct Il2CppMethod;          // 方法定义（MethodInfo）
struct Il2CppField;           // 字段定义（FieldInfo）
struct Il2CppType;            // 对象类型（Il2CppType）
struct Il2CppString;          // 托管字符串
struct Il2CppArray;           // 托管数组
struct Il2CppException;       // 托管异常

// ============================================================
// IL2CPP 类型枚举（Il2CppType）
// ============================================================
// 与 IL2CPP 运行时 Il2CppTypeEnum 完全一致
// 用于判断字段/方法参数/返回值的具体类型
enum Il2CppTypeEnum : uint8_t
{
    TYPE_END         = 0x00, // 列表结束标记
    TYPE_VOID        = 0x01, // void
    TYPE_BOOLEAN     = 0x02, // bool
    TYPE_CHAR        = 0x03, // char (UTF-16)
    TYPE_I1          = 0x04, // sbyte  (int8)
    TYPE_U1          = 0x05, // byte   (uint8)
    TYPE_I2          = 0x06, // short  (int16)
    TYPE_U2          = 0x07, // ushort (uint16)
    TYPE_I4          = 0x08, // int    (int32)
    TYPE_U4          = 0x09, // uint   (uint32)
    TYPE_I8          = 0x0a, // long   (int64)
    TYPE_U8          = 0x0b, // ulong  (uint64)
    TYPE_R4          = 0x0c, // float  (single)
    TYPE_R8          = 0x0d, // double
    TYPE_STRING      = 0x0e, // string
    TYPE_PTR         = 0x0f, // 指针类型 T*
    TYPE_BYREF       = 0x10, // 引用类型 ref T
    TYPE_VALUETYPE   = 0x11, // 值类型（struct）
    TYPE_CLASS       = 0x12, // 引用类型（class）
    TYPE_VAR         = 0x13, // 类泛型参数 T
    TYPE_ARRAY       = 0x14, // 多维数组
    TYPE_GENERICINST = 0x15, // 泛型实例化类型
    TYPE_TYPEDBYREF  = 0x16, // TypedReference
    TYPE_I           = 0x18, // IntPtr (平台相关整数)
    TYPE_U           = 0x19, // UIntPtr
    TYPE_FNPTR       = 0x1b, // 函数指针
    TYPE_OBJECT      = 0x1c, // object
    TYPE_SZARRAY     = 0x1d, // 一维零基数组 T[]
    TYPE_MVAR        = 0x1e, // 方法泛型参数 TMethod
    TYPE_ENUM        = 0x55, // 枚举（底层是值类型）
};

// ============================================================
// IL2CPP 结构体内存布局 (Windows x64)
// ============================================================
// 这些布局基于 IL2CPP 在 x64 下的实际内存排列
// 我们直接按偏移量读写 不依赖导出函数（某些导出可能不存在）

/**
 * Il2CppObject 的内存布局（x64）
 * 所有托管对象都以这个头部开始
 * 
 * ·offset 0x00: klass 指针 — 指向该对象的 Il2CppClass
 * ·offset 0x08: monitor 指针 — 用于线程同步（lock 语句）
 * 
 * 实例字段数据紧随头部之后 从 offset 0x10 开始
 */
struct Il2CppObjectLayout
{
    void* klass;       // 0x00: Il2CppClass*
    void* monitor;     // 0x08: 监视器/同步块
};
// 对象头大小 = 16 字节（2 个指针）
constexpr uint32_t OBJECT_HEADER_SIZE = sizeof(Il2CppObjectLayout);

/**
 * Il2CppString 的内存布局（x64）
 * 
 * ·offset 0x00: klass 指针
 * ·offset 0x08: monitor 指针
 * ·offset 0x10: int32_t length — 字符串长度（UTF-16 码元数）
 * ·offset 0x14: uint16_t chars[] — 实际字符数据（UTF-16LE）
 */
struct Il2CppStringLayout
{
    void*    klass;     // 0x00
    void*    monitor;   // 0x08
    int32_t  length;    // 0x10: 字符长度
    uint16_t chars[1];  // 0x14: 首字符（柔性数组）
};

/**
 * Il2CppArray 的内存布局（x64 一维零基数组）
 * 
 * ·offset 0x00: klass 指针
 * ·offset 0x08: monitor 指针
 * ·offset 0x10: bounds 指针（多维数组用 一维为 null）
 * ·offset 0x18: uint64_t max_length — 数组长度
 * ·offset 0x20: 首个元素数据开始
 */
struct Il2CppArrayLayout
{
    void*    klass;       // 0x00
    void*    monitor;     // 0x08
    void*    bounds;      // 0x10: 多维数组边界信息
    uint64_t max_length;  // 0x18: 元素个数
};
// 数组元素数据起始偏移
constexpr uint32_t ARRAY_DATA_OFFSET = 0x20;

/**
 * MethodInfo 的关键字段偏移（x64）
 * IL2CPP 的 MethodInfo 结构体很大且版本相关 
 * 但methodPointer始终位于 offset 0x00 
 * 这是方法 Hook 的核心操作位置
 */
constexpr uint32_t METHODINFO_METHODPOINTER_OFFSET = 0x00;
// 方法标志位（用于判断静态/实例/虚方法等）
constexpr uint32_t METHOD_FLAG_STATIC    = 0x0010;  // 静态方法
constexpr uint32_t METHOD_FLAG_VIRTUAL   = 0x0040;  // 虚方法

// ============================================================
// 辅助宏
// ============================================================
// 消除未使用参数警告
#define BRIDGE_UNUSED(x) (void)(x)

// 数组元素个数
#define BRIDGE_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// 安全读取指针偏移处的值（带空指针检查）
// 用法：READ_OFFSET(objPtr, offset, Type) -> Type*
// 注意：宏内部使用 const_cast 处理 const 指针 
// 使其对 const 和非 const 指针均安全可用
#define READ_OFFSET(obj, off, type) (reinterpret_cast<type*>(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(obj)) + (off)))

// 判断指针是否对齐有效（粗略检查 x64 指针应 8 字节对齐）
#define IS_VALID_PTR(p) ((p) != nullptr && (reinterpret_cast<uintptr_t>(p) & 0x7) == 0)
