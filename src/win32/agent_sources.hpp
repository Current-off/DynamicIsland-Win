// balance_island / 编码 Agent 会话监视（Win32 文件 IO）
//
// 只做三件事：
//   1. 找到各家 Agent 落盘的会话文件（claude / workbuddy / codex / gemini）
//   2. 增量 tail（只读新增的那一段），喂给 core/agent_session.hpp
//   3. 按「最近还活着」筛出要显示在岛上的会话
//
// 为什么不是「每次重读整个文件」：
//   一个跑了一下午的会话文件能到几十 MB。每 800ms 重读一次等于持续把磁盘
//   拖满，而且 mic 上的风扇会替用户投诉。增量 offset 是唯一可行的做法。
//
// 为什么首轮只读头尾两段：
//   头 64KB 里有 sessionId / cwd / 第一条用户输入（用来显示「它在做什么任务」），
//   尾 1MB 里有最近的动作与任务清单。中间的历史对「现在在干什么」没有价值，
//   却占了 99% 的字节。
#ifndef BI_SRC_WIN32_AGENT_SOURCES_HPP
#define BI_SRC_WIN32_AGENT_SOURCES_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <shlobj.h>

#include "../core/agent_session.hpp"
#include "sqlite_lite.hpp"

namespace bi {
namespace win32 {

// 支持矩阵（写在这里，README 与设置界面引用同一份说法）：
//   claude    Claude Code        ~/.claude/projects/<proj>/<session>.jsonl
//   workbuddy WorkBuddy/CodeBuddy ~/.workbuddy/projects/<proj>/<session>.jsonl
//   codex     OpenAI Codex CLI   ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl
//   gemini    Gemini CLI         ~/.gemini/tmp/<proj>/chats/session-*.json
//   pi        Pi                 ~/.pi/agent/sessions/--<cwd>--/<ts>_<uuid>.jsonl
struct AgentProviderDef {
    const char* id;
    const char* label;
    const char* folder;   // 相对 %USERPROFILE%
    const char* ext;      // ".jsonl" / ".json"
    bool jsonl;           // true = 逐行 JSON；false = 整文件一个 JSON
    int max_depth;        // 根目录下的目录深度上限
};

inline const AgentProviderDef* AgentProviders(int* count) {
    static const AgentProviderDef kProviders[] = {
        {"claude", "Claude Code", ".claude\\projects", ".jsonl", true, 3},
        {"workbuddy", "WorkBuddy", ".workbuddy\\projects", ".jsonl", true, 3},
        {"codex", "Codex", ".codex\\sessions", ".jsonl", true, 5},
        {"gemini", "Gemini CLI", ".gemini\\tmp", ".json", false, 4},
        {"pi", "Pi", ".pi\\agent\\sessions", ".jsonl", true, 2},
    };
    if (count != nullptr) *count = static_cast<int>(sizeof(kProviders) / sizeof(kProviders[0]));
    return kProviders;
}

inline std::string AgentNarrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], need, nullptr,
                          nullptr);
    return out;
}

inline std::wstring AgentWiden(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], need);
    return out;
}

// 用户主目录。用 SHGetKnownFolderPath 而不是读 %USERPROFILE%：
// 后者在部分企业镜像里被改指向网络盘，Agent 的真实数据还在本地 profile 下。
inline std::wstring AgentProfileDir() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &raw)) && raw != nullptr) {
        std::wstring out(raw);
        ::CoTaskMemFree(raw);
        if (!out.empty()) return out;
    }
    wchar_t buf[MAX_PATH * 2] = {0};
    const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH * 2);
    return n > 0 ? std::wstring(buf, n) : std::wstring();
}

// 递归收集指定扩展名的文件。深度受限，因为 Agent 的目录层级是已知的：
// 无脑递归会把 node_modules 之类整个扫进来（claude 项目目录里确实可能有）。
inline void CollectAgentFiles(const std::wstring& dir, const std::wstring& ext, int depth,
                              std::vector<std::wstring>* out) {
    if (depth < 0 || out == nullptr) return;
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;
        const std::wstring full = dir + L"\\" + name;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            // subagents / tool-results 是同一会话的分片文件，收进来会变成
            // 「同一个 agent 冒出三个岛」，必须整棵子树排除。
            if (name == L"subagents" || name == L"tool-results" || name == L"node_modules" ||
                name == L".git") {
                continue;
            }
            CollectAgentFiles(full, ext, depth - 1, out);
            continue;
        }
        if (name.size() <= ext.size()) continue;
        const std::wstring tail = name.substr(name.size() - ext.size());
        if (::CompareStringOrdinal(tail.c_str(), -1, ext.c_str(), -1, TRUE) != CSTR_EQUAL) continue;
        out->push_back(full);
        // break 而不是 return：return 会跳过循环外的 FindClose，
        // 在 ≥400 个会话文件的机器上每 3 秒漏一个句柄（issue #2 第 11 条）。
        if (out->size() >= 400) break;  // 单人机器不可能同时跑 400 个会话
    } while (::FindNextFileW(find, &fd) != FALSE);
    ::FindClose(find);
}

