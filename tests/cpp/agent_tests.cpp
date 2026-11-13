// Agent 会话解析回归：四家会话格式（Claude Code / WorkBuddy / Codex CLI / Gemini CLI）
// 归一化成同一套事件，并断言「它现在在干什么」这类产品级输出。
// 这里只测纯逻辑（字符串 -> 事件 -> 会话），文件发现与增量读取在 agent_probe 里验。
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/core/agent_session.hpp"

using diw::AgentEvent;
using diw::AgentSession;
using diw::AgentStateAt;
using diw::AgentTodo;
using diw::ApplyAgentEvent;
using diw::EllipsizeUtf8;
using diw::Iso8601ToMs;
using diw::LooksLikeSystemText;
using diw::OneLine;
using diw::ParseAgentJsonLine;
using diw::ParseGeminiSession;
using diw::ProjectOf;
using diw::TokenText;
using diw::ToolTarget;
using diw::ToolVerb;

static int g_run = 0;
static int g_failed = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        ++g_run;                                                \
        if (!(expr)) {                                          \
            ++g_failed;                                         \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
        }                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                                        \
    do {                                                                                      \
        ++g_run;                                                                              \
        const std::string lhs = (a);                                                          \
        const std::string rhs = (b);                                                          \
        if (lhs != rhs) {                                                                     \
            ++g_failed;                                                                       \
            std::printf("FAIL line %d: %s == %s  (got \"%s\", want \"%s\")\n", __LINE__, #a,   \
                        #b, lhs.c_str(), rhs.c_str());                                        \
        }                                                                                     \
    } while (0)

// 喂一整块 JSONL（模拟一次 tail 读取），返回累积后的会话。
static AgentSession Feed(const std::string& provider, const std::string& blob) {
    AgentSession s;
    s.provider = provider;
    s.session_id = "fixture";
    std::size_t start = 0;
    while (start <= blob.size()) {
        const std::size_t nl = blob.find('\n', start);
        const std::string line = blob.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::vector<AgentEvent> events;
        ParseAgentJsonLine(provider, line, &events);
        for (const auto& ev : events) ApplyAgentEvent(&s, ev);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return s;
}

void TestIso8601() {
    // 2024-01-01T00:00:00Z = 1704067200000
    CHECK(Iso8601ToMs("2024-01-01T00:00:00.000Z") == 1704067200000LL);
    CHECK(Iso8601ToMs("2024-01-01T00:00:00Z") == 1704067200000LL);
    // 带 +08:00 的同一时刻，字符串写法不同，结果必须相同。
    CHECK(Iso8601ToMs("2024-01-01T08:00:00+08:00") == 1704067200000LL);
    CHECK(Iso8601ToMs("2024-01-01") == 1704067200000LL);
    CHECK(Iso8601ToMs("") == 0);
    CHECK(Iso8601ToMs("not-a-date") == 0);
    // 闰年 2 月 29 日必须能算
    CHECK(Iso8601ToMs("2024-02-29T12:00:00Z") > 0);
}

void TestTextHelpers() {
    CHECK_EQ(EllipsizeUtf8("abcdef", 6), "abcdef");   // 正好等于上限，不加尾巴
    CHECK_EQ(EllipsizeUtf8("abcdefg", 6), "abcdef…");
    CHECK_EQ(EllipsizeUtf8("中文测试", 2), "中文…");
    CHECK_EQ(EllipsizeUtf8("中文测试", 4), "中文测试");
    CHECK_EQ(EllipsizeUtf8("", 3), "");
    // 截断点必须落在字符边界：结果里不能出现半个汉字的残字节。
    const std::string cut = EllipsizeUtf8("岛上的 Agent", 3);
    CHECK_EQ(cut, "岛上的…");

    CHECK_EQ(OneLine("npm run\n  build", 40), "npm run build");
    CHECK_EQ(OneLine("Command: ls -la", 40), "ls -la");
    CHECK_EQ(ProjectOf("C:\\Users\\ow1\\WorkBuddy\\demo"), "demo");
    CHECK_EQ(ProjectOf("/home/u/proj/"), "proj");
    CHECK_EQ(ProjectOf("proj"), "proj");
    CHECK_EQ(ProjectOf(""), "");

    // 时间戳式目录名对用户没有信息量，会被换成会话标题（见 live_mapping）。
    CHECK(diw::IsGenericProjectName("2026-11-12-14-12-37"));
    CHECK(diw::IsGenericProjectName("20261112"));
    CHECK(!diw::IsGenericProjectName("DynamicIsland-Win"));
    CHECK(!diw::IsGenericProjectName("api-server"));
    CHECK(!diw::IsGenericProjectName(""));

    CHECK_EQ(ToolVerb("Edit"), "正在编辑");
    CHECK_EQ(ToolVerb("Bash"), "正在执行");
    CHECK_EQ(ToolVerb("grep"), "正在搜索");
    CHECK_EQ(ToolVerb("WebFetch"), "正在联网查");
    CHECK_EQ(ToolVerb("TodoWrite"), "正在更新任务");
    CHECK(ToolVerb("SomeNewTool").find("SomeNewTool") != std::string::npos);

    CHECK(TokenText(0).empty());
    CHECK_EQ(TokenText(800), "800");
    CHECK_EQ(TokenText(48999), "48k");
    CHECK_EQ(TokenText(1250000), "1.2m");

    CHECK(LooksLikeSystemText("<system-reminder>x"));
    CHECK(LooksLikeSystemText("Caveat: The messages below were generated"));
    CHECK(!LooksLikeSystemText("帮我改一下渲染层"));
    CHECK(LooksLikeSystemText(""));
}

void TestToolTarget() {
    diw::json::Value input;
    CHECK(diw::json::Parse(R"({"file_path":"C:\\proj\\src\\win32\\renderer.hpp"})", input, nullptr));
    CHECK_EQ(ToolTarget("Edit", input), "win32/renderer.hpp");

    diw::json::Value shell;
    CHECK(diw::json::Parse(R"({"command":["bash","-lc","npm test -- --watch=false"]})", shell, nullptr));
    CHECK_EQ(ToolTarget("shell", shell), "npm test -- --watch=false");

    diw::json::Value grep;
    CHECK(diw::json::Parse(R"({"pattern":"BoxBlurAxis","path":"src"})", grep, nullptr));
    CHECK_EQ(ToolTarget("Grep", grep), "BoxBlurAxis");

    diw::json::Value url;
    CHECK(diw::json::Parse(R"({"url":"https://github.com/Liset999/ZenDesktop/"})", url, nullptr));
    CHECK_EQ(ToolTarget("WebFetch", url), "github.com/Liset999/ZenDesktop");

    // 环境准备段不该占满整行：`cd ... && 真命令` 只显示后半段。
    diw::json::Value cd;
    CHECK(diw::json::Parse(R"({"command":"cd \"C:/p/demo\" && npm test"})", cd, nullptr));
    CHECK_EQ(ToolTarget("Bash", cd), "npm test");
    diw::json::Value plain;
    CHECK(diw::json::Parse(R"({"command":"git status"})", plain, nullptr));
    CHECK_EQ(ToolTarget("Bash", plain), "git status");

    // 环境准备段可以有多层，全部剥掉。
    diw::json::Value env;
    CHECK(diw::json::Parse(R"({"command":"export A=1 && export B=2 && ctest -V"})", env, nullptr));
    CHECK_EQ(ToolTarget("Bash", env), "ctest -V");

    // 命令里嵌的脚本自带 &&：不能把后半截当成「真正的命令」。
    diw::json::Value script;
    CHECK(diw::json::Parse(R"({"command":"python -c \"if a && b: pass\""})", script, nullptr));
    CHECK(ToolTarget("Bash", script).find("python -c") == 0);
}

void TestClaude() {
    const std::string blob = R"({"type":"summary","summary":"修岛不显示的问题"})"
                             "\n"
                             R"({"type":"user","sessionId":"s1","cwd":"C:\\proj\\DynamicIsland-Win","message":{"role":"user","content":"<system-reminder>环境信息</system-reminder>"},"timestamp":"2024-01-01T00:00:00.000Z"})"
                             "\n"
                             R"({"type":"user","sessionId":"s1","cwd":"C:\\proj\\DynamicIsland-Win","message":{"role":"user","content":"给岛加一个 agent 面板"},"timestamp":"2024-01-01T00:00:01.000Z"})"
                             "\n"
                             R"({"type":"assistant","cwd":"C:\\proj\\DynamicIsland-Win","message":{"model":"claude-sonnet-4-5","usage":{"input_tokens":12000,"output_tokens":300},"content":[{"type":"text","text":"好的，我先看渲染层"},{"type":"tool_use","name":"Read","input":{"file_path":"C:\\proj\\DynamicIsland-Win\\src\\win32\\renderer.hpp"}}]},"timestamp":"2024-01-01T00:00:05.000Z"})"
                             "\n"
                             R"({"type":"assistant","cwd":"C:\\proj\\DynamicIsland-Win","message":{"model":"claude-sonnet-4-5","content":[{"type":"tool_use","name":"Edit","input":{"file_path":"C:\\proj\\DynamicIsland-Win\\src\\win32\\renderer.hpp"}}]},"timestamp":"2024-01-01T00:00:09.000Z"})";

    const AgentSession s = Feed("claude", blob);
    CHECK(s.Present());
    CHECK_EQ(s.project, "DynamicIsland-Win");
    CHECK_EQ(s.task, "修岛不显示的问题");  // summary 优先于首条 prompt
    CHECK_EQ(s.model, "claude-sonnet-4-5");
    CHECK_EQ(s.action, "正在编辑 win32/renderer.hpp");
    CHECK_EQ(s.detail, "Edit · win32/renderer.hpp");
    CHECK(s.tool_calls == 2);
    CHECK(s.ctx_tokens == 12000);
    CHECK(s.started_ms == 1704067200000LL);
    CHECK(s.last_event_ms == 1704067209000LL);
    CHECK(s.timeline.size() == 2);
    // 时间线最新在前
    CHECK(s.timeline[0].find("Edit") == 0);
    CHECK(s.last_text == "好的，我先看渲染层");

    // 没有 summary 时，首条真人输入当任务描述，system-reminder 必须被跳过。
    const std::string no_summary = R"({"type":"user","cwd":"C:\\p\\demo","message":{"role":"user","content":"<system-reminder>ignore me</system-reminder>"},"timestamp":"2024-01-01T00:00:00Z"})"
                                   "\n"
                                   R"({"type":"user","cwd":"C:\\p\\demo","message":{"role":"user","content":"重构一下规则引擎"},"timestamp":"2024-01-01T00:00:01Z"})";
    const AgentSession s2 = Feed("claude", no_summary);
    CHECK_EQ(s2.task, "重构一下规则引擎");
}

void TestClaudeTodos() {
    const std::string blob =
        R"({"type":"assistant","cwd":"C:\\p\\app","message":{"content":[{"type":"tool_use","name":"TodoWrite","input":{"todos":[{"content":"梳理事件源","status":"completed"},{"content":"写映射层","status":"completed"},{"content":"画 agent 面板","status":"in_progress"},{"content":"补回归测试","status":"pending"}]}}]},"timestamp":"2024-01-01T00:10:00Z"})";
    const AgentSession s = Feed("claude", blob);
    CHECK(s.todo_count == 4);
    CHECK(s.done_count == 2);
    CHECK_EQ(s.todos[2].status, "in_progress");
    CHECK(s.todos[2].Active());
    CHECK(s.todos[0].Done());
    // 最新动作应当跟着「进行中」那条走，而不是停在 TodoWrite 本身。
    CHECK_EQ(s.action, "正在做 画 agent 面板");

    // 清单被后续调用覆盖（agent 会反复 update）
    const std::string more =
        R"({"type":"assistant","cwd":"C:\\p\\app","message":{"content":[{"type":"tool_use","name":"TodoWrite","input":{"todos":[{"content":"a","status":"completed"},{"content":"b","status":"completed"},{"content":"c","status":"completed"}]}}]},"timestamp":"2024-01-01T00:12:00Z"})";
    const AgentSession s2 = Feed("claude", more);
    CHECK(s2.todo_count == 3);
    CHECK(s2.done_count == 3);
}

void TestWorkbuddy() {
    // 本机真实格式：工具调用摊平在顶层，arguments 是「JSON 字符串」。
    const std::string blob =
        R"({"type":"message","role":"user","sessionId":"w1","cwd":"c:\\Users\\ow1\\WorkBuddy\\demo","content":[{"type":"input_text","text":"把托盘图标做了"}],"timestamp":1794463960583})"
        "\n"
        R"({"type":"ai-title","aiTitle":"实现托盘图标与自启","sessionId":"w1","cwd":"c:\\Users\\ow1\\WorkBuddy\\demo","timestamp":1794463961000})"
        "\n"
        R"({"type":"reasoning","content":[],"sessionId":"w1","cwd":"c:\\Users\\ow1\\WorkBuddy\\demo","timestamp":1794463962000})"
        "\n"
        R"({"type":"function_call","name":"Bash","arguments":"{\"command\":\"cmake --build build\"}","sessionId":"w1","cwd":"c:\\Users\\ow1\\WorkBuddy\\demo","message":{"usage":{"total_tokens":51320}},"timestamp":"1794463965000"})"
        "\n"
        R"({"type":"function_call_result","name":"Bash","status":"completed","sessionId":"w1","cwd":"c:\\Users\\ow1\\WorkBuddy\\demo","timestamp":1794463970000})";

    const AgentSession s = Feed("workbuddy", blob);
    CHECK(s.Present());
    CHECK_EQ(s.project, "demo");
    CHECK_EQ(s.task, "实现托盘图标与自启");
    CHECK_EQ(s.action, "正在执行 cmake --build build");
    CHECK(s.tool_calls == 1);
    CHECK(s.ctx_tokens == 51320);
    CHECK(s.last_event_ms == 1794463970000LL);  // function_call_result 也推进活跃时间
}

void TestPi() {
    const std::string blob =
        R"({"type":"session","version":3,"id":"p1","timestamp":"2024-01-01T00:00:00.000Z","cwd":"C:////Users////ow1////Desktop"})"
        "\n"
        R"({"type":"model_change","id":"m1","timestamp":"2024-01-01T00:00:01.000Z","provider":"deepseek","modelId":"deepseek-v4-pro"})"
        "\n"
        R"({"type":"thinking_level_change","id":"t1","timestamp":"2024-01-01T00:00:02.000Z","thinkingLevel":"high"})"
        "\n"
        R"({"type":"message","id":"u1","timestamp":"2024-01-01T00:00:03.000Z","message":{"role":"user","content":[{"type":"text","text":"做个贪吃蛇"}],"timestamp":1704067203000}})"
        "\n"
        R"({"type":"message","id":"a1","timestamp":"2024-01-01T00:00:04.000Z","message":{"role":"assistant","content":[{"type":"thinking","thinking":"写文件"},{"type":"text","text":"我来写一个"},{"type":"toolCall","id":"c1","name":"write","arguments":{"path":"C:/Users/ow1/Desktop/snake.html"}}],"timestamp":1704067204000}})"
        "\n"
        R"({"type":"message","id":"r1","timestamp":"2024-01-01T00:00:05.000Z","message":{"role":"toolResult","toolName":"write","content":[{"type":"text","text":"ok"}],"timestamp":1704067205000}})";

    const AgentSession s = Feed("pi", blob);
    CHECK(s.Present());
    // session_id 在真实链路里由首行 session 事件填（Feed 预置了一个占位值，
    // 不会覆盖，所以这里只验证它没被写坏）。
    CHECK(!s.session_id.empty());
    CHECK_EQ(s.project, "Desktop");
    CHECK_EQ(s.model, "deepseek-v4-pro");
    CHECK_EQ(s.task, "做个贪吃蛇");
    CHECK_EQ(s.action, "正在写入 Desktop/snake.html");
    CHECK_EQ(s.last_text, "我来写一个");
    CHECK(s.tool_calls == 1);
    // toolResult 也算「有动静」，时间要跟着往后走
    CHECK(s.last_event_ms == 1704067205000LL);
}

void TestCodex() {
    const std::string blob =
        R"({"timestamp":"2024-01-01T00:00:00.000Z","type":"session_meta","payload":{"id":"c1","cwd":"D:\\work\\island"}})"
        "\n"
        R"({"timestamp":"2024-01-01T00:00:01.000Z","type":"turn_context","payload":{"cwd":"D:\\work\\island","model":"gpt-5-codex"}})"
        "\n"
        R"({"timestamp":"2024-01-01T00:00:02.000Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"跑一下测试"}]}})"
        "\n"
        R"({"timestamp":"2024-01-01T00:00:03.000Z","type":"response_item","payload":{"type":"function_call","name":"shell","arguments":"{\"command\":[\"bash\",\"-lc\",\"ctest --output-on-failure\"]}"}})"
        "\n"
        R"({"timestamp":"2024-01-01T00:00:04.000Z","type":"response_item","payload":{"type":"function_call","name":"update_plan","arguments":"{\"plan\":[{\"step\":\"读代码\",\"status\":\"completed\"},{\"step\":\"跑测试\",\"status\":\"in_progress\"}]}"}})"
        "\n"
        R"({"timestamp":"2024-01-01T00:00:05.000Z","type":"event_msg","payload":{"type":"agent_message","message":"测试已经跑完，两个用例失败。"}})";

    const AgentSession s = Feed("codex", blob);
    CHECK_EQ(s.project, "island");
    CHECK_EQ(s.model, "gpt-5-codex");
    CHECK_EQ(s.task, "跑一下测试");
    CHECK(s.todo_count == 2);
    CHECK(s.done_count == 1);
    CHECK_EQ(s.action, "正在做 跑测试");  // update_plan 覆盖了 shell 的动作
    CHECK(s.tool_calls == 2);
    CHECK_EQ(s.last_text, "测试已经跑完，两个用例失败。");
}

