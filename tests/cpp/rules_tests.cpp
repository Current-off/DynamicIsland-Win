// 规则引擎与实时活动映射的确定性单元测试（无第三方依赖）。
//
// 这两块是「用户能不能控制胶囊」的全部所在，也是最容易出静默 bug 的地方：
// 白名单空列表的语义、跨零点免打扰、扩展名精确匹配、同一次复制只弹一次 ——
// 每一条都写成断言，而不是靠人盯着屏幕看有没有弹出来。
#include "live_mapping.hpp"
#include "mini_json.hpp"
#include "rules.hpp"

#include <cstdio>
#include <string>

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

void TestMiniJson() {
    json::Value root;
    std::string error;
    const bool ok = json::Parse(R"({"a":1,"b":[true,false,"x\n\"y"],"c":{"d":2.5}})", root, &error);
    CHECK(ok);
    CHECK(root.Find("a") != nullptr);
    CHECK(root.At("a").AsInt(0) == 1);
    CHECK(root.At("b").IsArray());
    CHECK(root.At("b").items.size() == 3);
    CHECK(root.At("b").AtIndex(0).AsBool(false));
    CHECK(root.At("b").AtIndex(2).AsString() == "x\n\"y");
    CHECK(root.At("c").At("d").AsDouble(0.0) == 2.5);

    json::Value bad;
    CHECK(!json::Parse("{\"a\":}", bad, &error));
    CHECK(!error.empty());
    CHECK(!json::Parse("", bad, nullptr));

    json::Value unicode;
    CHECK(json::Parse(R"({"t":"\u4f60\u597d"})", unicode, nullptr));
    CHECK(unicode.At("t").AsString() == "你好");

    // NaN / inf / 十六进制：strtod 都接受，但都不是 JSON 数字。
    // NaN 最危险——它会经 IPC 的 relevanceScore 进排序比较器，
    // 破坏严格弱序导致 std::sort UB（issue #2 第 1 条）。
    json::Value bad_num;
    CHECK(!json::Parse(R"({"score":NaN})", bad_num, nullptr));
    CHECK(!json::Parse(R"({"score":inf})", bad_num, nullptr));
    CHECK(!json::Parse(R"({"score":0x10})", bad_num, nullptr));
    // 合法数字不能被误伤
    CHECK(json::Parse(R"({"score":-1.5e3})", bad_num, nullptr));
    CHECK(bad_num.At("score").AsDouble(0.0) == -1500.0);
}

void TestExtensionAndClock() {
    CHECK(ExtensionOf("报告.PDF") == "pdf");
    CHECK(ExtensionOf("a.tar.gz") == "gz");
    CHECK(ExtensionOf(".gitignore") == "gitignore");
    CHECK(ExtensionOf("noext") == "");
    CHECK(ExtensionOf("C:\\dir.v2\\file.txt") == "txt");

    CHECK(NormalizeExtension(".TMP") == "tmp");
    CHECK(NormalizeExtension("*zip") == "zip");
    CHECK(NormalizeExtension("  ISO ") == "iso");

    CHECK(ParseClock("23:05") == 1385);
    CHECK(ParseClock("0:00") == 0);
    CHECK(ParseClock("24:00") == -1);
    CHECK(ParseClock("7:5x") == -1);
    CHECK(ParseClock("ab:cd") == -1);
    CHECK(FormatClock(1385) == "23:05");
}

void TestSecrets() {
    CHECK(LooksLikeSecret("my password is hunter2"));
    CHECK(LooksLikeSecret("密码：abc"));
    CHECK(LooksLikeSecret("-----BEGIN RSA PRIVATE KEY-----"));
    CHECK(LooksLikeSecret("a1b2c3d4e5f6g7h8i9j0k1l2"));  // 24 位无空格字母数字
    CHECK(!LooksLikeSecret("hello world"));
    CHECK(!LooksLikeSecret("123456789012345678901234567890"));  // 纯数字不算
}

