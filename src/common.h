/**
 * common.h — 公共基础设施
 * 本文件定义整个项目共享的基础类型
 *
 * ·启动阶段使用的错误码枚举 BridgeResult
 * ·IL2CPP 原生结构体的前置声明
 * ·IL2CPP 类型枚举 Il2CppTypeEnum（与运行时完全一致）
 * ·少量稳定的 IL2CPP 对象布局常量（x64 专用）
 * ·通用辅助宏
 *
 * 仅针对 Windows x64 + Unity IL2CPP 未做任何跨平台适配
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>

// 进程终止时 Windows 不再允许 DLL 等待工作线程或执行完整桥接清理。
// 这个进程内标志只由 DllMain 的 DLL_PROCESS_DETACH 路径置位，供静态
// 对象析构函数跳过可能阻塞的 Shutdown；正常自卸载路径仍由工作线程显式清理。
namespace bridge_lifecycle
{
    inline std::atomic<bool> g_processTerminating{ false };
}
// 统一错误码
// 启动阶段的公共返回值。运行时绑定 API 使用 Lua 错误或 bool 返回值，
// 不再维护一套没有调用方消费的“全局错误码目录”。
enum class BridgeResult : int32_t
{
    OK                        =  0,
    ERR_ALREADY_INITIALIZED   = -1,
    ERR_IL2CPP_RESOLVE_FAILED = -2,
};
// IL2CPP 原生结构体前置声明
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
// IL2CPP 类型枚举（Il2CppType）
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
// IL2CPP 结构体内存布局 (Windows x64)
// 这些布局基于 IL2CPP 在 x64 下的实际内存排列
// 我们直接按偏移量读写 不依赖导出函数（某些导出可能不存在）

// 字符串内容通过 Il2CppResolver::StringChars 读取，数组长度与元素地址也只
// 在 Resolver/容器实现中按固定偏移读取，避免在公共头中复制一组易失布局结构体。
// 一维零基数组的数据区从对象头后的 0x20 开始。
constexpr uint32_t ARRAY_DATA_OFFSET = 0x20;

/**
 * MethodInfo 的关键字段偏移（x64）
 * IL2CPP 的 MethodInfo 结构体很大且版本相关
 * 但methodPointer始终位于 offset 0x00
 * 这是方法 Hook 的核心操作位置
 */
constexpr uint32_t METHODINFO_METHODPOINTER_OFFSET = 0x00;
// 方法标志位（用于判断静态/实例/虚方法等）
constexpr uint32_t METHOD_FLAG_STATIC = 0x0010;
// 辅助宏
// 消除未使用参数警告
#define BRIDGE_UNUSED(x) (void)(x)

// 读取固定布局中的字段地址。
// 这是原始内存访问辅助宏，不负责验证 obj、地址可读性或对象生命周期；
// 调用方必须先完成相应检查。
#define READ_OFFSET(obj, off, type) (reinterpret_cast<type*>(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(obj)) + (off)))
