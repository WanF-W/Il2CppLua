/**
 * pipe_channel.cpp — DLL 端管道通信客户端实现
 * 本文件实现 pipe_channel.h 中声明的 PipeChannel 类
 *
 * 模块组成
 *
 * ·单例获取与析构
 * ·初始化（读共享内存 → 连接管道）
 * ·帧发送（线程安全 互斥锁保护）
 * ·帧接收（消息循环独占，关闭时可被取消）
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
 */

#include "pipe_channel.h"
#include <algorithm>
#include <chrono>

namespace
{
    constexpr size_t MAX_LOG_LENGTH = 64 * 1024;
    constexpr size_t MAX_LOG_QUEUE_ITEMS = 1024;
    constexpr size_t MAX_LOG_QUEUE_BYTES = 4 * 1024 * 1024;
    constexpr DWORD WRITE_TIMEOUT_MS = 10000;
}
// 单例获取 C++11 线程安全的局部静态变量初始化
PipeChannel& PipeChannel::Instance()
{
    static PipeChannel instance;
    return instance;
}
// 析构函数 防御性编程 一般正常结束会调用Shutdown
PipeChannel::~PipeChannel()
{
    if (bridge_lifecycle::g_processTerminating.load(std::memory_order_acquire))
    {
        // 进程终止时工作线程可能已被系统终止，不能等待 activeReads；
        // detach 只释放 std::thread 的句柄所有权，剩余资源由进程回收。
        if (m_logThread.joinable()) m_logThread.detach();
        return;
    }
    Shutdown();
}
// 初始化
bool PipeChannel::Init()
{
    std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);

    // 防止重复初始化。句柄状态由 state mutex 保护。
    if (IsConnected()) return true;

    // 连接可能因对端断开而失效。完整 Shutdown 会等待旧的 overlapped
    // 读取结束，并关闭旧 HANDLE；重连不能只停止日志线程，否则旧读线程
    // 结束时可能把新连接的 HANDLE 误关掉。
    Shutdown();

    // 从共享内存读取管道名称。
    std::wstring pipeName;
    // 共享内存不存在或读取失败 这通常意味着 DLL 不是通过注入器加载的
    if (!ReadPipeNameFromSharedMemory(pipeName)) return false;

    // 连接到命名管道服务器失败
    if (!ConnectToPipe(pipeName)) return false;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_connected = true;
        m_pipeForCancel.store(m_pipe, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_logStopping = false;
        m_logWriting = false;
        m_logQueue.clear();
        m_logQueueBytes = 0;
        m_logAccepted = m_logCompleted = 0;
        m_logFailed = false;
        m_rejectedLogBatches.store(0, std::memory_order_relaxed);
        m_discardedLogFrames.store(0, std::memory_order_relaxed);
        m_droppedLogBytes.store(0, std::memory_order_relaxed);
    }
    m_stopping.store(false, std::memory_order_release);
    try
    {
        m_logThread = std::thread(&PipeChannel::LogWorker, this);
    }
    catch (...)
    {
        // 日志线程不是协议连接的必要条件，但不能留下一个没有日志消费方
        // 的“半初始化”通道。
        Shutdown();
        return false;
    }

    // 返回连接成功
    return true;
}
// 从共享内存读取管道名称
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

    // 查不到可能是注入端已经释放或者非 Lune 注入此 DLL
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
    // 最多读取映射容量减一个终止符；内容本身可以占满剩余空间。
    const wchar_t* rawData = static_cast<const wchar_t*>(mapped);

    // 限制最大长度 防止缓冲区溢出
    const size_t capacity = protocol::SHARED_MEM_SIZE / sizeof(wchar_t);
    const size_t length = wcsnlen_s(rawData, capacity);
    if (length == capacity)
    {
        // 没有终止符时不能把截断后的共享内存内容当成合法管道名。
        UnmapViewOfFile(mapped);
        CloseHandle(hMap);
        return false;
    }
    outPipeName.assign(rawData, length);

    // 清理共享内存资源
    // 读取完成后立即解除映射并关闭句柄
    // 共享内存本身由注入器负责释放
    UnmapViewOfFile(mapped);
    CloseHandle(hMap);

    // 验证管道名称非空
    return !outPipeName.empty();
}
// 打开管道客户端句柄（带 ERROR_PIPE_BUSY 重试）
// 服务器已创建管道但尚未调用 ConnectNamedPipe 时 CreateFileW
// 会返回 ERROR_PIPE_BUSY 需要用 WaitNamedPipeW 等待后重试
// 句柄必须以 FILE_FLAG_OVERLAPPED 打开:
// 阻塞模式下同一句柄的读写会被序列化——worker 线程常年阻塞在
// RecvFrame(ReadFile) 等待命令 若游戏线程的 Hook 回调 print
// -> SendLog(WriteFile) 复用同一句柄会被 pending read 卡死
// 重叠模式下挂起的读不会阻塞其他线程的写
static HANDLE OpenPipeClient(const std::wstring& pipeName, DWORD access)
{
    const ULONGLONG deadline = GetTickCount64() + protocol::HANDSHAKE_TIMEOUT;
    for (;;)
    {
        HANDLE h = CreateFileW(
            pipeName.c_str(), access, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return INVALID_HANDLE_VALUE;

        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return INVALID_HANDLE_VALUE;
        if (error == ERROR_FILE_NOT_FOUND)
        {
            Sleep(static_cast<DWORD>((std::min<ULONGLONG>)(50, deadline - now)));
            continue;
        }
        const DWORD remaining = static_cast<DWORD>(std::min<ULONGLONG>(
            deadline - now, static_cast<ULONGLONG>(protocol::WAITSERVER_BUSY)));
        if (!WaitNamedPipeW(pipeName.c_str(), remaining)
            && GetLastError() != ERROR_SEM_TIMEOUT && GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            return INVALID_HANDLE_VALUE;
        }
    }
}
// 重叠 I/O 读取（阻塞等待完成）
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
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &bytesRead, TRUE);
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
// 重叠 I/O 写入（阻塞等待完成）
// 与 ReadPipeOverlapped 同理 保证挂起的读不阻塞本写入
static bool WritePipeOverlapped(HANDLE pipe, const void* buf, DWORD len, ULONGLONG deadline)
{
    if (GetTickCount64() >= deadline) return false;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(pipe, buf, len, &written, &ov);
    if (!ok)
    {
        if (GetLastError() == ERROR_IO_PENDING)
        {
            const ULONGLONG now = GetTickCount64();
            const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
            if (WaitForSingleObject(ov.hEvent, remaining) == WAIT_OBJECT_0)
            {
                ok = GetOverlappedResult(pipe, &ov, &written, FALSE);
            }
            else
            {
                // Cancellation is asynchronous: keep ov, its event and buf alive
                // until the operation has completed, even on the timeout path.
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &written, TRUE);
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
// 连接到命名管道服务器
bool PipeChannel::ConnectToPipe(const std::wstring& pipeName)
{
    // 打开唯一的全双工句柄（FILE_FLAG_OVERLAPPED）
    HANDLE pipe = OpenPipeClient(pipeName, GENERIC_READ | GENERIC_WRITE);

    // 连接失败
    if (pipe == INVALID_HANDLE_VALUE) return false;

    // 设置管道为字节模式
    // 无论服务器创建时用什么模式 客户端强制设为字节模式
    // 我们的协议是二进制帧 不依赖 Windows 管道消息边界
    DWORD pipeMode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(pipe, &pipeMode, nullptr, nullptr))
    {
        CloseHandle(pipe);
        return false;
    }

    // 只在连接完全配置成功后发布 HANDLE；读写和 Shutdown 都通过同一
    // 状态锁读取 m_pipe，避免看到半初始化或已失败的句柄。
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_pipe = pipe;
    }

    return true;
}
// 通用帧发送（线程安全）
bool PipeChannel::SendFrame(uint8_t type, const void* data, uint32_t len)
{
    if (len > protocol::MAX_PAYLOAD || (len > 0 && data == nullptr)) return false;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool attempted = false;
    bool success = false;
    {
        // 状态锁覆盖整个写入过程，Shutdown 不会在写操作使用句柄时关闭它。
        std::scoped_lock lock(m_stateMutex, m_writeMutex);
        if (m_stopping.load(std::memory_order_acquire)
            || !m_connected || m_pipe == INVALID_HANDLE_VALUE) return false;
        pipe = m_pipe;
        attempted = true;

        // 构造帧头: 1 字节类型 + 4 字节长度（小端）
        uint8_t header[protocol::HEADER_SIZE]{};
        header[0] = type;
        header[1] = static_cast<uint8_t>(len & 0xFF);
        header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>((len >> 16) & 0xFF);
        header[4] = static_cast<uint8_t>((len >> 24) & 0xFF);

        // 写帧头（重叠 I/O 挂起的读不会阻塞本写入）
        const ULONGLONG deadline = GetTickCount64() + WRITE_TIMEOUT_MS;
        success = WritePipeOverlapped(pipe, header, protocol::HEADER_SIZE, deadline);

        // 写负载
        if (success && len > 0)
            success = WritePipeOverlapped(pipe, data, len, deadline);
    }

    // 写失败后立即把句柄标为断开，避免后续线程继续拼接半个协议帧。
    if (!success && attempted) MarkDisconnected(pipe);
    return success;
}
// 发送握手帧
bool PipeChannel::SendHello()
{
    // 发送 MSG_HELLO 帧 负载为版本字符串
    // EXE 收到后会检查版本是否匹配
    const size_t length = strnlen_s(protocol::VERSION, protocol::MAX_PAYLOAD + 1);
    return length <= protocol::MAX_PAYLOAD
        && SendFrame(protocol::MSG_HELLO, protocol::VERSION, static_cast<uint32_t>(length));
}
// 发送就绪帧
bool PipeChannel::SendReady(const char* statusMsg)
{
    // 发送 MSG_READY 帧 负载为状态描述文本
    // 例如 "IL2CPP resolved: 42 images, Lua ready"
    const char* msg = (statusMsg != nullptr) ? statusMsg : "ready";
    const size_t length = strnlen_s(msg, protocol::MAX_PAYLOAD + 1);
    if (!FlushLogs()) return false;
    return length <= protocol::MAX_PAYLOAD
        && SendFrame(protocol::MSG_READY, msg, static_cast<uint32_t>(length));
}
// 发送日志帧
bool PipeChannel::SendLog(const char* text)
{
    if (text == nullptr) return false;
    if (m_stopping.load(std::memory_order_acquire)) return false;

    const size_t length = strnlen_s(text, MAX_LOG_QUEUE_BYTES + 1);
    const auto dropped = [this, length] {
        m_rejectedLogBatches.fetch_add(1, std::memory_order_relaxed);
        m_droppedLogBytes.fetch_add(length, std::memory_order_relaxed);
        return false;
    };
    if (length > MAX_LOG_QUEUE_BYTES) return dropped();
    // UTF-8 字符最多占 4 字节；预留帧数考虑末尾最多回退 3 字节。
    const size_t chunks = length == 0 ? 1 : (length + MAX_LOG_LENGTH - 4) / (MAX_LOG_LENGTH - 3);

    // Hook 和 Lua 回调可能运行在游戏线程，日志只入有界队列，不在这里
    // 等待管道写入。队列满时丢弃日志，不能为了保留日志拖住游戏逻辑。
    try
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        if (m_logStopping || m_logFailed || chunks > MAX_LOG_QUEUE_ITEMS - m_logQueue.size()
            || length > MAX_LOG_QUEUE_BYTES - m_logQueueBytes) return dropped();
        const size_t previousSize = m_logQueue.size();
        const size_t previousBytes = m_logQueueBytes;
        try
        {
            size_t offset = 0;
            do
            {
                size_t bytes = (std::min)(MAX_LOG_LENGTH, length - offset);
                if (offset + bytes < length)
                    while (bytes > 0 && (static_cast<unsigned char>(text[offset + bytes]) & 0xC0) == 0x80) --bytes;
                // 非 UTF-8 输入仍保证有进展；正常 UTF-8 不会走到这里。
                if (bytes == 0 && offset < length) bytes = (std::min)(MAX_LOG_LENGTH, length - offset);
                m_logQueue.emplace_back(text + offset, bytes);
                m_logQueueBytes += bytes;
                offset += bytes;
            } while (offset < length);
        }
        catch (...)
        {
            while (m_logQueue.size() > previousSize) m_logQueue.pop_back();
            m_logQueueBytes = previousBytes;
            return dropped();
        }
        m_logAccepted += m_logQueue.size() - previousSize;
    }
    catch (...)
    {
        return dropped();
    }
    m_logReady.notify_one();
    return true;
}
// 发送结构化错误帧。先排空此前已经产生的普通输出，保证一个命令的
// MSG_LOG 不会被后面的 MSG_ERROR 反超。
bool PipeChannel::SendError(protocol::ErrorCategory category, int32_t line, const char* text)
{
    const char* msg = (text != nullptr) ? text : "unknown error";
    std::vector<uint8_t> payload;
    if (!protocol::EncodeErrorPayload(category, line, msg, payload)) return false;
    if (!FlushLogs()) return false;
    return SendFrame(protocol::MSG_ERROR, payload.data(), static_cast<uint32_t>(payload.size()));
}