void TestConfigRoundTrip() {
    RuleConfig cfg;
    cfg.events.volume = false;
    cfg.app_block = {"wechat", "迅雷"};
    cfg.ext_block = {"tmp", "iso"};
    cfg.clipboard.min_chars = 4;
    cfg.clipboard.max_chars = 40;
    cfg.quiet.enabled = true;
    cfg.quiet.from_min = 22 * 60 + 30;
    cfg.quiet.to_min = 7 * 60;
    cfg.fullscreen_hold = false;
    cfg.glass = false;
    // 含反斜杠的代理串：ToJson 若不做转义，产出的 JSON 无法解析，
    // FromJson 会整份回退默认值 —— 用户全部设置丢失（issue #2 第 6 条）。
    cfg.lyrics_proxy = "http://192.168.1.1\\x:8080";

    const std::string text = cfg.ToJson();
    json::Value root;
    CHECK(json::Parse(text, root, nullptr));
    const RuleConfig back = RuleConfig::FromJson(root);
    CHECK(back.events.volume == false);
    CHECK(back.events.media == true);    CHECK(back.app_block.size() == 2);
    CHECK(back.app_block[1] == "迅雷");
    // 反斜杠代理串必须原样往返，且不能牵连其它字段
    CHECK(back.lyrics_proxy == cfg.lyrics_proxy);
    CHECK(back.ext_block.size() == 2);
    CHECK(back.ext_block[0] == "tmp");
    CHECK(back.clipboard.min_chars == 4);
    CHECK(back.clipboard.max_chars == 40);
    CHECK(back.quiet.enabled);
    CHECK(back.quiet.from_min == 22 * 60 + 30);
    CHECK(back.quiet.to_min == 7 * 60);
    CHECK(back.fullscreen_hold == false);
    // 玻璃开关必须往返；缺省（老配置文件没有这个键）必须为 true，
    // 否则升级后老用户会突然掉回不透明样式。
    CHECK(back.glass == false);
    CHECK(RuleConfig::Defaults().glass == true);
    json::Value legacy;
    json::Parse(R"({"version":1,"fullscreen_hold":true})", legacy, nullptr);
    CHECK(RuleConfig::FromJson(legacy).glass == true);

    // 坏 JSON 不能让程序崩，只能回落到默认值。
    json::Value junk;
    json::Parse(R"({"events":"oops","apps":[1,2],"quiet_hours":{"from":"nope"}})", junk, nullptr);
    const RuleConfig fallback = RuleConfig::FromJson(junk);
    CHECK(fallback.events.media == true);
    CHECK(fallback.app_block.empty());
    CHECK(fallback.quiet.from_min == 23 * 60);
}

void TestEventToggles() {
    RuleConfig cfg;
    cfg.events.clipboard = false;
    RuleInput clip;
    clip.kind = "clipboard";
    clip.text = "hello";
    clip.app_id = "Code.exe";
    CHECK(!EvaluateRules(cfg, clip).show);
    CHECK(std::string(EvaluateRules(cfg, clip).reason) == "event-disabled");

    RuleInput vol;
    vol.kind = "volume";
    CHECK(EvaluateRules(cfg, vol).show);  // 只关了剪贴板，音量不受影响
}

void TestAppRules() {
    RuleConfig cfg;
    cfg.app_block = {"wechat", "com.tencent"};

    RuleInput wechat;
    wechat.kind = "notification";
    wechat.app_id = "WeChat.exe";
    wechat.app_label = "微信";
    const RuleVerdict v1 = EvaluateRules(cfg, wechat);
    CHECK(!v1.show);
    CHECK(std::string(v1.reason) == "app-blocked");
    CHECK(v1.matched == "wechat");  // 大小写不敏感

    RuleInput mail;
    mail.kind = "notification";
    mail.app_id = "HxOutlook.exe";
    CHECK(EvaluateRules(cfg, mail).show);

    // 白名单：非空即排他
    cfg.app_allow = {"code", "微信"};
    CHECK(EvaluateRules(cfg, wechat).show == false);  // 命中 block 的仍被拦
    RuleInput code;
    code.kind = "clipboard";
    code.app_id = "Code.exe";
    code.text = "abc";
    CHECK(EvaluateRules(cfg, code).show);

    // 事件没有应用身份时，白名单不该把它一起拦掉
    RuleInput device;
    device.kind = "device";
    CHECK(EvaluateRules(cfg, device).show);
}

