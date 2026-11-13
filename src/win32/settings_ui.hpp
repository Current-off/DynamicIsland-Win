// DynamicIsland-Win / 设置面板（全自绘，与胶囊本体同一套视觉语言）
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
#ifndef DIW_SRC_WIN32_SETTINGS_UI_HPP
#define DIW_SRC_WIN32_SETTINGS_UI_HPP

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

namespace diw {
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
    TitleBar
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
        static const wchar_t* kLabels[3][3] = {
            {L"液态玻璃", L"均衡", L"不透明"},
            {L"靠左", L"居中", L"靠右"},
            {L"", L"", L""},
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
    Text(g, text, Gdiplus::RectF(x, y, 400.0f * scale, 18.0f * scale), 10.5f * scale, true,
         Rgba(235, 130, 132, 146));
}

inline void Hairline(Gdiplus::Graphics& g, float x0, float x1, float y) {
    Gdiplus::Pen pen(Rgba(20, 255, 255, 255), 1.0f);
    g.DrawLine(&pen, x0, y, x1, y);
}

// 按钮：主按钮是强调色实心，次按钮是浅描边。
inline void Button(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const wchar_t* text, bool primary,
                   bool hover, float scale) {
    const float radius = rc.Height * 0.5f;
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

// ---------------------------------------------------------------- 布局
//
// 布局与命中**同一份数据**：BuildZones() 一次算出所有矩形，Paint() 只照着画，
// 鼠标命中只查这张表。分两处算的话，只要有一处改了就必然错位 ——
// 「看得见点不到」这类 bug 全是这么来的。
inline int BuildZones(State& st, std::vector<Zone>& zones) {
    zones.clear();
    const int dpi = st.dpi;
    const int pad = S(layout::kPad, dpi);
    const int width = st.width;
    const int content_w = width - pad * 2;
    int y = S(layout::kTitleY, dpi);

    // 标题区（同时也是拖动区）
    zones.push_back({Hit::TitleBar, {0, 0, width, S(84, dpi)}});
    zones.push_back({Hit::Close, {width - pad - S(24, dpi), S(22, dpi), width - pad, S(46, dpi)}});

    y = S(96, dpi);
    y += S(18, dpi);  // 分组标题占位（绘制时才画字，布局只占高度）

    // ---- 外观 ----
    const int seg_w = S(300, dpi);
    const int seg_h = S(34, dpi);
    const int seg_x = width - pad - seg_w;

    const int opacity_y = y;
    zones.push_back({Hit::Opacity0, {seg_x, opacity_y, seg_x + seg_w / 3, opacity_y + seg_h}});
    zones.push_back({Hit::Opacity1,
                     {seg_x + seg_w / 3, opacity_y, seg_x + seg_w * 2 / 3, opacity_y + seg_h}});
    zones.push_back({Hit::Opacity2,
                     {seg_x + seg_w * 2 / 3, opacity_y, seg_x + seg_w, opacity_y + seg_h}});
    y += S(layout::kRowH + 12, dpi);

    const int anchor_y = y;
    zones.push_back({Hit::Anchor0, {seg_x, anchor_y, seg_x + seg_w / 3, anchor_y + seg_h}});
    zones.push_back({Hit::Anchor1,
                     {seg_x + seg_w / 3, anchor_y, seg_x + seg_w * 2 / 3, anchor_y + seg_h}});
    zones.push_back({Hit::Anchor2,
                     {seg_x + seg_w * 2 / 3, anchor_y, seg_x + seg_w, anchor_y + seg_h}});
    y += S(layout::kRowH + 12, dpi);

    const int glass_y = y;
    zones.push_back({Hit::Glass, {width - pad - S(48, dpi), glass_y, width - pad,
                                  glass_y + S(26, dpi)}});
    y += S(layout::kRowH, dpi);

    y += S(layout::kGroupGap, dpi);

    // ---- 响应哪些操作（两列）----
    y += S(22, dpi);  // section title
    const int col_w = content_w / 2;
    const int row_h = S(38, dpi);
    for (int i = 0; i < 7; ++i) {
        const int col = i % 2;
        const int row = i / 2;
        const int cy = y + row * row_h;
        if (col == 0) {
            zones.push_back({EventHit(i),
                             {pad, cy, pad + col_w - S(60, dpi), cy + S(26, dpi)}});
        } else {
            zones.push_back({EventHit(i),
                             {pad + col_w, cy, width - pad, cy + S(26, dpi)}});
        }
    }
    y += row_h * 4;
    y += S(layout::kGroupGap, dpi);

    // ---- 其他 ----
    y += S(22, dpi);
    const int full_y = y;
    zones.push_back({Hit::Fullscreen,
                     {width - pad - S(48, dpi), full_y, width - pad, full_y + S(26, dpi)}});
    y += S(layout::kRowH, dpi);

    const int secret_y = y;
    zones.push_back({Hit::Secret,
                     {width - pad - S(48, dpi), secret_y, width - pad, secret_y + S(26, dpi)}});
    y += S(layout::kRowH, dpi);

    // 免打扰：第一行是标题 + 开关，第二行才是起止时间。
    // 挤在一行会变成「开关 − + 到 − +」六个小块贴着，谁也看不出哪个是哪个 ——
    // 而且时间文字会被 +/- 压成零宽。
    const int quiet_y = y;
    const int ctl_right = width - pad;
    zones.push_back({Hit::QuietOn,
                     {ctl_right - S(48, dpi), quiet_y, ctl_right, quiet_y + S(26, dpi)}});
    y += S(30, dpi);

    const int time_y = y;
    const int tw = S(26, dpi);    // − / + 的边长
    const int gap = S(4, dpi);
    int tx = pad;
    zones.push_back({Hit::QuietFromDec, {tx, time_y, tx + tw, time_y + tw}});
    tx += tw + gap;
    const int clock_w = S(58, dpi);
    tx += clock_w + gap;  // 时间文本占位（不可点）
    zones.push_back({Hit::QuietFromInc, {tx, time_y, tx + tw, time_y + tw}});
    tx += tw + S(22, dpi);  // 「到」
    zones.push_back({Hit::QuietToDec, {tx, time_y, tx + tw, time_y + tw}});
    tx += tw + gap;
    tx += clock_w + gap;
    zones.push_back({Hit::QuietToInc, {tx, time_y, tx + tw, time_y + tw}});
    y += S(38, dpi);

    // ---- 高级 ----
    zones.push_back({Hit::OpenConfig, {pad, y, pad + S(280, dpi), y + S(32, dpi)}});
    y += S(52, dpi);

    // ---- 底部 ----
    const int btn_h = S(36, dpi);
    zones.push_back({Hit::Cancel,
                     {width - pad - S(180, dpi), y, width - pad - S(96, dpi), y + btn_h}});
    zones.push_back({Hit::Save, {width - pad - S(88, dpi), y, width - pad, y + btn_h}});
    y += btn_h + S(24, dpi);

    return y;
}

// ---------------------------------------------------------------- 绘制

inline void Paint(State& st, HDC dc, int width, int height) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

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

    // ---- 标题 ----
    Text(g, L"灵动岛", Gdiplus::RectF(pad, F(18, st.dpi), content_w, F(28, st.dpi)),
         17.0f * scale, true, Rgba(255, color::kTextR, color::kTextG, color::kTextB));
    Text(g, L"外观与响应设置 · 保存后立即生效", Gdiplus::RectF(pad, F(48, st.dpi), content_w,
                                                              F(18, st.dpi)),
         10.5f * scale, false, Rgba(210, color::kSubR, color::kSubG, color::kSubB));

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

    // ---- 用 zones 表来画：矩形全部来自布局，绘制代码里不再出现任何坐标常量 ----
    const RECT* rc_op0 = nullptr;
    const RECT* rc_op1 = nullptr;
    const RECT* rc_op2 = nullptr;
    const RECT* rc_an0 = nullptr;
    const RECT* rc_an1 = nullptr;
    const RECT* rc_an2 = nullptr;
    const RECT* rc_glass = nullptr;
    const RECT* rc_full = nullptr;
    const RECT* rc_secret = nullptr;
    const RECT* rc_qon = nullptr;
    const RECT* rc_qfd = nullptr;
    const RECT* rc_qfi = nullptr;
    const RECT* rc_qtd = nullptr;
    const RECT* rc_qti = nullptr;
    const RECT* rc_open = nullptr;
    const RECT* rc_save = nullptr;
    const RECT* rc_cancel = nullptr;
    const RECT* rc_event[7] = {};

    for (const auto& z : st.zones) {
        switch (z.hit) {
            case Hit::Opacity0: rc_op0 = &z.rc; break;
            case Hit::Opacity1: rc_op1 = &z.rc; break;
            case Hit::Opacity2: rc_op2 = &z.rc; break;
            case Hit::Anchor0: rc_an0 = &z.rc; break;
            case Hit::Anchor1: rc_an1 = &z.rc; break;
            case Hit::Anchor2: rc_an2 = &z.rc; break;
            case Hit::Glass: rc_glass = &z.rc; break;
            case Hit::Fullscreen: rc_full = &z.rc; break;
            case Hit::Secret: rc_secret = &z.rc; break;
            case Hit::QuietOn: rc_qon = &z.rc; break;
            case Hit::QuietFromDec: rc_qfd = &z.rc; break;
            case Hit::QuietFromInc: rc_qfi = &z.rc; break;
            case Hit::QuietToDec: rc_qtd = &z.rc; break;
            case Hit::QuietToInc: rc_qti = &z.rc; break;
            case Hit::OpenConfig: rc_open = &z.rc; break;
            case Hit::Save: rc_save = &z.rc; break;
            case Hit::Cancel: rc_cancel = &z.rc; break;
            default: {
                const int idx = EventIndex(z.hit);
                if (idx >= 0) rc_event[idx] = &z.rc;
                break;
            }
        }
    }

    auto to_f = [](const RECT& r) {
        return Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                              static_cast<float>(r.right - r.left),
                              static_cast<float>(r.bottom - r.top));
    };

