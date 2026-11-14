// balance_island / 设置面板（全自绘，与胶囊本体同一套视觉语言）
//
// 为什么从「原生控件 + 系统灰底」改成全自绘：
//   原来的设置窗口是一个灰色对话框上堆 groupbox 和系统复选框，和顶上那颗
//   磨砂玻璃胶囊完全是两套东西 —— 用户打开设置会觉得是另一个软件。
//   而这次要加的两个功能（透明度档位、停靠位置）本身就是「视觉选择」，
//   用系统控件只能做成两个下拉框，看不出区别；自绘可以把效果直接画在选项里。
//
// 代价与取舍：
//   自绘要自己处理命中、悬停、拖窗口、Esc/Enter。这些加起来不到 100 行，
//   换来的是零原生控件、100% 统一的观感。
//   原来那两个黑名单多行文本框不再放进面板 —— 深色自绘面板里塞原生 EDIT
//   会立刻露馅（滚动条、光标、选中色都是系统的）。黑名单移到「打开配置文件」，
//   它是低频操作，而且配置文件本来才是权威来源。
//   唯一保留的"表单"是免打扰时间，用 −/+ 调整而不是输入框，点击即可。
//
// 交互约定（与胶囊一致）：
//   - 点开关/分段立即生效于本地副本，点「保存」才写盘；运行中的实例 1 秒内热加载。
//   - Esc = 取消，Enter = 保存，点右上 × = 取消。
//   - 标题栏区域可拖动（无边框窗口必须自己实现）。
#ifndef BI_SRC_WIN32_SETTINGS_UI_HPP
#define BI_SRC_WIN32_SETTINGS_UI_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "layered_surface.hpp"
#include "renderer.hpp"
#include "rules.hpp"

namespace bi {
namespace win32 {
namespace settings_ui {

// ---------------------------------------------------------------- 配色
//
// 全部取自胶囊本体：底色是 DrawOpaqueBody 的那组深色，强调色是 AccentFor
// 的蓝。改这里就等于同时改两处的观感 —— 这是"统一风格"能维持住的前提。
namespace color {
constexpr BYTE kBgTopR = 40, kBgTopG = 40, kBgTopB = 48;
constexpr BYTE kBgBotR = 22, kBgBotG = 22, kBgBotB = 27;
constexpr BYTE kTextR = 242, kTextG = 242, kTextB = 247;
constexpr BYTE kSubR = 150, kSubG = 152, kSubB = 164;
constexpr BYTE kAccentR = 30, kAccentG = 138, kAccentB = 250;
constexpr BYTE kCardR = 255, kCardG = 255, kCardB = 255;  // 用 alpha 调
}  // namespace color

// ---------------------------------------------------------------- 布局常量
namespace layout {
constexpr int kWidth = 640;       // 逻辑宽（96 DPI）
constexpr int kPad = 28;          // 左右内边距
constexpr int kTitleY = 24;
constexpr int kRowH = 46;         // 一行的高度（含上下留白）
constexpr int kGroupGap = 26;     // 分组之间的距离
}  // namespace layout

// ---------------------------------------------------------------- 分页与预设数据
// 四页共用同一个窗口高度与底部按钮：分页只换中间的控件，Tab 与保存按钮不跳动。
constexpr int kPageCount = 4;
constexpr int kAccentCount = 6;

// 强调色预设：hex 写进 cfg.accent（#RRGGBB），rgb 直接用于画色块，省去解析。
struct AccentPreset {
    const char* hex;
    BYTE r, g, b;
};
static const AccentPreset kAccents[kAccentCount] = {
    {"#1E8AFA", 30, 138, 250},  // 默认蓝
    {"#F05858", 240, 88, 88},
    {"#34C759", 52, 199, 89},
    {"#FFB020", 255, 176, 32},
    {"#A66BFF", 166, 107, 255},
    {"#2AC7C7", 42, 199, 199},
};

// 各 Segment 的中文标签（纯文字版，沿用 Segment 的圆角槽 + 选中白片画法）。
static const wchar_t* const kTabLabels[kPageCount] = {L"外观", L"行为", L"性能", L"预设"};
static const wchar_t* const kThemeLabels[3] = {L"深色", L"浅色", L"跟随系统"};
static const wchar_t* const kQuietFormLabels[3] = {L"呼吸点", L"细把手", L"完全隐藏"};
static const wchar_t* const kYieldKeyLabels[3] = {L"Ctrl", L"Alt", L"Shift"};
static const wchar_t* const kGlassQualityLabels[4] = {L"流畅", L"均衡", L"精细", L"极致"};
static const wchar_t* const kTargetMonitorLabels[3] = {L"主屏", L"光标所在屏", L"指定序号"};

// ---------------------------------------------------------------- 可点区域
enum class Hit : int {
    None = 0,
    Opacity0, Opacity1, Opacity2,
    Anchor0, Anchor1, Anchor2,
    Glass,
    EventBase = 100,              // +0..+6
    Fullscreen = 200,
    Secret,
    QuietOn,
    QuietFromDec,
    QuietFromInc,
    QuietToDec,
    QuietToInc,
    OpenConfig,
    Save,
    Cancel,
    Close,
    TitleBar,
    // ---- 以下全部追加在枚举末尾 ----
    // 绝不改动上面 EventBase=100 / Fullscreen=200 那一批显式数值：
    // 现有点击路由靠它们定位，一动就整体错位。新值从 212 起自动递增，
    // 与 EventIndex 检查的 100..106 区间也不冲突。
    // 同组多选控件（Tab / Segment / 色块）的 Hit 必须连续，布局用 base+i 分派。
    TabAppearance, TabBehavior, TabPerformance, TabPreset,  // 四个分页 Tab
    // 外观页
    Theme0, Theme1, Theme2,
    Accent0, Accent1, Accent2, Accent3, Accent4, Accent5,
    UiScaleDec, UiScaleInc,
    FontOffsetDec, FontOffsetInc,
    CornerDec, CornerInc,
    TopOffsetDec, TopOffsetInc,
    // 行为页
    AutoHideFocus, AutoHidePaused, AutoHideFullscreen,
    QuietForm0, QuietForm1, QuietForm2,
    YieldEnabled,
    YieldKey0, YieldKey1, YieldKey2,
    ReduceMotion, LowPowerDowngrade,
    // 性能页
    GlassQuality0, GlassQuality1, GlassQuality2, GlassQuality3,
    CaptureRefreshDec, CaptureRefreshInc,
    TargetMonitor0, TargetMonitor1, TargetMonitor2,
    MonitorIndexDec, MonitorIndexInc,
    // 预设页：四张预设卡 + 恢复出厂。卡片同组连续，布局用 Preset0+i 分派。
    Preset0, Preset1, Preset2, Preset3,
    PresetReset
};

struct Zone {
    Hit hit = Hit::None;
    RECT rc{};
};

struct State {
    RuleConfig cfg;
    std::wstring path;
    std::wstring shot_path;
    bool saved = false;
    bool shot_done = false;
    bool selftest = false;
    std::string selftest_note;
    HWND hwnd = nullptr;
    int dpi = 96;
    int width = 0;   // 客户区宽（物理像素）
    int height = 0;  // 客户区高
    Hit hover = Hit::None;
    Hit pressed = Hit::None;
    bool dragging = false;
    POINT drag_origin{};
    RECT drag_window{};
    std::vector<Zone> zones;
    int page = 0;  // 当前分页：0 外观 / 1 行为 / 2 性能 / 3 预设

    // ---- 滚动 ----
    // 1080p + 150% 缩放下「行为」页比工作区还高，固定高度会把底部按钮顶出屏幕。
    // 所以标题与 Tab 固定、按钮固定、中间内容区可滚（滚轮 + 右侧细滚动条）。
    int max_height = 0;  // 工作区允许的最大高度，0 = 不限制
    int scroll = 0;      // 内容区上移量（物理像素）
    int max_scroll = 0;  // 由 BuildZones 算出
    int view_top = 0;    // 内容视口上沿
    int view_h = 0;      // 内容视口高度
};

inline int S(int value, int dpi) { return ::MulDiv(value, dpi, 96); }
inline float F(int value, int dpi) { return static_cast<float>(MulDiv(value, dpi, 96)); }

inline std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), &out[0], need,
                          nullptr, nullptr);
    return out;
}

inline int EventIndex(Hit h) {
    const int base = static_cast<int>(Hit::EventBase);
    const int v = static_cast<int>(h);
    if (v >= base && v < base + 7) return v - base;
    return -1;
}

inline Hit EventHit(int i) { return static_cast<Hit>(static_cast<int>(Hit::EventBase) + i); }

// ---------------------------------------------------------------- 绘制工具

inline Gdiplus::Color Rgba(BYTE a, BYTE r, BYTE g, BYTE b) { return Gdiplus::Color(a, r, g, b); }

inline void FillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                      const Gdiplus::Color& c) {
    Gdiplus::GraphicsPath path;
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(c);
    g.FillPath(&brush, &path);
}