void TestExtRules() {
    RuleConfig cfg;
    CHECK(cfg.ext_block.size() == 4);  // 默认拦掉下载临时文件

    RuleInput tmp;
    tmp.kind = "file";
    tmp.file_name = "setup.tmp";
    CHECK(!EvaluateRules(cfg, tmp).show);
    CHECK(std::string(EvaluateRules(cfg, tmp).reason) == "ext-blocked");

    RuleInput png;
    png.kind = "file";
    png.file_name = "shot.png";
    CHECK(EvaluateRules(cfg, png).show);

    // 精确匹配：默认拦 download，但不该把 "my.downloads.rar" 里的 rar 之外的东西误判
    RuleInput apng;
    apng.kind = "file";
    apng.file_name = "anim.apng";
    CHECK(EvaluateRules(cfg, apng).show);

    cfg.ext_allow = {"png", "jpg"};
    CHECK(EvaluateRules(cfg, png).show);
    RuleInput pdf;
    pdf.kind = "file";
    pdf.file_name = "a.pdf";
    CHECK(!EvaluateRules(cfg, pdf).show);
    CHECK(std::string(EvaluateRules(cfg, pdf).reason) == "ext-not-allowed");

    // 不涉及文件的事件不受扩展名规则影响
    RuleInput vol;
    vol.kind = "volume";
    CHECK(EvaluateRules(cfg, vol).show);
}

void TestQuietHours() {
    RuleConfig cfg;
    cfg.quiet.enabled = true;
    cfg.quiet.from_min = 23 * 60;
    cfg.quiet.to_min = 8 * 60;

    RuleInput clip;
    clip.kind = "clipboard";
    clip.text = "hello";
    clip.minute_of_day = 2 * 60;
    CHECK(!EvaluateRules(cfg, clip).show);
    CHECK(std::string(EvaluateRules(cfg, clip).reason) == "quiet-hours");

    clip.minute_of_day = 23 * 60 + 30;
    CHECK(!EvaluateRules(cfg, clip).show);

    clip.minute_of_day = 12 * 60;
    CHECK(EvaluateRules(cfg, clip).show);

    // 媒体是常驻底噪，免打扰时段仍然显示
    RuleInput media;
    media.kind = "media";
    media.minute_of_day = 2 * 60;
    CHECK(EvaluateRules(cfg, media).show);

    cfg.quiet.enabled = false;
    CHECK(EvaluateRules(cfg, clip).show);
}

void TestClipboardRules() {
    RuleConfig cfg;
    RuleInput tiny;
    tiny.kind = "clipboard";
    tiny.text = "a";
    CHECK(!EvaluateRules(cfg, tiny).show);
    CHECK(std::string(EvaluateRules(cfg, tiny).reason) == "too-short");

    tiny.text = "ab";
    CHECK(EvaluateRules(cfg, tiny).show);
}

// ---------------------------------------------------------------- 映射

Snapshot ClipSnapshot(std::uint64_t seq, const std::string& text, const std::string& app = "Code.exe") {
    Snapshot s;
    s.clipboard.seq = seq;
    s.clipboard.kind = "text";
    s.clipboard.text = text;
    s.clipboard.app_id = app;
    s.clipboard.app_label = "Code";
    s.power.known = true;
    s.power.has_battery = true;
    s.power.ac_online = true;
    s.power.pct = 80;
    return s;
}

