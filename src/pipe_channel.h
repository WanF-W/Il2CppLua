/**
 * pipe_channel.h — DLL 端管道通信客户端声明
 * 本模块运行在注入的 DLL 内
 *
 * ·从共享内存读取命名管道名称（由注入器创建）
 * ·连接到注入器创建的命名管道服务器
 * ·提供帧级别的发送/接收接口
 * ·线程安全的写入操作（Hook 回调线程可能并发写入）
 *
 * 通信流程（DLL 侧视角）
 *
 * ·Init()：读共享内存 → 获取管道名 → 连接管道
 * ·SendHello()：发送版本握手帧
 * ·等待完成 IL2CPP + Lua 初始化
 * ·SendReady()：发送就绪帧
 * ·消息循环：RecvFrame() → 处理 → SendOk/SendError
 * ·Shutdown()：断开管道
 *
 * 线程安全
 *
 * ·写操作（Send* 方法）受互斥锁保护
 * ·读操作由消息循环独占；Shutdown 会取消挂起读取并等待其退出
 * ·命名管道本身是全双工的 读写可同时进行
 *
 * 仅针对 Windows x64
 */
#pragma once
#include "common.h"
#include "protocol.h"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
// PipeChannel — DLL 端管道通信客户端（单例）
class PipeChannel
{
public:
    // 获取单例实例
    static PipeChannel& Instance();

    // ---- 生命周期 ----

    /**
     * 初始化管道通道
     *
     * ·用 GetCurrentProcessId() 构造共享内存名
     * ·打开共享内存 读取管道名称
     * ·关闭共享内存（只需读取一次）
     * ·连接到命名管道服务器
     *
     * @return true 连接成功 false 失败
     */
    bool Init();

    /**
     * 关闭管道通道
     * 断开管道连接 释放资源
     */
    void Shutdown();

    // 检测连接状态。状态由内部锁保护，避免与 Shutdown 并发读取句柄状态。
    bool IsConnected() const;

    // 发送帧（DLL → EXE）
    // 以下方法均为线程安全（内部加锁）

    // 发送握手帧（版本字符串）
    bool SendHello();

    // 发送就绪帧（状态描述文本）
    bool SendReady(const char* statusMsg);

    // 发送日志帧（Lua print 输出/返回值回显）
    bool SendLog(const char* text);
    uint64_t GetRejectedLogBatches() const { return m_rejectedLogBatches.load(std::memory_order_relaxed); }
    uint64_t GetDiscardedLogFrames() const { return m_discardedLogFrames.load(std::memory_order_relaxed); }
    uint64_t GetDroppedLogBytes() const { return m_droppedLogBytes.load(std::memory_order_relaxed); }

    // 发送错误帧（命令执行失败）
    bool SendError(protocol::ErrorCategory category, int32_t line, const char* text);

    // 发送成功帧（命令执行成功 无负载）
    bool SendOk();

    // 发送退出帧（通知 EXE 即将断开）
    bool SendExit();

    // 接收帧（EXE → DLL）
    // 阻塞读取一个完整的帧
    // 由消息循环独占调用；Shutdown 可在其他线程取消其挂起读取。
    //
    // @param type    [out] 接收到的消息类型
    // @param payload [out] 接收到的负载数据（拷贝到 vector 中）
    // @return true 接收成功 false 管道断开
    bool RecvFrame(uint8_t& type, std::vector<uint8_t>& payload);

    // 通用发送
    // 线程安全的帧发送 供外部直接使用
    bool SendFrame(uint8_t type, const void* data, uint32_t len);

private:
    PipeChannel()  = default;
    ~PipeChannel();
    PipeChannel(const PipeChannel&) = delete;
    PipeChannel& operator=(const PipeChannel&) = delete;

    // 内部辅助

    /**
     * 从共享内存读取管道名称
     *
     * 共享内存名称格式：Il2CppLua_Config_<PID>
     * 内容：宽字符串管道名称（如 \\.\pipe\Il2CppLua_5454）
     *
     * @param outPipeName [out] 输出管道名称
     * @return true 读取成功 false 共享内存不存在或读取失败
     */
    bool ReadPipeNameFromSharedMemory(std::wstring& outPipeName);

    /**
     * 连接到命名管道服务器
     *
     * @param pipeName 管道名称（如 \\.\pipe\Il2CppLua_5454）
     * @return true 连接成功 false 失败
     */
    bool ConnectToPipe(const std::wstring& pipeName);

    // 读取期间保留一个活动计数，Shutdown 会先取消 I/O，再等待所有读操作
    // 返回后关闭句柄，避免关闭仍被 overlapped I/O 使用的 HANDLE。
    bool BeginRead(HANDLE& pipe);
    void EndRead();
    void MarkDisconnected(HANDLE pipe);
    void LogWorker();
    void StopLogWorker();
    bool FlushLogs();

    // ---- 成员变量 ----
    HANDLE       m_pipe       = INVALID_HANDLE_VALUE; // 命名管道句柄（重叠模式）
    bool         m_connected  = false;                // 连接状态标志
    // Init 与 Shutdown 不能并发进行；尤其是重连时必须先回收旧读句柄，
    // 再把新 HANDLE 写入 m_pipe，避免旧读线程结束时误关新连接。
    std::recursive_mutex m_lifecycleMutex;
    mutable std::mutex m_stateMutex;                  // 句柄和状态互斥锁
    std::condition_variable m_readFinished;
    size_t       m_activeReads = 0;                   // 当前正在等待的读操作数量
    std::mutex   m_writeMutex;                        // 写操作互斥锁

    // 日志不能阻塞 Hook 或游戏线程；由独立线程负责发送。
    std::mutex   m_logMutex;
    std::condition_variable m_logReady;
    std::condition_variable m_logDrained;
    std::deque<std::string> m_logQueue;
    std::thread  m_logThread;
    bool         m_logStopping = false;
    bool         m_logWriting = false;
    size_t       m_logQueueBytes = 0;
    uint64_t m_logAccepted = 0;
    uint64_t m_logCompleted = 0;
    bool m_logFailed = false;
    std::atomic<uint64_t> m_rejectedLogBatches{0};
    std::atomic<uint64_t> m_discardedLogFrames{0};
    std::atomic<uint64_t> m_droppedLogBytes{0};
    std::atomic<bool> m_stopping{false};
    std::atomic<void*> m_pipeForCancel{nullptr};
};