// 文字：统一走这里，保证行高对齐方式一致（垂直居中 + 单行省略）。
inline void Text(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::RectF& box,
                 float size, bool bold, const Gdiplus::Color& c,
                 Gdiplus::StringAlignment align = Gdiplus::StringAlignmentNear) {
    if (s.empty()) return;
    // FontFamily 不能拷贝赋值，所以用「先选一个可用的名字」再构造一次。
    const wchar_t* name = L"Microsoft YaHei UI";
    {
        Gdiplus::FontFamily probe(name);
        if (probe.GetLastStatus() != Gdiplus::Ok || !probe.IsAvailable()) name = L"Segoe UI";
    }
    Gdiplus::FontFamily family(name);
    if (family.GetLastStatus() != Gdiplus::Ok) return;
    Gdiplus::Font font(&family, size, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    if (font.GetLastStatus() != Gdiplus::Ok) return;
    Gdiplus::StringFormat fmt;
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush brush(c);
    g.DrawString(s.c_str(), -1, &font, box, &fmt, &brush);
}

// 开关：44×24 的药丸 + 一个会随状态滑动的圆点。
// 不做动画 —— 设置面板是"看一眼就走"的地方，动效只会拖慢点击反馈。
inline void Toggle(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, bool on, bool hover,
                   float scale) {
    const float radius = rc.Height * 0.5f;
    const Gdiplus::Color track =
        on ? Rgba(255, color::kAccentR, color::kAccentG, color::kAccentB)
           : Rgba(static_cast<BYTE>(hover ? 90 : 64), 255, 255, 255);
    FillRound(g, rc, radius, track);
    const float knob = rc.Height - 6.0f * scale;
    const float x = on ? (rc.X + rc.Width - knob - 3.0f * scale) : (rc.X + 3.0f * scale);
    Gdiplus::SolidBrush white(Rgba(255, 255, 255, 255));
    g.FillEllipse(&white, x, rc.Y + 3.0f * scale, knob, knob);
}

// 分段选择器：整体一个圆角槽，选中项是一块更亮的圆角片。
// 每段都可以带一个"示意的图形"，这样透明度和位置不用读文字也能看懂。
enum class SegIcon { None, Opacity, Anchor };

inline void Segment(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, int count, int selected,
                    int hover_index, float scale, SegIcon icon) {
    FillRound(g, rc, rc.Height * 0.5f, Rgba(56, 255, 255, 255));
    const float gap = 3.0f * scale;
    const float w = (rc.Width - gap * 2.0f) / static_cast<float>(count);
    for (int i = 0; i < count; ++i) {
        const Gdiplus::RectF cell(rc.X + gap + static_cast<float>(i) * w, rc.Y + gap,
                                  w - gap, rc.Height - gap * 2.0f);
        const bool on = i == selected;
        if (on) {
            FillRound(g, cell, cell.Height * 0.5f, Rgba(255, 255, 255, 255));
        } else if (i == hover_index) {
            FillRound(g, cell, cell.Height * 0.5f, Rgba(30, 255, 255, 255));
        }

        // 示意图形：画在格子中央偏左，文字在右。
        float text_x = cell.X + 8.0f * scale;
        if (icon != SegIcon::None) {
            const float ix = cell.X + 9.0f * scale;
            const float iy = cell.Y + cell.Height * 0.5f;
            if (icon == SegIcon::Opacity) {
                // 三个小方块：空心 / 半透 / 实心，直接对应三档。
                const Gdiplus::RectF box(ix, iy - 5.0f * scale, 14.0f * scale, 10.0f * scale);
                if (i == 0) {
                    // 液态玻璃：浅透底 + 一圈亮边。**不能画成空心** ——
                    // 空心读成"什么都没有"，而这一档恰恰是有材质的，只是很薄。
                    // 选中格子的底是白的，浅色图标压上去等于消失 —— 必须反色。
                    FillRound(g, box, 2.0f * scale,
                              on ? Rgba(58, 44, 48, 56) : Rgba(96, 255, 255, 255));
                    Gdiplus::GraphicsPath lp;
                    const Gdiplus::RectF ib(box.X + 0.5f * scale, box.Y + 0.5f * scale,
                                            box.Width - 1.0f * scale, box.Height - 1.0f * scale);
                    const float d = 2.0f * 2.0f * scale;
                    lp.AddArc(ib.X, ib.Y, d, d, 180.0f, 90.0f);
                    lp.AddArc(ib.X + ib.Width - d, ib.Y, d, d, 270.0f, 90.0f);
                    lp.AddArc(ib.X + ib.Width - d, ib.Y + ib.Height - d, d, d, 0.0f, 90.0f);
                    lp.AddArc(ib.X, ib.Y + ib.Height - d, d, d, 90.0f, 90.0f);
                    lp.CloseFigure();
                    Gdiplus::Pen pen(on ? Rgba(150, 44, 48, 56) : Rgba(205, 255, 255, 255),
                                     1.1f * scale);
                    g.DrawPath(&pen, &lp);
                } else if (i == 1) {
                    FillRound(g, box, 2.0f * scale, Rgba(on ? 170 : 140, 60, 60, 70));
                } else {
                    FillRound(g, box, 2.0f * scale, Rgba(235, 30, 30, 36));
                }
            } else {
                // 一个代表屏幕的小框 + 一个点，点在左/中/右。
                const Gdiplus::RectF box(ix, iy - 6.0f * scale, 20.0f * scale, 12.0f * scale);
                Gdiplus::Pen pen(Rgba(150, 255, 255, 255), 1.0f * scale);
                g.DrawRectangle(&pen, box.X, box.Y, box.Width, box.Height);
                const float dot = 3.0f * scale;
                float cx = box.X + box.Width * 0.5f;
                if (i == 0) cx = box.X + dot;
                if (i == 2) cx = box.X + box.Width - dot;
                Gdiplus::SolidBrush b(on ? Rgba(255, color::kAccentR, color::kAccentG, color::kAccentB)
                                         : Rgba(200, 255, 255, 255));
                g.FillEllipse(&b, cx - dot * 0.5f, iy - dot * 0.5f, dot, dot);
            }
            text_x = ix + 26.0f * scale;
        }
        static const wchar_t* kLabels[2][3] = {
            {L"液态玻璃", L"均衡", L"不透明"},
            {L"靠左", L"居中", L"靠右"},
        };
        const wchar_t* label = (icon == SegIcon::Opacity) ? kLabels[0][i]
                              : (icon == SegIcon::Anchor) ? kLabels[1][i]
                                                          : L"";
        if (label[0] != 0) {
            Text(g, label,
                 Gdiplus::RectF(text_x, cell.Y, cell.GetRight() - text_x - 4.0f * scale,
                                cell.Height),
                 11.5f * scale, on, on ? Rgba(255, 20, 20, 26) : Rgba(225, 255, 255, 255));
        }
    }
}

// 一行「标题 + 副标题 + 右侧控件」。
inline void RowText(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const wchar_t* title,
                    const wchar_t* sub, float scale) {
    const bool has_sub = sub != nullptr && sub[0] != 0;
    const float title_y = has_sub ? rc.Y + rc.Height * 0.5f - 15.0f * scale
                                  : rc.Y + rc.Height * 0.5f - 9.0f * scale;
    Text(g, title, Gdiplus::RectF(rc.X, title_y, rc.Width, 20.0f * scale), 12.5f * scale, false,
         Rgba(255, color::kTextR, color::kTextG, color::kTextB));
    if (has_sub) {
        Text(g, sub, Gdiplus::RectF(rc.X, title_y + 19.0f * scale, rc.Width, 17.0f * scale),
             10.0f * scale, false, Rgba(220, color::kSubR, color::kSubG, color::kSubB));
    }
}

inline void SectionTitle(Gdiplus::Graphics& g, const wchar_t* text, float x, float y,
                         float scale) {
    // 10.5px 的小字在深底上到 (130,132,146) 只有约 3.9:1，达不到 AA 的 4.5:1，
    // 章节标题是层级线索，不能比正文更糊。
    Text(g, text, Gdiplus::RectF(x, y, 400.0f * scale, 18.0f * scale), 10.5f * scale, true,
         Rgba(240, 152, 155, 172));
}

inline void Hairline(Gdiplus::Graphics& g, float x0, float x1, float y) {
    Gdiplus::Pen pen(Rgba(20, 255, 255, 255), 1.0f);
    g.DrawLine(&pen, x0, y, x1, y);
}

// 按钮：主按钮是强调色实心，次按钮是浅描边。
inline void Button(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const wchar_t* text, bool primary,
                   bool hover, float scale, bool disabled = false) {
    const float radius = rc.Height * 0.5f;
    if (disabled) {
        // 禁用态：暗底灰字。调用方不会为它生成命中区，画出来只是如实告知
        //「这个功能还没启用」，不是留一个点了没反应的假按钮。
        FillRound(g, rc, radius, Rgba(20, 255, 255, 255));
        Text(g, text, rc, 12.0f * scale, false,
             Rgba(90, color::kSubR, color::kSubG, color::kSubB),
             Gdiplus::StringAlignmentCenter);
        return;
    }
    if (primary) {
        FillRound(g, rc, radius,
                  Rgba(static_cast<BYTE>(hover ? 255 : 235), color::kAccentR, color::kAccentG,
                       color::kAccentB));
    } else {
        FillRound(g, rc, radius, Rgba(static_cast<BYTE>(hover ? 60 : 34), 255, 255, 255));
    }
    Text(g, text, rc, 12.0f * scale, primary,
         primary ? Rgba(255, 255, 255, 255) : Rgba(230, color::kTextR, color::kTextG, color::kTextB),
         Gdiplus::StringAlignmentCenter);
}

// ---------------------------------------------------------------- 分页辅助
inline Hit HitAt(Hit base, int i) {
    return static_cast<Hit>(static_cast<int>(base) + i);
}

inline Gdiplus::RectF ToRectF(const RECT& r) {
    return Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                          static_cast<float>(r.right - r.left),
                          static_cast<float>(r.bottom - r.top));
}

// 布局与命中同一份 zones：绘制按需查矩形，不再另算一套坐标。
inline const RECT* FindRect(const State& st, Hit h) {
    for (const auto& z : st.zones) {
        if (z.hit == h) return &z.rc;
    }
    return nullptr;
}

// 一段多选控件的整体矩形（首尾两段合并）。
inline Gdiplus::RectF UnionSeg(const State& st, Hit base, int count) {
    const RECT* a = FindRect(st, base);
    const RECT* b = FindRect(st, HitAt(base, count - 1));
    if (a == nullptr || b == nullptr) return Gdiplus::RectF();
    return Gdiplus::RectF(static_cast<float>(a->left), static_cast<float>(a->top),
                          static_cast<float>(b->right - a->left),
                          static_cast<float>(a->bottom - a->top));
}

// 当前悬停落在第几段（不在该控件上返回 -1）。
inline int SegHover(const State& st, Hit base, int count) {
    const int v = static_cast<int>(st.hover);
    const int b = static_cast<int>(base);
    return (v >= b && v < b + count) ? v - b : -1;
}

// 一行的「中文标题 + 一行说明」，以右侧控件的中心垂直对齐。
inline void DrawRowLabel(Gdiplus::Graphics& g, const RECT& anchor, const wchar_t* title,
                         const wchar_t* sub, float scale, float pad, float content_w) {
    const float row_h = 46.0f * scale;
    const float cy = static_cast<float>(anchor.top + anchor.bottom) * 0.5f;
    RowText(g, Gdiplus::RectF(pad, cy - row_h * 0.5f, content_w * 0.5f, row_h),
            title, sub, scale);
}

// 纯文字版分段选择器：沿用 Segment 的圆角槽 + 选中白片，只是标签由调用方给。
inline void LabeledSegment(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, int count,
                           int selected, int hover_index, float scale,
                           const wchar_t* const* labels) {
    if (count <= 0) return;
    FillRound(g, rc, rc.Height * 0.5f, Rgba(56, 255, 255, 255));
    const float gap = 3.0f * scale;
    const float w = (rc.Width - gap * 2.0f) / static_cast<float>(count);
    for (int i = 0; i < count; ++i) {
        const Gdiplus::RectF cell(rc.X + gap + static_cast<float>(i) * w, rc.Y + gap,
                                  w - gap, rc.Height - gap * 2.0f);
        const bool on = i == selected;
        if (on) {
            FillRound(g, cell, cell.Height * 0.5f, Rgba(255, 255, 255, 255));
        } else if (i == hover_index) {
            FillRound(g, cell, cell.Height * 0.5f, Rgba(30, 255, 255, 255));
        }
        Text(g, labels[i], cell, 11.5f * scale, on,
             on ? Rgba(255, 20, 20, 26) : Rgba(225, 255, 255, 255),
             Gdiplus::StringAlignmentCenter);
    }
}