// 发送成功帧
bool PipeChannel::SendOk()
{
    // 发送 MSG_OK 帧 无负载
    // 用于通知 EXE 命令执行成功
    if (!FlushLogs()) return false;
    return SendFrame(protocol::MSG_OK, nullptr, 0);
}
// 发送退出帧
bool PipeChannel::SendExit()
{
    // 发送 MSG_EXIT 帧 无负载
    // 通知 EXE 即将断开连接（DLL 卸载）
    if (!FlushLogs()) return false;
    return SendFrame(protocol::MSG_EXIT, nullptr, 0);
}
// 接收帧（阻塞）
bool PipeChannel::RecvFrame(uint8_t& type, std::vector<uint8_t>& payload)
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    if (!BeginRead(pipe)) return false;

    bool success = false;
    do
    {
        // ---- 读取帧头: 1 字节类型 + 4 字节长度（小端）----
        uint8_t header[protocol::HEADER_SIZE]{};
        DWORD total = 0;
        while (total < protocol::HEADER_SIZE)
        {
            DWORD chunk = 0;
            if (!ReadPipeOverlapped(pipe, header + total,
                    static_cast<DWORD>(protocol::HEADER_SIZE) - total, chunk)
                || chunk == 0) break;
            total += chunk;
        }
        if (total != protocol::HEADER_SIZE) break;

        type = header[0];
        const uint32_t len = static_cast<uint32_t>(header[1])
                           | (static_cast<uint32_t>(header[2]) << 8)
                           | (static_cast<uint32_t>(header[3]) << 16)
                           | (static_cast<uint32_t>(header[4]) << 24);
        if (len > protocol::MAX_PAYLOAD) break;

        payload.clear();
        if (len > 0)
        {
            payload.resize(len);
            total = 0;
            while (total < len)
            {
                DWORD chunk = 0;
                if (!ReadPipeOverlapped(pipe, payload.data() + total, len - total, chunk)
                    || chunk == 0) break;
                total += chunk;
            }
            if (total != len) break;
        }

        success = true;
    } while (false);

    EndRead();
    if (!success) MarkDisconnected(pipe);
    return success;
}
// 关闭
void PipeChannel::Shutdown()
{
    std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);
    m_stopping.store(true, std::memory_order_release);

    // 先取消日志线程可能正在等待的写入，再等待日志线程退出；否则 join
    // 可能永久等待一个已经断开的管道。
    HANDLE activePipe = static_cast<HANDLE>(m_pipeForCancel.load(std::memory_order_acquire));
    if (activePipe != INVALID_HANDLE_VALUE && activePipe != nullptr)
        CancelIoEx(activePipe, nullptr);
    StopLogWorker();

    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_connected = false;
        pipe = m_pipe;
    }

    // 取消可能阻塞的 overlapped 读取；CloseHandle 要等待读操作退出后再做。
    if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);

    std::unique_lock<std::mutex> stateLock(m_stateMutex);
    m_readFinished.wait(stateLock, [this] { return m_activeReads == 0; });
    {
        std::lock_guard<std::mutex> writeLock(m_writeMutex);
        if (m_pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
        m_pipeForCancel.store(nullptr, std::memory_order_release);
    }
}

