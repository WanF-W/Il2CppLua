/**
 * ============================================================
 * protocol.h — 二进制帧通信协议
 * ============================================================
 * 定义注入器（ilune.exe）与游戏内 DLL 之间的通信协议
 *
 * 帧格式
 *
 * [1字节类型][4字节长度LE][N字节负载]
 * ·类型：见 MessageType 枚举
 * ·长度：小端 32 位无符号 最大 1MB
 * ·负载：任意二进制数据
 *
 * 通信流程
 * 
 * ·EXE 创建命名管道服务器
 * ·EXE 注入 DLL（通过共享内存传递管道名）
 * ·DLL 连接管道 发送 HELLO（版本握手）
 * ·DLL 初始化 IL2CPP + Lua 发送 READY
 * ·EXE 可选发送 CMD（执行 Lua 代码）或 FILE（执行 Lua 文件）
 * ·DLL 执行后回送 OK / ERROR / LOG
 * ·任一方发送 EXIT 结束通信
 *
 * 本文件同时被 DLL 和 EXE 包含 确保两端协议一致
 * ============================================================
 */
#pragma once
#include "common.h"
#include <windows.h>

namespace protocol
{
    // ============================================================
    // 消息类型定义
    // ============================================================
    enum MessageType : uint8_t
    {
        // DLL → EXE
        MSG_HELLO = 0x10,   // 握手：负载为版本字符串 "Il2CppLua/2.0.0"
        MSG_READY = 0x11,   // 初始化就绪：负载为状态描述文本
        MSG_LOG   = 0x20,   // Lua print 输出：负载为输出文本
        MSG_ERROR = 0x21,   // 错误信息：负载为错误描述
        MSG_OK    = 0x22,   // 命令执行成功：无负载

        // EXE → DLL
        MSG_CMD   = 0x30,   // 执行 Lua 代码：负载为 Lua 源码
        MSG_FILE  = 0x31,   // 执行 Lua 文件：负载为文件路径

        // 双向
        MSG_EXIT  = 0xFF,   // 退出通知：无负载
    };


    // ============================================================
    // 协议常量
    // ============================================================

    // 帧头大小：1 字节类型 + 4 字节长度 = 5 字节
    constexpr size_t HEADER_SIZE = 5;

    // 最大负载大小：1 MB
    constexpr size_t MAX_PAYLOAD = 1024 * 1024;

    // LoadLibraryW 执行超时（DLL 注入后执行 DllMain 超时）
    constexpr DWORD DLLINIT_TIMEOUT = 10000;

    // DLL 初始化成功后留给他读取共享空间获取命名管道名称的时间
    constexpr DWORD FOR_DLLREADMEM_TIME = 3000;

    // 首次连接超时：15 秒（DLL PipeChannel 初始化调用 ConnectToPipe 连接命名管道允许延迟时间）
    constexpr int HANDSHAKE_TIMEOUT = 15000;

    // 首次连接失败超时：3秒（ConnectToPipe 连接失败工作线程退出超时强制关闭）
    constexpr int THREADEXIT_TIMEOUT = 3000;

    // Hello超时：5 秒（DLL 连接命名管道后发送 Hello 允许延迟时间）
    constexpr int DLLSAYHELLO_TIMEOUT = 5000;

    // Ready超时：60 秒（DLL Hello 后等待 IL2CPP 初始化 初始化 Lua 后发送 Ready 允许延迟时间）
    constexpr int DLLSAYREADY_TIMEOUT = 45000;

    // 命令执行超时：30 秒
    constexpr int COMMAND_TIMEOUT = 30000;

    // 文件执行超时：60 秒
    constexpr int LUAFILE_TIMEOUT = 60000;

    // 管道忙超时 等待服务器管道 ConnectNamedPipe
    constexpr DWORD WAITSERVER_BUSY = 5000;

    // 版本标识（HELLO 帧的负载内容）
    constexpr const char* VERSION = "Il2CppLua/2.0.0";

    // 共享内存名称前缀：注入器创建共享内存写入管道名 
    // DLL 加载后读取 完整名称 = 前缀 + 目标进程 PID
    // 例如：Il2CppLua_Config_5454
    constexpr const wchar_t* SHARED_MEM_PREFIX = L"Il2CppLua_Config_";

    // 命名管道名称前缀 完整名称 = 前缀 + 目标进程 PID
    // 例如：\\.\pipe\Il2CppLua_5454
    constexpr const wchar_t* PIPE_PREFIX = L"\\\\.\\pipe\\Il2CppLua_";