// − [数值] + 的步进器：数值显示在两个按钮之间的空隙。
inline void DrawStepper(Gdiplus::Graphics& g, const State& st, Hit dec, Hit inc,
                        const std::wstring& value, float scale) {
    const RECT* d = FindRect(st, dec);
    const RECT* i = FindRect(st, inc);
    if (d == nullptr || i == nullptr) return;
    const Gdiplus::Color dim = Rgba(255, color::kTextR, color::kTextG, color::kTextB);
    const Gdiplus::RectF mr = ToRectF(*d);
    const Gdiplus::RectF pr = ToRectF(*i);
    FillRound(g, mr, mr.Height * 0.5f,
              Rgba(static_cast<BYTE>(st.hover == dec ? 70 : 38), 255, 255, 255));
    Text(g, L"−", mr, 14.0f * scale, false, dim, Gdiplus::StringAlignmentCenter);
    FillRound(g, pr, pr.Height * 0.5f,
              Rgba(static_cast<BYTE>(st.hover == inc ? 70 : 38), 255, 255, 255));
    Text(g, L"+", pr, 14.0f * scale, false, dim, Gdiplus::StringAlignmentCenter);
    const float vx = static_cast<float>(d->right) + 3.0f * scale;
    const float vw = static_cast<float>(i->left - d->right) - 6.0f * scale;
    Text(g, value,
         Gdiplus::RectF(vx, static_cast<float>(d->top), vw,
                        static_cast<float>(d->bottom - d->top)),
         12.0f * scale, true, dim, Gdiplus::StringAlignmentCenter);
}

// 强调色预设色块：选中时套一圈白环，悬停时轻微提亮。
inline void DrawAccentSwatches(Gdiplus::Graphics& g, const State& st, float scale) {
    for (int i = 0; i < kAccentCount; ++i) {
        const RECT* r = FindRect(st, HitAt(Hit::Accent0, i));
        if (r == nullptr) continue;
        const Gdiplus::RectF rc = ToRectF(*r);
        const bool on = st.cfg.accent == kAccents[i].hex;
        if (on) {
            const Gdiplus::RectF ring(rc.X - 3.0f * scale, rc.Y - 3.0f * scale,
                                      rc.Width + 6.0f * scale, rc.Height + 6.0f * scale);
            FillRound(g, ring, ring.Height * 0.5f, Rgba(255, 255, 255, 255));
        } else if (st.hover == HitAt(Hit::Accent0, i)) {
            const Gdiplus::RectF ring(rc.X - 2.0f * scale, rc.Y - 2.0f * scale,
                                      rc.Width + 4.0f * scale, rc.Height + 4.0f * scale);
            FillRound(g, ring, ring.Height * 0.5f, Rgba(70, 255, 255, 255));
        }
        FillRound(g, rc, rc.Height * 0.5f,
                  Rgba(255, kAccents[i].r, kAccents[i].g, kAccents[i].b));
    }
}

// 两列开关：整格可点，开关贴在每格右端，与「响应哪些操作」同一手感。
inline void DrawTwoColToggles(Gdiplus::Graphics& g, const State& st, const Hit* hits,
                              const wchar_t* const* labels, const wchar_t* const* subs,
                              const bool* ons, int n, float scale) {
    for (int k = 0; k < n; ++k) {
        const RECT* r = FindRect(st, hits[k]);
        if (r == nullptr) continue;
        const float tw = 44.0f * scale;
        const float th = 24.0f * scale;
        const Gdiplus::RectF tr(
            static_cast<float>(r->right) - tw,
            static_cast<float>(r->top) +
                (static_cast<float>(r->bottom - r->top) - th) * 0.5f,
            tw, th);
        const float text_w = static_cast<float>(r->right - r->left) - tw - 12.0f * scale;
        RowText(g, Gdiplus::RectF(static_cast<float>(r->left), static_cast<float>(r->top),
                                  text_w, static_cast<float>(r->bottom - r->top)),
                labels[k], subs[k], scale);
        Toggle(g, tr, ons[k], st.hover == hits[k], scale);
    }
}

// 分页 Tab：选中为强调色实心，悬停为浅底。
inline void DrawTab(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const wchar_t* label,
                    bool selected, bool hover, float scale) {
    if (selected) {
        FillRound(g, rc, rc.Height * 0.5f,
                  Rgba(255, color::kAccentR, color::kAccentG, color::kAccentB));
    } else if (hover) {
        FillRound(g, rc, rc.Height * 0.5f, Rgba(34, 255, 255, 255));
    }
    Text(g, label, rc, 12.5f * scale, selected,
         selected ? Rgba(255, 255, 255, 255)
                  : Rgba(205, color::kTextR, color::kTextG, color::kTextB),
         Gdiplus::StringAlignmentCenter);
}

// ---------------------------------------------------------------- 布局
//
// 布局与命中**同一份数据**：BuildZones() 一次算出所有矩形，Paint() 只照着画，
// 鼠标命中只查这张表。分两处算的话，只要有一处改了就必然错位 ——
// 「看得见点不到」这类 bug 全是这么来的。
//
// 返回值是这个分页真正需要的内容高度（含底部按钮与路径那一行）。
// 四页内容长短差很多，固定高度会在短页留一大片空白，所以窗口高度跟着它走。
inline int BuildZones(State& st, std::vector<Zone>& zones) {
    zones.clear();
    const int dpi = st.dpi;
    const int pad = S(layout::kPad, dpi);
    const int width = st.width;
    const int content_w = width - pad * 2;

    // 标题区（同时也是拖动区）与关闭 ×
    zones.push_back({Hit::TitleBar, {0, 0, width, S(84, dpi)}});
    zones.push_back({Hit::Close, {width - pad - S(24, dpi), S(22, dpi), width - pad, S(46, dpi)}});

    // 四个分页 Tab：紧贴标题下方，等宽平分内容区。每页重建 zones 时都会重新加入，
    // 所以 Tab 在四页里始终可点。
    const int tab_y = S(90, dpi);
    const int tab_h = S(32, dpi);
    const int tab_w = content_w / kPageCount;
    for (int i = 0; i < kPageCount; ++i) {
        const int x0 = pad + i * tab_w;
        zones.push_back({HitAt(Hit::TabAppearance, i), {x0, tab_y, x0 + tab_w, tab_y + tab_h}});
    }

    const int seg_w = S(300, dpi);
    const int seg_x = width - pad - seg_w;
    const int seg_h = S(34, dpi);
    const int row_step = S(46, dpi);  // 单列行步进
    const int col_h = S(38, dpi);     // 两列行步进

    // 一段多选控件：每段一个 zone，整体右对齐宽 seg_w（同组 Hit 连续，base+i 分派）。
    auto add_segment = [&](Hit base, int count, int yy) {
        const int cw = seg_w / count;
        for (int i = 0; i < count; ++i) {
            zones.push_back({HitAt(base, i),
                             {seg_x + i * cw, yy, seg_x + (i + 1) * cw, yy + seg_h}});
        }
    };
    // 单个开关：右端 48 宽。
    auto add_toggle = [&](Hit h, int yy) {
        const int tw = S(48, dpi);
        zones.push_back({h, {width - pad - tw, yy, width - pad, yy + S(26, dpi)}});
    };
    // − [数值] + 步进器：右对齐，两按钮之间留出数值文本空隙（不可点）。
    auto add_stepper = [&](Hit dec, Hit inc, int yy) {
        const int btn = S(26, dpi);
        const int gap = S(4, dpi);
        const int text_w = S(56, dpi);
        const int total = btn * 2 + gap * 2 + text_w;
        int x = width - pad - total;
        zones.push_back({dec, {x, yy, x + btn, yy + btn}});
        x += btn + gap + text_w + gap;
        zones.push_back({inc, {x, yy, x + btn, yy + btn}});
    };
    // 强调色色块：右对齐一排。
    auto add_swatches = [&](Hit base, int count, int yy) {
        const int sw = S(30, dpi);
        const int gap = S(8, dpi);
        const int total = count * sw + (count - 1) * gap;
        int x = width - pad - total;
        for (int i = 0; i < count; ++i) {
            zones.push_back({HitAt(base, i), {x, yy, x + sw, yy + sw}});
            x += sw + gap;
        }
    };
    // 两列开关：整格可点，左列右端留 60px 避免两列贴死（与旧版事件区一致）。
    auto add_two_col = [&](const Hit* hits, int n, int yy, int rh) {
        const int col_w = content_w / 2;
        for (int i = 0; i < n; ++i) {
            const int col = i % 2;
            const int row = i / 2;
            const int cy = yy + row * rh;
            const int left = col == 0 ? pad : pad + col_w;
            const int right = col == 0 ? pad + col_w - S(60, dpi) : width - pad;
            zones.push_back({hits[i], {left, cy, right, cy + S(26, dpi)}});
        }
        return yy + ((n + 1) / 2) * rh;
    };

    int y = S(132, dpi);  // 内容区起点（Tab 下方）
    const std::size_t content_begin = zones.size();  // 之后的区域都跟着滚动走
    switch (st.page) {
        case 0: {  // ---- 外观：材质 / 位置 / 玻璃 + 主题 / 强调色 / 缩放 / 字号 / 圆角 / 间距 ----
            y += S(22, dpi);  // 「外观」分组标题占位
            add_segment(Hit::Opacity0, 3, y); y += row_step;
            add_segment(Hit::Anchor0, 3, y); y += row_step;
            add_toggle(Hit::Glass, y); y += row_step;
            add_segment(Hit::Theme0, 3, y); y += row_step;
            add_swatches(Hit::Accent0, kAccentCount, y); y += row_step;
            add_stepper(Hit::UiScaleDec, Hit::UiScaleInc, y); y += row_step;
            add_stepper(Hit::FontOffsetDec, Hit::FontOffsetInc, y); y += row_step;
            add_stepper(Hit::CornerDec, Hit::CornerInc, y); y += row_step;
            add_stepper(Hit::TopOffsetDec, Hit::TopOffsetInc, y); y += row_step;
            break;
        }
        case 1: {  // ---- 行为：响应哪些操作 + 其他 + 自动隐藏与让路 ----
            y += S(22, dpi);  // 「响应哪些操作」分组标题
            {
                Hit hits[7];
                for (int i = 0; i < 7; ++i) hits[i] = EventHit(i);
                y = add_two_col(hits, 7, y, col_h);
            }
            y += S(14, dpi);
            y += S(20, dpi);  // 「其他」分组标题
            add_toggle(Hit::Fullscreen, y); y += row_step;
            add_toggle(Hit::Secret, y); y += row_step;
            add_toggle(Hit::QuietOn, y); y += S(30, dpi);
            {
                // 免打扰起止时间：两段 −/+ 各占一块，中间夹时间文本（不可点）。
                const int time_y = y;
                const int tw = S(26, dpi);
                const int gap = S(4, dpi);
                const int clock_w = S(58, dpi);
                int tx = pad;
                zones.push_back({Hit::QuietFromDec, {tx, time_y, tx + tw, time_y + tw}});
                tx += tw + gap + clock_w + gap;
                zones.push_back({Hit::QuietFromInc, {tx, time_y, tx + tw, time_y + tw}});
                tx += tw + S(22, dpi);  // 「到」
                zones.push_back({Hit::QuietToDec, {tx, time_y, tx + tw, time_y + tw}});
                tx += tw + gap + clock_w + gap;
                zones.push_back({Hit::QuietToInc, {tx, time_y, tx + tw, time_y + tw}});
                y += S(38, dpi);
            }
            zones.push_back({Hit::OpenConfig, {pad, y, pad + S(280, dpi), y + S(32, dpi)}});
            y += S(46, dpi);
            y += S(12, dpi);
            y += S(20, dpi);  // 「自动隐藏与让路」分组标题
            {
                static const Hit kHits[6] = {
                    Hit::AutoHideFocus, Hit::AutoHidePaused, Hit::AutoHideFullscreen,
                    Hit::YieldEnabled, Hit::ReduceMotion, Hit::LowPowerDowngrade};
                y = add_two_col(kHits, 6, y, col_h);
            }
            add_segment(Hit::QuietForm0, 3, y); y += row_step;
            add_segment(Hit::YieldKey0, 3, y); y += row_step;
            break;
        }
        case 2: {  // ---- 性能 ----
            y += S(22, dpi);  // 「性能」分组标题
            add_segment(Hit::GlassQuality0, 4, y); y += row_step;
            add_stepper(Hit::CaptureRefreshDec, Hit::CaptureRefreshInc, y); y += row_step;
            add_segment(Hit::TargetMonitor0, 3, y); y += row_step;
            add_stepper(Hit::MonitorIndexDec, Hit::MonitorIndexInc, y); y += row_step;
            break;
        }
        case 3: {  // ---- 预设：2×2 卡片 + 恢复出厂 ----
            y += S(22, dpi);
            {
                const int gap = S(12, dpi);
                const int cw = (content_w - gap) / 2;
                const int ch = S(78, dpi);
                for (int i = 0; i < 4; ++i) {
                    const int col = i % 2;
                    const int row = i / 2;
                    const int x = pad + col * (cw + gap);
                    const int cy = y + row * (ch + gap);
                    zones.push_back({HitAt(Hit::Preset0, i), {x, cy, x + cw, cy + ch}});
                }
                y += 2 * (ch + gap);
            }
            zones.push_back({Hit::PresetReset, {pad, y, pad + S(150, dpi), y + S(34, dpi)}});
            y += S(34, dpi);
            break;
        }
        default: break;
    }

    // ---- 高度、视口与滚动 ----
    //
    // st.height == 0 表示「按内容自然高度开窗」（第一次调用时用这个探高度）；
    // 否则按给定高度排，并把放不下的内容交给滚动。
    const int btn_h = S(36, dpi);
    const int footer_h = S(18, dpi) + btn_h + S(42, dpi);  // 按钮 + 底部路径那一行
    const int natural_h = y + footer_h;
    int window_h = (st.height > 0) ? st.height : natural_h;
    if (st.max_height > 0 && window_h > st.max_height) window_h = st.max_height;

    const int view_top = S(132, dpi);
    const int btn_y = window_h - S(42, dpi) - btn_h;
    const int view_bottom = btn_y - S(14, dpi);
    st.view_top = view_top;
    st.view_h = (std::max)(view_bottom - view_top, 0);
    st.max_scroll = (std::max)(0, (y - view_top) - st.view_h);
    if (st.scroll > st.max_scroll) st.scroll = st.max_scroll;
    if (st.scroll < 0) st.scroll = 0;

    // 内容区整体上移，并裁到视口内 —— 不裁的话滚出去的行会盖住底部按钮，
    // 于是「点保存」变成点到一个已经看不见的开关。
    for (std::size_t i = content_begin; i < zones.size(); ++i) {
        RECT& r = zones[i].rc;
        r.top -= st.scroll;
        r.bottom -= st.scroll;
        if (r.top < view_top) r.top = view_top;
        if (r.bottom > view_bottom) r.bottom = view_bottom;
    }
    zones.erase(std::remove_if(zones.begin() + static_cast<std::ptrdiff_t>(content_begin),
                               zones.end(),
                               [](const Zone& z) { return z.rc.bottom <= z.rc.top; }),
                zones.end());

    // 按钮钉在窗口底部，与滚动无关。
    zones.push_back({Hit::Cancel,
                     {width - pad - S(180, dpi), btn_y, width - pad - S(96, dpi),
                      btn_y + btn_h}});
    zones.push_back({Hit::Save,
                     {width - pad - S(88, dpi), btn_y, width - pad, btn_y + btn_h}});
    return window_h;
}