void TestClipboardMappingOnce() {
    const RuleConfig cfg;
    LiveBaselines base;

    // 采集层在启动时就把「启动前已存在的剪贴板」对齐成基线（clip_seq_seen_），
    // 因此快照里的 seq 从 0 开始：第一次真实复制必须立刻弹出来。
    // 早先这里加了 clip_known 门闩，结果把启动后的第一次复制吞掉了 —— 实机复现后改掉。
    const Snapshot first = ClipSnapshot(100, "hello");
    const LiveActivities a1 = BuildActivities(first, 0, 600, base, cfg);
    CHECK(a1.list.size() == 1);
    CHECK(a1.list[0].id == "live-clipboard");
    CHECK(a1.list[0].kind == "clipboard");
    CHECK(a1.list[0].title == "已复制");
    CHECK(a1.list[0].stale_at_ms.has_value());
    CHECK(*a1.list[0].stale_at_ms == 3200);
    CHECK(a1.next.clip_seq == 100);

    // 同一个序号再算一次不能重弹，否则每帧都会刷一次。
    const LiveActivities a2 = BuildActivities(first, 16, 600, a1.next, cfg);
    CHECK(a2.list.empty());

    // 新的一次复制：再弹一次。
    const Snapshot second = ClipSnapshot(101, "world");
    const LiveActivities a3 = BuildActivities(second, 32, 600, a2.next, cfg);
    CHECK(a3.list.size() == 1);
    CHECK(*a3.list[0].stale_at_ms == 32 + 3200);
}

void TestClipboardKindsAndSecret() {
    const RuleConfig cfg;
    LiveBaselines base;
    base.clip_seq = 1;

    Snapshot shot;
    shot.clipboard.seq = 2;
    shot.clipboard.kind = "screenshot";
    shot.clipboard.text = "2560×1440";
    shot.clipboard.app_id = "SnippingTool.exe";
    shot.clipboard.app_label = "SnippingTool";
    shot.clipboard.width = 2560;
    shot.clipboard.height = 1440;
    const LiveActivities a = BuildActivities(shot, 0, 600, base, cfg);
    CHECK(a.list.size() == 1);
    CHECK(a.list[0].title == "已截图");

    Snapshot files;
    files.clipboard.seq = 3;
    files.clipboard.kind = "files";
    files.clipboard.text = "报告.xlsx";
    files.clipboard.item_count = 3;
    const LiveActivities b = BuildActivities(files, 0, 600, base, cfg);
    CHECK(b.list.size() == 1);
    CHECK(b.list[0].title == "已复制 3 个文件");

    Snapshot secret;
    secret.clipboard.seq = 4;
    secret.clipboard.kind = "text";
    secret.clipboard.text = "my password is hunter2";
    secret.clipboard.app_id = "KeePass.exe";
    const LiveActivities c = BuildActivities(secret, 0, 600, base, cfg);
    CHECK(c.list.size() == 1);
    CHECK(c.list[0].content_json.find("hunter2") == std::string::npos);
    CHECK(c.list[0].content_json.find("\"secret\":true") != std::string::npos);

    // 关闭隐藏后原文仍然可见
    RuleConfig show = cfg;
    show.clipboard.hide_secrets = false;
    const LiveActivities d = BuildActivities(secret, 0, 600, base, show);
    CHECK(d.list.size() == 1);
    CHECK(d.list[0].content_json.find("hunter2") != std::string::npos);
}

void TestBlockedNotes() {
    RuleConfig cfg;
    cfg.app_block = {"keepass"};

    LiveBaselines base;
    base.clip_seq = 1;

    Snapshot s = ClipSnapshot(2, "some secret text", "KeePass.exe");
    s.clipboard.app_label = "KeePass";
    const LiveActivities a = BuildActivities(s, 0, 600, base, cfg);
    CHECK(a.list.empty());
    CHECK(a.blocked.size() == 1);
    CHECK(a.blocked[0].kind == "clipboard");
    CHECK(a.blocked[0].reason == "app-blocked");
    CHECK(a.blocked[0].matched == "keepass");
    // 基线照常推进，否则被拦掉的事件会一直重试。
    CHECK(a.next.clip_seq == 2);
}

