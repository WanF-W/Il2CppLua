/**
 * ============================================================
 * pipe_channel.cpp — DLL 端管道通信客户端实现
 * ============================================================
 * 本文件实现 pipe_channel.h 中声明的 PipeChannel 类 
 *
 * 模块组成
 * 
 * ·单例获取与析构
 * ·初始化（读共享内存 → 连接管道）
 * ·帧发送（线程安全 互斥锁保护）
 * ·帧接收（阻塞读取 仅在主线程调用）
 * ·关闭
 *
 * 共享内存机制
 * 
 * ·注入器在注入 DLL 之前创建一块共享内存 
 * ·名称格式为 "Il2CppLua_Config_<PID>" 
 * ·内容为命名管道的完整路径（宽字符串） 
 * ·DLL 加载后通过 OpenFileMappingW 打开并读取 
 *
 * 命名管道连接
 * 
 * ·使用 CreateFileW 连接到 EXE 创建的管道服务器 
 * ·如果服务器尚未就绪（ERROR_PIPE_BUSY） 
 * ·使用 WaitNamedPipeW 等待服务器调用 ConnectNamedPipe 
 * ============================================================
 */

#include "pipe_channel.h"

// ============================================================
// 单例获取 C++11 线程安全的局部静态变量初始化
// ============================================================
PipeChannel& PipeChannel::Instance()
{
    static PipeChannel instance;
    return instance;
}

// ============================================================
// 析构函数 防御性编程 一般正常结束会调用Shutdown
// ============================================================
PipeChannel::~PipeChannel()
{
    if (m_connected) Shutdown();
}

// ============================================================
// 初始化
// ============================================================
bool PipeChannel::Init()
{
    // 防止重复初始化
    if (m_connected) return true;

    // 从共享内存d读取管道名称
    std::wstring pipeName;
    // 共享内存不存在或读取失败 这通常意味着 DLL 不是通过注入器加载的
    if (!ReadPipeNameFromSharedMemory(pipeName)) return false;

    // 连接到命名管道服务器失败
    if (!ConnectToPipe(pipeName)) return false;

    // 设置为连接状态
    m_connected = true;

    // 返回连接成功
    return true;
}

// ============================================================
// 从共享内存读取管道名称
// ============================================================
bool PipeChannel::ReadPipeNameFromSharedMemory(std::wstring& outPipeName)
{
    // 共享内存名称格式 -> "Il2CppLua_Config_<PID>"
    // 注入器用目标进程的 PID 创建共享内存
    // DLL 用 GetCurrentProcessId() 获取同一个 PID
    DWORD pid = GetCurrentProcessId();
    // 共享内存名称缓冲区
    wchar_t shmName[128];
    // 拼接得到最终共享内存名称
    swprintf_s(shmName, 128, L"%s%lu", protocol::SHARED_MEM_PREFIX, pid);

    // 注入器在注入 DLL 前创建共享内存
    // 理论上 DLL 加载时已存在
    // 尝试打开共享内存（只读权限）
    HANDLE hMap = OpenFileMappingW(
            FILE_MAP_READ, // 只读访问权限
            FALSE,         // 不继承句柄给子进程
            shmName);      // 共享内存名称

    // 查不到可能是注入端已经释放或者非 ILune 注入此 DLL
    if (hMap == nullptr) return false;

    // 映射共享内存到进程地址空间
    void* mapped = MapViewOfFile(
        hMap,          // 共享内存句柄
        FILE_MAP_READ, // 只读访问
        0,             // 文件偏移高 32 位（从开头开始）
        0,             // 文件偏移低 32 位
        0);            // 映射全部（0 = 整个映射）

    // 映射失败
    if (mapped == nullptr)
    {
        CloseHandle(hMap);
        return false;
    }

    // 读取管道名称
    // 共享内存内容为宽字符串（以 '\0' 结尾）
    // 最多读取 SHARED_MEM_SIZE-1 个 wchar_t（留一个位置给零终止符）
    const wchar_t* rawData = static_cast<const wchar_t*>(mapped);

    // 限制最大长度 防止缓冲区溢出
    size_t maxChars = protocol::SHARED_MEM_SIZE / sizeof(wchar_t) - 1;
    // 安全拷贝 
    outPipeName.assign(rawData, wcsnlen_s(rawData, maxChars));

    // 清理共享内存资源
    // 读取完成后立即解除映射并关闭句柄
    // 共享内存本身由注入器负责释放
    UnmapViewOfFile(mapped);
    CloseHandle(hMap);

    // 验证管道名称非空
    return !outPipeName.empty();
}