    // 共享内存最大大小（字节） 足够容纳一个管道名称
    constexpr size_t SHARED_MEM_SIZE = 512;


    // ============================================================
    // 帧读写函数
    // ============================================================

    /**
     * 向管道写入一个完整的帧
     *
     * @param pipe   管道句柄（由 CreateFile 或 CreateNamedPipe 创建）
     * @param type   消息类型
     * @param data   负载数据指针（可为 nullptr 当 len=0）
     * @param len    负载数据长度
     * @return true 写入成功 false 失败
     */
    inline bool WriteFrame(HANDLE pipe, uint8_t type, const void* data, uint32_t len)
    {
        // 构造帧头：1 字节类型 + 4 字节长度（小端序）
        uint8_t header[HEADER_SIZE]{};
        header[0] = type;
        header[1] = static_cast<uint8_t>(len & 0xFF);
        header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>((len >> 16) & 0xFF);
        header[4] = static_cast<uint8_t>((len >> 24) & 0xFF);

        // 写入帧头
        DWORD written = 0;
        if (!WriteFile(pipe, header, HEADER_SIZE, &written, nullptr) || written != HEADER_SIZE) return false;

        // 写入负载（如果有）
        if (len > 0 && data != nullptr)
        {
            if (!WriteFile(pipe, data, len, &written, nullptr) || written != len) return false;
        }

        return true;
    }

    /**
     * 写帧的便捷重载：直接传 C 字符串
     */
    inline bool WriteFrame(HANDLE pipe, uint8_t type, const char* text)
    {
        uint32_t len = text ? static_cast<uint32_t>(strlen(text)) : 0;
        return WriteFrame(pipe, type, text, len);
    }

    /**
     * 从管道读取一个完整的帧
     *
     * 使用内部静态缓冲区 返回的 data 指针在下次调用时会被覆盖
     * 调用者应在调用下一次 ReadFrame 前处理完数据
     *
     * @param pipe   管道句柄
     * @param type   [out] 接收消息类型
     * @param data   [out] 接收负载数据指针（指向内部静态缓冲区）
     * @param len    [out] 接收负载长度
     * @return true 读取成功 false 失败（管道断开等）
     */
    inline bool ReadFrame(HANDLE pipe, uint8_t& type, uint8_t*& data, uint32_t& len)
    {
        // 静态缓冲区：避免每次调用都分配内存
        // 大小 = 最大负载 + 帧头 + 1（末尾零终止符 方便当字符串用）
        // 注意：需要 MAX_PAYLOAD + 1 字节来容纳 s_buffer[len] = 0 当 len == MAX_PAYLOAD 时
        static uint8_t s_buffer[MAX_PAYLOAD + 1];

        // ---- 读取帧头 ----
        uint8_t header[HEADER_SIZE]{};
        DWORD totalRead = 0;

        // 循环读取直到帧头完整（管道可能分多次返回数据）
        while (totalRead < HEADER_SIZE)
        {
            DWORD chunk = 0;
            DWORD remaining = static_cast<DWORD>(HEADER_SIZE) - totalRead;
            if (!ReadFile(pipe, &header[totalRead], remaining, &chunk, nullptr) || chunk == 0) return false;
            totalRead += chunk;
        }

        // 解析帧头（此时 totalRead == HEADER_SIZE header 已完整填充）
        type = header[0];
        len  = static_cast<uint32_t>(header[1])
             | (static_cast<uint32_t>(header[2]) << 8)
             | (static_cast<uint32_t>(header[3]) << 16)
             | (static_cast<uint32_t>(header[4]) << 24);

        // 长度校验：防止恶意/损坏的帧头导致缓冲区溢出
        if (len > MAX_PAYLOAD) return false;

        // ---- 读取负载 ----
        if (len > 0)
        {
            totalRead = 0;
            while (totalRead < len)
            {
                DWORD chunk = 0;
                DWORD remaining = len - totalRead;
                if (!ReadFile(pipe, &s_buffer[totalRead], remaining, &chunk, nullptr) || chunk == 0) return false;
                totalRead += chunk;
            }
            // 末尾放零终止符 方便调用方当字符串处理
            // s_buffer 大小为 MAX_PAYLOAD + 1 len <= MAX_PAYLOAD 因此 s_buffer[len] 不越界
            s_buffer[len] = 0;
        }

        data = s_buffer;
        return true;
    }
}