void TestFileMapping() {
    RuleConfig cfg;
    LiveBaselines base;
    Snapshot s;
    s.file.name = "setup.tmp";
    s.file.ext = "tmp";
    s.file.age_ms = 1200;
    const LiveActivities a = BuildActivities(s, 1000, 600, base, cfg);
    CHECK(a.list.empty());
    CHECK(a.blocked.size() == 1);
    CHECK(a.blocked[0].reason == "ext-blocked");

    cfg.ext_block.clear();
    const LiveActivities b = BuildActivities(s, 1000, 600, base, cfg);
    CHECK(b.list.size() == 1);
    CHECK(b.list[0].kind == "file");
    CHECK(b.list[0].title == "setup.tmp");

    // 超过 8 秒就不再提示（免得每次刷新桌面都弹一次）
    Snapshot old = s;
    old.file.age_ms = 9000;
    CHECK(BuildActivities(old, 1000, 600, base, cfg).list.empty());
}

void TestVolumeMapping() {
    const RuleConfig cfg;
    LiveBaselines base;
    Snapshot s;
    s.volume_pct = 42;
    s.volume_known = true;

    const LiveActivities a = BuildActivities(s, 0, 600, base, cfg);
    CHECK(a.list.empty());  // 首次只采基线
    CHECK(a.next.vol_pct == 42);

    s.volume_pct = 55;
    const LiveActivities b = BuildActivities(s, 16, 600, a.next, cfg);
    CHECK(b.list.size() == 1);
    CHECK(b.list[0].title == "音量 55%");

    const LiveActivities c = BuildActivities(s, 32, 600, b.next, cfg);
    CHECK(c.list.empty());

    // 只静音、不改音量，也要提示
    Snapshot muted = s;
    muted.muted = true;
    const LiveActivities d = BuildActivities(muted, 48, 600, c.next, cfg);
    CHECK(d.list.size() == 1);
    CHECK(d.list[0].title == "已静音");
}

void TestPowerMapping() {
    RuleConfig cfg;
    LiveBaselines base;
    Snapshot s;
    s.power.known = true;
    s.power.has_battery = true;
    s.power.ac_online = true;
    s.power.pct = 60;

    const LiveActivities a = BuildActivities(s, 0, 600, base, cfg);
    CHECK(a.list.empty());  // 首次只采基线，不该报「已接通电源」
    CHECK(a.next.ac_known);

    s.power.ac_online = false;
    const LiveActivities b = BuildActivities(s, 16, 600, a.next, cfg);
    CHECK(b.list.size() == 1);
    CHECK(b.list[0].title == "已断开电源");

    // 低电量是常驻活动（没有 stale_at），源消失才结束
    s.power.pct = 12;
    const LiveActivities c = BuildActivities(s, 32, 600, b.next, cfg);
    bool has_battery_low = false;
    for (const auto& item : c.list) {
        if (item.id == "live-battery") {
            has_battery_low = true;
            CHECK(!item.stale_at_ms.has_value());
        }
    }
    CHECK(has_battery_low);

    s.power.ac_online = true;
    s.power.pct = 80;
    const LiveActivities d = BuildActivities(s, 48, 600, c.next, cfg);
    for (const auto& item : d.list) CHECK(item.id != "live-battery");
    CHECK(d.list.size() == 1);  // 只剩「已接通电源」
    CHECK(d.list[0].title == "已接通电源");
}