struct AgentFileStamp {
    std::uint64_t size = 0;
    std::int64_t mtime_ms = 0;
    std::int64_t ctime_ms = 0;
    bool ok = false;
};

inline std::int64_t FileTimeToUnixMs(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 1601-01-01 起的 100ns -> 1970 起的 ms
    const std::uint64_t ticks = u.QuadPart;
    if (ticks < 116444736000000000ULL) return 0;
    return static_cast<std::int64_t>((ticks - 116444736000000000ULL) / 10000ULL);
}

inline std::int64_t NowUnixMs() {
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    return FileTimeToUnixMs(ft);
}

inline AgentFileStamp StampOf(const std::wstring& path) {
    AgentFileStamp st;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) == FALSE) return st;
    st.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    st.mtime_ms = FileTimeToUnixMs(info.ftLastWriteTime);
    st.ctime_ms = FileTimeToUnixMs(info.ftCreationTime);
    st.ok = true;
    return st;
}

// 读 [from, from+want) 这一段。agent 正在写这个文件，必须带 SHARE_WRITE，
// 否则打开就失败（表现为「岛永远不出现，但文件明明在长」）。
inline std::string ReadFileRange(const std::wstring& path, std::uint64_t from, std::uint64_t want,
                                 std::uint64_t* got_from) {
    std::string out;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(from);
    if (::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) == FALSE) {
        ::CloseHandle(h);
        return out;
    }
    out.resize(static_cast<std::size_t>(want));
    DWORD read = 0;
    const BOOL ok = ::ReadFile(h, &out[0], static_cast<DWORD>(want), &read, nullptr);
    ::CloseHandle(h);
    if (ok == FALSE) return std::string();
    out.resize(read);
    if (got_from != nullptr) *got_from = from;
    return out;
}

// ---------------------------------------------------------------- 监视器

class AgentWatcher {
public:
    struct FileState {
        std::string provider;
        std::string provider_label;
        std::wstring path;
        std::uint64_t offset = 0;   // 已解析到的字节位置（总在行首）
        std::uint64_t size = 0;
        std::int64_t mtime_ms = 0;
        bool primed = false;
        AgentSession session;
    };

    void SetEnabled(bool on) { enabled_.store(on); }
    bool Enabled() const { return enabled_.load(); }
    void SetMaxIslands(int n) { max_islands_.store(std::max(1, std::min(n, 40))); }
    void SetIdleHideMs(std::int64_t ms) { idle_hide_ms_.store(std::max<std::int64_t>(30000, ms)); }
    void SetLogger(std::function<void(const std::string&)> fn) { log_fn_ = std::move(fn); }
    // 静默期内不显示（全屏看电影时不该有 agent 岛）
    void SetSuppressed(bool on) { suppressed_.store(on); }
    // 调试用：把「用户主目录」指到别处，用一份假会话文件验证 UI 与解析。
    // 没有这个口子，想测「有任务清单的展开态」就只能真去跑一个写完
    // TodoWrite 的会话——那种测试不可重复，也没法在 CI 里跑。
    void SetHomeOverride(const std::wstring& home) { home_override_ = home; }
    // 调试用：忽略「最近多久」的时间窗。用户问「我的某个 agent 为什么没上岛」时，
    // 第一件要分清的事是「没数据」还是「有数据但没解析出来」——
    // 打开它就能看到所有历史会话的解析结果。
    void SetIgnoreWindow(bool on) { ignore_window_ = on; }