// 切页后重算矩形，并把窗口高度调到这一页真正需要的高度（受 max_height 限制）。
inline void Relayout(HWND hwnd, State& st) {
    st.scroll = 0;
    st.height = 0;
    const int natural = BuildZones(st, st.zones);  // 第一遍：只求自然高度
    st.height = (st.max_height > 0 && natural > st.max_height) ? st.max_height : natural;
    BuildZones(st, st.zones);  // 第二遍：按最终高度排，必要时启用滚动
    if (hwnd == nullptr) return;
    ::SetWindowPos(hwnd, nullptr, 0, 0, st.width, st.height,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    const int r = S(18, st.dpi);
    HRGN region = ::CreateRoundRectRgn(0, 0, st.width + 1, st.height + 1, r * 2, r * 2);
    if (region != nullptr) ::SetWindowRgn(hwnd, region, TRUE);  // 接管所有权，不删
}

// ---------------------------------------------------------------- 预设
//
// 预设只改「外观与行为」，绝不碰事件开关 / 黑名单 / 代理 / Agent 读取权限 ——
// 那些是用户自己的决定，被「换个皮肤」顺手改掉会很恼人，而且难以察觉。
struct PresetInfo {
    const wchar_t* name;
    const char* key;  // 存进 cfg.preset_name 的键（UTF-8）
    const wchar_t* desc;
};

static const PresetInfo kPresets[4] = {
    {L"极简", "极简", L"收成一条，几乎不出声"},
    {L"均衡", "均衡", L"默认：该出现时才出现"},
    {L"沉浸", "沉浸", L"常驻显示，玻璃更透"},
    {L"节能", "节能", L"降帧降质，笔记本省电"},
};

inline int StepCorner(int cur, int delta) {
    // 0 是哨兵值「用范式默认 28」。往下退到 8 以下就回到 0，而不是卡在 8……
    if (cur == 0) return delta > 0 ? 8 : 0;
    const int next = cur + delta;
    if (next < 8) return 0;
    return next > 40 ? 40 : next;
}

// idx: 0..3 四个预设；-1 = 恢复出厂。
inline void ApplyPreset(RuleConfig& c, int idx) {
    // 先把「不该被预设改动」的字段抄出来，重置后原样还回去。
    const auto app_block = c.app_block;
    const auto app_allow = c.app_allow;
    const auto ext_block = c.ext_block;
    const auto ext_allow = c.ext_allow;
    const EventToggles events = c.events;
    const ClipboardRule clipboard = c.clipboard;
    const QuietHours quiet = c.quiet;
    const std::string proxy = c.lyrics_proxy;
    const bool agents = c.agents;
    const bool browsers = c.browsers;
    const bool meetings = c.meetings;
    const bool editors = c.editors;
    const int agent_max = c.agent_max;
    const int agent_idle = c.agent_idle_sec;
    const bool simplify = c.simplify_chinese;
    const bool lyrics = c.lyrics;
    const bool sleep_bar = c.sleep_bar;
    const int auto_sleep = c.auto_sleep_sec;

    c = RuleConfig();

    c.app_block = app_block;
    c.app_allow = app_allow;
    c.ext_block = ext_block;
    c.ext_allow = ext_allow;
    c.events = events;
    c.clipboard = clipboard;
    c.quiet = quiet;
    c.lyrics_proxy = proxy;
    c.agents = agents;
    c.browsers = browsers;
    c.meetings = meetings;
    c.editors = editors;
    c.agent_max = agent_max;
    c.agent_idle_sec = agent_idle;
    c.simplify_chinese = simplify;
    c.lyrics = lyrics;
    c.sleep_bar = sleep_bar;
    c.auto_sleep_sec = auto_sleep;

    if (idx < 0 || idx >= 4) {  // 恢复出厂：上面那批字段保持原值，其余回默认
        c.preset_name = "默认";
        return;
    }
    c.preset_name = kPresets[idx].key;
    switch (idx) {
        case 0:  // 极简
            c.opacity = 2;
            c.glass = false;
            c.ui_scale = 90;
            c.font_offset = -1;
            c.corner_radius = 8;
            c.top_offset = 4;
            c.quiet_form = 0;
            c.auto_hide_focus = true;
            c.auto_hide_paused = true;
            c.auto_hide_fullscreen = true;
            c.yield_enabled = true;
            c.reduce_motion = true;
            c.low_power_downgrade = true;
            c.glass_quality = 0;
            c.capture_refresh_ms = 4000;
            break;
        case 1:  // 均衡 = 出厂外观值（restore 之后本来就是默认，这里只写清意图）
            break;
        case 2:  // 沉浸
            c.opacity = 0;
            c.glass = true;
            c.ui_scale = 105;
            c.font_offset = 1;
            c.corner_radius = 20;
            c.quiet_form = 1;
            c.auto_hide_focus = false;
            c.auto_hide_paused = false;
            c.auto_hide_fullscreen = false;
            c.yield_enabled = false;
            c.reduce_motion = false;
            c.low_power_downgrade = false;
            c.glass_quality = 3;
            c.capture_refresh_ms = 1000;
            break;
        case 3:  // 节能
            c.opacity = 2;
            c.glass = false;
            c.ui_scale = 95;
            c.font_offset = 0;
            c.corner_radius = 8;
            c.quiet_form = 1;
            c.auto_hide_focus = true;
            c.auto_hide_paused = true;
            c.auto_hide_fullscreen = true;
            c.yield_enabled = true;
            c.reduce_motion = true;
            c.low_power_downgrade = true;
            c.glass_quality = 0;
            c.capture_refresh_ms = 6000;
            break;
        default: break;
    }
}

inline int CurrentPreset(const RuleConfig& c) {
    for (int i = 0; i < 4; ++i) {
        if (c.preset_name == kPresets[i].key) return i;
    }
    return c.preset_name == "默认" ? 1 : -1;
}

// ---------------------------------------------------------------- 绘制
//
// 这里不出现任何坐标常量：所有矩形都来自 zones 表（FindRect / UnionSeg）。
// 「画的坐标」和「点的坐标」一旦分成两份，迟早有一份忘记同步 ——
// v0.3.0 就是这样：控件进了布局表却没人画，屏幕上只剩三个旧控件。
// probes/settings_probe.cpp 现在会逐块比像素，专抓「有命中区但没画出来」。

inline float LabelWidth(const State& st, Hit anchor, float pad, float scale) {
    const RECT* r = FindRect(st, anchor);
    if (r == nullptr) return 0.0f;
    const float w = static_cast<float>(r->left) - pad - 12.0f * scale;
    return w > 0.0f ? w : 0.0f;
}

// 标签列宽一路算到控件左沿 —— 写死比例就会把副标题截断（曾经如此）。
inline void RowLabel(Gdiplus::Graphics& g, const State& st, Hit anchor, float pad, float scale,
                     const wchar_t* title, const wchar_t* sub) {
    const RECT* r = FindRect(st, anchor);
    if (r == nullptr) return;
    const float row_h = 46.0f * scale;
    const float cy = static_cast<float>(r->top + r->bottom) * 0.5f;
    RowText(g, Gdiplus::RectF(pad, cy - row_h * 0.5f, LabelWidth(st, anchor, pad, scale), row_h),
            title, sub, scale);
}

// 开关贴在命中区右端：命中区比开关大（整行可点），这是设置界面该有的手感 ——
// 没人愿意去瞄一个 44×24 的小方块。
inline void ToggleAt(Gdiplus::Graphics& g, const RECT& r, bool on, bool hover, float scale) {
    const float tw = 44.0f * scale;
    const float th = 24.0f * scale;
    const Gdiplus::RectF rc(
        static_cast<float>(r.right) - tw - 2.0f * scale,
        static_cast<float>(r.top) + (static_cast<float>(r.bottom - r.top) - th) * 0.5f, tw, th);
    Toggle(g, rc, on, hover, scale);
}

inline void ToggleRow(Gdiplus::Graphics& g, const State& st, Hit h, bool on,
                      const wchar_t* title, const wchar_t* sub, float pad, float scale) {
    RowLabel(g, st, h, pad, scale, title, sub);
    const RECT* r = FindRect(st, h);
    if (r != nullptr) ToggleAt(g, *r, on, st.hover == h, scale);
}

inline void SegmentRow(Gdiplus::Graphics& g, const State& st, Hit base, int count, int selected,
                       const wchar_t* const* labels, SegIcon icon, const wchar_t* title,
                       const wchar_t* sub, float pad, float scale) {
    RowLabel(g, st, base, pad, scale, title, sub);
    const Gdiplus::RectF rc = UnionSeg(st, base, count);
    if (rc.Width <= 0.0f) return;
    const int hover = SegHover(st, base, count);
    if (icon == SegIcon::None) {
        const wchar_t* const* safe = labels;
        static const wchar_t* const kEmpty[4] = {L"", L"", L"", L""};
        if (safe == nullptr) safe = kEmpty;
        LabeledSegment(g, rc, count, selected, hover, scale, safe);
    } else {
        Segment(g, rc, count, selected, hover, scale, icon);
    }
}

inline void StepperRow(Gdiplus::Graphics& g, const State& st, Hit dec, Hit inc,
                       const std::wstring& value, const wchar_t* title, const wchar_t* sub,
                       float pad, float scale, bool enabled = true) {
    RowLabel(g, st, dec, pad, scale, title, sub);
    if (enabled) {
        DrawStepper(g, st, dec, inc, value, scale);
        return;
    }
    // 不生效时画成「灰掉的步进器」而不是干脆不画：形状还在，说明这一行确实存在，
    // 只是要先满足上面的条件。比留一个点了没反应的按钮诚实，也比凭空消失好懂。
    const RECT* d = FindRect(st, dec);
    const RECT* i = FindRect(st, inc);
    if (d == nullptr || i == nullptr) return;
    const Gdiplus::Color faded = Rgba(110, color::kSubR, color::kSubG, color::kSubB);
    const Gdiplus::RectF mr = ToRectF(*d);
    const Gdiplus::RectF pr = ToRectF(*i);
    FillRound(g, mr, mr.Height * 0.5f, Rgba(16, 255, 255, 255));
    Text(g, L"−", mr, 14.0f * scale, false, faded, Gdiplus::StringAlignmentCenter);
    FillRound(g, pr, pr.Height * 0.5f, Rgba(16, 255, 255, 255));
    Text(g, L"+", pr, 14.0f * scale, false, faded, Gdiplus::StringAlignmentCenter);
    const float vx = static_cast<float>(d->right) + 3.0f * scale;
    const float vw = static_cast<float>(i->left - d->right) - 6.0f * scale;
    Text(g, value,
         Gdiplus::RectF(vx, static_cast<float>(d->top), vw, static_cast<float>(d->bottom - d->top)),
         11.5f * scale, false, faded, Gdiplus::StringAlignmentCenter);
}

inline void SwatchRow(Gdiplus::Graphics& g, const State& st, const wchar_t* title,
                      const wchar_t* sub, float pad, float scale) {
    RowLabel(g, st, Hit::Accent0, pad, scale, title, sub);
    DrawAccentSwatches(g, st, scale);
}

inline std::wstring SignedW(int v) {
    return (v > 0 ? std::wstring(L"+") : std::wstring()) + std::to_wstring(v);
}
inline std::wstring PxText(int v) { return std::to_wstring(v) + L" px"; }
inline std::wstring CornerText(int v) { return v == 0 ? std::wstring(L"自动") : PxText(v); }
inline std::wstring MsText(int v) { return std::to_wstring(v) + L" ms"; }

inline void DrawTabs(Gdiplus::Graphics& g, const State& st, float scale) {
    for (int i = 0; i < kPageCount; ++i) {
        const Hit h = HitAt(Hit::TabAppearance, i);
        const RECT* r = FindRect(st, h);
        if (r == nullptr) continue;
        Gdiplus::RectF rc = ToRectF(*r);
        rc.X += 2.0f * scale;
        rc.Y += 3.0f * scale;
        rc.Width -= 4.0f * scale;
        rc.Height -= 6.0f * scale;
        DrawTab(g, rc, kTabLabels[i], st.page == i, st.hover == h, scale);
    }
}

inline void DrawPresetCard(Gdiplus::Graphics& g, const RECT& r, const PresetInfo& info,
                           bool selected, bool hover, float scale) {
    const Gdiplus::RectF rc = ToRectF(r);
    FillRound(g, rc, 12.0f * scale,
              selected ? Rgba(44, color::kAccentR, color::kAccentG, color::kAccentB)
                       : Rgba(static_cast<BYTE>(hover ? 40 : 22), 255, 255, 255));
    if (selected) {
        // 左侧一条强调色竖条：选中态同时靠色与形，色弱也能分辨。
        FillRound(g,
                  Gdiplus::RectF(rc.X + 12.0f * scale, rc.Y + 18.0f * scale, 3.0f * scale,
                                 rc.Height - 36.0f * scale),
                  1.5f * scale, Rgba(255, color::kAccentR, color::kAccentG, color::kAccentB));
    }
    Text(g, info.name, Gdiplus::RectF(rc.X + 24.0f * scale, rc.Y + 14.0f * scale,
                                      rc.Width - 38.0f * scale, 24.0f * scale),
         13.0f * scale, true, Rgba(255, color::kTextR, color::kTextG, color::kTextB));
    Text(g, info.desc, Gdiplus::RectF(rc.X + 24.0f * scale, rc.Y + 40.0f * scale,
                                      rc.Width - 38.0f * scale, 22.0f * scale),
         10.5f * scale, false, Rgba(214, color::kSubR, color::kSubG, color::kSubB));
}

// ---- 外观页 ----
inline void PaintAppearance(Gdiplus::Graphics& g, const State& st, float pad, float scale) {
    const RECT* first = FindRect(st, Hit::Opacity0);
    if (first == nullptr) return;
    SectionTitle(g, L"外观", pad, static_cast<float>(first->top) - 22.0f * scale, scale);

    SegmentRow(g, st, Hit::Opacity0, 3, st.cfg.opacity, nullptr, SegIcon::Opacity, L"材质",
               L"液态玻璃最透，均衡最稳，不透明任何壁纸下都读得清", pad, scale);
    SegmentRow(g, st, Hit::Anchor0, 3, st.cfg.anchor, nullptr, SegIcon::Anchor, L"位置",
               L"靠边时留 28px，永不越界", pad, scale);
    ToggleRow(g, st, Hit::Glass, st.cfg.glass, L"磨砂玻璃", L"透出背后的屏幕内容",
              pad, scale);
    SegmentRow(g, st, Hit::Theme0, 3, st.cfg.theme, kThemeLabels, SegIcon::None, L"主题",
               L"浅色面板配白底窗口，跟随系统按应用主题切换", pad, scale);
    SwatchRow(g, st, L"强调色", L"进度条、波形与选中态的颜色", pad, scale);
    StepperRow(g, st, Hit::UiScaleDec, Hit::UiScaleInc,
               std::to_wstring(st.cfg.ui_scale) + L"%", L"界面缩放",
               L"胶囊整体大小，80% – 140%", pad, scale);
    StepperRow(g, st, Hit::FontOffsetDec, Hit::FontOffsetInc, SignedW(st.cfg.font_offset),
               L"字号偏移", L"标题与副标题的字号微调", pad, scale);
    StepperRow(g, st, Hit::CornerDec, Hit::CornerInc, CornerText(st.cfg.corner_radius),
               L"圆角", L"「自动」= 按范式默认 28px", pad, scale);
    StepperRow(g, st, Hit::TopOffsetDec, Hit::TopOffsetInc, PxText(st.cfg.top_offset),
               L"距顶间距", L"胶囊离工作区顶边多远", pad, scale);
}

// ---- 行为页 ----
inline void PaintBehavior(Gdiplus::Graphics& g, const State& st, float pad, float scale) {
    static const wchar_t* kEventLabel[7] = {L"媒体播放", L"消息通知", L"复制 / 截图", L"文件落盘",
                                            L"音量变化", L"电源 / 电量", L"设备接入"};
    static const wchar_t* kEventSub[7] = {L"歌名 / 进度", L"微信、邮件等", L"文本、图片、文件",
                                          L"下载完成", L"调音量时", L"插拔、低电量",
                                          L"U 盘、移动硬盘"};

    const RECT* ev0 = FindRect(st, EventHit(0));
    if (ev0 != nullptr) {
        SectionTitle(g, L"响应哪些操作", pad,
                     static_cast<float>(ev0->top) - 22.0f * scale, scale);
        Hit hits[7];
        bool ons[7];
        const EventToggles& e = st.cfg.events;
        ons[0] = e.media;
        ons[1] = e.notification;
        ons[2] = e.clipboard;
        ons[3] = e.file;
        ons[4] = e.volume;
        ons[5] = e.power;
        ons[6] = e.device;
        for (int i = 0; i < 7; ++i) hits[i] = EventHit(i);
        DrawTwoColToggles(g, st, hits, kEventLabel, kEventSub, ons, 7, scale);
    }

    const RECT* full = FindRect(st, Hit::Fullscreen);
    if (full != nullptr) {
        SectionTitle(g, L"其他", pad, static_cast<float>(full->top) - 22.0f * scale, scale);
    }
    ToggleRow(g, st, Hit::Fullscreen, st.cfg.fullscreen_hold, L"全屏时收成一条",
              L"看视频 / 演示时让路，点一下叫醒", pad, scale);
    ToggleRow(g, st, Hit::Secret, st.cfg.clipboard.hide_secrets, L"隐藏疑似密码的复制内容",
              L"检测到密钥 / 密码时只显示「已复制」", pad, scale);
    ToggleRow(g, st, Hit::QuietOn, st.cfg.quiet.enabled, L"免打扰时段",
              L"这段时间内不产生任何提示", pad, scale);

    // 免打扰起止时间：四个 −/+ 与夹在中间的时间文本。
    {
        const RECT* fd = FindRect(st, Hit::QuietFromDec);
        const RECT* fi = FindRect(st, Hit::QuietFromInc);
        const RECT* td = FindRect(st, Hit::QuietToDec);
        const RECT* ti = FindRect(st, Hit::QuietToInc);
        if (fd != nullptr && fi != nullptr && td != nullptr && ti != nullptr) {
            const Gdiplus::Color dim = st.cfg.quiet.enabled
                                           ? Rgba(255, color::kTextR, color::kTextG, color::kTextB)
                                           : Rgba(110, 150, 152, 164);
            const float clock_w = F(58, st.dpi);
            auto stepper = [&](const RECT& minus, const RECT& plus, int minutes, Hit dh, Hit ih) {
                const Gdiplus::RectF mr = ToRectF(minus);
                FillRound(g, mr, mr.Height * 0.5f,
                          Rgba(static_cast<BYTE>(st.hover == dh ? 70 : 38), 255, 255, 255));
                Text(g, L"−", mr, 14.0f * scale, false, dim, Gdiplus::StringAlignmentCenter);
                const Gdiplus::RectF pr = ToRectF(plus);
                FillRound(g, pr, pr.Height * 0.5f,
                          Rgba(static_cast<BYTE>(st.hover == ih ? 70 : 38), 255, 255, 255));
                Text(g, L"+", pr, 14.0f * scale, false, dim, Gdiplus::StringAlignmentCenter);
                Text(g, Widen(FormatClock(minutes)),
                     Gdiplus::RectF(mr.GetRight() + 4.0f * scale, mr.Y, clock_w, mr.Height),
                     12.0f * scale, true, dim, Gdiplus::StringAlignmentCenter);
            };
            stepper(*fd, *fi, st.cfg.quiet.from_min, Hit::QuietFromDec, Hit::QuietFromInc);
            stepper(*td, *ti, st.cfg.quiet.to_min, Hit::QuietToDec, Hit::QuietToInc);
            const float mid =
                (static_cast<float>(fi->right) + static_cast<float>(td->left)) * 0.5f;
            Text(g, L"到",
                 Gdiplus::RectF(mid - 12.0f * scale, static_cast<float>(fd->top),
                                24.0f * scale, static_cast<float>(fd->bottom - fd->top)),
                 10.0f * scale, false, Rgba(150, color::kSubR, color::kSubG, color::kSubB),
                 Gdiplus::StringAlignmentCenter);
        }
    }

    const RECT* open = FindRect(st, Hit::OpenConfig);
    if (open != nullptr) {
        Button(g, ToRectF(*open), L"打开配置文件（黑名单等高级项）", false,
               st.hover == Hit::OpenConfig, scale);
    }

    const RECT* ah = FindRect(st, Hit::AutoHideFocus);
    if (ah != nullptr) {
        SectionTitle(g, L"自动隐藏与让路", pad, static_cast<float>(ah->top) - 22.0f * scale,
                     scale);
        static const wchar_t* kLabels[6] = {L"焦点离开时收起", L"暂停播放后收起",
                                            L"全屏时收起",     L"按住让路键让路",
                                            L"减少动效",       L"省电时降级"};
        static const wchar_t* kSubs[6] = {L"切到别的窗口 5 秒后", L"暂停超过 10 秒",
                                          L"整块隐藏，不只是收成一条", L"松开即恢复",
                                          L"不播放弹入 / 位移", L"电池供电时自动降档"};
        Hit hits[6] = {Hit::AutoHideFocus,  Hit::AutoHidePaused, Hit::AutoHideFullscreen,
                       Hit::YieldEnabled,   Hit::ReduceMotion,   Hit::LowPowerDowngrade};
        const bool ons[6] = {st.cfg.auto_hide_focus, st.cfg.auto_hide_paused,
                             st.cfg.auto_hide_fullscreen, st.cfg.yield_enabled,
                             st.cfg.reduce_motion, st.cfg.low_power_downgrade};
        DrawTwoColToggles(g, st, hits, kLabels, kSubs, ons, 6, scale);
    }

    SegmentRow(g, st, Hit::QuietForm0, 3, st.cfg.quiet_form, kQuietFormLabels, SegIcon::None,
               L"安静态形态", L"睡着之后留在屏幕上的那一小块", pad, scale);
    SegmentRow(g, st, Hit::YieldKey0, 3, st.cfg.yield_key, kYieldKeyLabels, SegIcon::None,
               L"让路键", L"按住时胶囊变淡并让鼠标穿透", pad, scale);
}

// ---- 性能页 ----
inline void PaintPerformance(Gdiplus::Graphics& g, const State& st, float pad, float scale) {
    const RECT* first = FindRect(st, Hit::GlassQuality0);
    if (first == nullptr) return;
    SectionTitle(g, L"性能", pad, static_cast<float>(first->top) - 22.0f * scale, scale);

    SegmentRow(g, st, Hit::GlassQuality0, 4, st.cfg.glass_quality, kGlassQualityLabels,
               SegIcon::None, L"玻璃质量", L"流畅省算力，极致最接近真实磨砂", pad, scale);
    StepperRow(g, st, Hit::CaptureRefreshDec, Hit::CaptureRefreshInc,
               MsText(st.cfg.capture_refresh_ms), L"抓屏间隔",
               L"越小越跟手，也越费电（500 – 8000 ms）", pad, scale);
    SegmentRow(g, st, Hit::TargetMonitor0, 3, st.cfg.target_monitor, kTargetMonitorLabels,
               SegIcon::None, L"目标屏幕", L"多显示器时岛显示在哪一块", pad, scale);
    StepperRow(g, st, Hit::MonitorIndexDec, Hit::MonitorIndexInc,
               st.cfg.target_monitor == 2
                   ? (L"第 " + std::to_wstring(st.cfg.monitor_index + 1) + L" 块")
                   : std::wstring(L"——"),
               L"屏幕序号", L"仅「指定序号」时生效", pad, scale,
               st.cfg.target_monitor == 2);
}

// ---- 预设页 ----
inline void PaintPreset(Gdiplus::Graphics& g, const State& st, float pad, float scale) {
    const RECT* first = FindRect(st, Hit::Preset0);
    if (first == nullptr) return;
    SectionTitle(g, L"预设", pad, static_cast<float>(first->top) - 22.0f * scale, scale);

    const int cur = CurrentPreset(st.cfg);
    for (int i = 0; i < 4; ++i) {
        const Hit h = HitAt(Hit::Preset0, i);
        const RECT* r = FindRect(st, h);
        if (r == nullptr) continue;
        DrawPresetCard(g, *r, kPresets[i], cur == i, st.hover == h, scale);
    }

    const RECT* reset = FindRect(st, Hit::PresetReset);
    if (reset != nullptr) {
        Button(g, ToRectF(*reset), L"恢复出厂设置", false, st.hover == Hit::PresetReset, scale);
        const wchar_t* cur_name = (cur >= 0) ? kPresets[cur].name : L"自定义";
        const std::wstring note = std::wstring(L"当前：") + cur_name +
                                  L"　·　预设只改外观与行为，事件开关、黑名单与代理不受影响";
        Text(g, note,
             Gdiplus::RectF(static_cast<float>(reset->right) + 14.0f * scale,
                            static_cast<float>(reset->top),
                            st.width - pad - static_cast<float>(reset->right) - 20.0f * scale,
                            static_cast<float>(reset->bottom - reset->top)),
             10.0f * scale, false, Rgba(200, color::kSubR, color::kSubG, color::kSubB));
    }
}

inline void Paint(State& st, HDC dc, int width, int height) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // 深色底上用 ClearType 会在笔画边缘出现彩色描边（底部那行路径最明显），
    // 灰度抗锯齿在深底上更干净。
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    const float scale = static_cast<float>(st.dpi) / 96.0f;
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float radius = 18.0f * scale;

    // 背景：深色纵向渐变 + 顶部一层冷光。和胶囊的 DrawOpaqueBody 同一配方，
    // 只是这里没有圆角裁剪的毛玻璃 —— 设置面板是自己的一整块，不需要透出桌面。
    {
        Gdiplus::GraphicsPath path;
        const float d = radius * 2.0f;
        path.AddArc(0.0f, 0.0f, d, d, 180.0f, 90.0f);
        path.AddArc(w - d, 0.0f, d, d, 270.0f, 90.0f);
        path.AddArc(w - d, h - d, d, d, 0.0f, 90.0f);
        path.AddArc(0.0f, h - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
        Gdiplus::LinearGradientBrush bg(
            Gdiplus::PointF(0, 0), Gdiplus::PointF(0, h),
            Rgba(255, color::kBgTopR, color::kBgTopG, color::kBgTopB),
            Rgba(255, color::kBgBotR, color::kBgBotG, color::kBgBotB));
        g.FillPath(&bg, &path);
        Gdiplus::PathGradientBrush glow(&path);
        glow.SetCenterPoint(Gdiplus::PointF(w * 0.5f, h * 0.06f));
        glow.SetCenterColor(Rgba(46, 120, 140, 210));
        glow.SetFocusScales(0.3f, 0.08f);
        const INT n = path.GetPointCount();
        if (n > 0) {
            std::vector<Gdiplus::Color> ring(static_cast<std::size_t>(n), Rgba(0, 0, 0, 0));
            INT count = n;
            glow.SetSurroundColors(ring.data(), &count);
            g.FillPath(&glow, &path);
        }
        Gdiplus::Pen rim(Rgba(34, 255, 255, 255), 1.0f * scale);
        g.DrawPath(&rim, &path);
    }

    const float pad = F(layout::kPad, st.dpi);
    const float content_w = w - pad * 2.0f;

    // ---- 标题：副标题随分页变，免得停在「外观」两个字上 ----
    static const wchar_t* kSubtitles[kPageCount] = {
        L"外观 · 材质、主题与整体尺寸",
        L"行为 · 响应哪些操作、何时让路",
        L"性能 · 玻璃质量与抓屏节奏",
        L"预设 · 一键换一套外观与行为",
    };
    Text(g, L"平衡岛", Gdiplus::RectF(pad, F(18, st.dpi), content_w, F(28, st.dpi)),
         17.0f * scale, true, Rgba(255, color::kTextR, color::kTextG, color::kTextB));
    Text(g, kSubtitles[st.page < 0 || st.page >= kPageCount ? 0 : st.page],
         Gdiplus::RectF(pad, F(48, st.dpi), content_w, F(18, st.dpi)), 10.5f * scale, false,
         Rgba(210, color::kSubR, color::kSubG, color::kSubB));

    // 关闭 ×（两笔线，不依赖字体里的符号）
    {
        const float cx = w - pad - 12.0f * scale;
        const float cy = F(32, st.dpi);
        const bool hot = st.hover == Hit::Close;
        const Gdiplus::Color c = hot ? Rgba(255, 255, 255, 255) : Rgba(170, 160, 162, 176);
        Gdiplus::Pen pen(c, 1.6f * scale);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        const float r = 6.0f * scale;
        g.DrawLine(&pen, cx - r, cy - r, cx + r, cy + r);
        g.DrawLine(&pen, cx + r, cy - r, cx - r, cy + r);
    }

    DrawTabs(g, st, scale);

    // 内容区裁到视口内：滚动时上下都不能溢出到标题/按钮区。
    const Gdiplus::GraphicsState clip = g.Save();
    g.SetClip(Gdiplus::RectF(0.0f, static_cast<float>(st.view_top), w,
                             static_cast<float>(st.view_h)));
    switch (st.page) {
        case 1: PaintBehavior(g, st, pad, scale); break;
        case 2: PaintPerformance(g, st, pad, scale); break;
        case 3: PaintPreset(g, st, pad, scale); break;
        case 0:
        default: PaintAppearance(g, st, pad, scale); break;
    }
    g.Restore(clip);

    // 上下渐隐：内容被裁切时直接切断，看起来像渲染坏了（半截文字卡在按钮上方）。
    // 用和背景同一组渐变色收边，读起来才是「下面还有」而不是「画错了」。
    if (st.max_scroll > 0 && st.view_h > 0) {
        auto bg_at = [h](float y, BYTE* rgb) {
            const float t = (h > 1.0f) ? (y / h) : 0.0f;
            rgb[0] = static_cast<BYTE>(static_cast<float>(color::kBgTopR) +
                                       static_cast<float>(color::kBgBotR - color::kBgTopR) * t);
            rgb[1] = static_cast<BYTE>(static_cast<float>(color::kBgTopG) +
                                       static_cast<float>(color::kBgBotG - color::kBgTopG) * t);
            rgb[2] = static_cast<BYTE>(static_cast<float>(color::kBgTopB) +
                                       static_cast<float>(color::kBgBotB - color::kBgTopB) * t);
        };
        const float fade = 42.0f * scale;
        const float top = static_cast<float>(st.view_top);
        const float bottom = top + static_cast<float>(st.view_h);
        BYTE rgb[3];
        if (st.scroll > 0) {
            bg_at(top, rgb);
            Gdiplus::LinearGradientBrush b(Gdiplus::PointF(0.0f, top),
                                           Gdiplus::PointF(0.0f, top + fade),
                                           Rgba(255, rgb[0], rgb[1], rgb[2]),
                                           Rgba(0, rgb[0], rgb[1], rgb[2]));
            g.FillRectangle(&b, 0.0f, top, w, fade);
        }
        if (st.scroll < st.max_scroll) {
            bg_at(bottom, rgb);
            Gdiplus::LinearGradientBrush b(Gdiplus::PointF(0.0f, bottom - fade),
                                           Gdiplus::PointF(0.0f, bottom),
                                           Rgba(0, rgb[0], rgb[1], rgb[2]),
                                           Rgba(255, rgb[0], rgb[1], rgb[2]));
            g.FillRectangle(&b, 0.0f, bottom - fade, w, fade);
        }
    }

    // 细滚动条：只在真的放不下时出现，位置一眼能看出还剩多少。
    if (st.max_scroll > 0 && st.view_h > 0) {
        const float track_x = w - 7.0f * scale;
        const float track_w = 3.0f * scale;
        const float track_top = static_cast<float>(st.view_top);
        const float track_h = static_cast<float>(st.view_h);
        FillRound(g, Gdiplus::RectF(track_x, track_top, track_w, track_h), track_w * 0.5f,
                  Rgba(26, 255, 255, 255));
        const int content_h = st.view_h + st.max_scroll;
        const float thumb_h =
            (std::max)(24.0f * scale, track_h * static_cast<float>(st.view_h) /
                                          static_cast<float>(content_h));
        const float t = static_cast<float>(st.scroll) / static_cast<float>(st.max_scroll);
        const float thumb_y = track_top + (track_h - thumb_h) * t;
        FillRound(g, Gdiplus::RectF(track_x, thumb_y, track_w, thumb_h), track_w * 0.5f,
                  Rgba(110, 255, 255, 255));
    }

    // ---- 底部：按钮与配置文件路径（四页共用，位置由布局给出）----
    const RECT* rc_cancel = FindRect(st, Hit::Cancel);
    const RECT* rc_save = FindRect(st, Hit::Save);
    if (rc_cancel != nullptr) {
        Button(g, ToRectF(*rc_cancel), L"取消", false, st.hover == Hit::Cancel, scale);
    }
    if (rc_save != nullptr) {
        Button(g, ToRectF(*rc_save), L"保存", true, st.hover == Hit::Save, scale);
    }
    Text(g, st.path, Gdiplus::RectF(pad, h - 20.0f * scale, content_w, 15.0f * scale),
         9.0f * scale, false, Rgba(150, color::kSubR, color::kSubG, color::kSubB));
}

// ---------------------------------------------------------------- 交互

inline Hit ZoneAt(const State& st, int x, int y) {
    // 倒序：后加入的区域更具体（× 按钮压在整块拖动区上面）。
    // 正序会让「点 × 变成拖窗口」—— 而且看起来像按钮失灵。
    for (auto it = st.zones.rbegin(); it != st.zones.rend(); ++it) {
        if (x >= it->rc.left && x < it->rc.right && y >= it->rc.top && y < it->rc.bottom) {
            return it->hit;
        }
    }
    return Hit::None;
}

inline void ApplyToggle(RuleConfig& cfg, Hit h) {
    switch (h) {
        case Hit::Glass: cfg.glass = !cfg.glass; return;
        case Hit::Fullscreen: cfg.fullscreen_hold = !cfg.fullscreen_hold; return;
        case Hit::Secret: cfg.clipboard.hide_secrets = !cfg.clipboard.hide_secrets; return;
        case Hit::QuietOn: cfg.quiet.enabled = !cfg.quiet.enabled; return;
        default: break;
    }
    const int idx = EventIndex(h);
    if (idx < 0) return;
    bool* slot[7] = {&cfg.events.media,     &cfg.events.notification, &cfg.events.clipboard,
                     &cfg.events.file,      &cfg.events.volume,       &cfg.events.power,
                     &cfg.events.device};
    *slot[idx] = !*slot[idx];
}

// 免打扰时间每次调 15 分钟。步长太大跳过头，太小点半天。
inline void NudgeQuiet(RuleConfig& cfg, Hit h) {
    auto wrap = [](int m) {
        m %= 1440;
        return m < 0 ? m + 1440 : m;
    };
    const int step = 15;
    switch (h) {
        case Hit::QuietFromDec: cfg.quiet.from_min = wrap(cfg.quiet.from_min - step); return;
        case Hit::QuietFromInc: cfg.quiet.from_min = wrap(cfg.quiet.from_min + step); return;
        case Hit::QuietToDec: cfg.quiet.to_min = wrap(cfg.quiet.to_min - step); return;
        case Hit::QuietToInc: cfg.quiet.to_min = wrap(cfg.quiet.to_min + step); return;
        default: return;
    }
}

// 一个控件被点中之后做什么。返回 true = 分页变了，需要重算布局并调窗口高度。
//
// 这里必须覆盖**每一个**生成了命中区的 Hit：命中区存在但分支不存在，
// 用户看到的就是「点了没反应」——比没有这个控件更糟。
inline bool HandleHit(State& st, Hit h) {
    RuleConfig& c = st.cfg;
    const int v = static_cast<int>(h);

    // 分页 Tab（同组连续）
    if (v >= static_cast<int>(Hit::TabAppearance) && v <= static_cast<int>(Hit::TabPreset)) {
        const int next = v - static_cast<int>(Hit::TabAppearance);
        if (next == st.page) return false;  // 重复点同一个 Tab 不该触发重排/改窗口尺寸
        st.page = next;
        return true;
    }
    // 预设卡（同组连续）与恢复出厂
    if (v >= static_cast<int>(Hit::Preset0) && v <= static_cast<int>(Hit::Preset3)) {
        ApplyPreset(c, v - static_cast<int>(Hit::Preset0));
        return false;
    }
    if (h == Hit::PresetReset) {
        ApplyPreset(c, -1);
        return false;
    }

    // 一切「同组连续、结果就是下标」的分段控件都走这里，省得逐个写 case。
    auto pick = [&](Hit base, int count, int* dst) {
        const int b = static_cast<int>(base);
        if (v >= b && v < b + count) {
            *dst = v - b;
            return true;
        }
        return false;
    };
    if (pick(Hit::Theme0, 3, &c.theme)) return false;
    if (pick(Hit::QuietForm0, 3, &c.quiet_form)) return false;
    if (pick(Hit::YieldKey0, 3, &c.yield_key)) return false;
    if (pick(Hit::GlassQuality0, 4, &c.glass_quality)) return false;
    // 目标屏幕决定「屏幕序号」这一行是否可用，选完要重排。
    if (pick(Hit::TargetMonitor0, 3, &c.target_monitor)) return true;
    {
        const int b = static_cast<int>(Hit::Accent0);
        if (v >= b && v < b + kAccentCount) {
            c.accent = kAccents[v - b].hex;
            return false;
        }
    }

    switch (h) {
        // ---- 外观页步进器 ----
        case Hit::UiScaleDec: c.ui_scale = (std::max)(80, c.ui_scale - 5); return false;
        case Hit::UiScaleInc: c.ui_scale = (std::min)(140, c.ui_scale + 5); return false;
        case Hit::FontOffsetDec: c.font_offset = (std::max)(-2, c.font_offset - 1); return false;
        case Hit::FontOffsetInc: c.font_offset = (std::min)(4, c.font_offset + 1); return false;
        case Hit::CornerDec: c.corner_radius = StepCorner(c.corner_radius, -2); return false;
        case Hit::CornerInc: c.corner_radius = StepCorner(c.corner_radius, 2); return false;
        case Hit::TopOffsetDec: c.top_offset = (std::max)(4, c.top_offset - 2); return false;
        case Hit::TopOffsetInc: c.top_offset = (std::min)(64, c.top_offset + 2); return false;
        // ---- 行为页开关 ----
        case Hit::AutoHideFocus: c.auto_hide_focus = !c.auto_hide_focus; return false;
        case Hit::AutoHidePaused: c.auto_hide_paused = !c.auto_hide_paused; return false;
        case Hit::AutoHideFullscreen:
            c.auto_hide_fullscreen = !c.auto_hide_fullscreen;
            return false;
        case Hit::YieldEnabled: c.yield_enabled = !c.yield_enabled; return false;
        case Hit::ReduceMotion: c.reduce_motion = !c.reduce_motion; return false;
        case Hit::LowPowerDowngrade: c.low_power_downgrade = !c.low_power_downgrade; return false;
        // ---- 性能页步进器 ----
        case Hit::CaptureRefreshDec:
            c.capture_refresh_ms = (std::max)(500, c.capture_refresh_ms - 500);
            return false;
        case Hit::CaptureRefreshInc:
            c.capture_refresh_ms = (std::min)(8000, c.capture_refresh_ms + 500);
            return false;
        case Hit::MonitorIndexDec:
            if (c.target_monitor != 2) return false;  // 未选「指定序号」时这一行不生效
            c.monitor_index = (std::max)(0, c.monitor_index - 1);
            return false;
        case Hit::MonitorIndexInc:
            if (c.target_monitor != 2) return false;
            c.monitor_index = (std::min)(7, c.monitor_index + 1);
            return false;
        default: break;
    }

    // 剩下的就是原有的开关：玻璃 / 全屏收条 / 密码 / 免打扰 / 七个事件。
    ApplyToggle(c, h);
    return false;
}

inline void Invalidate(HWND hwnd) { ::InvalidateRect(hwnd, nullptr, FALSE); }

inline LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* st = reinterpret_cast<State*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_CREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        st = static_cast<State*>(create->lpCreateParams);
        st->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        if (st->dpi == 0) st->dpi = 96;

        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        st->width = rc.right - rc.left;
        st->height = rc.bottom - rc.top;
        BuildZones(*st, st->zones);

        // 圆角窗口：SetWindowRgn 只影响命中与裁剪，不会像 DWM 那样把边框画进来。
        // 不做的话四角会露出窗口类的方形底色。
        const int r = S(18, st->dpi);
        HRGN region = ::CreateRoundRectRgn(0, 0, st->width + 1, st->height + 1, r * 2, r * 2);
        if (region != nullptr) ::SetWindowRgn(hwnd, region, TRUE);

        if (!st->shot_path.empty()) ::SetTimer(hwnd, 1, 600, nullptr);

        if (st->selftest) {
            // 三件事一起验：
            // ① 命中表 —— 每个可点区域的中心必须命中它自己。这条能抓出
            //    「被拖动区盖住」「两块区域重叠」「矩形算反了」这类问题，
            //    而它们在人眼看来只是「这个开关点不动」。
            // ② 越界 —— 区域必须落在本页的真实高度内。v0.3.0 的「控件画不出来」
            //    正是布局比窗口矮造成的，光看点击是发现不了的一半。
            // ③ 配置往返 —— 越界值要被夹住，而不是把整块外观打回默认。
            int bad = 0;
            int checked = 0;
            int outside = 0;
            const int keep_page = st->page;
            for (int page = 0; page < kPageCount; ++page) {
                st->page = page;
                std::vector<Zone> zones;
                const int page_h = BuildZones(*st, zones);
                st->zones = zones;
                for (const auto& z : st->zones) {
                    if (z.hit == Hit::TitleBar) continue;  // 整块标题区，被更具体的区域压着
                    ++checked;
                    const int cx = (z.rc.left + z.rc.right) / 2;
                    const int cy = (z.rc.top + z.rc.bottom) / 2;
                    if (ZoneAt(*st, cx, cy) != z.hit) ++bad;
                    if (z.rc.right > st->width || z.rc.bottom > page_h) ++outside;
                }
            }
            st->page = keep_page;
            BuildZones(*st, st->zones);

            st->cfg.opacity = 9;
            st->cfg.anchor = -3;
            const std::string json = st->cfg.ToJson();
            bi::json::Value root;
            bi::json::Parse(json, root, nullptr);
            const RuleConfig round = RuleConfig::FromJson(root);
            st->cfg.opacity = round.opacity;
            st->cfg.anchor = round.anchor;
            const bool clamp_ok = round.opacity == 2 && round.anchor == 0;
            char report[256];
            std::snprintf(report, sizeof(report),
                          "zones=%d unclickable=%d outside=%d clamp=opacity%d/anchor%d", checked,
                          bad, outside, round.opacity, round.anchor);
            st->selftest_note = report;
            st->saved = clamp_ok && bad == 0 && outside == 0;
            ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    if (st == nullptr) return ::DefWindowProcW(hwnd, msg, wparam, lparam);

    switch (msg) {
        case WM_ERASEBKGND: return 1;  // 自己全画，别让系统刷一遍灰底（会闪）

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            // 双缓冲：自绘面板直接往窗口 DC 上画会有明显撕裂（尤其是渐变）。
            HDC mem = ::CreateCompatibleDC(dc);
            HBITMAP bmp = ::CreateCompatibleBitmap(dc, w, h);
            HBITMAP old = static_cast<HBITMAP>(::SelectObject(mem, bmp));
            try {
                Paint(*st, mem, w, h);
            } catch (...) {
            }
            ::BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            ::SelectObject(mem, old);
            ::DeleteObject(bmp);
            ::DeleteDC(mem);
            ::EndPaint(hwnd, &ps);
            return 0;
        }

        // PrintWindow(PW_RENDERFULLCONTENT) 会发这个：不处理的话截图是纯黑。
        case WM_PRINTCLIENT: {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            try {
                Paint(*st, reinterpret_cast<HDC>(wparam), rc.right - rc.left, rc.bottom - rc.top);
            } catch (...) {
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (st->dragging) {
                POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ::ClientToScreen(hwnd, &pt);
                ::SetWindowPos(hwnd, nullptr, st->drag_window.left + (pt.x - st->drag_origin.x),
                               st->drag_window.top + (pt.y - st->drag_origin.y), 0, 0,
                               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            const Hit h = ZoneAt(*st, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (h != st->hover) {
                st->hover = h;
                // 拖动区不显示手型以外的提示；其余可点区域给手型。
                ::SetCursor(::LoadCursorW(nullptr, h == Hit::None ? IDC_ARROW : IDC_HAND));
                Invalidate(hwnd);
            }
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                POINT pt{};
                ::GetCursorPos(&pt);
                ::ScreenToClient(hwnd, &pt);
                const Hit h = ZoneAt(*st, pt.x, pt.y);
                ::SetCursor(::LoadCursorW(nullptr, h == Hit::None ? IDC_ARROW : IDC_HAND));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const Hit h = ZoneAt(*st, x, y);
            if (h == Hit::TitleBar) {
                st->dragging = true;
                POINT pt{x, y};
                ::ClientToScreen(hwnd, &pt);
                st->drag_origin = pt;
                RECT wr{};
                ::GetWindowRect(hwnd, &wr);
                st->drag_window = wr;
                ::SetCapture(hwnd);
                return 0;
            }
            st->pressed = h;
            return 0;
        }

        case WM_LBUTTONUP: {
            if (st->dragging) {
                st->dragging = false;
                ::ReleaseCapture();
                return 0;
            }
            const Hit h = ZoneAt(*st, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (h != st->pressed) {
                st->pressed = Hit::None;
                return 0;
            }
            st->pressed = Hit::None;
            switch (h) {
                case Hit::Close:
                case Hit::Cancel: ::PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
                case Hit::Save: st->saved = true; ::PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
                case Hit::OpenConfig:
                    ::ShellExecuteW(nullptr, L"open", st->path.c_str(), nullptr, nullptr,
                                    SW_SHOWNORMAL);
                    return 0;
                case Hit::QuietFromDec:
                case Hit::QuietFromInc:
                case Hit::QuietToDec:
                case Hit::QuietToInc:
                    NudgeQuiet(st->cfg, h);
                    Invalidate(hwnd);
                    return 0;
                case Hit::None: return 0;
                default: {
                    // 其余（分页 / 预设 / 三档选择 / 步进器 / 开关）统一交给 HandleHit。
                    // 旧版这里是 ApplyToggle 一把梭，导致新加的控件「点了没反应」。
                    if (HandleHit(*st, h)) {
                        Relayout(hwnd, *st);  // 换页：布局与窗口高度都要重算
                    }
                    Invalidate(hwnd);
                    return 0;
                }
            }
        }

        case WM_MOUSEWHEEL: {
            if (st->max_scroll <= 0) return 0;
            const int step = S(64, st->dpi);
            const int before = st->scroll;
            // 向上滚 delta 为正，内容要往下走（scroll 变小）。
            st->scroll -= GET_WHEEL_DELTA_WPARAM(wparam) * step / WHEEL_DELTA;
            if (st->scroll < 0) st->scroll = 0;
            if (st->scroll > st->max_scroll) st->scroll = st->max_scroll;
            if (st->scroll != before) {
                BuildZones(*st, st->zones);  // 高度不变，只把内容按新偏移重排
                Invalidate(hwnd);
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            if (wparam == VK_RETURN) {
                st->saved = true;
                ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            return 0;

        case WM_TIMER:
            if (wparam == 1) {
                ::KillTimer(hwnd, 1);
                if (!st->shot_done && !st->shot_path.empty()) {
                    RECT wr{};
                    ::GetWindowRect(hwnd, &wr);
                    const int w = wr.right - wr.left;
                    const int h = wr.bottom - wr.top;
                    if (w > 0 && h > 0) {
                        LayeredSurface surface;
                        surface.EnsureSize(w, h);
                        if (!::PrintWindow(hwnd, surface.Dc(), PW_RENDERFULLCONTENT)) {
                            ::SendMessageW(hwnd, WM_PRINTCLIENT,
                                           reinterpret_cast<WPARAM>(surface.Dc()), PRF_CLIENT);
                        }
                        surface.ForceOpaqueAlpha();
                        surface.SavePng(st->shot_path);
                    }
                    st->shot_done = true;
                    ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
            }
            return 0;

        case WM_CLOSE: ::DestroyWindow(hwnd); return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace settings_ui

// 打开设置面板并阻塞到关闭。返回 true 且 saved=true 表示用户点了保存。
inline bool ShowSettings(const RuleConfig& current, const std::wstring& path,
                         const std::wstring& shot_path, RuleConfig* out, bool* saved,
                         bool* shot_done, bool selftest = false, std::string* selftest_note = nullptr) {
    static bool registered = false;
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = settings_ui::Proc;
        wc.hInstance = instance;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // 全自绘：给一个画刷反而会闪
        wc.lpszClassName = L"BI_SettingsWindow";
        if (::RegisterClassExW(&wc) == 0) return false;
        registered = true;
    }

    settings_ui::State state;
    state.cfg = current;
    state.path = path;
    state.shot_path = shot_path;
    state.selftest = selftest;

    int dpi = static_cast<int>(::GetDpiForSystem());
    if (dpi <= 0) dpi = 96;
    state.dpi = dpi;

    const int client_w = settings_ui::S(settings_ui::layout::kWidth, dpi);

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    ::GetMonitorInfoW(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int work_w = monitor.rcWork.right - monitor.rcWork.left;
    const int work_h = monitor.rcWork.bottom - monitor.rcWork.top;

    state.width = client_w;
    state.max_height = work_h - settings_ui::S(40, dpi);  // 留出任务栏与呼吸空间

    // 高度由布局算：先按内容自然高度求一遍，放不下才退到工作区高度并用滚动兜住。
    // 写死高度的版本每次加一行都得手改，而且改了会忘。
    state.height = 0;
    std::vector<settings_ui::Zone> zones;
    const int natural_h = settings_ui::BuildZones(state, zones);
    const int final_h = (state.max_height > 0 && natural_h > state.max_height)
                            ? state.max_height
                            : natural_h;
    state.height = final_h;
    const int x = monitor.rcWork.left + (work_w - client_w) / 2;
    const int y = monitor.rcWork.top + (work_h - final_h) / 2;

    HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW, L"BI_SettingsWindow",
                                  L"平衡岛设置", WS_POPUP, x, y, client_w, final_h, nullptr,
                                  nullptr, instance, &state);
    if (hwnd == nullptr) return false;

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);

    MSG msg;
    while (::IsWindow(hwnd) && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (selftest_note != nullptr) *selftest_note = state.selftest_note;
    if (saved != nullptr) *saved = state.saved;
    if (shot_done != nullptr) *shot_done = state.shot_done;
    if (state.saved && out != nullptr) *out = state.cfg;
    return true;
}

}  // namespace win32
}  // namespace bi

#endif  // BI_SRC_WIN32_SETTINGS_UI_HPP
