// DynamicIsland-Win / 命名管道服务端（C++17 + Win32）
//
// 让**任何程序**都能往岛上推一条活动：CI 跑完、下载器完成、脚本跑完、IDE 编译
// 结束 —— 这些信息本来就该出现在胶囊上，而不是各自弹一个右下角通知。
//
// 设计取舍（都是刻意的）：
// 1. 一次连接一条消息，读写完即断开。长连接需要心跳、重连退避与半包处理，
//    而"一条命令推一条活动"用完就关是最容易被别的语言调用成功的形态
//    （PowerShell / Python / C# 都是三行代码）。
// 2. 消费放在工作线程，主线程只从队列取现成的 Request —— 与采集层同一套模型，
//    渲染主循环不会被一个卡住的客户端拖住。
// 3. 管道名固定、走默认 DACL：创建者（当前用户）独占，其他会话拿到的是
//    ACCESS_DENIED 而不是劫持。这满足协议文档里"per-user"的语义，
//    也省掉一堆 SID 拼接代码。
// 4. 上限全在解码器里（core/ipc_protocol.hpp），这一层只负责"别读爆内存"。
#ifndef DIW_SRC_WIN32_IPC_SERVER_HPP
#define DIW_SRC_WIN32_IPC_SERVER_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/ipc_protocol.hpp"

namespace diw {
namespace win32 {

class IpcServer {
public:
    static constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\DynamicIslandWin-v1";
    // 未消费请求的上限。见 HandleOne 里的注释：这是防止本机进程 Flood 的一道闸。
    static constexpr std::size_t kMaxQueued = 256;

    IpcServer() = default;
    ~IpcServer() { Stop(); }
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    // 日志注入必须在 Start() 之前完成（与采集层同一约定）。
    void SetLogger(std::function<void(const std::string&)> logger) { logger_ = std::move(logger); }

    bool Start() {
        if (running_.load()) return true;
        running_.store(true);
        worker_ = std::thread([this] { Loop(); });
        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        // ConnectNamedPipe 是阻塞的，自己连一下把工作线程叫醒 —— 比引入
        // OVERLAPPED + 事件对象简单得多，而这台机器上只会有我们自己连。
        HANDLE wake = ::CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        const DWORD wake_err = (wake == INVALID_HANDLE_VALUE) ? ::GetLastError() : 0;
        if (wake != INVALID_HANDLE_VALUE) ::CloseHandle(wake);
        if (!worker_.joinable()) return;
        // 上面那招有前提：当前没有别的客户端连着。若某个客户端「连上却不发/不读」，
        // 工作线程会卡在 ReadFile 或 FlushFileBuffers 里，wake 连不进来，
        // join() 就永久阻塞 —— 实测就是这个现象（进程退不掉）。
        // CancelSynchronousIo 只取消**当前那一次**同步 I/O，所以要反复取消：
        // 线程可能刚被从 ReadFile 里救出来，又卡进 FlushFileBuffers。
        const HANDLE worker_handle = reinterpret_cast<HANDLE>(worker_.native_handle());
        for (int i = 0; i < 25 && !worker_done_.load(); ++i) {
            ::CancelSynchronousIo(worker_handle);
            ::Sleep(16);
        }
        Log("ipc: stop wake_err=" + std::to_string(wake_err) +
            " done=" + std::to_string(worker_done_.load() ? 1 : 0));
        if (worker_done_.load()) {
            worker_.join();
            Log("ipc: stopped");
        } else {
            // 极端情况：客户端既不发也不读，线程仍卡在管道 I/O 上。
            // 此时 join 会让进程永远退不掉，比「泄漏一个正在退出的线程」更糟。
            // 进程紧随其后就结束了，detach 的线程随之消失。
            worker_.detach();
            Log("ipc: stop timeout, thread detached (process exiting)");
        }
    }

    // 主循环每帧调一次：把攒下的请求整体取走（不阻塞）。
    void TakeAll(std::vector<ipc::Request>* out) {
        if (out == nullptr) return;
        std::lock_guard<std::mutex> lock(queue_mutex_);
        out->insert(out->end(), queue_.begin(), queue_.end());
        queue_.clear();
    }

