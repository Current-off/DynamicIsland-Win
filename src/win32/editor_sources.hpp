#pragma once
// 编辑器状态采集：只读「前台窗口是谁 + 标题是什么」。
//
// 和 browser_sources 同构，但简单得多 —— 编辑器没有 SQLite、没有下载队列，
// 窗口标题就是全部信息。所以这里没有任何 IO 节流，每一拍（300ms）都可以读。

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include "../core/editor_state.hpp"

namespace bi {
namespace win32 {

inline std::wstring EditorWiden(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline std::string EditorNarrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                                        nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr,
                          nullptr);
    return out;
}

class EditorWatcher {
public:
    void SetEnabled(bool on) { enabled_ = on; }
    void SetSuppressed(bool on) { suppressed_ = on; }
    void SetLogger(std::function<void(const std::string&)> fn) { log_fn_ = std::move(fn); }
    // 调试：伪造一个编辑器标题。真实链路要求「用户正开着编辑器并打开了一个文件」，
    // 而截图回归必须能重复，所以留这条注入。
    void SetForce(const std::string& exe_stem, const std::string& title) {
        force_exe_ = exe_stem;
        force_title_ = title;
    }

    void Poll(std::int64_t now_ms) {
        if (!enabled_ || suppressed_) {
            std::lock_guard<std::mutex> lock(mutex_);
            active_.clear();
            return;
        }

        std::string exe_stem;
        std::string title;
        if (!force_title_.empty()) {
            exe_stem = force_exe_;
            title = force_title_;
        } else {
            ReadForeground(&exe_stem, &title);
        }

        EditorState st;
        const bool ok = !title.empty() && BuildEditorState(exe_stem, title, &st);
        const bool informative = ok && !IsEditorChrome(st.file);
        if (!informative) {
            // 前台不是编辑器、或者只是编辑器的欢迎页/设置页：
            // 不更新状态，但也不立刻清空 —— 交给下面的宽限期处理，
            // 否则「点开设置看一眼再切回来」会让岛闪一下。
            if (ok && !reported_idle_) {
                reported_idle_ = true;
                Log("editor: foreground=" + (exe_stem.empty() ? std::string("?") : exe_stem) +
                    " title=" + (title.empty() ? std::string("-") : title.substr(0, 40)) +
                    " → 不出岛（不是编辑器或无障碍信息）");
            }
        } else {
            reported_idle_ = false;
            st.foreground = true;
            st.last_fg_ms = now_ms;
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_.file != st.file || current_.project != st.project ||
                current_.brand != st.brand || !had_) {
                Log("editor: " + st.label + " 「" + (st.project.empty() ? "-" : st.project) + "」 " +
                    st.file + (st.language.empty() ? "" : " (" + st.language + ")"));
            }
            current_ = st;
            had_ = true;
        }

        // 离开宽限 2.5 秒：Alt+Tab 回编辑器的那一帧岛不该已经消失。
        std::lock_guard<std::mutex> lock(mutex_);
        if (!had_) {
            active_.clear();
            return;
        }
        if (now_ms - current_.last_fg_ms > 2500) {
            active_.clear();
            had_ = false;
            current_ = EditorState{};
            return;
        }
        active_.assign(1, current_);
    }

    std::vector<EditorState> Active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }

    std::string Diag() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!had_) return "idle";
        return current_.brand + ":" + current_.file;
    }

private:
    void Log(const std::string& line) {
        if (log_fn_) log_fn_(line);
    }

    static void ReadForeground(std::string* exe_stem, std::string* title) {
        exe_stem->clear();
        title->clear();
        HWND fg = ::GetForegroundWindow();
        if (fg == nullptr) return;
        wchar_t buf[512] = {0};
        const int len = ::GetWindowTextW(fg, buf, 512);
        if (len > 0) *title = EditorNarrow(std::wstring(buf, static_cast<std::size_t>(len)));

        DWORD pid = 0;
        ::GetWindowThreadProcessId(fg, &pid);
        if (pid == 0) return;
        HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc == nullptr) return;
        wchar_t path[1024] = {0};
        DWORD size = 1024;
        if (::QueryFullProcessImageNameW(proc, 0, path, &size) != 0) {
            std::wstring full(path, size);
            const std::size_t slash = full.find_last_of(L"\\/");
            std::wstring base = slash == std::wstring::npos ? full : full.substr(slash + 1);
            if (base.size() > 4 && base.compare(base.size() - 4, 4, L".exe") == 0) {
                base.resize(base.size() - 4);
            }
            *exe_stem = EditorNarrow(base);
        }
        ::CloseHandle(proc);
    }

    bool enabled_ = true;
    bool suppressed_ = false;
    bool reported_idle_ = false;
    std::string force_exe_;
    std::string force_title_;

    mutable std::mutex mutex_;
    EditorState current_;
    bool had_ = false;
    std::vector<EditorState> active_;
    std::function<void(const std::string&)> log_fn_;
};

}  // namespace win32
}  // namespace bi