    // ---- 外观 ----
    {
        const float sy = rc_op0 != nullptr ? static_cast<float>(rc_op0->top) - 24.0f * scale : 0.0f;
        SectionTitle(g, L"外观", pad, sy, scale);
    }
    if (rc_op0 != nullptr) {
        const int y = rc_op0->top;
        RowText(g, Gdiplus::RectF(pad, static_cast<float>(y), content_w * 0.42f,
                                  static_cast<float>(rc_op0->bottom - rc_op0->top)),
                L"材质", L"液态玻璃最透，均衡最稳，不透明任何壁纸下都读得清", scale);
        const RECT& r0 = *rc_op0;
        const RECT& r2 = *rc_op2;
        Gdiplus::RectF seg(static_cast<float>(r0.left), static_cast<float>(r0.top),
                           static_cast<float>(r2.right - r0.left),
                           static_cast<float>(r0.bottom - r0.top));
        int hover_idx = -1;
        if (st.hover == Hit::Opacity0) hover_idx = 0;
        if (st.hover == Hit::Opacity1) hover_idx = 1;
        if (st.hover == Hit::Opacity2) hover_idx = 2;
        Segment(g, seg, 3, st.cfg.opacity, hover_idx, scale, SegIcon::Opacity);
    }
    if (rc_an0 != nullptr) {
        const int y = rc_an0->top;
        RowText(g, Gdiplus::RectF(pad, static_cast<float>(y), content_w * 0.42f,
                                  static_cast<float>(rc_an0->bottom - rc_an0->top)),
                L"位置", L"靠边时留 28px，永不越界", scale);
        const RECT& r0 = *rc_an0;
        const RECT& r2 = *rc_an2;
        Gdiplus::RectF seg(static_cast<float>(r0.left), static_cast<float>(r0.top),
                           static_cast<float>(r2.right - r0.left),
                           static_cast<float>(r0.bottom - r0.top));
        int hover_idx = -1;
        if (st.hover == Hit::Anchor0) hover_idx = 0;
        if (st.hover == Hit::Anchor1) hover_idx = 1;
        if (st.hover == Hit::Anchor2) hover_idx = 2;
        Segment(g, seg, 3, st.cfg.anchor, hover_idx, scale, SegIcon::Anchor);
    }
    if (rc_glass != nullptr) {
        const int y = rc_glass->top;
        RowText(g, Gdiplus::RectF(pad, static_cast<float>(y), content_w * 0.6f,
                                  static_cast<float>(rc_glass->bottom - rc_glass->top)),
                L"磨砂玻璃", L"透出背后的屏幕内容（仅「均衡」档生效）", scale);
        Toggle(g, to_f(*rc_glass), st.cfg.glass, st.hover == Hit::Glass, scale);
    }