void TestDeviceAndSuspend() {
    const RuleConfig cfg;
    LiveBaselines base;
    Snapshot s;
    s.power.known = true;
    s.device.seq = 1;
    s.device.label = "E: 已接入";

    // 采集层的设备序号只在真的收到 WM_DEVICECHANGE 时自增，
    // 所以「从 0 变成 1」就是一次真实插入，必须提示。
    const LiveActivities a = BuildActivities(s, 0, 600, base, cfg);
    CHECK(a.list.size() == 1);
    CHECK(a.list[0].kind == "device");
    CHECK(a.list[0].title == "E: 已接入");

    const LiveActivities b = BuildActivities(s, 16, 600, a.next, cfg);
    CHECK(b.list.empty());

    s.device.seq = 2;
    s.device.label = "F: 已接入";
    const LiveActivities c = BuildActivities(s, 32, 600, b.next, cfg);
    CHECK(c.list.size() == 1);
    CHECK(c.list[0].title == "F: 已接入");

    // 全屏避让
    s.fullscreen = true;
    CHECK(BuildActivities(s, 48, 600, c.next, cfg).suspend);
    RuleConfig no_hold = cfg;
    no_hold.fullscreen_hold = false;
    CHECK(!BuildActivities(s, 48, 600, c.next, no_hold).suspend);
}

void TestNotificationMapping() {
    const RuleConfig cfg;
    LiveBaselines base;

    // 通知语义：seq 变化 = 采到新通知。字段齐全时 content_json
    // 必须带 icon（来源应用图标缓存路径），渲染层靠它替换默认图形。
    Snapshot s;
    s.notif.ok = true;
    s.notif.seq = 1;
    s.notif.app = "微信";
    s.notif.app_id = "com.tencent.weixin";
    s.notif.title = "张三";
    s.notif.body = "在吗";
    s.notif.icon_path = "C:\\covers\\abc.png";

    const LiveActivities a = BuildActivities(s, 0, 600, base, cfg);
    CHECK(a.list.size() == 1);
    CHECK(a.list[0].id == "live-notif");
    CHECK(a.list[0].kind == "notification");
    CHECK(a.list[0].title == "张三");
    CHECK(a.list[0].content_json.find("\"icon\":\"C:\\\\covers\\\\abc.png\"") != std::string::npos);
    CHECK(a.list[0].content_json.find("\"body\":\"在吗\"") != std::string::npos);
    CHECK(a.next.notif_seq == 1);

    // seq 不变：不重弹
    const LiveActivities b = BuildActivities(s, 16, 600, a.next, cfg);
    CHECK(b.list.empty());

    // 新通知（seq 变）：标题回落 / 正文为空 / 图标缺失时不崩、json 仍合法
    s.notif.seq = 2;
    s.notif.title.clear();
    s.notif.body.clear();
    s.notif.icon_path.clear();
    const LiveActivities c = BuildActivities(s, 32, 600, b.next, cfg);
    CHECK(c.list.size() == 1);
    CHECK(c.list[0].title == "微信");  // 无标题回落应用名
    CHECK(c.list[0].content_json.find("\"icon\":\"\"") != std::string::npos);

    // 通知开关关掉：不产生活动
    RuleConfig off = cfg;
    off.events.notification = false;
    CHECK(BuildActivities(s, 48, 600, c.next, off).list.empty());
}

void TestMediaAppRule() {
    RuleConfig cfg;
    cfg.app_block = {"foobar"};
    LiveBaselines base;
    Snapshot s;
    s.media.present = true;
    s.media.title = "晴天";
    s.media.artist = "周杰伦";
    s.media.app_id = "foobar2000.exe";
    s.media.app_label = "foobar2000";

    CHECK(BuildActivities(s, 0, 600, base, cfg).list.empty());
    CHECK(BuildActivities(s, 0, 600, base, cfg).blocked.size() == 1);

    cfg.app_block.clear();
    const LiveActivities ok = BuildActivities(s, 0, 600, base, cfg);
    CHECK(ok.list.size() == 1);
    CHECK(ok.list[0].content_json.find("周杰伦") != std::string::npos);
    // 一览点击拉起：媒体的 AUMID 必须进 content_json（SMTC 来源），
    // 否则 BuildView 拿不到 launch 目标，点了岛没反应。
    CHECK(ok.list[0].content_json.find("\"app_id\":\"foobar2000.exe\"") != std::string::npos);
}