// ============================================================
// 连接到命名管道服务器
// ============================================================
bool PipeChannel::ConnectToPipe(const std::wstring& pipeName)
{
    // 第一次尝试连接
    // CreateFileW 以客户端身份连接到命名管道
    m_pipe = CreateFileW(
        pipeName.c_str(),             // 管道名称
        GENERIC_READ | GENERIC_WRITE, // 读写权限（全双工）
        0,                            // 不共享
        nullptr,                      // 默认安全属性
        OPEN_EXISTING,                // 管道必须已存在
        0,                            // 默认属性
        nullptr);                     // 无模板文件

    // 如果连接失败 检查错误原因
    if (m_pipe == INVALID_HANDLE_VALUE)
    {
        // 获取错误码
        DWORD err = GetLastError();

        // 管道忙 服务器已创建管道但尚未调用 ConnectNamedPipe
        if (err == ERROR_PIPE_BUSY)
        {
            // 使用 WaitNamedPipeW 等待服务器就绪
            if (WaitNamedPipeW(pipeName.c_str(), protocol::WAITSERVER_BUSY))
            {
                // 服务器已就绪 再次尝试连接
                m_pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            }
        }
        // 其他错误码（如 ERROR_FILE_NOT_FOUND）表示管道不存在
        // 直接返回失败
    }

    // 检查连接是否成功
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    // 设置管道为字节模式
    // 无论服务器创建时用什么模式 客户端强制设为字节模式
    // 我们的协议是二进制帧 不依赖 Windows 管道消息边界
    DWORD pipeMode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(m_pipe, &pipeMode, nullptr, nullptr);

    return true;
}

// ============================================================
// 通用帧发送（线程安全）
// ============================================================
bool PipeChannel::SendFrame(uint8_t type, const void* data, uint32_t len)
{
    // 前置检查：确保已连接
    if (!m_connected || m_pipe == INVALID_HANDLE_VALUE) return false;

    // 加锁保护写操作
    // 多个线程可能同时调用 SendLog（主线程执行 Lua + Hook 回调线程）
    std::lock_guard<std::mutex> lock(m_writeMutex);

    // 调用协议层写入帧
    // protocol::WriteFrame 内部完成帧头构造和数据写入
    return protocol::WriteFrame(m_pipe, type, data, len);
}

// ============================================================
// 发送握手帧
// ============================================================
bool PipeChannel::SendHello()
{
    // 发送 MSG_HELLO 帧 负载为版本字符串
    // EXE 收到后会检查版本是否匹配
    return SendFrame(protocol::MSG_HELLO, protocol::VERSION, static_cast<uint32_t>(strlen(protocol::VERSION)));
}

// ============================================================
// 发送就绪帧
// ============================================================
bool PipeChannel::SendReady(const char* statusMsg)
{
    // 发送 MSG_READY 帧 负载为状态描述文本
    // 例如 "IL2CPP resolved: 42 images, Lua ready"
    const char* msg = (statusMsg != nullptr) ? statusMsg : "ready";
    return SendFrame(protocol::MSG_READY, msg, static_cast<uint32_t>(strlen(msg)));
}

// ============================================================
// 发送日志帧
// ============================================================
bool PipeChannel::SendLog(const char* text)
{
    // 发送 MSG_LOG 帧 负载为输出文本
    // 这是 LuaEngine 的 print 重定向和返回值回显的输出通道
    if (text == nullptr) return false;

    return SendFrame(protocol::MSG_LOG, text, static_cast<uint32_t>(strlen(text)));
}

// ============================================================
// 发送错误帧
// ============================================================
bool PipeChannel::SendError(const char* text)
{
    // 发送 MSG_ERROR 帧 负载为错误描述
    // 用于通知 EXE 命令执行失败
    const char* msg = (text != nullptr) ? text : "unknown error";
    return SendFrame(protocol::MSG_ERROR, msg, static_cast<uint32_t>(strlen(msg)));
}

// ============================================================
// 发送成功帧
// ============================================================
bool PipeChannel::SendOk()
{
    // 发送 MSG_OK 帧 无负载
    // 用于通知 EXE 命令执行成功
    return SendFrame(protocol::MSG_OK, nullptr, 0);
}

// ============================================================
// 发送退出帧
// ============================================================
bool PipeChannel::SendExit()
{
    // 发送 MSG_EXIT 帧 无负载
    // 通知 EXE 即将断开连接（DLL 卸载）
    return SendFrame(protocol::MSG_EXIT, nullptr, 0);
}

// ============================================================
// 接收帧（阻塞）
// ============================================================
bool PipeChannel::RecvFrame(uint8_t& type, std::vector<uint8_t>& payload)
{
    // 前置检查
    if (!m_connected || m_pipe == INVALID_HANDLE_VALUE) return false;

    // 调用协议层读取帧
    // protocol::ReadFrame 使用内部静态缓冲区
    // 返回的 data 指针在下次调用时失效
    // 因此需要立即拷贝数据
    uint8_t* data = nullptr;
    uint32_t len = 0;

    // 读取失败：管道断开或错误
    if (!protocol::ReadFrame(m_pipe, type, data, len))
    {
        m_connected = false;
        return false;
    }

    // 将数据拷贝到 vector 中（安全持有）
    if (len > 0 && data != nullptr)
    {
        // 深拷贝负载数据
        payload.assign(data, data + len);
    }
    else
    {
        // 无负载 清空
        payload.clear();
    }

    return true;
}

// ============================================================
// 关闭
// ============================================================
void PipeChannel::Shutdown()
{
    if (!m_connected) return;

    // 加锁确保没有其他线程正在写入
    std::lock_guard<std::mutex> lock(m_writeMutex);

    // 关闭管道句柄
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        // 关闭管道句柄
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    m_connected = false;
}
