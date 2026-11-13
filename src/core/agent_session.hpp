// DynamicIsland-Win / 编码 Agent 会话解析（C++17，纯函数，无平台依赖）
//
// 这个文件回答一个问题：**这个 Agent 现在在干什么**。
//
// 为什么从「会话文件」读，而不是接各家 SDK / API：
//   1. 主流编码 Agent（Claude Code、Codex CLI、Gemini CLI、WorkBuddy…）
//      全都把每一步落盘成一串 JSON 行，这是唯一「零侵入、可离线、
//      不需要用户再配一次密钥」的共同点。
//   2. 用户要的是「它现在在干什么」，不是「它说了什么」—— 工具调用
//      （Edit / Bash / Grep）本身就是最准确的动作描述，比任何 hook 都细。
//   3. 纯文件解析可以单元测试：给一段固定的 JSONL，断言它解析出的
//      动作、进度、状态。渲染和 IO 都不参与。
//
// 分两层：
//   ParseAgentJsonLine()  一行 JSON -> 0..N 个归一化事件（各家格式差异都在这里）
//   ApplyAgentEvent()     事件 -> 会话状态（任务、动作、清单、用量）
// 采集层（win32/agent_sources.hpp）只负责找文件、读增量、喂这两步。
#ifndef DIW_SRC_CORE_AGENT_SESSION_HPP
#define DIW_SRC_CORE_AGENT_SESSION_HPP

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mini_json.hpp"
#include "rules.hpp"  // TrimSpaces / AsciiLower / ContainsCaseless

namespace diw {

// ---------------------------------------------------------------- 数据

// 清单里的一条。status 与 TodoWrite / update_plan 的取值保持一致，
// 不在这里翻译成枚举：多一层映射就多一处可能对不上的地方。
struct AgentTodo {
    std::string text;
    std::string status;  // pending | in_progress | completed

    bool Done() const { return status == "completed"; }
    bool Active() const { return status == "in_progress"; }
};

struct AgentSession {
    // provider 决定品牌图形、配色与「点标题该拉起什么」。
    std::string provider;        // claude | codex | gemini | workbuddy
    std::string provider_label;  // Claude Code / Codex CLI / Gemini CLI / WorkBuddy
    std::string session_id;      // 文件名（uuid），同一会话内稳定
    std::string cwd;             // 工作目录，点标题打开的就是它
    std::string project;         // cwd 末段，紧凑态的标题
    std::string model;           // 最近一次出现的模型名

    // 「用户让它干什么」：优先取会话自带的标题（WorkBuddy 的 ai-title、
    // Claude Code 的 summary），没有才回落到第一条真人输入。
    // 这一行是展开态最上面的那行背景信息 —— 没有它，「3/7 完成」毫无意义。
    std::string task;
    // 「它现在在干什么」：最近一次工具调用，动词 + 目标。
    std::string action;
    // 「具体是什么」：工具名 · 目标，展开态底部用。
    std::string detail;
    // 最近一句 agent 回复（等待输入时用它说明「它在等你」）。
    std::string last_text;

    std::int64_t started_ms = 0;
    std::int64_t last_event_ms = 0;
    std::int64_t ctx_tokens = 0;  // 上下文规模（不是累计账单，见 ApplyAgentEvent）

    int tool_calls = 0;
    int done_count = 0;
    int todo_count = 0;

    std::vector<AgentTodo> todos;
    // 最近几条动作（新的在前，最多 4 条）。没有清单的 Agent（Codex 早期版本、
    // 纯聊天型）靠这条时间线回答「它在干什么」，而不是只留一句动词。
    std::vector<std::string> timeline;

    std::string log_path;  // 会话文件，点开可直接查看全文

    bool Present() const { return !provider.empty() && !session_id.empty(); }
};

// 归一化事件。Kind::Activity 表示「有新内容，但没有可展示的信息」
// （模型思考、工具结果回包）——它只推进活跃时间，不污染文案。
struct AgentEvent {
    enum class Kind { None, Activity, Meta, Prompt, Reply, Tool, Todo };
    Kind kind = Kind::None;
    std::int64_t ts_ms = 0;
    std::string text;
    std::string tool;
    std::string detail;
    std::string cwd;
    std::string model;
    std::string session_id;
    std::string title;  // ai-title / summary
    std::int64_t ctx_tokens = 0;
    std::vector<AgentTodo> todos;
};

// ---------------------------------------------------------------- 小工具

// 按 UTF-8 字符（不是字节）截断。截半个汉字会写出非法序列，
// 该序列在 GDI+ 里画成方块，比截断本身更难看。
inline std::string EllipsizeUtf8(const std::string& in, std::size_t max_chars) {
    if (max_chars == 0) return std::string();
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::size_t len = 1;
        if ((c & 0x80u) == 0) {
            len = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            len = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            len = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            len = 4;
        }
        if (i + len > in.size()) break;
        // 只有「确实还有内容被砍掉」时才加省略号：正好等于上限的短标题
        // 加一个尾巴，看起来像被截断了，其实一个字都没少。
        if (chars == max_chars) return in.substr(0, i) + "…";
        i += len;
        ++chars;
    }
    return in;
}