void TestClipboardLaunchPath() {
    const RuleConfig cfg;
    LiveBaselines base;
    Snapshot s = ClipSnapshot(200, "hello");
    // 剪贴板活动带完整 exe 路径（一览点击直接 ShellExecute 用）。
    s.clipboard.app_path = "C:\\Program Files\\Microsoft VS Code\\Code.exe";
    const LiveActivities a = BuildActivities(s, 0, 600, base, cfg);
    CHECK(a.list.size() == 1);
    CHECK(a.list[0].content_json.find("\"app_path\":\"C:\\\\Program Files\\\\Microsoft VS Code\\\\Code.exe\"") !=
          std::string::npos);
}

// ---------------------------------------------------------------- 个性化配置

// 全部新字段设成非默认值后 ToJson→Parse→FromJson 往返，逐一核对。
void TestPersonalizationRoundTrip() {
    RuleConfig cfg;
    cfg.theme = 2;
    cfg.accent = "#FF6A3D";
    cfg.ui_scale = 120;
    cfg.font_offset = 3;
    cfg.corner_radius = 20;
    cfg.top_offset = 32;
    cfg.auto_hide_focus = true;
    cfg.auto_hide_paused = true;
    cfg.auto_hide_fullscreen = false;
    cfg.quiet_form = 2;
    cfg.yield_enabled = false;
    cfg.yield_key = 2;
    cfg.reduce_motion = true;
    cfg.low_power_downgrade = false;
    cfg.glass_quality = 3;
    cfg.capture_refresh_ms = 5000;
    cfg.target_monitor = 2;
    cfg.monitor_index = 3;
    // 预设名刻意带引号与反斜杠：ToJson 若忘了 json::Quote，
    // 这里产出的就是非法 JSON，Parse 失败会整份回退默认。
    cfg.preset_name = R"(夜班 "安静" \ 预设)";

    const std::string text = cfg.ToJson();
    json::Value root;
    CHECK(json::Parse(text, root, nullptr));  // 输出必须是合法 JSON
    const RuleConfig back = RuleConfig::FromJson(root);
    CHECK(back.theme == 2);
    CHECK(back.accent == cfg.accent);
    CHECK(back.ui_scale == 120);
    CHECK(back.font_offset == 3);
    CHECK(back.corner_radius == 20);
    CHECK(back.top_offset == 32);
    CHECK(back.auto_hide_focus == true);
    CHECK(back.auto_hide_paused == true);
    CHECK(back.auto_hide_fullscreen == false);
    CHECK(back.quiet_form == 2);
    CHECK(back.yield_enabled == false);
    CHECK(back.yield_key == 2);
    CHECK(back.reduce_motion == true);
    CHECK(back.low_power_downgrade == false);
    CHECK(back.glass_quality == 3);
    CHECK(back.capture_refresh_ms == 5000);
    CHECK(back.target_monitor == 2);
    CHECK(back.monitor_index == 3);
    CHECK(back.preset_name == cfg.preset_name);

    // 强调色也是字符串字段，同样必须经 Quote 转义后才能往返。
    cfg.accent = "bad\"\\color";
    json::Value root2;
    CHECK(json::Parse(cfg.ToJson(), root2, nullptr));
    CHECK(RuleConfig::FromJson(root2).accent == cfg.accent);
}