    // 采集线程调用。返回的会话数不超过 max_islands_，按「最近有动静」排序。
    void Poll(std::int64_t now_ms) {
        if (!enabled_.load() || suppressed_.load()) return;
        if (now_ms - last_scan_ms_ >= kScanEveryMs || files_.empty()) {
            last_scan_ms_ = now_ms;
            Scan(now_ms);
            // 库类 provider 和文件扫描同频（3s）：它们本身就是 SQLite，
            // 每次查询都要 open+prepare，800ms 一拍属于白烧 IO。
            PollSqliteProviders(now_ms);
        }
        for (auto& f : files_) {
            const AgentFileStamp st = StampOf(f.path);
            if (!st.ok) continue;
            if (!f.primed) {
                Prime(f, st);
                continue;
            }
            if (st.size == f.size && st.mtime_ms == f.mtime_ms) continue;
            f.size = st.size;
            f.mtime_ms = st.mtime_ms;
            if (st.size < f.offset) {
                // 文件被截断 / 轮转：从头再来一遍，否则永远读不到新内容。
                f.offset = 0;
                f.session = AgentSession{};
                f.session.provider = f.provider;
                f.session.provider_label = f.provider_label;
                f.primed = false;
                Prime(f, st);
                continue;
            }
            std::uint64_t from = f.offset;
            // 落后太多（应用挂了很久 / 断点续传式长跑）：只补最后 512KB，
            // 中间的动作早就被后面的覆盖了，读了也不会显示。
            if (st.size - from > kMaxCatchUpBytes) from = st.size - kMaxCatchUpBytes;
            std::string chunk = ReadFileRange(f.path, from, st.size - from, nullptr);
            if (chunk.empty()) continue;
            // base 是「chunk[0] 在文件里的绝对偏移」。跳过不完整行时会擦掉一段，
            // 基准必须跟着前移，否则下一拍从早 (nl+1) 字节处重读，本轮已消费的行
            // 会被再解析一遍 —— 表现为 timeline / tool_calls 重复累计（issue #2 第 3a 条）。
            std::uint64_t base = from;
            if (from != f.offset) {
                // 从中间切进去：丢掉第一个不完整的行。
                const std::size_t nl = chunk.find('\n');
                if (nl == std::string::npos) continue;
                chunk.erase(0, nl + 1);
                base = from + nl + 1;
            }
            // 只喂到最后一个换行为止：剩下的半行留到下一拍。
            // 否则尾部半行本拍被当成完整行解析一次、下一拍再解析一次
            //（写入端「先 flush 正文、换行随下次写入落地」是常态）—— 第 3b 条。
            const std::size_t last_nl = chunk.rfind('\n');
            if (last_nl == std::string::npos) continue;  // 这次连一行完整都没有
            chunk.resize(last_nl + 1);
            ConsumeLines(f, chunk);
            f.offset = base + chunk.size();
        }
        Publish(now_ms);
    }

    // 调试：SQLite 类 provider 这一拍看到了什么（几条会话 / 打开失败）。
    std::string SqliteDiag() const {
        std::lock_guard<std::mutex> lock(sql_mutex_);
        return sql_diag_;
    }

    std::vector<AgentSession> Active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }

    // 诊断用：当前跟踪的文件与解析结果。探针直接打印它。
    std::vector<FileState> Files() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return files_snapshot_;
    }