void TestGemini() {
    const std::string text = R"({"sessionId":"g1","startTime":"2024-01-01T00:00:00.000Z","messages":[
        {"id":"1","timestamp":"2024-01-01T00:00:01.000Z","type":"user","content":[{"text":"解释一下这个仓库"}]},
        {"id":"2","timestamp":"2024-01-01T00:00:02.000Z","type":"gemini","content":[{"text":"我先看目录结构"}],"toolCalls":[{"id":"t1","name":"list_directory","args":{"path":"src"}}]},
        {"id":"3","timestamp":"2024-01-01T00:00:03.000Z","type":"gemini","content":[{"text":"看完了"}],"toolCalls":[{"id":"t2","name":"read_file","args":{"file_path":"src/main.ts"}}]}
    ]})";
    AgentSession s;
    s.provider = "gemini";
    for (const auto& ev : ParseGeminiSession(text)) ApplyAgentEvent(&s, ev);
    CHECK_EQ(s.session_id, "g1");
    CHECK_EQ(s.task, "解释一下这个仓库");
    CHECK_EQ(s.action, "正在阅读 src/main.ts");
    CHECK(s.tool_calls == 2);
    CHECK_EQ(s.last_text, "看完了");
    CHECK(s.started_ms == 1704067200000LL);

    AgentSession broken;
    CHECK(ParseGeminiSession("{not json").empty());
}