    // ---- 响应哪些操作 ----
    {
        float sy = 0.0f;
        for (const RECT* r : rc_event) {
            if (r != nullptr) {
                sy = static_cast<float>(r->top) - 24.0f * scale;
                break;
            }
        }
        SectionTitle(g, L"响应哪些操作", pad, sy, scale);
    }
    static const wchar_t* kEventLabel[7] = {
        L"媒体播放", L"消息通知", L"复制 / 截图", L"文件落盘", L"音量变化", L"电源 / 电量", L"设备接入"};
    static const wchar_t* kEventSub[7] = {
        L"歌名 / 进度", L"微信、邮件等", L"文本、图片、文件", L"下载完成",
        L"调音量时", L"插拔、低电量", L"U 盘、移动硬盘"};
    for (int i = 0; i < 7; ++i) {
        if (rc_event[i] == nullptr) continue;
        const RECT& r = *rc_event[i];
        const bool on = (i == 0)   ? st.cfg.events.media
                        : (i == 1) ? st.cfg.events.notification
                        : (i == 2) ? st.cfg.events.clipboard
                        : (i == 3) ? st.cfg.events.file
                        : (i == 4) ? st.cfg.events.volume
                        : (i == 5) ? st.cfg.events.power
                                   : st.cfg.events.device;
        const bool hover = st.hover == EventHit(i);
        // 开关放在每个可点区域的右端：命中区比开关大（整行都能点），
        // 这是设置界面该有的手感 —— 没人愿意去瞄一个 44×24 的小方块。
        const float tw = 44.0f * scale;
        const float th = 24.0f * scale;
        const Gdiplus::RectF tr(static_cast<float>(r.right) - tw,
                                static_cast<float>(r.top) + (static_cast<float>(r.bottom - r.top) -
                                                             th) * 0.5f,
                                tw, th);
        const float text_w = static_cast<float>(r.right - r.left) - tw - 12.0f * scale;
        RowText(g, Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top), text_w,
                                  static_cast<float>(r.bottom - r.top)),
                kEventLabel[i], kEventSub[i], scale);
        Toggle(g, tr, on, hover, scale);
    }

    // ---- 其他 ----
    {
        float sy = rc_full != nullptr ? static_cast<float>(rc_full->top) - 24.0f * scale : 0.0f;
        SectionTitle(g, L"其他", pad, sy, scale);
    }
    if (rc_full != nullptr) {
        const int y = rc_full->top;
        RowText(g, Gdiplus::RectF(pad, static_cast<float>(y), content_w * 0.66f,
                                  static_cast<float>(rc_full->bottom - rc_full->top)),
                L"全屏时收成一条", L"看视频 / 演示时让路，点一下叫醒", scale);
        Toggle(g, to_f(*rc_full), st.cfg.fullscreen_hold, st.hover == Hit::Fullscreen, scale);
    }
    if (rc_secret != nullptr) {
        const int y = rc_secret->top;
        RowText(g, Gdiplus::RectF(pad, static_cast<float>(y), content_w * 0.66f,
                                  static_cast<float>(rc_secret->bottom - rc_secret->top)),
                L"隐藏疑似密码的复制内容", L"检测到密钥 / 密码时只显示「已复制」", scale);
        Toggle(g, to_f(*rc_secret), st.cfg.clipboard.hide_secrets, st.hover == Hit::Secret,
               scale);
    }
    if (rc_qon != nullptr) {
        const int y = rc_qon->top;
        RowText(g, Gdiplus::RectF(pad, static_cast<float>(y), content_w * 0.34f,
                                  static_cast<float>(rc_qon->bottom - rc_qon->top)),
                L"免打扰时段", L"这段时间内不产生任何提示", scale);
        Toggle(g, to_f(*rc_qon), st.cfg.quiet.enabled, st.hover == Hit::QuietOn, scale);

        const Gdiplus::Color dim = st.cfg.quiet.enabled
                                       ? Rgba(255, color::kTextR, color::kTextG, color::kTextB)
                                       : Rgba(110, 150, 152, 164);
        const float clock_w = F(58, st.dpi);
        auto stepper = [&](const RECT& minus, const RECT& plus, int minutes, Hit dh, Hit ih) {
            const Gdiplus::RectF mr = to_f(minus);
            FillRound(g, mr, mr.Height * 0.5f,
                      Rgba(static_cast<BYTE>(st.hover == dh ? 70 : 38), 255, 255, 255));
            Text(g, L"−", mr, 14.0f * scale, false, dim, Gdiplus::StringAlignmentCenter);
            const Gdiplus::RectF pr = to_f(plus);
            FillRound(g, pr, pr.Height * 0.5f,
                      Rgba(static_cast<BYTE>(st.hover == ih ? 70 : 38), 255, 255, 255));
            Text(g, L"+", pr, 14.0f * scale, false, dim, Gdiplus::StringAlignmentCenter);
            // 时间显示在 − 与 + 之间那块空隙，也就是它自己 +/- 的中间。
            Text(g, Widen(FormatClock(minutes)),
                 Gdiplus::RectF(mr.GetRight() + 4.0f * scale, mr.Y, clock_w,
                                mr.Height),
                 12.0f * scale, true, dim, Gdiplus::StringAlignmentCenter);
        };
        stepper(*rc_qfd, *rc_qfi, st.cfg.quiet.from_min, Hit::QuietFromDec, Hit::QuietFromInc);
        stepper(*rc_qtd, *rc_qti, st.cfg.quiet.to_min, Hit::QuietToDec, Hit::QuietToInc);
        const float mid = (static_cast<float>(rc_qfi->right) + static_cast<float>(rc_qtd->left)) *
                          0.5f;
        Text(g, st.cfg.quiet.enabled ? L"到" : L"到",
             Gdiplus::RectF(mid - 12.0f * scale, static_cast<float>(rc_qfd->top), 24.0f * scale,
                            static_cast<float>(rc_qfd->bottom - rc_qfd->top)),
             10.0f * scale, false, Rgba(150, color::kSubR, color::kSubG, color::kSubB),
             Gdiplus::StringAlignmentCenter);
    }

    if (rc_open != nullptr) {
        Button(g, to_f(*rc_open), L"打开配置文件（黑名单等高级项）", false,
               st.hover == Hit::OpenConfig, scale);
    }

    // ---- 底部 ----
    if (rc_cancel != nullptr) {
        Button(g, to_f(*rc_cancel), L"取消", false, st.hover == Hit::Cancel, scale);
    }
    if (rc_save != nullptr) {
        Button(g, to_f(*rc_save), L"保存", true, st.hover == Hit::Save, scale);
    }
    Text(g, st.path, Gdiplus::RectF(pad, h - 22.0f * scale, content_w, 16.0f * scale),
         9.0f * scale, false, Rgba(120, color::kSubR, color::kSubG, color::kSubB));
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
            // 两件事一起验：
            // ① 命中表 —— 每个可点区域的中心必须命中它自己。这条能抓出
            //    「被拖动区盖住」「两块区域重叠」「矩形算反了」这类问题，
            //    而它们在人眼看来只是「这个开关点不动」。
            // ② 配置往返 —— 越界值要被夹住，而不是把整块外观打回默认。
            int bad = 0;
            int checked = 0;
            for (const auto& z : st->zones) {
                if (z.hit == Hit::TitleBar) continue;  // 整块标题区，被更具体的区域压着
                ++checked;
                const int cx = (z.rc.left + z.rc.right) / 2;
                const int cy = (z.rc.top + z.rc.bottom) / 2;
                if (ZoneAt(*st, cx, cy) != z.hit) ++bad;
            }
            st->cfg.opacity = 9;
            st->cfg.anchor = -3;
            const std::string json = st->cfg.ToJson();
            diw::json::Value root;
            diw::json::Parse(json, root, nullptr);
            const RuleConfig round = RuleConfig::FromJson(root);
            st->cfg.opacity = round.opacity;
            st->cfg.anchor = round.anchor;
            const bool clamp_ok = round.opacity == 2 && round.anchor == 0;
            char report[256];
            std::snprintf(report, sizeof(report),
                          "zones=%d unclickable=%d clamp=opacity%d/anchor%d", checked, bad,
                          round.opacity, round.anchor);
            st->selftest_note = report;
            st->saved = clamp_ok && bad == 0;
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
                case Hit::Opacity0: st->cfg.opacity = 0; Invalidate(hwnd); return 0;
                case Hit::Opacity1: st->cfg.opacity = 1; Invalidate(hwnd); return 0;
                case Hit::Opacity2: st->cfg.opacity = 2; Invalidate(hwnd); return 0;
                case Hit::Anchor0: st->cfg.anchor = 0; Invalidate(hwnd); return 0;
                case Hit::Anchor1: st->cfg.anchor = 1; Invalidate(hwnd); return 0;
                case Hit::Anchor2: st->cfg.anchor = 2; Invalidate(hwnd); return 0;
                case Hit::QuietFromDec:
                case Hit::QuietFromInc:
                case Hit::QuietToDec:
                case Hit::QuietToInc:
                    NudgeQuiet(st->cfg, h);
                    Invalidate(hwnd);
                    return 0;
                default:
                    ApplyToggle(st->cfg, h);
                    Invalidate(hwnd);
                    return 0;
            }
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
        wc.lpszClassName = L"DIW_SettingsWindow";
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
    // 高度由布局算：先给一个足够大的值让 BuildZones 算完，再按它返回的下沿定窗口。
    // 写死高度的版本每次加一行都得手改，而且改了会忘。
    const int probe_h = settings_ui::S(900, dpi);  // 只用来占位，实际高度由布局算出
    state.width = client_w;
    state.height = probe_h;
    std::vector<settings_ui::Zone> zones;
    const int content_h = settings_ui::BuildZones(state, zones);

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    ::GetMonitorInfoW(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int work_w = monitor.rcWork.right - monitor.rcWork.left;
    const int work_h = monitor.rcWork.bottom - monitor.rcWork.top;
    // 小屏兜底：面板比工作区还高时贴顶并让内容可滚 —— 这里不做滚动，
    // 而是把高度夹住（设置项不多，夹住最多是底部按钮紧一点）。
    const int final_h = content_h > work_h - 40 ? work_h - 40 : content_h;
    state.height = final_h;  // 让 WM_CREATE 里的圆角区域按真实高度算
    const int x = monitor.rcWork.left + (work_w - client_w) / 2;
    const int y = monitor.rcWork.top + (work_h - final_h) / 2;

    HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW, L"DIW_SettingsWindow",
                                  L"灵动岛设置", WS_POPUP, x, y, client_w, final_h, nullptr,
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
}  // namespace diw

#endif  // DIW_SRC_WIN32_SETTINGS_UI_HPP