private:
    static constexpr std::int64_t kScanEveryMs = 3000;
    // 只关心最近这么久的库会话：Cursor 里躺着几百个历史 composer，
    // 全捞出来既慢又没意义。
    static constexpr std::int64_t kSqliteWindowMs = 6LL * 60LL * 60LL * 1000LL;
    static constexpr std::uint64_t kHeadBytes = 64ull * 1024ull;
    static constexpr std::uint64_t kTailBytes = 1024ull * 1024ull;
    static constexpr std::uint64_t kMaxCatchUpBytes = 512ull * 1024ull;
    static constexpr std::size_t kMaxLineBytes = 256ull * 1024ull;
    static constexpr std::int64_t kConsiderWindowMs = 45LL * 60LL * 1000LL;
    static constexpr int kMaxFiles = 48;

    void Log(const std::string& line) {
        if (log_fn_) log_fn_(line);
    }

    void Scan(std::int64_t now_ms) {
        const std::wstring home = home_override_.empty() ? AgentProfileDir() : home_override_;
        if (home.empty()) return;
        int count = 0;
        const AgentProviderDef* defs = AgentProviders(&count);

        std::vector<std::pair<std::wstring, const AgentProviderDef*>> found;
        for (int i = 0; i < count; ++i) {
            const AgentProviderDef& def = defs[i];
            const std::wstring root = home + L"\\" + AgentWiden(def.folder);
            if (::GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            std::vector<std::wstring> files;
            CollectAgentFiles(root, AgentWiden(def.ext), def.max_depth, &files);
            for (auto& f : files) found.emplace_back(std::move(f), &def);
        }
        // 只跟踪「最近动过」的文件：历史会话一坨几十个，全登进岛没有意义，
        // 而且每个都要 stat 一次，纯浪费。
        std::vector<std::pair<std::int64_t, std::size_t>> ranked;
        std::vector<AgentFileStamp> stamps(found.size());
        for (std::size_t i = 0; i < found.size(); ++i) {
            stamps[i] = StampOf(found[i].first);
            if (!stamps[i].ok) continue;
            if (!ignore_window_ && now_ms - stamps[i].mtime_ms > kConsiderWindowMs) continue;
            ranked.emplace_back(stamps[i].mtime_ms, i);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        if (static_cast<int>(ranked.size()) > kMaxFiles) ranked.resize(kMaxFiles);

        std::vector<FileState> next;
        next.reserve(ranked.size());
        for (const auto& entry : ranked) {
            const std::wstring& path = found[entry.second].first;
            const AgentProviderDef* def = found[entry.second].second;
            auto it = std::find_if(files_.begin(), files_.end(),
                                   [&path](const FileState& s) { return s.path == path; });
            if (it != files_.end()) {
                it->mtime_ms = stamps[entry.second].mtime_ms;
                it->size = stamps[entry.second].size;
                next.push_back(std::move(*it));
            } else {
                FileState st;
                st.provider = def->id;
                st.provider_label = def->label;
                st.path = path;
                st.mtime_ms = stamps[entry.second].mtime_ms;
                st.size = stamps[entry.second].size;
                next.push_back(std::move(st));
            }
        }
        files_ = std::move(next);
        if (ranked.empty() && !warned_empty_) {
            warned_empty_ = true;
            Log("agent: no session files under " + AgentNarrow(home) +
                " (.claude / .workbuddy / .codex / .gemini)");
        }
    }

    // 首次：头一段拿身份与任务，尾一段拿当前动作与清单。
    void Prime(FileState& f, const AgentFileStamp& st) {
        f.session = AgentSession{};
        f.session.provider = f.provider;
        f.session.provider_label = f.provider_label;
        f.session.log_path = AgentNarrow(f.path);
        f.size = st.size;
        f.mtime_ms = st.mtime_ms;

        const AgentProviderDef* def = nullptr;
        int count = 0;
        const AgentProviderDef* all = AgentProviders(&count);
        for (int i = 0; i < count; ++i) {
            if (f.provider == all[i].id) {
                def = &all[i];
                break;
            }
        }
        if (def != nullptr && !def->jsonl) {
            // 整文件 JSON（Gemini）：读全文，限 4MB。再大就只读尾部会切坏 JSON，
            // 那还不如不显示。
            const std::uint64_t want = std::min<std::uint64_t>(st.size, 4ull * 1024ull * 1024ull);
            std::string body = ReadFileRange(f.path, 0, want, nullptr);
            if (!body.empty()) {
                for (const auto& ev : ParseGeminiSession(body)) ApplyAgentEvent(&f.session, ev);
            }
            f.offset = st.size;
            f.primed = true;
            FinalizeStart(f, st);
            return;
        }

        std::uint64_t head_consumed = 0;
        std::string head = ReadFileRange(f.path, 0, std::min<std::uint64_t>(st.size, kHeadBytes), nullptr);
        if (!head.empty()) {
            // 头段最后一行多半是半截，直接切掉。
            const std::size_t nl = head.rfind('\n');
            if (nl != std::string::npos) {
                head.resize(nl);
                head_consumed = static_cast<std::uint64_t>(nl) + 1;  // 那个换行也算读过了
            } else {
                head_consumed = head.size();
            }
            ConsumeLines(f, head);
        }
        // 尾部窗口不能和头段重叠：小文件（几 KB）时 head 已经读完了全文，
        // 再从 0 读一遍会把每个事件应用两次 —— 表现是「工具次数翻倍、
        // 时间线重复」，而这类重复在界面上看起来像数据错乱，很难联想到读取范围。
        const std::uint64_t tail_from =
            st.size > kTailBytes ? st.size - kTailBytes : 0;
        const std::uint64_t read_from = (std::max)(tail_from, head_consumed);
        std::string tail = ReadFileRange(f.path, read_from, st.size - read_from, nullptr);
        if (!tail.empty()) {
            // 只有「从中间切进去」时才要丢掉第一个半行。
            if (read_from > head_consumed) {
                const std::size_t nl = tail.find('\n');
                if (nl == std::string::npos) tail.clear();
                else tail.erase(0, nl + 1);
            }
            ConsumeLines(f, tail);
        }
        f.offset = st.size;
        f.primed = true;
        FinalizeStart(f, st);
        if (!f.session.session_id.empty()) {
            Log("agent: prime " + f.provider + " 「" + f.session.project + "」 action=" +
                (f.session.action.empty() ? "-" : f.session.action) + " tools=" +
                std::to_string(f.session.tool_calls));
        }
    }

    // 会话起点取「解析出的第一行时间」与「文件创建时间」里更早的那个：
    // 创建时间在复制/同步过来的会话上会被重置成今天，而首行时间才是真的开始。
    static void FinalizeStart(FileState& f, const AgentFileStamp& st) {
        const bool ctime_ok = st.ctime_ms > 1577836800000LL;  // 2020-01-01
        const std::int64_t parsed = f.session.started_ms;
        if (parsed > 0 && ctime_ok) {
            f.session.started_ms = (std::min)(parsed, st.ctime_ms);
        } else if (parsed > 0) {
            f.session.started_ms = parsed;
        } else if (ctime_ok) {
            f.session.started_ms = st.ctime_ms;
        }
    }

    void ConsumeLines(FileState& f, const std::string& blob) {
        std::size_t start = 0;
        while (start < blob.size()) {
            const std::size_t nl = blob.find('\n', start);
            const std::size_t end = nl == std::string::npos ? blob.size() : nl;
            const std::size_t len = end > start ? end - start : 0;
            // 超长行直接跳过：那基本是工具的完整输出（function_call_result），
            // 对「在干什么」没有信息量，却可能一条就 100KB —— 解它的 JSON
            // 是纯烧 CPU，还会把采集线程的 100ms 节拍顶穿。
            if (len > 0 && len <= kMaxLineBytes) {
                std::vector<AgentEvent> events;
                ParseAgentJsonLine(f.provider, blob.substr(start, len), &events);
                for (const auto& ev : events) ApplyAgentEvent(&f.session, ev);
            } else if (len > kMaxLineBytes) {
                // 但要让它算「有动静」：否则跑长命令的 30 秒里状态会掉成 waiting。
                f.session.last_event_ms = (std::max)(f.session.last_event_ms, NowUnixMs());
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        if (f.session.session_id.empty()) {
            f.session.session_id = StemOf(f.path);
        }
        if (f.session.project.empty() && !f.session.cwd.empty()) {
            f.session.project = ProjectOf(f.session.cwd);
        }
    }

    static std::string StemOf(const std::wstring& path) {
        std::size_t slash = path.find_last_of(L"\\/");
        std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        // 去掉 " roll-2025-..-uuid" 这类前缀之外的东西：只留文件名主体。
        const std::size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name = name.substr(0, dot);
        return AgentNarrow(name);
    }

    // ---------------------------------------------------------------- 库类 provider
    // Cursor 与 ZCode 不吃 JSONL，会话在 SQLite 里。它们的结构完全不同，
    // 所以各写一个提取器，只负责把「查出来的行」翻译成同一套 AgentEvent，
    // 后面的状态机、排序、岛的内容全部复用。
    void PollSqliteProviders(std::int64_t now_ms) {
        if (!SqliteLite::ApiReady()) return;
        std::vector<AgentSession> out;
        sql_diag_.clear();
        PollZCode(now_ms, &out);
        PollCursor(now_ms, &out);
        if (sql_diag_.empty()) sql_diag_ = "no sqlite provider found";
        // 单独一把锁：Publish 末尾已经持有 mutex_，这里再拿会自锁
        // （std::mutex 不可重入，表现是主循环整个卡住不刷新）。
        std::lock_guard<std::mutex> lock(sql_mutex_);
        sql_sessions_ = std::move(out);
    }

    // ZCode（智谱 GLM / DeepSeek 的编码 CLI）：`~/.zcode/cli/db/db.sqlite`
    //   session(id, title, directory, time_created, time_updated)
    //   message(session_id, data{role,time,modelId})
    //   part(session_id, data{type:tool|text|reasoning, tool, state.input})
    //   todo(session_id, content, status, position)
    // 这四个表比任何 JSONL 都规整：连任务清单都是结构化列。
    void PollZCode(std::int64_t now_ms, std::vector<AgentSession>* out) {
        const std::wstring home = home_override_.empty() ? AgentProfileDir() : home_override_;
        const std::wstring path = home + L"\\.zcode\\cli\\db\\db.sqlite";
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        SqliteLite db;
        if (!db.Open(path)) {
            sql_diag_ += "zcode:open-failed ";
            return;
        }

        struct Cursor {
            std::string id;
            std::string title;
            std::string directory;
            std::int64_t created = 0;
            std::int64_t updated = 0;
        };
        std::vector<Cursor> sessions;
        const std::string since = std::to_string(ignore_window_ ? 0 : now_ms - kSqliteWindowMs);
        db.Query(
            "select id, coalesce(title,''), coalesce(directory,''), time_created, time_updated "
            "from session where time_updated > ? order by time_updated desc limit 3",
            {since},
            [&sessions](const std::vector<std::string>& row, const std::vector<bool>&) {
                Cursor c;
                c.id = row[0];
                c.title = row[1];
                c.directory = row[2];
                c.created = std::strtoll(row[3].c_str(), nullptr, 10);
                c.updated = std::strtoll(row[4].c_str(), nullptr, 10);
                if (!c.id.empty()) sessions.push_back(std::move(c));
                return true;
            });

        sql_diag_ += "zcode:" + std::to_string(sessions.size()) + " ";
        for (const auto& c : sessions) {
            AgentSession s;
            s.provider = "zcode";
            s.provider_label = "ZCode";
            s.session_id = c.id;
            s.cwd = c.directory;
            s.project = ProjectOf(c.directory);
            s.task = OneLine(c.title, 56);
            s.started_ms = c.created;
            s.last_event_ms = c.updated;
            s.log_path = AgentNarrow(path);

            // part 按时间正序回放：动作/文本都靠「后写覆盖前写」，
            // 倒序应用会让界面停在三步之前。
            struct PartRow {
                std::string data;
            };
            std::vector<PartRow> parts;
            db.Query(
                "select data from part where session_id=? order by time_created desc, sequence desc limit 16",
                {c.id}, [&parts](const std::vector<std::string>& row, const std::vector<bool>&) {
                    parts.push_back({row[0]});
                    return true;
                });
            for (std::size_t i = parts.size(); i-- > 0;) {
                json::Value data;
                if (!json::Parse(parts[i].data, data, nullptr)) continue;
                const std::string ptype = data.At("type").AsString();
                AgentEvent ev;
                ev.ts_ms = 0;  // part 没有自己的时间戳，由会话 updated 兜底
                if (ptype == "tool") {
                    ev.kind = AgentEvent::Kind::Tool;
                    ev.tool = data.At("tool").AsString();
                    const json::Value& input = data.At("state").At("input");
                    ev.detail = ToolTarget(ev.tool, input);
                    // ZCode 的 TodoWrite 入参和 Claude 同构，直接喂给同一套解析；
                    // 真正的清单下面还会用 todo 表覆盖（那份是权威的）。
                    if (ev.tool == "TodoWrite" || ev.tool == "update_plan") {
                        const json::Value& todos =
                            input.At("todos").IsArray() ? input.At("todos") : input.At("plan");
                        std::vector<AgentTodo> parsed = ParseTodos(todos);
                        if (!parsed.empty()) {
                            ev.kind = AgentEvent::Kind::Todo;
                            ev.todos = std::move(parsed);
                        }
                    }
                } else if (ptype == "text") {
                    ev.kind = AgentEvent::Kind::Reply;
                    ev.text = data.At("text").AsString();
                } else {
                    ev.kind = AgentEvent::Kind::Activity;
                }
                ApplyAgentEvent(&s, ev);
            }

            // 模型名：最近一条 assistant 消息里带着
            db.Query("select data from message where session_id=? order by time_created desc limit 1",
                     {c.id}, [&s](const std::vector<std::string>& row, const std::vector<bool>&) {
                         json::Value m;
                         if (json::Parse(row[0], m, nullptr)) {
                             std::string model = m.At("modelId").AsString();
                             if (model.empty()) model = m.At("model").AsString();
                             if (!model.empty()) s.model = model;
                             const std::string role = m.At("role").AsString();
                             if (role != "assistant") s.last_text.clear();
                         }
                         return true;
                     });

            // 清单以 todo 表为准：它是对「当前状态」的权威快照，
            // 而 part 里的 TodoWrite 入参可能落后好几步。
            std::vector<AgentTodo> todos;
            db.Query("select content, status from todo where session_id=? order by position, rowid",
                     {c.id}, [&todos](const std::vector<std::string>& row, const std::vector<bool>&) {
                         AgentTodo t;
                         t.text = row[0];
                         t.status = row[1].empty() ? "pending" : row[1];
                         if (!t.text.empty()) todos.push_back(std::move(t));
                         return true;
                     });
            if (!todos.empty()) {
                s.todos = std::move(todos);
                s.todo_count = static_cast<int>(s.todos.size());
                s.done_count = 0;
                for (const auto& t : s.todos) {
                    if (t.Done()) ++s.done_count;
                }
                for (const auto& t : s.todos) {
                    if (t.Active()) {
                        s.action = "正在做 " + EllipsizeUtf8(t.text, 34);
                        break;
                    }
                }
            }
            if (s.task.empty()) s.task = s.action;
            if (!s.session_id.empty()) out->push_back(std::move(s));
        }
    }

    // Cursor：`%APPDATA%\Cursor\User\globalStorage\state.vscdb`
    //   composerHeaders(composerId, workspaceId, lastUpdatedAt, isArchived, value{name,subtitle,status})
    //   cursorDiskKV(key='composerData:<id>' | 'bubbleId:<cid>:<bid>', value=JSON)
    // Cursor 不把项目路径存在 composer 里，只有 workspaceId 的哈希 ——
    // 真路径在 workspaceStorage/<hash>/workspace.json 的 folder 字段。
    void PollCursor(std::int64_t now_ms, std::vector<AgentSession>* out) {
        PWSTR raw = nullptr;
        if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) || raw == nullptr) {
            return;
        }
        std::wstring appdata(raw);
        ::CoTaskMemFree(raw);
        const std::wstring db_path = appdata + L"\\Cursor\\User\\globalStorage\\state.vscdb";
        if (::GetFileAttributesW(db_path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        SqliteLite db;
        if (!db.Open(db_path)) {
            sql_diag_ += "cursor:open-failed ";
            return;
        }

        struct Header {
            std::string id;
            std::string workspace;
            std::int64_t updated = 0;
            std::string value;
        };
        std::vector<Header> headers;
        db.Query(
            "select composerId, coalesce(workspaceId,''), lastUpdatedAt, value from composerHeaders "
            "where lastUpdatedAt > ? and ifnull(isArchived,0)=0 and ifnull(isSubagent,0)=0 "
            "order by lastUpdatedAt desc limit 3",
            {std::to_string(ignore_window_ ? 0 : now_ms - kSqliteWindowMs)},
            [&headers](const std::vector<std::string>& row, const std::vector<bool>&) {
                Header h;
                h.id = row[0];
                h.workspace = row[1];
                h.updated = std::strtoll(row[2].c_str(), nullptr, 10);
                h.value = row[3];
                if (!h.id.empty()) headers.push_back(std::move(h));
                return true;
            });

        sql_diag_ += "cursor:" + std::to_string(headers.size()) + " ";
        for (const auto& h : headers) {
            json::Value head;
            if (!json::Parse(h.value, head, nullptr)) continue;

            // 空草稿（点开对话框还没发消息）也会写进 composerHeaders，
            // 它的 name/subtitle 就是输入框里的原始字符 —— 收到岛上会变成
            // 一条「Cursor · 1」的怪东西。直接跳过。
            if (head.At("isDraft").AsBool(false)) continue;

            AgentSession s;
            s.provider = "cursor";
            s.provider_label = "Cursor";
            s.session_id = h.id;
            s.task = OneLine(head.At("name").AsString(), 56);
            s.started_ms = static_cast<std::int64_t>(head.At("createdAt").AsDouble(0));
            s.last_event_ms = h.updated;
            s.log_path = AgentNarrow(db_path);

            // 项目路径：workspaceStorage/<hash>/workspace.json -> folder
            const std::wstring ws_json =
                appdata + L"\\Cursor\\User\\workspaceStorage\\" + AgentWiden(h.workspace) +
                L"\\workspace.json";
            std::string ws_text = ReadFileRange(ws_json, 0, 8192, nullptr);
            if (!ws_text.empty()) {
                json::Value ws;
                if (json::Parse(ws_text, ws, nullptr)) {
                    const std::string folder = ws.At("folder").AsString();
                    const std::string uri = ws.At("workspace").AsString();  // 多根工作区
                    const std::wstring path =
                        UriToWindowsPath(!folder.empty() ? folder : uri);
                    s.cwd = AgentNarrow(path);
                    s.project = ProjectOf(s.cwd);
                }
            }
            if (s.cwd.empty() && !h.workspace.empty()) s.cwd = h.workspace;
            if (s.project.empty()) s.project = s.cwd;

            const std::string status = head.At("status").AsString();
            // 最后一条 AI bubble：它的 toolFormerData 才是「此刻在干什么」。
            std::string last_bubble;
            for (const auto& b : head.At("fullConversationHeadersOnly").items) {
                if (b.At("type").AsInt(0) == 2) last_bubble = b.At("bubbleId").AsString();
                const std::string created = b.At("createdAt").AsString();
                const std::int64_t ts = Iso8601ToMs(created);
                if (ts > s.last_event_ms) s.last_event_ms = ts;
            }
            if (!last_bubble.empty()) {
                const std::string key = "bubbleId:" + h.id + ":" + last_bubble;
                db.Query("select value from cursorDiskKV where key=?", {key},
                         [&s](const std::vector<std::string>& row, const std::vector<bool>&) {
                             // 一个 bubble 可能带上百 KB 的代码片段；超过 512KB
                             // 就不解析了 —— 里面最有价值的两三个字段不值得这个代价。
                             if (row[0].size() > 512u * 1024u) return true;
                             json::Value b;
                             if (!json::Parse(row[0], b, nullptr)) return true;
                             const json::Value& tool = b.At("toolFormerData");
                             const std::string tool_name = tool.At("name").AsString();
                             if (!tool_name.empty()) {
                                 AgentEvent ev;
                                 ev.kind = AgentEvent::Kind::Tool;
                                 ev.tool = tool_name;
                                 const json::Value& params = tool.At("params");
                                 json::Value parsed;
                                 const json::Value* use = &params;
                                 if (params.IsString() && json::Parse(params.AsString(), parsed, nullptr)) {
                                     use = &parsed;
                                 }
                                 ev.detail = ToolTarget(tool_name, *use);
                                 ApplyAgentEvent(&s, ev);
                             }
                             const std::string text = b.At("text").AsString();
                             if (!text.empty()) {
                                 AgentEvent ev;
                                 ev.kind = AgentEvent::Kind::Reply;
                                 ev.text = text;
                                 ApplyAgentEvent(&s, ev);
                             }
                             return true;
                         });
            }
            if (s.action.empty()) {
                // subtitle 是 Cursor 自己给的一句话结论；只有一两个字符时
                // （"1"、"ok"）说明这条会话没什么内容，别把它当动作显示。
                const std::string subtitle = head.At("subtitle").AsString();
                const bool usable = subtitle.size() >= 4;
                s.action = usable ? OneLine(subtitle, 40)
                                  : (status == "generating" ? "正在生成…" : "空闲");
            }
            // Cursor 的 generating 是唯一可靠的「正在跑」信号：它写库是在
            // 每一步结束之后，中途 lastUpdatedAt 不动。
            if (status == "generating") s.last_event_ms = now_ms;
            out->push_back(std::move(s));
        }
    }

    void Publish(std::int64_t now_ms) {
        struct Ranked {
            bool running = false;
            std::int64_t last = 0;
            AgentSession session;
        };
        std::vector<Ranked> list;
        // 库类 provider 每一拍已经算好，直接并进同一个候选池。
        {
            std::vector<AgentSession> snapshot;
            {
                std::lock_guard<std::mutex> lock(sql_mutex_);
                snapshot = sql_sessions_;
            }
            for (const auto& s0 : snapshot) {
                const std::string state =
                    ignore_window_ ? std::string("waiting") : AgentStateAt(s0, now_ms, idle_hide_ms_.load());
                if (state == "done") continue;
                Ranked r;
                r.running = state == "running";
                r.last = s0.last_event_ms;
                r.session = s0;
                list.push_back(std::move(r));
            }
        }
        for (const auto& f : files_) {
            if (!f.primed) continue;
            if (f.session.session_id.empty()) continue;
            const std::string state =
                ignore_window_ ? std::string("waiting") : AgentStateAt(f.session, now_ms, idle_hide_ms_.load());
            if (state == "done") continue;
            Ranked r;
            r.running = state == "running";
            r.last = f.session.last_event_ms;
            r.session = f.session;
            list.push_back(std::move(r));
        }
        // 正在跑的一定排在等着的前面；同状态下比最近活跃。
        std::stable_sort(list.begin(), list.end(), [](const Ranked& a, const Ranked& b) {
            if (a.running != b.running) return a.running;
            return a.last > b.last;
        });
        std::vector<AgentSession> out;
        out.reserve(list.size());
        for (auto& r : list) out.push_back(std::move(r.session));
        const int cap = max_islands_.load();
        if (static_cast<int>(out.size()) > cap) out.resize(static_cast<std::size_t>(cap));
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = std::move(out);
        files_snapshot_ = files_;
    }

    // 配置项由窗口线程写、采集线程读：全部用 atomic，避免「热加载配置」
    // 和「正在扫描文件」撞在一起出现撕裂读。
    std::atomic<bool> enabled_{true};
    std::atomic<bool> suppressed_{false};
    std::atomic<int> max_islands_{3};
    std::atomic<std::int64_t> idle_hide_ms_{8 * 60 * 1000};
    std::wstring home_override_;  // 仅调试：指向 fixture 目录
    std::int64_t last_scan_ms_ = -100000;
    bool warned_empty_ = false;
    bool ignore_window_ = false;
    std::vector<FileState> files_;
    // SQLite 类 provider（Cursor / ZCode）的最近结果，每 3 秒整体重算。
    std::vector<AgentSession> sql_sessions_;
    mutable std::mutex sql_mutex_;
    std::string sql_diag_;
    std::vector<AgentSession> active_;
    std::vector<FileState> files_snapshot_;
    mutable std::mutex mutex_;
    std::function<void(const std::string&)> log_fn_;
};

}  // namespace win32
}  // namespace bi

#endif  // BI_SRC_WIN32_AGENT_SOURCES_HPP