void TestState() {
    AgentSession s;
    s.provider = "claude";
    s.session_id = "x";
    s.last_event_ms = 100000;
    s.started_ms = 90000;
    CHECK_EQ(AgentStateAt(s, 100000 + 5000), "running");
    CHECK_EQ(AgentStateAt(s, 100000 + 45000), "running");  // 边界仍算在跑
    CHECK_EQ(AgentStateAt(s, 100000 + 46000), "waiting");
    CHECK_EQ(AgentStateAt(s, 100000 + 9 * 60 * 1000), "done");
    // 没有时间戳（刚读到的旧文件 / 时钟异常）不要假装在跑
    AgentSession blank;
    CHECK_EQ(AgentStateAt(blank, 123456), "waiting");

    s.started_ms = 90000;
    CHECK_EQ(diw::AgentElapsedText(s, 90000 + 65 * 1000), "01:05");
    CHECK_EQ(diw::AgentElapsedText(s, 90000 + 3600 * 1000 * 2 + 5 * 60 * 1000), "2h05");
    CHECK(diw::AgentElapsedText(s, 0).empty());
}

void TestBadLines() {
    // 坏行、空行、纯文本行都不能崩，也不能产生事件
    std::vector<AgentEvent> ev;
    (void)ev;
    std::vector<AgentEvent> e1;
    ParseAgentJsonLine("claude", "", &e1);
    CHECK(e1.empty());
    ParseAgentJsonLine("claude", "not json at all", &e1);
    CHECK(e1.empty());
    ParseAgentJsonLine("claude", "{\"type\":", &e1);  // 写到一半被读到
    CHECK(e1.empty());
    ParseAgentJsonLine("codex", "{\"type\":\"unknown_kind\"}", &e1);
    CHECK(e1.empty());
}

int main() {
    TestIso8601();
    TestTextHelpers();
    TestToolTarget();
    TestClaude();
    TestClaudeTodos();
    TestWorkbuddy();
    TestPi();
    TestCodex();
    TestGemini();
    TestState();
    TestBadLines();
    std::printf("agent_tests: run=%d failed=%d\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}