// 老 rules.json 没有任何新键：每个新字段都必须等于默认值，
// 且原有字段仍按旧值加载（不能整份回退）。
void TestPersonalizationLegacyDefaults() {
    json::Value legacy;
    CHECK(json::Parse(R"({"version":1,"fullscreen_hold":false,"glass":false})", legacy, nullptr));
    const RuleConfig c = RuleConfig::FromJson(legacy);
    CHECK(c.fullscreen_hold == false);  // 旧字段生效
    CHECK(c.glass == false);
    CHECK(c.theme == 0);
    CHECK(c.accent == "#1E8AFA");
    CHECK(c.ui_scale == 100);
    CHECK(c.font_offset == 0);
    CHECK(c.corner_radius == 0);
    CHECK(c.top_offset == 4);
    CHECK(c.auto_hide_focus == false);
    CHECK(c.auto_hide_paused == false);
    CHECK(c.auto_hide_fullscreen == true);
    CHECK(c.quiet_form == 0);
    CHECK(c.yield_enabled == true);
    CHECK(c.yield_key == 0);
    CHECK(c.reduce_motion == false);
    CHECK(c.low_power_downgrade == true);
    CHECK(c.glass_quality == 1);
    CHECK(c.capture_refresh_ms == 2000);
    CHECK(c.target_monitor == 0);
    CHECK(c.monitor_index == 0);
    CHECK(c.preset_name == "默认");
}

// 越界值一律 clamp 到合法区间，而不是让整块外观回退默认。
void TestPersonalizationClamp() {
    json::Value hi;
    CHECK(json::Parse(R"({
        "ui_scale":999, "font_offset":99, "corner_radius":5, "top_offset":-5,
        "theme":9, "quiet_form":-3, "yield_key":42, "glass_quality":99,
        "capture_refresh_ms":1, "target_monitor":7, "monitor_index":999
    })", hi, nullptr));
    const RuleConfig c = RuleConfig::FromJson(hi);
    CHECK(c.ui_scale == 140);
    CHECK(c.font_offset == 4);
    CHECK(c.corner_radius == 0);  // 5 不在 {0}∪[8,40] → 归 0
    CHECK(c.top_offset == 0);
    CHECK(c.theme == 2);
    CHECK(c.quiet_form == 0);
    CHECK(c.yield_key == 2);
    CHECK(c.glass_quality == 3);
    CHECK(c.capture_refresh_ms == 500);
    CHECK(c.target_monitor == 2);
    CHECK(c.monitor_index == 15);

    json::Value lo;
    CHECK(json::Parse(R"({
        "ui_scale":1, "font_offset":-99, "corner_radius":999, "top_offset":999,
        "theme":-9, "quiet_form":99, "yield_key":-9, "glass_quality":-9,
        "capture_refresh_ms":99999, "target_monitor":-9, "monitor_index":-9
    })", lo, nullptr));
    const RuleConfig d = RuleConfig::FromJson(lo);
    CHECK(d.ui_scale == 80);
    CHECK(d.font_offset == -2);
    CHECK(d.corner_radius == 40);  // 999 夹到上界 40
    CHECK(d.top_offset == 64);
    CHECK(d.theme == 0);
    CHECK(d.quiet_form == 2);
    CHECK(d.yield_key == 0);
    CHECK(d.glass_quality == 0);
    CHECK(d.capture_refresh_ms == 8000);
    CHECK(d.target_monitor == 0);
    CHECK(d.monitor_index == 0);
}

}  // namespace

int main() {
    std::printf("balance_island / rules + live mapping unit tests\n");
    TestMiniJson();
    TestExtensionAndClock();
    TestSecrets();
    TestConfigRoundTrip();
    TestPersonalizationRoundTrip();
    TestPersonalizationLegacyDefaults();
    TestPersonalizationClamp();
    TestEventToggles();
    TestAppRules();
    TestExtRules();
    TestQuietHours();
    TestClipboardRules();
    TestClipboardMappingOnce();
    TestClipboardKindsAndSecret();
    TestBlockedNotes();
    TestFileMapping();
    TestVolumeMapping();
    TestPowerMapping();
    TestDeviceAndSuspend();
    TestNotificationMapping();
    TestMediaAppRule();
    TestClipboardLaunchPath();
    std::printf("run=%d failed=%d\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}