bool PipeChannel::IsConnected() const
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_connected && m_pipe != INVALID_HANDLE_VALUE;
}

bool PipeChannel::BeginRead(HANDLE& pipe)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_connected || m_pipe == INVALID_HANDLE_VALUE) return false;
    pipe = m_pipe;
    ++m_activeReads;
    return true;
}

void PipeChannel::EndRead()
{
    HANDLE pipeToClose = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_activeReads > 0) --m_activeReads;
        if (m_activeReads == 0 && !m_connected
            && m_pipe != INVALID_HANDLE_VALUE)
        {
            pipeToClose = m_pipe;
            m_pipe = INVALID_HANDLE_VALUE;
        }
    }
    m_readFinished.notify_all();

    // 发送线程可能先发现断开，但读取仍在进行；等最后一次读取完成后
    // 再关闭句柄，避免 CloseHandle 与 overlapped I/O 并发。
    if (pipeToClose != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(pipeToClose, nullptr);
        CloseHandle(pipeToClose);
        m_pipeForCancel.store(nullptr, std::memory_order_release);
    }
}

void PipeChannel::MarkDisconnected(HANDLE pipe)
{
    HANDLE pipeToClose = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_pipe == pipe)
        {
            m_connected = false;
            // Wake a pending reader even when EndRead owns final closure.
            CancelIoEx(pipe, nullptr);
            // 活动读操作结束前保留句柄；EndRead 会负责最后关闭。
            if (m_activeReads == 0)
            {
                m_pipe = INVALID_HANDLE_VALUE;
                pipeToClose = pipe;
            }
        }
    }
    if (pipeToClose != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(pipeToClose, nullptr);
        CloseHandle(pipeToClose);
        m_pipeForCancel.store(nullptr, std::memory_order_release);
    }
}

