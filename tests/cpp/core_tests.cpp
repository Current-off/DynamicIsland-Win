// C++ 核心域确定性单元测试（无第三方依赖）。
// 覆盖正常路径、边界值、反例与降级链；与 Python 侧 52 项用例语义对应。
#include "activity.hpp"
#include "ipc_protocol.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_run = 0;
int g_failed = 0;

void Check(bool cond, const char* name, int line) {
    ++g_run;
    if (!cond) {
        ++g_failed;
        std::printf("  FAIL  %s (line %d)\n", name, line);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

using namespace bi;

Activity Make(const std::string& id, double score = 0.0,
              std::int64_t created = 0, const std::string& kind = "media") {
    Activity a;
    a.id = id;
    a.kind = kind;
    a.title = id;
    a.relevance_score = score;
    a.created_at_ms = created;
    return a;
}

void TestActivityExpiry() {
    Activity a = Make("a");
    a.stale_at_ms = 100;
    CHECK(!IsExpiredAt(a, 99));
    CHECK(IsExpiredAt(a, 100));   // 边界：等号判为过期
    CHECK(IsExpiredAt(a, 101));
    CHECK(!IsExpiredAt(Make("b"), 1000000000));  // 无 staleDate 永不过期
    a.state = ActivityState::Ended;
    CHECK(IsExpiredAt(a, 0));
}

void TestStore() {
    ActivityStore s;
    CHECK(s.Upsert(Make("a")));
    CHECK(!s.Upsert(Make("a", 9.0)));  // 幂等更新
    CHECK(s.Size() == 1);
    CHECK(s.Get("a")->relevance_score == 9.0);
    CHECK(s.Mark("a", ActivityState::Ended));
    bool threw = false;
    try { s.Upsert(Make("a")); } catch (const std::logic_error&) { threw = true; }
    CHECK(threw);  // 终止态不可复活
    CHECK(s.Remove("a"));
    CHECK(s.Upsert(Make("a")));

    bool bad = false;
    try { s.Upsert(Make("")); } catch (const std::invalid_argument&) { bad = true; }
    CHECK(bad);
    bad = false;
    Activity stale = Make("s");
    stale.created_at_ms = 100;
    stale.stale_at_ms = 50;
    try { s.Upsert(stale); } catch (const std::invalid_argument&) { bad = true; }
    CHECK(bad);

    ActivityStore p;
    for (const char* id : {"c", "a", "b"}) {
        Activity x = Make(id);
        x.stale_at_ms = 10;
        p.Upsert(x);
    }
    auto changed = p.PurgeExpired(10);
    CHECK(changed.size() == 3);
    CHECK(changed[0] == "a" && changed[1] == "b" && changed[2] == "c");
    CHECK(p.PurgeExpired(99).empty());  // 只标记一次
    CHECK(p.Get("a")->state == ActivityState::Stale);
    CHECK(!p.Mark("missing", ActivityState::Ended));
    CHECK(!p.Get("missing"));
}

void TestScheduler() {
    Scheduler s;
    CHECK(s.Select({}, 0).IsIdle());

    auto one = s.Select({Make("a")}, 0);
    CHECK(one.frame.visible.size() == 1);
    CHECK(one.frame.visible[0].presentation == Presentation::Compact);
    CHECK(one.frame.visible[0].region == "compactLeading");

    auto two = s.Select({Make("low", 1), Make("high", 9)}, 0);
    CHECK(two.frame.visible.size() == 2);
    CHECK(two.frame.visible[0].activity.id == "high");
    CHECK(two.frame.visible[1].presentation == Presentation::Minimal);

    // 同分决胜：先创建时间，再 id；输入顺序反转结果必须一致
    std::vector<Activity> tie = {Make("b", 5, 10), Make("a", 5, 10), Make("c", 5, 1)};
    auto r1 = s.Select(tie, 0);
    std::vector<Activity> reversed(tie.rbegin(), tie.rend());
    auto r2 = s.Select(reversed, 0);
    CHECK(r1.frame.visible[0].activity.id == "c");
    CHECK(r1.frame.visible[1].activity.id == "a");
    CHECK(r1.frame.visible[0].activity.id == r2.frame.visible[0].activity.id);
    CHECK(r1.frame.visible[1].activity.id == r2.frame.visible[1].activity.id);

    // 上限 4：同时放歌、看视频、跑两个 agent 都要能各占一个岛。
    // 超过上限的进 suppressed，用户仍能在一览 / 右键菜单里看到「还有几件」。
    std::vector<Activity> four = {Make("a"), Make("b"), Make("c"), Make("d")};
    auto many = s.Select(four, 0);
    CHECK(many.frame.visible.size() == 4);
    CHECK(many.frame.suppressed_count == 0);

    std::vector<Activity> six = {Make("a"), Make("b"), Make("c"), Make("d"), Make("e"), Make("f")};
    auto over = s.Select(six, 0);
    CHECK(over.frame.visible.size() == 4);
    CHECK(over.frame.suppressed_count == 2);

    Activity old = Make("old", 100);
    old.stale_at_ms = 5;
    auto exp = s.Select({old, Make("fresh", 1)}, 10);
    CHECK(exp.expired_ids.size() == 1 && exp.expired_ids[0] == "old");
    CHECK(exp.frame.visible.size() == 1 && exp.frame.visible[0].activity.id == "fresh");

    auto expanded = s.Select({Make("a", 9), Make("b", 1)}, 0, "b");
    CHECK(expanded.frame.visible[1].presentation == Presentation::Expanded);
    CHECK(expanded.frame.visible[1].region == "expanded");

    bool threw = false;
    try { Scheduler bad(0); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void TestStateMachine() {
    CapsuleStateMachine sm;
    CHECK(sm.State() == CapsuleState::Idle);
    CHECK(!sm.Can(CapsuleEvent::Expand));
    CHECK(sm.Send(CapsuleEvent::ActivityStarted) == CapsuleState::Compact);
    CHECK(sm.Send(CapsuleEvent::Expand) == CapsuleState::Expanded);
    CHECK(sm.Send(CapsuleEvent::Collapse) == CapsuleState::Compact);
    CHECK(sm.Send(CapsuleEvent::Dismiss) == CapsuleState::Dismissed);
    CHECK(sm.History().size() == 4);

    CapsuleStateMachine illegal;
    bool threw = false;
    try { illegal.Send(CapsuleEvent::Expand); } catch (const InvalidTransition&) { threw = true; }
    CHECK(threw);
    CHECK(illegal.State() == CapsuleState::Idle);
    CHECK(illegal.History().empty());
}

void TestTokens() {
    CHECK(InnerCornerRadius() == 24.0);
    CHECK(AnimationDurationMs(320) == 320);
    CHECK(AnimationDurationMs(99999) == 2000);
    CHECK(AnimationDurationMs(320, true) == 0);
    bool threw = false;
    try { AnimationDurationMs(-1); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    CHECK(IsValidExpandedHeight(84.0));
    CHECK(IsValidExpandedHeight(160.0));
    CHECK(!IsValidExpandedHeight(83.9));
    CHECK(!IsValidExpandedHeight(161.0));
}

void TestRuntimeDegradation() {
    ActivityRuntime rt;
    rt.StartActivity(Make("m", 80));
    rt.StartActivity(Make("c", 90, 0, "bridge.calendar"));
    CHECK(rt.Tick(0).frame.visible.size() == 2);

    rt.SetBridgeOnline(false);
    auto offline = rt.Tick(1);
    CHECK(!offline.IsIdle());
    CHECK(offline.frame.visible.size() == 1);
    CHECK(offline.frame.visible[0].activity.id == "m");

    rt.SetBridgeOnline(true);
    rt.SetSuspended(true);
    CHECK(rt.Tick(2).IsIdle());
    CHECK(rt.Store().Get("m")->state == ActivityState::Active);  // 状态保留
    rt.SetSuspended(false);
    CHECK(rt.Tick(3).frame.visible.size() == 2);

    Activity download = Make("d", 85);
    download.created_at_ms = 510;
    download.stale_at_ms = 800;
    rt.StartActivity(download);
    auto expired = rt.Tick(800);
    CHECK(expired.expired_ids.size() == 1);
    CHECK(expired.expired_ids[0] == "d");
    CHECK(rt.Store().Get("d")->state == ActivityState::Stale);

    CHECK(rt.StateMachine().State() == CapsuleState::Minimal);
}


// 回归：同一种瞬时事件在一段时间内第二次发生时，必须能再次提示。
//
// 这是实机抓到的致命 bug：PurgeExpired 只标记 Stale 不删除，
// 固定 id（live-clipboard 之类）第二次触发时 Get(id) 不为空，
// 主循环的「不存在才插入」判断永远为假 —— 于是复制第二次起完全没有反应。
void TestStaleReentry() {
    ActivityStore store;
    Activity clip = Make("live-clipboard", 68, 100, "clipboard");
    clip.stale_at_ms = 300;
    CHECK(store.Upsert(clip));
    store.PurgeExpired(300);
    CHECK(store.Get("live-clipboard") != nullptr);
    CHECK(store.Get("live-clipboard")->state == ActivityState::Stale);

    // 残留的 Stale 记录必须先被清掉，否则第二次插入会被挡住。
    CHECK(store.RemoveIfStale("live-clipboard"));
    CHECK(store.Get("live-clipboard") == nullptr);

    Activity clip2 = Make("live-clipboard", 68, 900, "clipboard");
    clip2.stale_at_ms = 4100;
    CHECK(store.Upsert(clip2));  // 必须是「新插入」，而不是覆盖
    CHECK(store.Get("live-clipboard")->created_at_ms == 900);
    CHECK(store.Get("live-clipboard")->state == ActivityState::Active);

    // 活跃中的活动不能被 RemoveIfStale 误删：它是当前提示，删了会闪一下。
    CHECK(!store.RemoveIfStale("live-clipboard"));
    CHECK(store.Get("live-clipboard") != nullptr);

    // 不存在的 id 也不应报错。
    CHECK(!store.RemoveIfStale("no-such-id"));
}


// 回归：同一个 id 的活动结束之后，能不能重新开始。
//
// 真实场景：音乐暂停 → 活动结束（Mark Ended）→ 用户再按播放 → 同一个 live-media
// 再次 StartActivity。而 Upsert 对终止态明确抛 logic_error，主循环里又没有
// try/catch，异常会一路冒到 wWinMain 的 catch 里，进程直接退出。
// 也就是说「暂停一下再播放」会让灵动岛整个消失。
void TestRestartAfterEnd() {
    ActivityRuntime rt;
    Activity media = Make("live-media", 60, 100);
    media.content_json = "{\"title\":\"A\"}";
    rt.StartActivity(media);
    CHECK(rt.Store().Get("live-media") != nullptr);

    CHECK(rt.EndActivity("live-media"));
    CHECK(rt.Store().Get("live-media")->state == ActivityState::Ended);

    // 再播放：同一个 id 重新开始，不能抛异常。
    Activity again = Make("live-media", 60, 5000);
    again.content_json = "{\"title\":\"B\"}";
    bool threw = false;
    try {
        rt.StartActivity(again);
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(rt.Store().Get("live-media") != nullptr);
    CHECK(rt.Store().Get("live-media")->state == ActivityState::Active);
    CHECK(rt.Store().Get("live-media")->content_json == "{\"title\":\"B\"}");
}

// IPC 协议解码：与 docs/protocol.md 的 v1 判定顺序一致。
// 这里只测"协议对不对"，不测管道读写（那属于 win32 层，需要真句柄）。
void TestIpcDecode() {
    using bi::ipc::DecodeLine;
    using bi::ipc::Request;
    std::string err;
    Request req;

    const std::string ok_line =
        R"({"protocolVersion":1,"seq":1,"type":"activity.upsert","ts":1000,)"
        R"("activity":{"id":"ci:build","kind":"bridge.ci","title":"构建通过","relevanceScore":80,)"
        R"("createdAtMs":900,"staleAtMs":9000,"content":{"body":"3m12s"}}})";
    CHECK(DecodeLine(ok_line, &req, &err));
    CHECK(req.type == Request::Type::Upsert);
    CHECK(req.activity.id == "ci:build");
    CHECK(req.activity.kind == "bridge.ci");
    CHECK(req.activity.title == "构建通过");
    CHECK(req.activity.relevance_score == 80.0);
    CHECK(req.activity.created_at_ms == 900);
    CHECK(req.activity.stale_at_ms.has_value() && *req.activity.stale_at_ms == 9000);
    // content 被重新序列化（紧凑、键序保持），渲染层只认 JSON 文本。
    CHECK(req.activity.content_json == "{\"body\":\"3m12s\"}");

    // 结束消息只需要 id。
    CHECK(DecodeLine(R"({"protocolVersion":1,"seq":2,"type":"activity.end","ts":1100,)"
                     R"("activity":{"id":"ci:build"}})",
                     &req, &err));
    CHECK(req.type == Request::Type::End);
    CHECK(req.activity.id == "ci:build");

    // 缺 content 时补空对象，渲染层据 kind 走默认图形。
    CHECK(DecodeLine(R"({"protocolVersion":1,"seq":3,"type":"activity.upsert","ts":1,)"
                     R"("activity":{"id":"x","kind":"bridge.t","title":"t"}})",
                     &req, &err));
    CHECK(req.activity.content_json == "{}");
    // 未知字段必须被忽略（前向兼容），而不是报错。
    CHECK(DecodeLine(R"({"protocolVersion":1,"seq":4,"type":"activity.upsert","ts":1,)"
                     R"("future":"ignored","activity":{"id":"y","kind":"k","title":"t","more":1}})",
                     &req, &err));

    // 以下每一条都必须被拒绝，且给出原因。
    const char* bad[] = {
        "",                                                                   // 空行
        "not json",                                                           // 非法 JSON
        "[]",                                                                 // 根不是对象
        R"({"seq":1,"type":"activity.upsert","ts":1,"activity":{"id":"a","kind":"k","title":"t"}})",
        R"({"protocolVersion":2,"seq":1,"type":"activity.upsert","ts":1,"activity":{"id":"a","kind":"k","title":"t"}})",
        R"({"protocolVersion":1,"seq":-1,"type":"activity.upsert","ts":1,"activity":{"id":"a","kind":"k","title":"t"}})",
        R"({"protocolVersion":1,"seq":1.5,"type":"activity.upsert","ts":1,"activity":{"id":"a","kind":"k","title":"t"}})",
        R"({"protocolVersion":1,"seq":true,"type":"activity.upsert","ts":1,"activity":{"id":"a","kind":"k","title":"t"}})",
        R"({"protocolVersion":1,"seq":1,"type":"nope","ts":1,"activity":{"id":"a","kind":"k","title":"t"}})",
        R"({"protocolVersion":1,"seq":1,"type":"activity.upsert","ts":1,"activity":[]})",
        R"({"protocolVersion":1,"seq":1,"type":"activity.upsert","ts":1,"activity":{"kind":"k","title":"t"}})",
        R"({"protocolVersion":1,"seq":1,"type":"activity.upsert","ts":1,"activity":{"id":"a","title":"t"}})",
        R"({"protocolVersion":1,"seq":1,"type":"activity.upsert","ts":1,"activity":{"id":"a","kind":"k"}})",
        R"({"protocolVersion":1,"seq":1,"type":"activity.upsert","ts":1,"activity":{"id":"a","kind":"k","title":"t","content":[]}})",
    };
    for (const char* line : bad) {
        err.clear();
        const bool ok = DecodeLine(line, &req, &err);
        ++g_run;
        if (ok || err.empty()) {
            ++g_failed;
            std::printf("  FAIL  should reject: %s\n", line);
        }
    }

    // 多行输入必须拒绝：一条消息一行是这套协议的基本假设。
    CHECK(!DecodeLine("{\"protocolVersion\":1}\n{\"protocolVersion\":1}", &req, &err));

    // 超长行必须拒绝（防止外部程序把服务端内存吃光）。
    std::string huge(9 * 1024, 'a');
    CHECK(!DecodeLine(huge, &req, &err));
}

}  // namespace

int main() {
    std::printf("balance_island / C++ core unit tests (MSVC C++17)\n");
    TestActivityExpiry();
    TestStore();
    TestScheduler();
    TestStateMachine();
    TestTokens();
    TestRuntimeDegradation();
    TestStaleReentry();
    TestRestartAfterEnd();
    TestIpcDecode();
    std::printf("run=%d failed=%d\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}
