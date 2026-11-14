// 设置面板逻辑测试：预设、步进器夹取、点击分派。
//
// 为什么这些值得单独测：
//   ApplyPreset() 会在内部把整个 RuleConfig 重置成默认值再回填。只要有一天有人
//   漏掉一个「该保留」的字段，用户换个预设就会丢掉自己配的黑名单 / 关掉的事件开关 ——
//   界面上看不出任何异常，只有下次某条通知不弹时才察觉。这类 bug 必须用断言钉住。
//
//   步进器则是另一种典型：夹取写错不会崩，只会让「圆角」卡在 8 而回不到「自动」。
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "../src/core/rules.hpp"
#include "../src/win32/settings_ui.hpp"

namespace {

int g_run = 0;
int g_failed = 0;

void Check(bool ok, const char* what) {
    ++g_run;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL %s\n", what);
    }
}

using bi::RuleConfig;
using bi::win32::settings_ui::ApplyPreset;
using bi::win32::settings_ui::CurrentPreset;
using bi::win32::settings_ui::HandleHit;
using bi::win32::settings_ui::Hit;
using bi::win32::settings_ui::State;
using bi::win32::settings_ui::StepCorner;

// 一份「用户已经调过」的配置：预设绝不能把这些改掉。
RuleConfig UserTuned() {
    RuleConfig c;
    c.app_block = {"game.exe", "bank.exe"};
    c.ext_block = {"tmp", "iso"};
    c.events.media = false;
    c.events.clipboard = false;
    c.clipboard.hide_secrets = true;
    c.quiet.enabled = true;
    c.quiet.from_min = 22 * 60;
    c.lyrics_proxy = "http://127.0.0.1:7890";
    c.agents = false;
    c.auto_sleep_sec = 120;
    return c;
}

void PreservesUserChoices(const char* tag) {
    const RuleConfig base = UserTuned();
    for (int idx = -1; idx < 4; ++idx) {
        RuleConfig c = base;
        ApplyPreset(c, idx);
        char label[128];
        std::snprintf(label, sizeof(label), "%s idx=%d 保留黑名单", tag, idx);
        Check(c.app_block == base.app_block, label);
        std::snprintf(label, sizeof(label), "%s idx=%d 保留扩展名黑名单", tag, idx);
        Check(c.ext_block == base.ext_block, label);
        std::snprintf(label, sizeof(label), "%s idx=%d 保留事件开关", tag, idx);
        Check(c.events.media == base.events.media && c.events.clipboard == base.events.clipboard,
              label);
        std::snprintf(label, sizeof(label), "%s idx=%d 保留免打扰时段", tag, idx);
        Check(c.quiet.enabled == base.quiet.enabled && c.quiet.from_min == base.quiet.from_min,
              label);
        std::snprintf(label, sizeof(label), "%s idx=%d 保留代理", tag, idx);
        Check(c.lyrics_proxy == base.lyrics_proxy, label);
        std::snprintf(label, sizeof(label), "%s idx=%d 保留 Agent 读取权限", tag, idx);
        Check(c.agents == base.agents, label);
        std::snprintf(label, sizeof(label), "%s idx=%d 保留自动休眠", tag, idx);
        Check(c.auto_sleep_sec == base.auto_sleep_sec, label);
    }
}

void PresetValues() {
    // 极简：不透明、无玻璃、最省
    RuleConfig c;
    ApplyPreset(c, 0);
    Check(c.opacity == 2, "极简 opacity=2");
    Check(!c.glass, "极简 glass=off");
    Check(c.capture_refresh_ms == 4000, "极简 抓屏 4s");
    Check(c.reduce_motion, "极简 减少动效");
    Check(c.preset_name == "极简", "极简 名字");

    // 沉浸：液态玻璃 + 最高质量 + 最短刷新
    ApplyPreset(c, 2);
    Check(c.opacity == 0, "沉浸 opacity=0");
    Check(c.glass, "沉浸 glass=on");
    Check(c.glass_quality == 3, "沉浸 质量=3");
    Check(c.capture_refresh_ms == 1000, "沉浸 抓屏 1s");
    Check(!c.auto_hide_focus, "沉浸 不自动隐藏");

    // 节能
    ApplyPreset(c, 3);
    Check(c.capture_refresh_ms == 6000, "节能 抓屏 6s");
    Check(c.glass_quality == 0, "节能 质量=0");
    Check(c.low_power_downgrade, "节能 低电降级");

    // 均衡 = 出厂外观，且必须与默认构造函数一致
    const RuleConfig d;
    ApplyPreset(c, 1);
    Check(c.opacity == d.opacity && c.glass == d.glass && c.ui_scale == d.ui_scale &&
              c.corner_radius == d.corner_radius,
          "均衡 = 默认外观");

    // 恢复出厂：外观回默认，preset_name 回到「默认」
    ApplyPreset(c, -1);
    Check(c.preset_name == "默认", "恢复出厂 名字");
    Check(c.opacity == d.opacity, "恢复出厂 opacity");
}