void PipeChannel::LogWorker()
{
    for (;;)
    {
        std::string message;
        {
            std::unique_lock<std::mutex> lock(m_logMutex);
            m_logReady.wait(lock, [this] {
                return m_logStopping || !m_logQueue.empty();
            });
            if (m_logStopping) return;
            message = std::move(m_logQueue.front());
            m_logQueueBytes -= message.size();
            m_logQueue.pop_front();
            m_logWriting = true;
        }

        // 日志线程是唯一的日志发送者；失败会终止本连接的消费并唤醒 flush。
        const bool sent = SendFrame(protocol::MSG_LOG, message.data(), static_cast<uint32_t>(message.size()));

        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            m_logWriting = false;
            if (sent) ++m_logCompleted;
            else
            {
                m_logFailed = true;
                m_discardedLogFrames.fetch_add(1 + m_logQueue.size(), std::memory_order_relaxed);
                m_droppedLogBytes.fetch_add(message.size() + m_logQueueBytes, std::memory_order_relaxed);
                m_logQueue.clear();
                m_logQueueBytes = 0;
            }
            m_logDrained.notify_all();
        }
        if (!sent) return;
    }
}

bool PipeChannel::FlushLogs()
{
    std::unique_lock<std::mutex> lock(m_logMutex);
    const uint64_t target = m_logAccepted;
    const bool completed = m_logDrained.wait_for(lock, std::chrono::milliseconds(WRITE_TIMEOUT_MS), [this, target] {
        return m_logStopping || m_logFailed || m_logCompleted >= target;
    });
    return completed && !m_logStopping && !m_logFailed && m_logCompleted >= target;
}

void PipeChannel::StopLogWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_logStopping = true;
        m_discardedLogFrames.fetch_add(m_logQueue.size(), std::memory_order_relaxed);
        m_droppedLogBytes.fetch_add(m_logQueueBytes, std::memory_order_relaxed);
        m_logQueue.clear();
        m_logQueueBytes = 0;
    }
    m_logReady.notify_all();
    m_logDrained.notify_all();
    if (m_logThread.joinable()) m_logThread.join();
}
