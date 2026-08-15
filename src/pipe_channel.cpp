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
// 打开管道客户端句柄（带 ERROR_PIPE_BUSY 重试）
// ============================================================
// 服务器已创建管道但尚未调用 ConnectNamedPipe 时 CreateFileW
// 会返回 ERROR_PIPE_BUSY 需要用 WaitNamedPipeW 等待后重试
// 句柄必须以 FILE_FLAG_OVERLAPPED 打开:
// 阻塞模式下同一句柄的读写会被序列化——worker 线程常年阻塞在
// RecvFrame(ReadFile) 等待命令 若游戏线程的 Hook 回调 print
// -> SendLog(WriteFile) 复用同一句柄会被 pending read 卡死
// 重叠模式下挂起的读不会阻塞其他线程的写
static HANDLE OpenPipeClient(const std::wstring& pipeName, DWORD access)
{
    HANDLE h = CreateFileW(
        pipeName.c_str(),     // 管道名称
        access,               // 读/写权限
        0,                    // 不共享
        nullptr,              // 默认安全属性
        OPEN_EXISTING,        // 管道必须已存在
        FILE_FLAG_OVERLAPPED, // 重叠模式（挂起读不阻塞同句柄写）
        nullptr);             // 无模板文件

    // 连接失败 检查错误原因
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY)
    {
        // 等待服务器就绪后再次尝试连接
        if (WaitNamedPipeW(pipeName.c_str(), protocol::WAITSERVER_BUSY))
        {
            h = CreateFileW(pipeName.c_str(), access, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        }
    }

    return h;
}

// ============================================================
// 重叠 I/O 读取（阻塞等待完成）
// ============================================================
// 每次调用创建事件 完成或失败后关闭
// 返回 false 表示管道断开或读取失败
static bool ReadPipeOverlapped(HANDLE pipe, void* buf, DWORD len, DWORD& bytesRead)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;

    BOOL ok = ReadFile(pipe, buf, len, &bytesRead, &ov);
    if (!ok)
    {
        // 读取尚未完成 等待事件后取结果
        if (GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, INFINITE) == WAIT_OBJECT_0)
            {
                ok = GetOverlappedResult(pipe, &ov, &bytesRead, FALSE);
            }
            else
            {
                ok = FALSE;
            }
        }
        else
        {
            // 管道断开等错误
            ok = FALSE;
        }
    }

    CloseHandle(ov.hEvent);
    return ok != FALSE;
}

// ============================================================
// 重叠 I/O 写入（阻塞等待完成）
// ============================================================
// 与 ReadPipeOverlapped 同理 保证挂起的读不阻塞本写入
static bool WritePipeOverlapped(HANDLE pipe, const void* buf, DWORD len)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(pipe, buf, len, &written, &ov);
    if (!ok)
    {
        if (GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, INFINITE) == WAIT_OBJECT_0)
            {
                ok = GetOverlappedResult(pipe, &ov, &written, FALSE);
            }
            else
            {
                ok = FALSE;
            }
        }
        else
        {
            ok = FALSE;
        }
    }

    CloseHandle(ov.hEvent);
    return ok != FALSE && written == len;
}

// ============================================================
// 连接到命名管道服务器
// ============================================================
bool PipeChannel::ConnectToPipe(const std::wstring& pipeName)
{
    // 打开唯一的全双工句柄（FILE_FLAG_OVERLAPPED）
    m_pipe = OpenPipeClient(pipeName, GENERIC_READ | GENERIC_WRITE);

    // 连接失败
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

    // 构造帧头: 1 字节类型 + 4 字节长度（小端）
    uint8_t header[protocol::HEADER_SIZE]{};
    header[0] = type;
    header[1] = static_cast<uint8_t>(len & 0xFF);
    header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>((len >> 16) & 0xFF);
    header[4] = static_cast<uint8_t>((len >> 24) & 0xFF);

    // 写帧头（重叠 I/O 挂起的读不会阻塞本写入）
    if (!WritePipeOverlapped(m_pipe, header, protocol::HEADER_SIZE)) return false;

    // 写负载
    if (len > 0 && data != nullptr)
    {
        if (!WritePipeOverlapped(m_pipe, data, len)) return false;
    }
    return true;
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

    // ---- 读取帧头: 1 字节类型 + 4 字节长度（小端）----
    uint8_t header[protocol::HEADER_SIZE]{};
    DWORD total = 0;
    while (total < protocol::HEADER_SIZE)
    {
        DWORD chunk = 0;
        if (!ReadPipeOverlapped(m_pipe, header + total,
                static_cast<DWORD>(protocol::HEADER_SIZE) - total, chunk) || chunk == 0)
        {
            // 管道断开或错误
            m_connected = false;
            return false;
        }
        total += chunk;
    }

    // 解析帧头
    type = header[0];
    uint32_t len = static_cast<uint32_t>(header[1])
                 | (static_cast<uint32_t>(header[2]) << 8)
                 | (static_cast<uint32_t>(header[3]) << 16)
                 | (static_cast<uint32_t>(header[4]) << 24);

    // 长度校验：防止恶意/损坏的帧头导致缓冲区溢出
    if (len > protocol::MAX_PAYLOAD)
    {
        m_connected = false;
        return false;
    }

    // ---- 读取负载 ----
    payload.clear();
    if (len > 0)
    {
        payload.resize(len);
        total = 0;
        while (total < len)
        {
            DWORD chunk = 0;
            if (!ReadPipeOverlapped(m_pipe, payload.data() + total, len - total, chunk) || chunk == 0)
            {
                m_connected = false;
                return false;
            }
            total += chunk;
        }
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
    // 关闭句柄会使任何阻塞的管道操作立即失败返回
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    m_connected = false;
}
