// Agent 会话探测：真实扫描 %USERPROFILE% 下各家 Agent 的会话文件，
// 打印解析出的「它在干什么」。这是唯一能确认 UI 上那行字不是编的证据 ——
// 单测只能证明解析器对固定输入正确，证明不了「本机真有数据、字段就在那儿」。
//
// 用法：
//   agent_probe                      扫一遍并打印
//   agent_probe --watch=6            连续观察 6 秒（每 800ms 一拍），看状态会不会随写入变化
//   agent_probe --home=D:\tmp\fixture 指定 profile 目录（用假数据验证发现逻辑）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/win32/agent_sources.hpp"

namespace {

std::int64_t NowMs() {
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<std::int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

void PrintSession(const diw::AgentSession& s, std::int64_t now) {
    std::printf("  [%s] %s  (%s)\n", s.provider.c_str(), s.project.c_str(),
                diw::AgentStateAt(s, now));
    std::printf("    session : %s\n", s.session_id.c_str());
    std::printf("    cwd     : %s\n", s.cwd.c_str());
    std::printf("    model   : %s\n", s.model.empty() ? "-" : s.model.c_str());
    std::printf("    task    : %s\n", s.task.empty() ? "-" : s.task.c_str());
    std::printf("    action  : %s\n", s.action.empty() ? "-" : s.action.c_str());
    std::printf("    detail  : %s\n", s.detail.empty() ? "-" : s.detail.c_str());
    std::printf("    reply   : %s\n", s.last_text.empty() ? "-" : s.last_text.c_str());
    std::printf("    clock   : started=%lld last=%lld now=%lld\n",
                static_cast<long long>(s.started_ms), static_cast<long long>(s.last_event_ms),
                static_cast<long long>(now));
    std::printf("    elapsed : %s   tools=%d  ctx=%s  todo=%d/%d\n",
                diw::AgentElapsedText(s, now).c_str(), s.tool_calls,
                diw::TokenText(s.ctx_tokens).empty() ? "-" : diw::TokenText(s.ctx_tokens).c_str(),
                s.done_count, s.todo_count);
    for (const auto& t : s.todos) {
        const char* mark = t.status == "completed" ? "[x]" : (t.status == "in_progress" ? "[>]" : "[ ]");
        std::printf("      %s %s\n", mark, t.text.c_str());
    }
    for (std::size_t i = 0; i < s.timeline.size(); ++i) {
        std::printf("      · %s\n", s.timeline[i].c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    double watch_sec = 0.0;
    std::wstring home;
    bool ignore_window = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--watch=", 8) == 0) {
            watch_sec = std::atof(argv[i] + 8);
        } else if (std::strncmp(argv[i], "--home=", 7) == 0) {
            home = diw::win32::AgentWiden(argv[i] + 7);
        } else if (std::strcmp(argv[i], "--all") == 0) {
            ignore_window = true;
        }
    }

    diw::win32::AgentWatcher watcher;
    watcher.SetMaxIslands(ignore_window ? 40 : 4);
    watcher.SetIgnoreWindow(ignore_window);
    if (!home.empty()) {
        watcher.SetHomeOverride(home);
        std::printf("home override: %s\n", argv[0]);
    }
    watcher.SetLogger([](const std::string& line) { std::printf("log: %s\n", line.c_str()); });

    const int rounds = watch_sec > 0.0 ? static_cast<int>(watch_sec * 1000.0 / 800.0) + 1 : 1;
    for (int r = 0; r < rounds; ++r) {
        const std::int64_t now = NowMs();
        watcher.Poll(now);
        const auto files = watcher.Files();
        const auto active = watcher.Active();
        std::printf("\n===== round %d/%d  files=%zu active=%zu =====\n", r + 1, rounds, files.size(),
                    active.size());
        std::printf("  sqlite: %s\n", watcher.SqliteDiag().c_str());
        for (const auto& f : files) {
            std::printf("  file %s  %s  offset=%llu size=%llu primed=%d\n", f.provider.c_str(),
                        diw::win32::AgentNarrow(f.path).c_str(),
                        static_cast<unsigned long long>(f.offset),
                        static_cast<unsigned long long>(f.size), f.primed ? 1 : 0);
        }
        for (const auto& s : active) PrintSession(s, now);
        if (r + 1 < rounds) ::Sleep(800);
    }
    std::printf("\nagent_probe: done\n");
    return 0;
}