// 单行化 + 去多余空白：命令、路径里常有换行与缩进，
// 原样塞进胶囊会把一行撑成一条竖线。
inline std::string OneLine(const std::string& in, std::size_t max_chars) {
    std::string out;
    out.reserve(in.size());
    bool prev_space = false;
    for (const char c : in) {
        const bool space = (c == '\n' || c == '\r' || c == '\t' || c == ' ');
        if (space) {
            if (!prev_space && !out.empty()) out.push_back(' ');
            prev_space = true;
        } else {
            out.push_back(c);
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    // 行首的 "Command: / Stdout:" 之类前缀对「它在干什么」没有信息量。
    static const char* kPrefixes[] = {"Command: ", "Stdout: ", "Output: "};
    for (const char* prefix : kPrefixes) {
        const std::string p(prefix);
        if (out.compare(0, p.size(), p) == 0) {
            out = out.substr(p.size());
            break;
        }
    }
    return EllipsizeUtf8(out, max_chars);
}

// ISO8601 -> epoch 毫秒。C++17 没有 timegm，手算一遍反而可测：
// 支持 "2025-11-12T06:12:37.123Z"、"2025-11-12T06:12:37+08:00"，
// 纯日期 "2025-11-12" 也算合法（取当天零点）。
inline std::int64_t Iso8601ToMs(const std::string& in) {
    if (in.size() < 10) return 0;
    // 用手写取数而不是 sscanf：sscanf 在 MSVC 上是 C4996（不安全函数），
    // 而本项目要求零警告；何况固定宽度字段用不着格式化引擎。
    auto digits = [&in](std::size_t pos, int len) -> int {
        if (pos + static_cast<std::size_t>(len) > in.size()) return -1;
        int value = 0;
        for (int i = 0; i < len; ++i) {
            const char c = in[pos + static_cast<std::size_t>(i)];
            if (c < '0' || c > '9') return -1;
            value = value * 10 + (c - '0');
        }
        return value;
    };
    const int y = digits(0, 4);
    const int mo = digits(5, 2);
    const int d = digits(8, 2);
    if (y < 0 || mo < 0 || d < 0) return 0;
    int h = 0, mi = 0, se = 0, ms = 0;
    if (in.size() >= 19 && in[10] == 'T') {
        h = digits(11, 2);
        mi = digits(14, 2);
        se = digits(17, 2);
        if (h < 0 || mi < 0 || se < 0) return 0;
        if (in.size() >= 23 && in[19] == '.') {
            ms = digits(20, 3);
            if (ms < 0) ms = 0;
        }
    }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;

    // days_from_civil（Howard Hinnant 算法）：纯整数，无时区依赖。
    const int yy = y - (mo <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const int yoe = yy - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;

    std::int64_t out = days * 86400000LL + (h * 3600LL + mi * 60LL + se) * 1000LL + ms;
    // 时区偏移："+08:00" 表示本地时间比 UTC 早 8 小时，换算成 UTC 要减。
    // 位置不能写死成 19：带小数秒时 19 位是 '.'，时区落在 23 位之后。
    // 原来只看 in[19]，于是 "…T06:12:37.123+08:00" 被当 UTC 解析 ——
    // last_event_ms 落到未来 8 小时，会话停了仍显示"运行中"（issue #2 第 5 条）。
    std::size_t tz = std::string::npos;
    for (std::size_t p = 19; p < in.size(); ++p) {
        if (in[p] == '+' || in[p] == '-') {
            tz = p;
            break;
        }
        if (in[p] == 'Z' || in[p] == 'z') break;  // UTC，无偏移
    }
    if (tz != std::string::npos && tz + 5 < in.size() && in[tz + 3] == ':') {
        const int oh = std::atoi(in.substr(tz + 1, 2).c_str());
        const int om = std::atoi(in.substr(tz + 4, 2).c_str());
        const std::int64_t offset = (oh * 3600LL + om * 60LL) * 1000LL;
        out += (in[tz] == '+') ? -offset : offset;
    }
    return out;
}

// 时间戳字段可能是数字毫秒、数字秒、字符串毫秒、ISO 字符串 —— 四种都见过。
inline std::int64_t TimestampMsOf(const json::Value& v) {
    if (v.IsNumber()) {
        const double n = v.AsDouble(0.0);
        if (n <= 0.0) return 0;
        // 小于 1e11 的是秒（1e11 ms ≈ 1973 年；现代秒级时间戳 ≈ 1.7e9）。
        return n < 1e11 ? static_cast<std::int64_t>(n * 1000.0) : static_cast<std::int64_t>(n);
    }
    if (v.IsString()) {
        const std::string s = v.AsString();
        if (s.empty()) return 0;
        // ISO 日期也以数字开头，必须先按形状分流：'YYYY-MM-DD' 里第 5 位是 '-'。
        // 直接 atof("2024-01-01T...") 会得到 2024，看起来像个 1970 年的秒级时间戳。
        if (s.size() >= 10 && s[4] == '-') return Iso8601ToMs(s);
        if (std::isdigit(static_cast<unsigned char>(s[0])) != 0) {
            const double n = std::atof(s.c_str());
            if (n <= 0.0) return 0;
            return n < 1e11 ? static_cast<std::int64_t>(n * 1000.0) : static_cast<std::int64_t>(n);
        }
        return Iso8601ToMs(s);
    }
    return 0;
}

// cwd 末段即项目名。"C:\\Users\\ow1\\WorkBuddy\\2026-11-12-14-12-37" -> 最后一段。
inline std::string ProjectOf(const std::string& cwd) {
    if (cwd.empty()) return std::string();
    std::size_t end = cwd.size();
    while (end > 0 && (cwd[end - 1] == '\\' || cwd[end - 1] == '/')) --end;
    const std::size_t slash = cwd.find_last_of("\\/", end - 1);
    const std::string base = slash == std::string::npos ? cwd.substr(0, end)
                                                       : cwd.substr(slash + 1, end - slash - 1);
    return base.empty() ? cwd : base;
}

// 有些工具拿时间戳 / uuid 当 workspace 目录名（WorkBuddy 的
// `2026-11-12-14-12-37` 就是）。这种名字放在标题上，用户读不出任何信息，
// 只是把一行宽度浪费掉 —— 判出来之后改用会话标题。
inline bool IsGenericProjectName(const std::string& name) {
    if (name.size() < 8 || name.size() > 48) return false;
    int digits = 0;
    int marks = 0;
    for (const char c : name) {
        if (c >= '0' && c <= '9') {
            ++digits;
        } else if (c == '-' || c == '_' || c == '.' || c == 'T' || c == 'Z' || c == 't' ||
                   // 十六进制字母必须计入：uuid 去掉横线全是 [0-9a-f]，
                   // 原来只认数字与分隔符，含字母的 32 位 uuid 永远判不出来
                   //（issue #2 第 10 条），注释里写的语义是落空的。
                   (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            ++marks;
        }
    }
    if (digits < 6) return false;
    // 全是数字 + 分隔符（时间戳式），或者 32 位十六进制（uuid 去横线）
    return digits + marks == static_cast<int>(name.size());
}

// ---------------------------------------------------------------- 工具 -> 人话
// 用户要的是「它在干什么」，不是工具名。工具名照写在 detail 里保留可追溯性，
// 动词则翻成人话 —— 这一层是产品语感，不该散落在渲染层。
inline std::string ToolVerb(const std::string& tool) {
    const std::string t = AsciiLower(tool);
    if (t == "edit" || t == "multiedit" || t == "notebookedit" || t == "apply_patch" ||
        t == "str_replace" || t == "str_replace_editor" || t == "search_replace") {
        return "正在编辑";
    }
    if (t == "write" || t == "create_file" || t == "write_file") return "正在写入";
    if (t == "read" || t == "view" || t == "read_file" || t == "read_many_files") return "正在阅读";
    if (t == "bash" || t == "shell" || t == "exec_command" || t == "run_command" ||
        t == "run_shell_command" || t == "terminal") {
        return "正在执行";
    }
    if (t == "grep" || t == "glob" || t == "search_files" || t == "search_file_content" ||
        t == "codebase_search" || t == "grep_search") {
        return "正在搜索";
    }
    if (t == "webfetch" || t == "websearch" || t == "web_fetch" || t == "google_web_search") {
        return "正在联网查";
    }
    if (t == "todowrite" || t == "update_plan" || t == "todo_write") return "正在更新任务";
    if (t == "task" || t == "agent" || t == "dispatch_agent") return "正在派子任务";
    if (t == "ls" || t == "list_directory" || t == "list_dir") return "正在看目录";
    return "正在用 " + tool;
}

// 从工具入参里挑出「最像目标」的那一个字段。
// 刻意按工具分派而不是盲扫所有字段：Bash 的 command 与 Edit 的 file_path
// 混在一堆参数里，挑错了就会显示成「正在执行 src/win32」这种废话。
inline std::string ToolTarget(const std::string& tool, const json::Value& input) {
    const std::string t = AsciiLower(tool);
    auto pick = [&input](const char* key) -> std::string {
        const json::Value& v = input.At(key);
        if (v.IsString()) return v.AsString();
        return std::string();
    };
    auto strip_cd = [](std::string s) -> std::string {
        // `cd "C:/proj" && npm test` / `export X=1 && cl.exe ...` 是这个生态里
        // 最常见的命令形状。前面的准备段对「它在干什么」零信息量，
        // 却会占满整行显示宽度 —— 剥掉它们，露出真正的动作。
        //
        // 只剥「看起来像准备命令」的段，而不是无脑取最后一个 && 之后：
        // 命令里嵌的脚本（python -c、heredoc）经常自带 &&，
        // 取最后一段会显示成 `colon + 1 < model.size()) { ...` 这种碎片。
        auto is_setup = [](const std::string& seg) -> bool {
            static const char* kSetup[] = {"cd ", "export ", "set ", "source ", "pushd ",
                                           "echo ", "mkdir "};
            for (const char* p : kSetup) {
                if (seg.compare(0, std::strlen(p), p) == 0) return true;
            }
            return false;
        };
        for (int guard = 0; guard < 4; ++guard) {
            const std::size_t sep = s.find(" && ");
            if (sep == std::string::npos || sep + 4 >= s.size()) break;
            if (!is_setup(s.substr(0, sep))) break;
            s = s.substr(sep + 4);
        }
        if (s.compare(0, 3, "cd ") == 0) {
            const std::size_t nl = s.find(';');
            if (nl != std::string::npos && nl + 1 < s.size()) return s.substr(nl + 1);
        }
        return s;
    };
    if (t == "bash" || t == "shell" || t == "exec_command" || t == "run_command" ||
        t == "run_shell_command" || t == "terminal") {
        const json::Value& cmd = input.At("command");
        if (cmd.IsArray()) {
            // Codex: ["bash","-lc","ls -la"] —— 真正的命令在最后一段。
            for (std::size_t i = cmd.items.size(); i-- > 0;) {
                if (cmd.items[i].IsString() && !cmd.items[i].text.empty()) {
                    return OneLine(strip_cd(cmd.items[i].text), 42);
                }
            }
        }
        const std::string s = pick("command");
        if (!s.empty()) return OneLine(strip_cd(s), 42);
        const std::string c = pick("cmd");
        return OneLine(strip_cd(c), 42);
    }
    if (t == "grep" || t == "glob" || t == "search_files" || t == "search_file_content" ||
        t == "codebase_search" || t == "grep_search" || t == "ls" ||
        t == "list_directory" || t == "list_dir") {
        std::string s = pick("pattern");
        if (s.empty()) s = pick("query");
        if (s.empty()) s = pick("path");
        if (s.empty()) s = pick("glob");
        return OneLine(s, 34);
    }
    if (t == "webfetch" || t == "websearch" || t == "web_fetch" || t == "google_web_search") {
        std::string s = pick("url");
        if (s.empty()) s = pick("query");
        if (s.find("://") != std::string::npos) {
            const std::size_t scheme = s.find("://");
            s = s.substr(scheme + 3);
            while (!s.empty() && s.back() == '/') s.pop_back();
        }
        return OneLine(s, 38);
    }
    // 文件类工具（Edit / Write / Read）优先 file_path，其次 path。
    std::string s = pick("file_path");
    if (s.empty()) s = pick("path");
    if (s.empty()) s = pick("notebook_path");
    if (s.empty()) s = pick("file");
    if (!s.empty()) {
        // 长路径只留「父目录/文件名」两段 —— 32 段的绝对路径在 42px 里毫无意义。
        std::string norm = s;
        std::replace(norm.begin(), norm.end(), '\\', '/');
        const std::vector<std::string> parts = [&norm] {
            std::vector<std::string> out;
            std::size_t start = 0;
            while (start <= norm.size()) {
                const std::size_t slash = norm.find('/', start);
                const std::string part =
                    norm.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                if (!part.empty()) out.push_back(part);
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
            return out;
        }();
        if (parts.size() >= 2) return EllipsizeUtf8(parts[parts.size() - 2] + "/" + parts.back(), 34);
        return EllipsizeUtf8(norm, 34);
    }
    const std::string desc = pick("description");
    return OneLine(desc, 34);
}

// 把 TodoWrite / update_plan 的数组解析成清单。两家的字段名不同：
//   TodoWrite:   [{content, status, activeForm}]
//   update_plan: [{step, status}]
inline std::vector<AgentTodo> ParseTodos(const json::Value& array) {
    std::vector<AgentTodo> out;
    if (!array.IsArray()) return out;
    for (const auto& item : array.items) {
        if (!item.IsObject()) continue;
        AgentTodo todo;
        todo.text = item.At("content").AsString();
        if (todo.text.empty()) todo.text = item.At("step").AsString();
        if (todo.text.empty()) todo.text = item.At("activeForm").AsString();
        todo.status = item.At("status").AsString("pending");
        if (todo.text.empty()) continue;
        out.push_back(todo);
        if (out.size() >= 24) break;  // 长清单只关心前 24 条，多出来的不会显示
    }
    return out;
}

// ---------------------------------------------------------------- 逐行解析
// 返回 false 表示这一行不该产生事件（空行、快照行、坏行）。
// 注意：**坏行不是错误**。会话文件写到一半被读到、版本升级改了字段，
// 都会产生解析不了的行；跳过它们继续读下一行，界面上最多晚 800ms。
inline void ParseClaudeLine(const json::Value& root, std::vector<AgentEvent>* out) {
    const std::string type = root.At("type").AsString();
    AgentEvent base;
    base.ts_ms = TimestampMsOf(root.At("timestamp"));
    base.cwd = root.At("cwd").AsString();
    base.session_id = root.At("sessionId").AsString();

    if (type == "summary") {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Meta;
        ev.title = root.At("summary").AsString();
        out->push_back(ev);
        return;
    }
    if (type == "file-history-snapshot" || type == "system" || type.empty()) {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Activity;
        out->push_back(ev);
        return;
    }
    // WorkBuddy / CodeBuddy 把工具调用摊在顶层：type=function_call + name + arguments。
    // 它没有 message.content 数组，走不到下面的分支，必须单独认。
    if (type == "function_call") {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Tool;
        ev.tool = root.At("name").AsString();
        const json::Value& args = root.At("arguments");
        json::Value parsed;
        const json::Value* use = &args;
        if (args.IsString() && json::Parse(args.AsString(), parsed, nullptr)) use = &parsed;
        ev.detail = ToolTarget(ev.tool, *use);
        if (ev.tool == "TodoWrite" || ev.tool == "update_plan" || ev.tool == "todo_write") {
            const json::Value& todos = use->At("todos").IsArray() ? use->At("todos") : use->At("plan");
            ev.todos = ParseTodos(todos);
            if (!ev.todos.empty()) ev.kind = AgentEvent::Kind::Todo;
        }
        const json::Value& usage = root.At("message").At("usage");
        ev.ctx_tokens = static_cast<std::int64_t>(usage.At("total_tokens").AsDouble(0));
        out->push_back(ev);
        return;
    }
    if (type == "function_call_result" || type == "reasoning" || type == "file-history-snapshot") {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Activity;
        out->push_back(ev);
        return;
    }
    if (type == "ai-title") {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Meta;
        ev.title = root.At("aiTitle").AsString();
        out->push_back(ev);
        return;
    }
    // Claude Code: assistant / user 消息体在 message.content 里；
    // WorkBuddy 把它摊平在顶层（role + content），两家共用一个解析分支。
    const json::Value& message = root.At("message").IsObject() ? root.At("message") : root;
    const std::string role = message.At("role").AsString();
    const json::Value& usage = message.At("usage");
    std::int64_t ctx = 0;
    if (usage.IsObject()) {
        // 上下文规模 = 输入 + 缓存读取。这不是「一共花了多少 token」，
        // 而是「它现在脑子里装了多少」——后者才是用户能对上号的数字。
        ctx = static_cast<std::int64_t>(usage.At("input_tokens").AsDouble(0)) +
              static_cast<std::int64_t>(usage.At("cache_read_input_tokens").AsDouble(0)) +
              static_cast<std::int64_t>(usage.At("prompt_tokens").AsDouble(0));
    }

    AgentEvent model_ev = base;
    model_ev.kind = AgentEvent::Kind::Meta;
    model_ev.model = message.At("model").AsString();
    if (model_ev.model.empty()) model_ev.model = root.At("providerData").At("requestModelName").AsString();
    if (!model_ev.model.empty()) out->push_back(model_ev);

    const json::Value& content = message.At("content");
    auto emit_text = [&](const std::string& text) {
        if (text.empty()) return;
        AgentEvent ev = base;
        if (role == "user" || role == "human") {
            ev.kind = AgentEvent::Kind::Prompt;
        } else {
            ev.kind = AgentEvent::Kind::Reply;
        }
        ev.text = text;
        ev.ctx_tokens = ctx;
        out->push_back(ev);
    };

    if (content.IsString()) {
        emit_text(content.AsString());
        return;
    }
    if (content.IsArray()) {
        for (const auto& block : content.items) {
            if (!block.IsObject()) continue;
            const std::string btype = block.At("type").AsString();
            if (btype == "text" || btype == "input_text" || btype == "output_text") {
                emit_text(block.At("text").AsString());
                continue;
            }
            if (btype == "tool_use" || btype == "function_call") {
                AgentEvent ev = base;
                ev.kind = AgentEvent::Kind::Tool;
                ev.tool = block.At("name").AsString();
                const json::Value& input = block.At("input").IsObject() ? block.At("input")
                                                                        : block.At("arguments");
                const json::Value* use = &input;
                json::Value parsed;
                if (input.IsString()) {
                    // WorkBuddy 的 arguments 是一段 JSON 字符串，得再解一层。
                    if (json::Parse(input.AsString(), parsed, nullptr)) use = &parsed;
                }
                ev.detail = ToolTarget(ev.tool, *use);
                if (ev.tool == "TodoWrite" || ev.tool == "update_plan" || ev.tool == "todo_write") {
                    const json::Value& todos =
                        use->At("todos").IsArray() ? use->At("todos") : use->At("plan");
                    ev.todos = ParseTodos(todos);
                    if (!ev.todos.empty()) ev.kind = AgentEvent::Kind::Todo;
                }
                out->push_back(ev);
                continue;
            }
            if (btype == "tool_result" || btype == "function_call_result") {
                AgentEvent ev = base;
                ev.kind = AgentEvent::Kind::Activity;
                out->push_back(ev);
            }
        }
        return;
    }
}

// Pi（`~/.pi/agent/sessions/--<cwd>--/<时间戳>_<uuid>.jsonl`）。
// 结构比 Claude 更规整：`message` 行里 role + content[]，工具调用是
// `{type:"toolCall", name, arguments:{...}}` —— arguments 已经是对象，
// 不用像 WorkBuddy 那样再解一层字符串。
inline void ParsePiLine(const json::Value& root, std::vector<AgentEvent>* out) {
    const std::string type = root.At("type").AsString();
    AgentEvent base;
    base.ts_ms = TimestampMsOf(root.At("timestamp"));
    base.session_id = root.At("id").AsString();

    if (type == "session") {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Meta;
        ev.cwd = root.At("cwd").AsString();
        out->push_back(ev);
        return;
    }
    if (type == "model_change") {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Meta;
        ev.model = root.At("modelId").AsString();
        out->push_back(ev);
        return;
    }
    if (type != "message") {
        // thinking_level_change 之类：算「有动静」，但不产生文案
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Activity;
        out->push_back(ev);
        return;
    }

    const json::Value& msg = root.At("message");
    const std::string role = msg.At("role").AsString();
    // message 自带毫秒时间戳，比外层的 ISO 更精确（两者都见过，取有值的）
    const std::int64_t inner_ts = TimestampMsOf(msg.At("timestamp"));
    if (inner_ts > 0) base.ts_ms = inner_ts;
    if (!msg.At("modelId").AsString().empty()) {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Meta;
        ev.model = msg.At("modelId").AsString();
        out->push_back(ev);
    }

    if (role == "toolResult") {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Activity;
        out->push_back(ev);
        return;
    }
    const json::Value& content = msg.At("content");
    if (content.IsString()) {
        AgentEvent ev = base;
        ev.kind = role == "user" ? AgentEvent::Kind::Prompt : AgentEvent::Kind::Reply;
        ev.text = content.AsString();
        out->push_back(ev);
        return;
    }
    for (const auto& block : content.items) {
        if (!block.IsObject()) continue;
        const std::string btype = block.At("type").AsString();
        if (btype == "text") {
            const std::string text = block.At("text").AsString();
            if (text.empty()) continue;
            AgentEvent ev = base;
            ev.kind = role == "user" ? AgentEvent::Kind::Prompt : AgentEvent::Kind::Reply;
            ev.text = text;
            out->push_back(ev);
        } else if (btype == "toolCall") {
            AgentEvent ev = base;
            ev.kind = AgentEvent::Kind::Tool;
            ev.tool = block.At("name").AsString();
            ev.detail = ToolTarget(ev.tool, block.At("arguments"));
            out->push_back(ev);
        } else if (btype == "thinking") {
            AgentEvent ev = base;
            ev.kind = AgentEvent::Kind::Activity;
            out->push_back(ev);
        }
    }
}

inline void ParseCodexLine(const json::Value& root, std::vector<AgentEvent>* out) {
    const std::string type = root.At("type").AsString();
    AgentEvent base;
    base.ts_ms = TimestampMsOf(root.At("timestamp"));
    const json::Value& payload = root.At("payload");

    if (type == "session_meta") {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Meta;
        ev.cwd = payload.At("cwd").AsString();
        ev.session_id = payload.At("id").AsString();
        out->push_back(ev);
        return;
    }
    if (type == "turn_context") {
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Meta;
        ev.cwd = payload.At("cwd").AsString();
        ev.model = payload.At("model").AsString();
        out->push_back(ev);
        return;
    }
    if (type == "event_msg") {
        const std::string ptype = payload.At("type").AsString();
        AgentEvent ev = base;
        if (ptype == "agent_message") {
            ev.kind = AgentEvent::Kind::Reply;
            ev.text = payload.At("message").AsString();
        } else if (ptype == "user_message") {
            ev.kind = AgentEvent::Kind::Prompt;
            ev.text = payload.At("message").AsString();
        } else {
            ev.kind = AgentEvent::Kind::Activity;
        }
        out->push_back(ev);
        return;
    }
    if (type == "response_item") {
        const std::string ptype = payload.At("type").AsString();
        if (ptype == "function_call") {
            AgentEvent ev = base;
            ev.kind = AgentEvent::Kind::Tool;
            ev.tool = payload.At("name").AsString();
            json::Value parsed;
            if (json::Parse(payload.At("arguments").AsString(), parsed, nullptr)) {
                ev.detail = ToolTarget(ev.tool, parsed);
                if (ev.tool == "update_plan" || ev.tool == "TodoWrite") {
                    ev.todos = ParseTodos(parsed.At("plan").IsArray() ? parsed.At("plan")
                                                                      : parsed.At("todos"));
                    if (!ev.todos.empty()) ev.kind = AgentEvent::Kind::Todo;
                }
            }
            out->push_back(ev);
            return;
        }
        if (ptype == "message") {
            const std::string role = payload.At("role").AsString();
            std::string text;
            for (const auto& c : payload.At("content").items) {
                const std::string t = c.At("text").AsString();
                if (!t.empty()) text += t;
            }
            AgentEvent ev = base;
            ev.kind = role == "user" ? AgentEvent::Kind::Prompt : AgentEvent::Kind::Reply;
            ev.text = text;
            out->push_back(ev);
            return;
        }
        AgentEvent ev = base;
        ev.kind = AgentEvent::Kind::Activity;
        out->push_back(ev);
    }
}

// Gemini CLI 的会话是**整个文件一个 JSON**（chats/session-*.json），不是 JSONL。
// 所以这里吃的是整段文本，一次性产出全部事件。
inline std::vector<AgentEvent> ParseGeminiSession(const std::string& body) {
    std::vector<AgentEvent> out;
    json::Value root;
    if (!json::Parse(body, root, nullptr)) return out;
    AgentEvent meta;
    meta.kind = AgentEvent::Kind::Meta;
    meta.session_id = root.At("sessionId").AsString();
    meta.ts_ms = Iso8601ToMs(root.At("startTime").AsString());
    if (!meta.session_id.empty()) out.push_back(meta);

    for (const auto& msg : root.At("messages").items) {
        const std::string type = msg.At("type").AsString();
        AgentEvent ev;
        ev.ts_ms = TimestampMsOf(msg.At("timestamp"));
        const json::Value& content = msg.At("content");
        std::string text;
        if (content.IsString()) {
            text = content.AsString();
        } else {
            for (const auto& part : content.items) text += part.At("text").AsString();
        }
        if (type == "user") {
            ev.kind = AgentEvent::Kind::Prompt;
            ev.text = text;
            out.push_back(ev);
        } else if (!text.empty()) {
            ev.kind = AgentEvent::Kind::Reply;
            ev.text = text;
            out.push_back(ev);
        }
        for (const auto& call : msg.At("toolCalls").items) {
            AgentEvent tc;
            tc.ts_ms = ev.ts_ms;
            tc.kind = AgentEvent::Kind::Tool;
            tc.tool = call.At("name").AsString();
            const json::Value& args = call.At("args");
            json::Value parsed;
            const json::Value* use = &args;
            if (args.IsString() && json::Parse(args.AsString(), parsed, nullptr)) use = &parsed;
            tc.detail = ToolTarget(tc.tool, *use);
            out.push_back(tc);
        }
    }
    return out;
}

// 统一入口：一行 -> 0..N 个事件。
inline void ParseAgentJsonLine(const std::string& provider, const std::string& line,
                               std::vector<AgentEvent>* out) {
    if (line.size() < 2) return;
    // 便宜的预筛：会话文件里混着大量不含 "type" 的行（空行、续行），
    // 先字符串挡掉，避免为它们启动 JSON 解析器。
    if (line.find("\"type\"") == std::string::npos && line.find("\"role\"") == std::string::npos) return;
    json::Value root;
    if (!json::Parse(line, root, nullptr) || !root.IsObject()) return;
    if (provider == "codex") {
        ParseCodexLine(root, out);
    } else if (provider == "pi") {
        ParsePiLine(root, out);
    } else {
        ParseClaudeLine(root, out);  // claude / workbuddy 同构
    }
}

// ---------------------------------------------------------------- 事件 -> 会话

// 判定「这条事件是不是模型自己写的开场白」。Claude Code / Codex 会把
// 系统提示、文件内容、技能清单当成 user 消息塞进会话文件，
// 直接拿来当「用户让它干什么」会得到一整段无关文本。
inline bool LooksLikeSystemText(const std::string& text) {
    if (text.empty()) return true;
    if (text[0] == '<') return true;  // <system-reminder> / <user_info> / <env>
    if (ContainsCaseless(text, "system-reminder")) return true;
    if (ContainsCaseless(text, "caveat: the messages below")) return true;
    return false;
}

inline void PushTimeline(AgentSession* s, const std::string& item) {
    if (item.empty()) return;
    // 相邻重复没有信息量（连着读同一个文件很常见）；隔一次再出现的重复
    // 同样没意义（改文件 → 跑测试 → 又改同一个文件），所以整表去重后前置。
    for (std::size_t i = s->timeline.size(); i-- > 0;) {
        if (s->timeline[i] == item) s->timeline.erase(s->timeline.begin() + static_cast<std::ptrdiff_t>(i));
    }
    s->timeline.insert(s->timeline.begin(), item);
    if (s->timeline.size() > 4) s->timeline.resize(4);
}

inline void ApplyAgentEvent(AgentSession* s, const AgentEvent& ev) {
    if (!ev.cwd.empty()) {
        s->cwd = ev.cwd;
        s->project = ProjectOf(ev.cwd);
    }
    if (!ev.model.empty()) s->model = ev.model;
    if (!ev.session_id.empty() && s->session_id.empty()) s->session_id = ev.session_id;
    if (ev.ts_ms > 0) {
        if (s->started_ms == 0) s->started_ms = ev.ts_ms;
        s->last_event_ms = (std::max)(s->last_event_ms, ev.ts_ms);
    }
    if (ev.ctx_tokens > 0) s->ctx_tokens = (std::max)(s->ctx_tokens, ev.ctx_tokens);

    switch (ev.kind) {
        case AgentEvent::Kind::None:
            break;
        case AgentEvent::Kind::Activity:
            break;
        case AgentEvent::Kind::Meta:
            // 会话自带的标题比首条 prompt 准确得多（它已经是「一句话概括」）。
            if (!ev.title.empty()) s->task = OneLine(ev.title, 56);
            break;
        case AgentEvent::Kind::Prompt:
            if (s->task.empty() && !LooksLikeSystemText(ev.text)) {
                s->task = OneLine(ev.text, 56);
            }
            break;
        case AgentEvent::Kind::Reply:
            if (!ev.text.empty() && !LooksLikeSystemText(ev.text)) {
                s->last_text = OneLine(ev.text, 80);
            }
            break;
        case AgentEvent::Kind::Tool: {
            ++s->tool_calls;
            const std::string verb = ToolVerb(ev.tool);
            s->action = ev.detail.empty() ? verb : (verb + " " + ev.detail);
            s->detail = ev.detail.empty() ? ev.tool : (ev.tool + " · " + ev.detail);
            PushTimeline(s, ev.detail.empty() ? ev.tool : (ev.tool + "  " + ev.detail));
            break;
        }
        case AgentEvent::Kind::Todo: {
            ++s->tool_calls;
            s->todos = ev.todos;
            s->done_count = 0;
            for (const auto& t : s->todos) {
                if (t.Done()) ++s->done_count;
            }
            s->todo_count = static_cast<int>(s->todos.size());
            // 清单本身就是进度，不需要再补一句动作 —— 但要让「最新动作」
            // 反映「刚更新了任务」，否则大岛顶部还停在改代码上。
            for (const auto& t : s->todos) {
                if (t.Active()) {
                    s->action = "正在做 " + EllipsizeUtf8(t.text, 34);
                    break;
                }
            }
            break;
        }
    }
}

// 状态判定。单独一层是因为它完全由「现在」和「最后一次写入」决定，
// 和解析无关；采集层每帧都会问一次，必须便宜且可测。
//   running: 最近一段时间还有新内容落盘 —— 它确实在跑
//   waiting: 安静下来了，但还没到该退场的程度 —— 多半在等权限确认 / 等下一句
//   done:    长时间没动静，采集层会把这条从岛上撤掉
//
// 「跑」的窗口是 45 秒而不是几秒：模型推理时不写文件（一次思考动辄 20-40 秒），
// 窗口太短会让岛在它正忙的时候变成「等待输入」，然后把主位让给音乐 ——
// 用户看到的是「它明明在跑，岛却说它在等我」。宁可慢 45 秒才承认它停了。
inline const char* AgentStateAt(const AgentSession& s, std::int64_t now_ms,
                                std::int64_t idle_hide_ms = 8 * 60 * 1000) {
    constexpr std::int64_t kRunningWindowMs = 45000;
    if (now_ms <= 0 || s.last_event_ms <= 0) return "waiting";
    const std::int64_t quiet = now_ms - s.last_event_ms;
    if (quiet <= kRunningWindowMs) return "running";
    if (quiet <= idle_hide_ms) return "waiting";
    return "done";
}

// 会话已经跑多久。超过一小时的用 "9h44" 而不是 "9:44"：
// 后者和媒体进度条上的「分:秒」长得一模一样，一眼看过去会以为只跑了 9 分。
inline std::string AgentElapsedText(const AgentSession& s, std::int64_t now_ms) {
    if (s.started_ms <= 0 || now_ms <= s.started_ms) return std::string();
    const std::int64_t sec = (now_ms - s.started_ms) / 1000;
    char buf[32];
    if (sec >= 3600) {
        std::snprintf(buf, sizeof(buf), "%lldh%02lld", static_cast<long long>(sec / 3600),
                      static_cast<long long>((sec % 3600) / 60));
    } else {
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld", static_cast<long long>(sec / 60),
                      static_cast<long long>(sec % 60));
    }
    return buf;
}

// 上下文规模：48k / 1.2m。数字太大时精度没有意义。
inline std::string TokenText(std::int64_t tokens) {
    if (tokens <= 0) return std::string();
    char buf[24];
    if (tokens >= 1000000) {
        std::snprintf(buf, sizeof(buf), "%.1fm", static_cast<double>(tokens) / 1000000.0);
    } else if (tokens >= 1000) {
        std::snprintf(buf, sizeof(buf), "%lldk", static_cast<long long>(tokens / 1000));
    } else {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(tokens));
    }
    return buf;
}

}  // namespace diw

#endif  // DIW_SRC_CORE_AGENT_SESSION_HPP