void PresetLookup() {
    RuleConfig c;
    ApplyPreset(c, 2);
    Check(CurrentPreset(c) == 2, "当前预设 = 沉浸");
    c.preset_name = "默认";
    Check(CurrentPreset(c) == 1, "「默认」视作均衡");
    c.preset_name = "手改的";
    Check(CurrentPreset(c) == -1, "未知预设名 -> 自定义");
}

void CornerStepping() {
    Check(StepCorner(0, 2) == 8, "自动 -> 加 = 8");
    Check(StepCorner(0, -2) == 0, "自动 -> 减 = 仍是自动");
    Check(StepCorner(8, -2) == 0, "8 -> 减 = 回到自动");
    Check(StepCorner(40, 2) == 40, "40 封顶");
    Check(StepCorner(24, 2) == 26, "24 -> 26");
}

void HandleClamps() {
    State st;
    st.cfg.ui_scale = 138;
    HandleHit(st, Hit::UiScaleInc);
    Check(st.cfg.ui_scale == 140, "界面缩放封顶 140");
    HandleHit(st, Hit::UiScaleDec);
    Check(st.cfg.ui_scale == 135, "界面缩放 -5");

    st.cfg.capture_refresh_ms = 7900;
    HandleHit(st, Hit::CaptureRefreshInc);
    Check(st.cfg.capture_refresh_ms == 8000, "抓屏间隔封顶 8000");
    st.cfg.capture_refresh_ms = 600;
    HandleHit(st, Hit::CaptureRefreshDec);
    Check(st.cfg.capture_refresh_ms == 500, "抓屏间隔下限 500");

    st.cfg.top_offset = 4;
    HandleHit(st, Hit::TopOffsetDec);
    Check(st.cfg.top_offset == 4, "距顶间距下限 4");

    st.cfg.font_offset = 4;
    HandleHit(st, Hit::FontOffsetInc);
    Check(st.cfg.font_offset == 4, "字号偏移封顶 +4");

    // 目标屏幕不是「指定序号」时，屏幕序号步进器必须什么都不做
    st.cfg.target_monitor = 0;
    st.cfg.monitor_index = 0;
    HandleHit(st, Hit::MonitorIndexInc);
    Check(st.cfg.monitor_index == 0, "主屏时 序号 + 无效");
    st.cfg.target_monitor = 2;
    HandleHit(st, Hit::MonitorIndexInc);
    Check(st.cfg.monitor_index == 1, "指定序号时 序号 + 生效");
}

void HitDispatch() {
    State st;
    st.page = 0;
    Check(!HandleHit(st, Hit::TabAppearance), "点当前页 Tab 不需要重排");
    Check(HandleHit(st, Hit::TabBehavior), "点行为 Tab 需要重排");
    Check(st.page == 1, "切到行为页");
    st.page = 0;

    // 同组连续的下标控件
    HandleHit(st, Hit::Theme2);
    Check(st.cfg.theme == 2, "主题=跟随系统");
    HandleHit(st, Hit::GlassQuality1);
    Check(st.cfg.glass_quality == 1, "玻璃质量=均衡");
    HandleHit(st, Hit::YieldKey1);
    Check(st.cfg.yield_key == 1, "让路键=Alt");
    HandleHit(st, Hit::Accent3);
    Check(st.cfg.accent == "#FFB020", "强调色=橙");
    HandleHit(st, Hit::QuietForm2);
    Check(st.cfg.quiet_form == 2, "安静态=完全隐藏");

    // 布尔开关（原来全落进 ApplyToggle 的空分支）
    st.cfg.auto_hide_focus = false;
    HandleHit(st, Hit::AutoHideFocus);
    Check(st.cfg.auto_hide_focus, "焦点离开收起 可切换");
    st.cfg.reduce_motion = false;
    HandleHit(st, Hit::ReduceMotion);
    Check(st.cfg.reduce_motion, "减少动效 可切换");
    st.cfg.yield_enabled = false;
    HandleHit(st, Hit::YieldEnabled);
    Check(st.cfg.yield_enabled, "让路开关 可切换");
}

}  // namespace

int main() {
    PreservesUserChoices("预设");
    PresetValues();
    PresetLookup();
    CornerStepping();
    HandleClamps();
    HitDispatch();
    std::printf("settings_tests: run=%d failed=%d\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}