    std::uint64_t Accepted() const { return accepted_.load(); }
    std::uint64_t Rejected() const { return rejected_.load(); }

private:
    void Log(const std::string& line) {
        if (logger_) logger_(line);
    }

    void Loop() {
        while (running_.load()) {
            // 每条连接单独建一个实例：连接断开就关掉重建，避免残留状态
            // （上一条消息读到一半断开时，缓冲区不能带到下一条连接里）。
            HANDLE pipe = ::CreateNamedPipeW(
                kPipeName, PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 8192, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                // 名字被别的实例占用（多开）或权限不足：退避后重试，不要忙等。
                ::Sleep(500);
                continue;
            }
            const BOOL connected = ::ConnectNamedPipe(pipe, nullptr);
            if (!running_.load()) {
                ::CloseHandle(pipe);
                break;
            }
            if (!connected && ::GetLastError() != ERROR_PIPE_CONNECTED) {
                ::CloseHandle(pipe);
                continue;
            }
            HandleOne(pipe);
            Log("ipc: handled, disconnecting");
            ::DisconnectNamedPipe(pipe);
            Log("ipc: disconnected");
            ::CloseHandle(pipe);
        }
        worker_done_.store(true);
        Log("ipc: loop exit");
    }

    void HandleOne(HANDLE pipe) {
        std::string line;
        char buf[1024];
        bool complete = false;
        for (;;) {
            DWORD got = 0;
            if (!::ReadFile(pipe, buf, sizeof(buf), &got, nullptr) || got == 0) {
                Log("ipc: read end, err=" + std::to_string(::GetLastError()));
                break;
            }
            for (DWORD i = 0; i < got; ++i) {
                if (buf[i] == '\n') {
                    complete = true;
                    break;
                }
                line.push_back(buf[i]);
            }
            if (complete) break;
            if (line.size() > ipc::kMaxLineBytes) break;  // 超长就地放弃，别把内存吃光
        }
        if (line.size() > ipc::kMaxLineBytes) {
            ++rejected_;
            Respond(pipe, false, "line too long");
            Log("ipc: reject (line too long)");
            return;
        }
        if (!complete) {
            ++rejected_;
            Respond(pipe, false, "no newline terminator");
            Log("ipc: reject (no newline)");
            return;
        }

        ipc::Request request;
        std::string error;
        if (!ipc::DecodeLine(line, &request, &error)) {
            ++rejected_;
            Respond(pipe, false, error);
            Log("ipc: reject (" + error + ")");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            // 队列上限：主循环每帧都会 TakeAll，正常流程攒不下几条。
            // 但本机**任意进程**都能往管道里 Flood —— 一个写错的脚本循环推 10 万条，
            // 这里原本没有任何上限，内存会被吃光，表现为岛突然膨胀然后被杀。
            if (queue_.size() >= kMaxQueued) {
                ++rejected_;
                Respond(pipe, false, "queue full");
                Log("ipc: reject (queue full)");
                return;
            }
            queue_.push_back(request);
        }
        ++accepted_;
        Respond(pipe, true, "");
    }

    static void Respond(HANDLE pipe, bool ok, const std::string& error) {
        std::string text = ok ? std::string("{\"ok\":true}\n")
                              : ("{\"ok\":false,\"error\":" + json::Quote(error) + "}\n");
        DWORD written = 0;
        ::WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        // 必须 flush：客户端靠 ReadAll 读到 EOF 才拿得到完整内容，
        // 不 flush 时实测收到 0 字节（响应丢在管道缓冲里）。
        // 代价是：客户端「连上却不读」时这里会阻塞 —— 由 Stop() 的
        // 反复 CancelSynchronousIo + 超时 detach 兜住，见 Stop() 的注释。
        ::FlushFileBuffers(pipe);
    }

    std::function<void(const std::string&)> logger_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::mutex queue_mutex_;
    std::vector<ipc::Request> queue_;
    // 工作线程是否已退出。Stop() 用它决定「join」还是「detach」（见 Stop）。
    std::atomic<bool> worker_done_{false};
};

}  // namespace win32
}  // namespace diw

#endif  // DIW_SRC_WIN32_IPC_SERVER_HPP
