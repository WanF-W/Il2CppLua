// DLL 与 Lune 的线协议：[1 字节类型][4 字节长度 LE][负载]。
// Lune 有独立协议头；变更常量时需同步核对两端。I/O 实现仅位于 PipeChannel。
#pragma once
#include "common.h"
#include "version.h"
#include <cstring>
#include <vector>
#include <windows.h>

namespace protocol
{
    // 消息类型定义
    enum MessageType : uint8_t
    {
        // DLL → EXE
        MSG_HELLO = 0x10,   // 握手：负载为 protocol::VERSION
        MSG_READY = 0x11,   // 初始化就绪：负载为状态描述文本
        MSG_LOG   = 0x20,   // Lua print 输出：负载为输出文本
        MSG_ERROR = 0x21,   // 错误信息：负载为 [类别][可选行号][错误描述]
        MSG_OK    = 0x22,   // 命令执行成功：无负载

        // EXE → DLL
        MSG_CMD   = 0x30,   // 执行 Lua 代码：负载为 Lua 源码
        MSG_FILE  = 0x31,   // 执行 Lua 文件：负载为文件路径

        // 双向
        MSG_EXIT  = 0xFF,   // 退出通知：无负载
    };

    // 最大负载大小：1 MB
    constexpr size_t MAX_PAYLOAD = 1024 * 1024;

    // MSG_ERROR 的统一错误层级。错误文本本身不再混入 Lua 的 source name，
    // 由协议显式传输类别和可选行号，CLI 只负责展示。
    enum class ErrorCategory : uint8_t
    {
        Lua = 1,
        Il2Cpp = 2,
        CSharp = 3,
        Lune = 4,
    };

    constexpr size_t ERROR_HEADER_SIZE = 5; // 类别 1 字节 + 行号 4 字节 LE

    inline bool EncodeErrorPayload(
        ErrorCategory category,
        int32_t line,
        const char* message,
        std::vector<uint8_t>& payload)
    {
        const size_t messageLength = message == nullptr ? 0 : std::strlen(message);
        if (messageLength > MAX_PAYLOAD - ERROR_HEADER_SIZE)
        {
            return false;
        }

        payload.assign(ERROR_HEADER_SIZE + messageLength, 0);
        payload[0] = static_cast<uint8_t>(category);

        const uint32_t encodedLine = static_cast<uint32_t>(line);
        payload[1] = static_cast<uint8_t>(encodedLine & 0xFF);
        payload[2] = static_cast<uint8_t>((encodedLine >> 8) & 0xFF);
        payload[3] = static_cast<uint8_t>((encodedLine >> 16) & 0xFF);
        payload[4] = static_cast<uint8_t>((encodedLine >> 24) & 0xFF);

        if (messageLength > 0)
        {
            std::memcpy(payload.data() + ERROR_HEADER_SIZE, message, messageLength);
        }
        return true;
    }

    // 协议常量
    // 帧头大小：1 字节类型 + 4 字节长度 = 5 字节
    constexpr size_t HEADER_SIZE = 5;

    // 首次连接超时：15 秒（DLL PipeChannel 初始化调用 ConnectToPipe 连接命名管道允许延迟时间）
    constexpr int HANDSHAKE_TIMEOUT = 15000;

    // 管道忙超时 等待服务器管道 ConnectNamedPipe
    constexpr DWORD WAITSERVER_BUSY = 5000;

    // 版本标识（HELLO 帧的负载内容）
    constexpr const char* VERSION = IL2CPPLUA_PROTOCOL_VERSION;

    // 共享内存名称前缀：注入器创建共享内存写入管道名
    // DLL 加载后读取 完整名称 = 前缀 + 目标进程 PID
    // 例如：Il2CppLua_Config_5454
    constexpr const wchar_t* SHARED_MEM_PREFIX = L"Il2CppLua_Config_";

    // 共享内存最大大小（字节） 足够容纳一个管道名称
    constexpr size_t SHARED_MEM_SIZE = 512;

}
