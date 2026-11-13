// DynamicIsland-Win / 胶囊渲染器（C++17 + GDI+）
//
// 只做「把 CapsuleView 画成像素」这一件事，不读时钟、不持状态、不碰调度，
// 因此可以用固定输入做确定性截图比对（见 tools/snapshot_cpp.sh）。
//
// 视觉上的三条硬约束（都是踩过坑之后写下来的，别凭手感改回去）：
//   1. 不用大面积线性渐变做玻璃：8bit 量化会把 0.10→0 的白色分成二十几级，
//      每级 4~6px，深色底上一眼能数出横条。改用「实色 + 径向受光」，
//      径向的等值线是弧线，人眼对弧线的宽容度远高于水平长直线。
//   2. 描边的亮度必须上亮下暗：整圈同亮度会显得像塑料贴纸，
//      上 0.30 → 下 0.045 才像一圈被光打过的玻璃边。
//   3. 投影必须真实模糊：多层同心矩形在浅色背景上是肉眼可见的「洋葱圈」
//      色带（用户感知为毛边）。离屏盒模糊给出单调平滑的 alpha 衰减。
#ifndef DIW_SRC_WIN32_RENDERER_HPP
#define DIW_SRC_WIN32_RENDERER_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// GDI+ 头文件需要 IStream / PROPID，WIN32_LEAN_AND_MEAN 下必须先引入 objidl.h。
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace diw {
namespace win32 {

// 渲染层布局常量（像素，96 DPI 基准；实际按 scale 放大）。
struct Layout {
    static constexpr float kMargin = 16.0f;  // 留给投影的外边距
    static constexpr float kGap = 8.0f;      // 主胶囊与分离小胶囊间距
    static constexpr float kHudHeight = 18.0f;
    static constexpr float kCompactW = 300.0f;
    static constexpr float kCompactH = 46.0f;
    static constexpr float kExpandedW = 420.0f;
    static constexpr float kExpandedH = 124.0f;       // 带进度条（媒体/下载）
    static constexpr float kExpandedShortH = 96.0f;   // 无进度条（复制/通知/设备）
    static constexpr float kExpandedLyricH = 196.0f;  // 媒体展开态：额外容纳三行歌词
    // 编码 Agent 展开态：标题 + 任务 + 当前动作 + 清单（最多 4 行）+ 页脚。
    // 高度按「实际有几行」算（kAgentBaseH + n*kAgentRowH）：只有 3 条清单时
    // 留一条空的横带，看着像内容没加载完。清单与时间线共用同一套行高。
    // 浏览器展开态：标题 + URL + 下载列表。没有下载时就是个矮面板，
    // 有下载时按行数长高 —— 和 agent 面板同一套「高度跟内容走」的原则。
    // 无下载时只有「标题 + URL + 页脚」，高度就该是矮的：118 会在中间留
    // 一条 33px 的空带，看着像内容没加载完。有下载时才多出分隔线与列表，
    // 每行 26，起始 111 是分隔线以下的固定开销。
    // 编辑器展开态：文件名 + 项目/语言 + 页脚。信息就那么几样，
    // 所以是个矮面板 —— 没必要为了"看起来充实"去堆空白。
    static constexpr float kEditorExpandedH = 92.0f;

    static constexpr float kBrowserBaseH = 92.0f;
    static constexpr float kBrowserWithRowsH = 111.0f;
    static constexpr float kBrowserRowH = 26.0f;
    static constexpr float kBrowserExpandedH = kBrowserWithRowsH + 3.0f * kBrowserRowH;
    static constexpr int kBrowserRows = 3;

    static constexpr float kAgentBaseH = 170.0f;
    static constexpr float kAgentRowH = 21.0f;
    static constexpr float kAgentExpandedH = kAgentBaseH + 4.0f * kAgentRowH;
    static constexpr int kAgentRows = 4;  // 展开态最多列几条清单/时间线
    // 休眠态：贴屏幕顶边的「把手」——一条又薄又淡的长条，不放时间、不放圆点。
    // 它的作用只是让用户知道程序还在跑、并且可以点，所以刻意做成
    // 「像按钮的细条」而不是一块会遮住内容的信息面板。
    static constexpr float kSleepBarW = 220.0f;
    // 10 逻辑像素：比 8 更容易点中（2x 下 20px），又仍是一条「细条」而不是面板。
    static constexpr float kSleepBarH = 10.0f;
    static constexpr float kSleepBarAlpha = 0.26f;  // 很淡，透过去看得见下面
    static constexpr float kDetachedSize = 46.0f;
    // 主胶囊旁边最多两个小圆：「一个大 + 两个小的」是这块区域的极限，
    // 再多就变成一排按钮，反而看不出哪个是主位。
    static constexpr int kDetachedMax = 2;
    static constexpr int kOverviewMax = 4;  // 一览模式最多同时铺开的岛数
};

// 一览模式（双击 / 右键「全部任务」）里每个独立小岛的极简内容。
// 只保留紧凑胶囊用得上的字段：图标、标题、副标题、封面。
struct OverviewItem {
    std::string kind;
    std::string title;
    std::string subtitle;
    std::string cover_path;
    // 点击这个岛要拉起的目标：AUMID（媒体/通知）或完整 exe 路径（复制）。
    // 渲染层不消费它，只负责从 BuildView 带到主循环的命中测试。
    std::string launch;
    // 编码 Agent 的 provider：决定一览里那个岛的图形与品牌色。
    // 不带它的话，三个不同 agent 在一览里会变成三个一样的紫色圆环。
    std::string provider;
    // 一览里也要有状态点：一屏铺开时，用户就是靠这个小圆判断
    // 「哪个在跑、哪个在等我」。
    std::string state;
};

// 展开态的一行清单 / 时间线。status 0=待办 1=进行中 2=已完成，
// 时间线行统一用 2（画成圆点而不是复选框）。
struct AgentRow {
    int status = 0;
    std::string text;
};

// 展开态的一行下载任务。progress < 0 表示总量未知（浏览器的 total_bytes
// 在服务端不给 Content-Length 时就是 0），这时画不确定态的条。
struct BrowserRow {
    std::string name;
    double progress = -1.0;
    std::string label;  // "62%" 或 "3.4 MB / 12 MB"
};

// 主胶囊旁边的小圆：代表「还排着的另一件事」。
// 以前只有一个、且字段是摊平的三个字符串；现在最多两个，
// 每个都要带自己的 activity_id —— 点它必须展开它代表的那一件，
// 而不是主胶囊代表的那一件（这就是用户报的「点小的却路由到大的」）。
struct DetachedItem {
    std::string kind;
    std::string cover_path;
    std::string provider;   // agent 品牌图形
    std::string state;      // agent 状态点
    std::string activity_id;  // 点击路由目标
};

inline std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &out[0], need);
    return out;
}

// 从一个极简 JSON 串里取 "progress": 0.42。纯函数，便于单测。
inline double ParseProgress(const std::string& json, double fallback = -1.0,
                            const char* field = "progress") {
    const std::string key = std::string("\"") + field + "\"";
    const auto pos = json.find(key);
    if (pos == std::string::npos) return fallback;
    auto i = json.find(':', pos + key.size());
    if (i == std::string::npos) return fallback;
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    const std::size_t begin = i;
    while (i < json.size() && (std::isdigit(static_cast<unsigned char>(json[i])) || json[i] == '.' ||
                               json[i] == '-' || json[i] == 'e' || json[i] == 'E' || json[i] == '+')) {
        ++i;
    }
    if (i == begin) return fallback;
    try {
        const double value = std::stod(json.substr(begin, i - begin));
        if (!(value >= 0.0 && value <= 1.0)) return fallback;
        return value;
    } catch (...) {
        return fallback;
    }
}

struct CapsuleView {
    double content_w = Layout::kCompactW;
    double content_h = Layout::kCompactH;
    double detached_w = 0.0;  // 0 表示不显示分离小胶囊
    bool expanded = false;
    bool overview = false;  // 一览：所有并行活动各占一个独立岛，横排铺开
    std::vector<OverviewItem> items;
    double alpha = 1.0;  // 0..1，整体不透明度
    // 本体形态：0 全透明（只留内容 + 文字投影）/ 1 均衡（磨砂玻璃）/ 2 不透明（深色实心）。
    // 放在视图里而不是渲染器成员里：截图回归时同一次 Draw 就要能准确表达「现在是哪一档」。
    int opacity_mode = 1;

    std::string title;
    std::string subtitle;
    std::string meta;
    std::string kind;
    std::string hud;

    std::string cover_path;  // 非空时用真实封面替换左侧色块（仅媒体活动）
    // 分离小圆胶囊代表的**第二件事**。以前这里空着，绘制时直接拿主活动的
    // kind / cover —— 于是小圆永远长得和主胶囊一模一样，用户看到的就是
    // 「两个一样的，有什么意义」。必须由 BuildView 填第二个活动的内容。
    // 分离小圆（最多 Layout::kDetachedMax 个）。每个自带 activity_id，
    // 主循环用它做命中测试与「点谁展开谁」。
    std::vector<DetachedItem> detached_items;

    double progress = -1.0;
    bool playing = false;
    double wave_phase = 0.0;
    // 还有几件「别的」任务（可见活动数 - 1）。紧凑态右侧画成 +N 角标：
    // 没有这个信号，用户看到的就是一个普通胶囊，根本不知道后面还排着任务，
    // 「怎么打开多岛」自然无从谈起。
    int more_count = 0;

    // 休眠态：无活动时不完全透明，画一枚只显示 HH:MM 的极简时钟药丸。
    bool sleep_bar = false;
    // 展开态歌词区（三行滚动）。lyric_status: loading / ok / none / error。
    bool lyric_active = false;
    std::string lyric_prev;
    std::string lyric_cur;
    std::string lyric_next;
    std::string lyric_status;

    // ---- 编码 Agent ----
    // 与媒体共用同一套胶囊几何，但内容完全不同：媒体是「在放什么」，
    // Agent 是「在干什么 + 干到哪一步」。这里只放已经算好的文案，
    // 渲染层不解析任何会话文件（那是 core/agent_session.hpp 的职责）。
    bool agent = false;
    std::string agent_provider;  // claude | workbuddy | codex | gemini，决定图形与配色
    std::string agent_name;      // "Claude Code"（品牌名，展开态标题）
    std::string agent_state;     // running | waiting | done
    std::string agent_task;      // 会话在做什么任务（标题级，最多一行）
    std::string agent_action;    // 当前动作：「正在编辑 renderer.hpp」
    std::string agent_meta;      // 紧凑态右侧：3/7 或 78 步
    std::string agent_sub;       // 展开态副标题：项目名 · 已运行时长
    std::string agent_footer;    // 展开态页脚左：工具 · 目标
    std::vector<AgentRow> agent_rows;  // 清单（优先）或动作时间线
    bool agent_rows_are_todos = false;  // true 时按复选框画，false 时按时间线画

    // ---- 浏览器 ----
    // 语义是「我在看什么 / 我在下什么」：标题与域名回答前者，下载列表回答后者。
    // 左侧方块放站点真实 favicon（来自浏览器自己的图标库），没有就退成品牌色。
    bool browser = false;
    std::string browser_brand;       // chrome / edge / firefox …（决定品牌色与字形）
    std::string browser_label;       // "Google Chrome"
    std::string browser_title;       // 标签页标题
    std::string browser_url;         // 完整 URL（展开态第二行）
    std::string browser_site;        // bilibili.com
    std::string browser_site_label;  // 哔哩哔哩
    double browser_dl = -1.0;        // 下载总进度（无下载 = -1）
    std::string browser_dl_hint;     // "62%"
    std::vector<BrowserRow> browser_rows;

    // ---- 编辑器 ----
    // 语义是「我正在改哪个文件」。三样东西：文件名（回答"在改什么"）、
    // 项目名（回答"在哪个仓库"）、语言（回答"这是什么"）。
    bool editor = false;
    std::string editor_brand;  // vscode / cursor / trae-cn / idea …
    std::string editor_label;  // "Trae CN"
    std::string editor_file;   // renderer.hpp
    std::string editor_project;
    std::string editor_language;
    bool editor_dirty = false;   // 有未保存修改
    bool editor_remote = false;  // 远程开发（SSH / WSL）
};

// 展开态可点击区域。绘制与命中测试共用这一份几何，避免「看得见点不到」。
// 坐标为画布（客户区）物理像素；pill 是 Draw() 里算出的胶囊矩形。
struct ExpandedHit {
    Gdiplus::RectF close;     // 右上角关闭
    Gdiplus::RectF play;      // 封面 = 播放 / 暂停
    Gdiplus::RectF progress;  // 加高后的进度条命中带（可点可拖）
    Gdiplus::RectF app;       // 图标 + 标题这一块 = 打开对应的应用
};

inline ExpandedHit ExpandedHitAreas(const Gdiplus::RectF& pill, float scale) {
    const float pad = 18.0f * scale;
    const float chip = 42.0f * scale;
    const float foot_y = pill.Y + pill.Height - pad - 6.0f * scale;
    const float bar_y = foot_y - 30.0f * scale;
    const float btn = 26.0f * scale;
    ExpandedHit h;
    h.close = Gdiplus::RectF(pill.GetRight() - pad - btn, pill.Y + pad - 4.0f * scale, btn, btn);
    h.play = Gdiplus::RectF(pill.X + pad, pill.Y + pad, chip, chip);
    // 4px 的条太细，命中带上下各放 9px，鼠标不必精准落在像素上。
    h.progress = Gdiplus::RectF(pill.X + pad, bar_y - 9.0f * scale, pill.Width - pad * 2.0f,
                                4.0f * scale + 18.0f * scale);
    // 「打开应用」的命中区：图标 + 标题 + 副标题（不含封面右下角的播放角标、
    // 也不含进度条与关闭键）。用户点岛最直觉的动作就是「我要回到那个 App」，
    // 所以给这一整块留了入口，而不是只让 42px 的图标可点。
    h.app = Gdiplus::RectF(pill.X + pad, pill.Y + pad, pill.Width - pad * 2.0f - btn - 10.0f * scale,
                           std::max(chip, 52.0f * scale));
    return h;
}

// 把进度条命中带里的 x 换成 0..1 的进度（供拖动预览与 seek）。
inline double ProgressFromX(const Gdiplus::RectF& progress_area, double x) {
    if (progress_area.Width <= 0.0f) return 0.0;
    return std::clamp((x - progress_area.X) / progress_area.Width, 0.0, 1.0);
}

// 玻璃参数：backdrop 是「胶囊背后那块屏幕」的模糊纹理（见 backdrop.hpp），
// (offset_x, offset_y) 是该纹理覆盖区域原点在**当前绘制表面**坐标系里的位置
//（通常为负 —— 纹理覆盖的是最大可能窗口，而当前窗口只是其中一部分）；
// (full_w, full_h) 是纹理映射回的全尺寸（backdrop 本身是 1/4 的降采样小图，
// DrawImage 时以全尺寸为目标矩形，放大即模糊的一部分）。
// backdrop == nullptr 时走不透明深色样式（无桌面会话 / 用户关掉玻璃）。
struct GlassParams {
    Gdiplus::Bitmap* backdrop = nullptr;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float full_w = 0.0f;
    float full_h = 0.0f;
};

class CapsuleRenderer {
public:
    CapsuleRenderer() : family_(MakeFamily()) {}

    void Draw(Gdiplus::Graphics& g, const CapsuleView& v, float scale) {
        Draw(g, v, scale, GlassParams{});
    }

    // 全透明模式要开文字投影。用成员而不是逐层传参：DrawText 的调用点有几十处，
    // 一处漏掉就会在那块文字上"糊"掉，而这种漏很难靠肉眼发现。
    void SetTextShadow(bool on) { text_shadow_ = on; }

    void Draw(Gdiplus::Graphics& g, const CapsuleView& v, float scale, const GlassParams& glass) {
        const float margin = Layout::kMargin * scale;
        const float w = static_cast<float>(v.content_w) * scale;
        const float h = static_cast<float>(v.content_h) * scale;
        const float detached = static_cast<float>(v.detached_w) * scale;
        const bool has_detached = detached > 0.0f && !v.expanded;

        const float group_w = w + (has_detached ? Layout::kGap * scale + detached : 0.0f);
        const float left = margin;
        const float top = margin;

        const BYTE a = static_cast<BYTE>(std::lround(std::clamp(v.alpha, 0.0, 1.0) * 255.0));

        // 全透明模式：没有底，也没有影。影子本身就是"看得见的底"，
        // 留着它会让全透明看起来像一块脏玻璃。
        text_shadow_ = (v.opacity_mode == 0);

        if (a > 0) {
            // 休眠扁条不用玻璃：玻璃会透出背景，浅色壁纸下整条变成浅灰，
            // 白字几乎看不见。待机条要的是「任何壁纸下都读得清」，
            // 所以固定走深色底 + 半透明（alpha 已由主循环压到 55%）。
            const bool glassy_text = glass.backdrop != nullptr && !v.sleep_bar;

            // ---- 一览模式：并行任务各自成为一个独立岛，横排铺开 ----
            // 每个岛都是完整的紧凑胶囊（不是小圆点）：图标 + 标题 + 副标题。
            // 画布宽度由 BuildView 按岛数给出，动画沿弹簧从单胶囊宽度滑过去。
            if (v.overview && !v.items.empty()) {
                const std::size_t n = std::min(v.items.size(),
                                               static_cast<std::size_t>(Layout::kOverviewMax));
                const float iw = Layout::kCompactW * scale;
                const float ih = Layout::kCompactH * scale;
                const float gap = Layout::kGap * scale;
                float x = left;
                for (std::size_t i = 0; i < n; ++i) {
                    const Gdiplus::RectF box(x, top, iw, ih);
                    DropShadow(g, box, a, scale);
                    DrawPill(g, box, a, scale, glass, v.opacity_mode);
                    DrawIsland(g, v.items[i], box, a, scale, glassy_text, v.wave_phase);
                    x += iw + gap;
                }
            } else {
            Gdiplus::RectF pill(left, top, w, h);
            // 小圆按「等间距横排」布局，圆本身固定 46，间距 kGap。
            // 不能用 detached/n 均分：那是「一个宽胶囊」的画法，
            // 两个圆会变成两个扁椭圆。
            const int dcount = static_cast<int>(v.detached_items.size());
            const float dsize = Layout::kDetachedSize * scale;
            std::vector<Gdiplus::RectF> dboxes;
            dboxes.reserve(static_cast<std::size_t>(dcount));
            for (int i = 0; i < dcount; ++i) {
                const float dx = left + w + Layout::kGap * scale +
                                 static_cast<float>(i) * (dsize + Layout::kGap * scale);
                dboxes.push_back(Gdiplus::RectF(dx, top + (h - dsize) * 0.5f, dsize, dsize));
            }

            // 投影先画，且和胶囊一起投 —— 否则分离小胶囊会像贴在玻璃上的贴纸。
            // 休眠扁条刻意不投：一条 26px 的待机条再拖个影子，反而更显眼、更挡东西。
            if (!v.sleep_bar) {
                // 液态玻璃也要投影：没有影子的透明块像直接印在壁纸上，浮不起来。
                // 但要比实心款淡得多（55%），否则在亮壁纸上会拖出一团灰雾。
                const BYTE sh = v.opacity_mode == 0 ? Clamp8(static_cast<int>(a * 0.55f)) : a;
                DropShadow(g, pill, sh, scale);
                for (const auto& box : dboxes) DropShadow(g, box, sh, scale);
            }

            DrawPill(g, pill, a, scale, v.sleep_bar ? GlassParams{} : glass, v.opacity_mode);
            for (const auto& box : dboxes) DrawPill(g, box, a, scale, glass, v.opacity_mode);

            if (v.sleep_bar) {
                DrawSleepBar(g, v, pill, a, scale, glassy_text);
            } else if (v.expanded) {
                DrawExpanded(g, v, pill, a, scale, glassy_text);
            } else {
                DrawCompact(g, v, pill, a, scale, glassy_text);
            }

            for (int i = 0; i < dcount; ++i) {
                const DetachedItem& item = v.detached_items[static_cast<std::size_t>(i)];
                const std::string& dk = item.kind.empty() ? v.kind : item.kind;
                const std::string& dc = item.kind.empty() ? v.cover_path : item.cover_path;
                const std::string dp = item.kind.empty() ? v.agent_provider : item.provider;
                const float chip_size = dsize * 0.60f;
                const Gdiplus::RectF chip = ChipRect(dboxes[static_cast<std::size_t>(i)], chip_size);
                DrawChipEx(g, dk, chip, chip_size, a, dc,
                           dk.rfind("agent", 0) == 0 ? AgentAccentFor(dp) : AccentFor(dk), dp);
                if (dk.rfind("agent", 0) == 0) {
                    DrawStateDot(g, chip, a, scale, item.state, v.wave_phase);
                }
            }
            }
        }

        if (!v.hud.empty()) {
            // 状态行画在一块深色小胶囊上。原来是裸的浅灰字，在浅色桌面上
            // 几乎看不见 —— 诊断信息看不清就等于没有。
            const float hud_h = Layout::kHudHeight * scale;
            const float hud_y = top + h + 4.0f * scale;
            const std::wstring hud_text = Widen(v.hud);
            const float text_w = MeasureText(g, family_.get(), hud_text, 10.5f * scale,
                                             Gdiplus::FontStyleRegular);
            const float plate_w = std::min(group_w, text_w + 18.0f * scale);
            const float plate_x = margin + (group_w - plate_w) * 0.5f;
            FillRounded(g, Gdiplus::RectF(plate_x, hud_y, plate_w, hud_h), hud_h * 0.5f,
                        Gdiplus::Color(Clamp8(static_cast<int>(a * 0.58f)), 16, 16, 20));
            Gdiplus::RectF hud_box(plate_x, hud_y, plate_w, hud_h);
            DrawText(g, hud_text, hud_box, 10.5f * scale, Gdiplus::FontStyleRegular,
                     Gdiplus::Color(Clamp8(static_cast<int>(a * 0.80f)), 208, 210, 218),
                     Gdiplus::StringAlignmentCenter);
        }
    }

private:
    std::unique_ptr<Gdiplus::FontFamily> MakeFamily() {
        auto primary = std::make_unique<Gdiplus::FontFamily>(L"Microsoft YaHei UI");
        if (primary->GetLastStatus() == Gdiplus::Ok && primary->IsAvailable()) return primary;
        return std::make_unique<Gdiplus::FontFamily>(L"Segoe UI");
    }

    // 强调色按「事件语义」分配，而不是按「哪个功能先做」。
    // 同一类事件在不同位置（主胶囊、分离小胶囊、展开态进度条）必须同色。
    static Gdiplus::Color AccentFor(const std::string& kind) {
        if (kind.rfind("media", 0) == 0) return Gdiplus::Color(255, 250, 60, 96);
        if (kind.rfind("notification", 0) == 0) return Gdiplus::Color(255, 108, 112, 240);
        if (kind.rfind("clipboard", 0) == 0) return Gdiplus::Color(255, 46, 186, 188);
        if (kind.rfind("file", 0) == 0 || kind.rfind("download", 0) == 0) {
            return Gdiplus::Color(255, 30, 138, 250);
        }
        if (kind.rfind("device", 0) == 0) return Gdiplus::Color(255, 52, 199, 96);
        if (kind.rfind("power", 0) == 0) return Gdiplus::Color(255, 250, 168, 40);
        // 会议用「录音红」：这个颜色在系统里几乎只表示"正在录/正在播"，
        // 用户扫一眼就知道自己还在别人耳朵里。
        if (kind.rfind("meeting", 0) == 0) return Gdiplus::Color(255, 232, 62, 62);
        if (kind.rfind("editor", 0) == 0) return Gdiplus::Color(255, 0, 122, 204);
        if (kind.rfind("volume", 0) == 0) return Gdiplus::Color(255, 150, 152, 160);
        if (kind.rfind("calendar", 0) == 0) return Gdiplus::Color(255, 250, 168, 40);
        if (kind.rfind("presence", 0) == 0) return Gdiplus::Color(255, 120, 124, 236);
        if (kind.rfind("ai", 0) == 0) return Gdiplus::Color(255, 52, 199, 96);
        return Gdiplus::Color(255, 120, 124, 236);
    }

    static BYTE Clamp8(int v) { return static_cast<BYTE>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

    // 整体提亮/压暗：给圆角方块做「上亮下暗」的立体感。
    static Gdiplus::Color Shift(const Gdiplus::Color& c, int d) {
        return Gdiplus::Color(c.GetA(), Clamp8(c.GetR() + d), Clamp8(c.GetG() + d), Clamp8(c.GetB() + d));
    }

    static void FillRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                            const Gdiplus::Color& color) {
        if (radius <= 0.0f) return;
        Gdiplus::GraphicsPath path;
        AddRounded(path, r, radius);
        Gdiplus::SolidBrush brush(color);
        g.FillPath(&brush, &path);
    }

    static void AddRounded(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
        const float d = std::min(radius * 2.0f, std::min(r.Width, r.Height));
        const float x = r.X, y = r.Y, w = r.Width, h = r.Height;
        if (d <= 0.0f) {
            path.AddRectangle(r);
            return;
        }
        path.AddArc(x, y, d, d, 180.0f, 90.0f);
        path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
        path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
        path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }

    // 投影：离屏画一枚实心圆角矩形 → 可分离盒模糊 ×3 趟 → 贴回主画布。
    //
    // 旧实现是 7 层逐层放大的同心圆角矩形，每层 alpha 硬叠加。浅色背景上
    // 层与层之间是肉眼可见的「洋葱圈」色带（用户反馈为"毛边"）——每层
    // 边缘本身又是硬边，两重毛糙叠加。盒模糊给出一条单调平滑的 alpha
    // 衰减曲线，既没有色带也没有硬边，这是所有正经窗口投影的画法。
    //
    // 模糊结果按 (宽, 高, alpha, 半径) 缓存：静止画面上零成本，
    // 只有出现/展开动画期间（alpha 或尺寸在变）每帧重算一次。
    void DropShadow(Gdiplus::Graphics& g, const Gdiplus::RectF& r, BYTE alpha, float scale) {
        const int radius_px = std::lround(7.0f * scale);
        const int drop_px = std::lround(3.5f * scale);
        const int pad = radius_px * 2 + drop_px + 2;
        const int bw = static_cast<int>(std::lround(r.Width)) + pad * 2;
        const int bh = static_cast<int>(std::lround(r.Height)) + pad * 2;
        const std::uint32_t key = static_cast<std::uint32_t>(bw) << 20 ^
                                  static_cast<std::uint32_t>(bh) << 6 ^
                                  static_cast<std::uint32_t>(alpha) << 16 ^
                                  static_cast<std::uint32_t>(radius_px);
        if (shadow_key_ != key || shadow_ == nullptr) {
            RenderShadowBitmap(bw, bh, static_cast<int>(std::lround(r.Width)),
                               static_cast<int>(std::lround(r.Height)), alpha, radius_px);
            shadow_key_ = key;
        }
        if (shadow_ == nullptr) return;
        g.DrawImage(shadow_.get(), Gdiplus::RectF(r.X - pad, r.Y - pad + drop_px,
                                                  static_cast<float>(bw), static_cast<float>(bh)),
                    0, 0, static_cast<float>(bw), static_cast<float>(bh), Gdiplus::UnitPixel);
    }

    // 离屏生成一枚模糊好的投影。缓存到成员，静止画面零成本。
    // 投影的 RGB 恒为 0（纯黑），模糊实质只作用在 alpha 通道上。
    void RenderShadowBitmap(int bw, int bh, int pw, int ph, BYTE alpha, int radius_px) {
        shadow_.reset();
        std::unique_ptr<Gdiplus::Bitmap> bmp(new Gdiplus::Bitmap(bw, bh, PixelFormat32bppARGB));
        if (bmp->GetLastStatus() != Gdiplus::Ok) return;
        {
            Gdiplus::Graphics g(bmp.get());
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const Gdiplus::RectF rect((bw - pw) * 0.5f, (bh - ph) * 0.5f,
                                      static_cast<float>(pw), static_cast<float>(ph));
            FillRounded(g, rect, ph * 0.5f,
                        Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.22f)), 0, 0, 0));
        }
        Gdiplus::BitmapData data;
        const Gdiplus::Rect lock(0, 0, bw, bh);
        if (bmp->LockBits(&lock, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite,
                          PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
            return;
        }
        auto* bits = static_cast<std::uint8_t*>(data.Scan0);
        const std::size_t stride = static_cast<std::size_t>(data.Stride >= 0 ? data.Stride : -data.Stride);
        std::vector<std::uint8_t> buf(bits, bits + stride * bh);
        for (int pass = 0; pass < 3; ++pass) {  // 3 趟盒模糊 ≈ 高斯，边缘单调无色带
            BoxBlurAxis(buf, bw, bh, radius_px, true);
            BoxBlurAxis(buf, bw, bh, radius_px, false);
        }
        for (int y = 0; y < bh; ++y) {
            std::memcpy(bits + static_cast<std::size_t>(y) * stride,
                        buf.data() + static_cast<std::size_t>(y) * stride,
                        static_cast<std::size_t>(bw) * 4u);
        }
        bmp->UnlockBits(&data);
        shadow_ = std::move(bmp);
    }

    // 可分离盒模糊（单方向一趟）。滑动窗口累加和，O(n) 与半径无关。
    // 与 backdrop.hpp 里的实现同源；那里那版的教训记在这里：
    // 输出指针必须随坐标前进，且边缘窗口样本数按实际数量相除，否则四边压暗。
    static void BoxBlurAxis(std::vector<std::uint8_t>& buf, int w, int h, int radius, bool horizontal) {
        if (radius <= 0) return;
        std::vector<std::uint8_t> tmp(buf.size());
        const int len = horizontal ? w : h;
        const int lines = horizontal ? h : w;
        for (int line = 0; line < lines; ++line) {
            const std::uint8_t* src;
            std::uint8_t* dst;
            std::size_t step;
            if (horizontal) {
                const std::size_t row = static_cast<std::size_t>(line) * w * 4u;
                src = buf.data() + row;
                dst = tmp.data() + row;
                step = 4u;
            } else {
                src = buf.data() + static_cast<std::size_t>(line) * 4u;
                dst = tmp.data() + static_cast<std::size_t>(line) * 4u;
                step = static_cast<std::size_t>(w) * 4u;
            }
            unsigned acc[4] = {0, 0, 0, 0};
            int clo = 0;
            int chi = (radius < len - 1 ? radius : len - 1);
            for (int i = clo; i <= chi; ++i) {
                const std::uint8_t* p = src + static_cast<std::size_t>(i) * step;
                acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
            }
            for (int x = 0; x < len; ++x) {
                const int lo = (x - radius > 0) ? x - radius : 0;
                const int hi = (x + radius < len - 1) ? x + radius : len - 1;
                while (chi < hi) {  // 右缘滑入
                    ++chi;
                    const std::uint8_t* p = src + static_cast<std::size_t>(chi) * step;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                }
                while (clo < lo) {  // 左缘滑出
                    const std::uint8_t* p = src + static_cast<std::size_t>(clo) * step;
                    acc[0] -= p[0]; acc[1] -= p[1]; acc[2] -= p[2]; acc[3] -= p[3];
                    ++clo;
                }
                std::uint8_t* o = dst + static_cast<std::size_t>(x) * step;
                o[0] = static_cast<std::uint8_t>(acc[0] / (hi - lo + 1));
                o[1] = static_cast<std::uint8_t>(acc[1] / (hi - lo + 1));
                o[2] = static_cast<std::uint8_t>(acc[2] / (hi - lo + 1));
                o[3] = static_cast<std::uint8_t>(acc[3] / (hi - lo + 1));
            }
        }
        buf.swap(tmp);
    }


    // 胶囊本体：玻璃或深色不透明底 + 上亮下暗的描边。
    //
    // 关键教训：**不透明路径的受光不能用半透明白色叠上去**。
    // GDI+ 的 HighQuality 合成走的是 gamma 空间的 alpha 混合，而底色 (10,10,13)
    // 在线性空间里几乎为 0 —— 叠 7.5% 的白，线性插值再转回 sRGB，实际输出是
    // RGB(64,64,64) 而不是预期的 RGB(26,26,26)。整条胶囊因此蒙上一层灰雾，
    // 怎么调 alpha 都觉得"脏"。
    // 改成**不透明渐变**（中心 #222229 → 边缘底色）后，写多少就是多少，
    // 用 readback 探针能逐像素核对，不再依赖对混合公式的猜测。
    // 玻璃路径（DrawGlassBody）不受此约束：它叠在"染色后的模糊背景"上，
    // 底色是中间调，gamma 混合的偏差肉眼不可辨。
    static void DrawPill(Gdiplus::Graphics& g, const Gdiplus::RectF& r, BYTE alpha, float scale,
                         const GlassParams& glass, int mode = 1) {
        // 圆角必须封顶。原先直接取 Height * 0.5：矮胶囊（46）是药丸没问题，
        // 但展开态涨到 196 后半径变成 98 —— 两端成了半圆，而封面、时间、
        // 页脚提示仍按矩形排布，全都落到圆弧外面，看起来就是「内容越界」。
        // 紧凑/休眠态依然走 Height*0.5（保持药丸），展开态固定 28 逻辑像素。
        const float radius = std::min(r.Height * 0.5f, 28.0f * scale);
        Gdiplus::GraphicsPath path;
        AddRounded(path, r, radius);

        if (mode == 0) {
            // 液态玻璃：**依然是玻璃**，只是把染色压到很低（0.32 对 0.70）。
            // 早先这一档是「一个像素都不画」，那是把"透明"理解成了"没有"——
            // 结果只剩几行字浮在壁纸上，既没有材质感，浅色壁纸上还读不清。
            // 真正好看的那种（iOS 26 的 liquid glass）是：透，但边缘有折射亮环、
            // 顶上有镜面线、底边有厚度，所以你能看出"这里有一层东西"。
            DrawLiquidGlassBody(g, path, r, alpha, glass, scale);
            DrawLiquidRim(g, path, r, alpha, scale);
            return;
        }
        const bool use_glass = mode != 2 && glass.backdrop != nullptr;
        if (use_glass) {
            DrawGlassBody(g, path, r, alpha, glass);
            DrawRim(g, path, r, alpha, scale, true);
            DrawInnerRim(g, r, radius, alpha, scale, true);
        } else {
            DrawOpaqueBody(g, path, r, alpha);
            DrawRim(g, path, r, alpha, scale, false);
            DrawInnerRim(g, r, radius, alpha, scale, false);
        }
    }

    // 液态玻璃本体。
    //
    // 和「均衡」档（DrawGlassBody）的区别不是"更透明"，而是材质不同：
    //   均衡    ：70% 深色染色 —— 稳、可读，但更像一块深色磨砂板
    //   液态玻璃：32% 染色 + 强边缘折射 + 左上一道反光 —— 像一层弯曲的玻璃
    //
    // 为什么不能只把 0.70 调成 0.32：染色一淡，背后的桌面就直接透上来，
    // 深色和浅色壁纸上会得到两种完全不同的东西（一个灰、一个亮）。
    // 所以必须同时补上"凭自身结构就能看出是玻璃"的三样东西：
    // 边缘聚光、顶部镜面线、左上的反射光斑。有了它们，颜色深浅就不再重要。
    static void DrawLiquidGlassBody(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& path,
                                    const Gdiplus::RectF& r, BYTE alpha, const GlassParams& glass,
                                    float scale) {
        const Gdiplus::GraphicsState state = g.Save();
        g.SetClip(&path);

        // 1) 背景纹理：和均衡档同一套对齐方式（按 offset 平移 + 全尺寸放大）。
        if (glass.backdrop != nullptr && glass.full_w > 0.0f && glass.full_h > 0.0f) {
            g.DrawImage(glass.backdrop,
                        Gdiplus::RectF(glass.offset_x, glass.offset_y, glass.full_w, glass.full_h), 0,
                        0, static_cast<float>(glass.backdrop->GetWidth()),
                        static_cast<float>(glass.backdrop->GetHeight()), Gdiplus::UnitPixel);
        }

        // 2) 极轻的冷色染色。
        //    这里踩过一次大坑：0.32 的中性深色在浅米色壁纸上会得到一片**均匀的灰**
        //    —— 看起来是"灰色塑料"，不是玻璃。玻璃通不透明不该靠"整体压暗"来实现。
        //    现在降到 0.18 并让色相偏冷蓝（不是中性灰），背景色才透得出来。
        //
        //    色相必须中性：早先用了 (16,18,30) 的冷蓝，配上冷白高光，把暖米色壁纸
        //    一路洗成了冷灰 —— 交互上没毛病，但看起来就是"灰色塑料"。
        {
            Gdiplus::SolidBrush tint(
                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.09f)), 20, 21, 26));
            g.FillRectangle(&tint, r);
        }

        // 3) 左上反光：一片从左上角淡出的冷白光斑。
        //    真实玻璃是弯的，光源在左上，凸面会把光聚到那一侧 ——
        //    没有这片光，玻璃就只是一块均匀的半透明薄膜。
        {
            Gdiplus::PathGradientBrush glow(&path);
            glow.SetCenterPoint(Gdiplus::PointF(r.X + r.Width * 0.24f, r.Y + r.Height * 0.10f));
            //    颜色必须是**中性白**。玻璃的高光来自白色光源，加一点蓝就会把
            //    暖色壁纸的色相整体拉冷 —— 浅米色桌面因此变成冷灰，这是
            //    "灰色塑料感"的最大来源。
            glow.SetCenterColor(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.17f)), 255, 255, 255));
            // focus 收得很小（0.30/0.22）：光斑只聚在左上角一小片。
            // 铺满整个面的话，白色叠加会把背景色的饱和度一点点吃掉 ——
            // 暖米色就是这么变成中性灰的（实测 R-B 差从 34 掉到 4）。
            glow.SetFocusScales(0.30f, 0.22f);
            const INT points = path.GetPointCount();
            if (points > 0) {
                std::vector<Gdiplus::Color> ring(static_cast<std::size_t>(points),
                                                 Gdiplus::Color(0, 255, 255, 255));
                INT count = points;
                glow.SetSurroundColors(ring.data(), &count);
                g.FillPath(&glow, &path);
            }
        }

        // 4) 底部压暗 + 内反射：玻璃有厚度，底边必然比中间沉。
        //    这两笔比均衡档淡，但必须有 —— 少了就没有"体积"。
        {
            // 底部压暗要比均衡档轻得多（0.09 对 0.20）。压重了整块会"上轻下重"，
            // 看着不像一层薄玻璃，倒像半块深色砖 —— 实测 0.16 时下半截已经发闷。
            Gdiplus::LinearGradientBrush depth(
                Gdiplus::PointF(r.X, r.Y + r.Height * 0.62f), Gdiplus::PointF(r.X, r.Y + r.Height),
                Gdiplus::Color(0, 0, 0, 0),
                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.06f)), 0, 0, 0));
            g.FillRectangle(&depth,
                            Gdiplus::RectF(r.X, r.Y + r.Height * 0.62f, r.Width, r.Height * 0.38f));

            Gdiplus::LinearGradientBrush reflect(
                Gdiplus::PointF(r.X, r.Y + r.Height * 0.84f), Gdiplus::PointF(r.X, r.Y + r.Height),
                Gdiplus::Color(0, 255, 255, 255),
                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.10f)), 255, 255, 255));
            g.FillRectangle(&reflect,
                            Gdiplus::RectF(r.X, r.Y + r.Height * 0.84f, r.Width, r.Height * 0.16f));
        }

        // 5) 上半受光：一层从顶缘向下衰减的冷白。
        //    玻璃是凸面，顶部会把光"铺开"，所以这里不能只有一条 1px 亮线 ——
        //    那是镀铬的观感。要有这一段渐变，材质才会显得"厚且有弧度"。
        {
            Gdiplus::Color stops[3] = {
                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.15f)), 255, 255, 255),
                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.05f)), 255, 255, 255),
                Gdiplus::Color(0, 255, 255, 255)};
            Gdiplus::REAL at[3] = {0.0f, 0.30f, 1.0f};
            Gdiplus::LinearGradientBrush sheen(
                Gdiplus::PointF(r.X, r.Y), Gdiplus::PointF(r.X, r.Y + r.Height * 0.28f),
                Gdiplus::Color(0, 0, 0, 0), Gdiplus::Color(0, 0, 0, 0));
            sheen.SetInterpolationColors(stops, at, 3);
            g.FillRectangle(&sheen, Gdiplus::RectF(r.X, r.Y, r.Width, r.Height * 0.28f));
        }

        // 6) 顶部镜面线。液态玻璃最标志性的一笔，比均衡档更亮更细，
        //    且只保留顶部 32% —— 整圈画就变成镀铬边框了。
        {
            Gdiplus::GraphicsPath band;
            band.AddRectangle(Gdiplus::RectF(r.X, r.Y, r.Width, r.Height * 0.32f));
            const Gdiplus::GraphicsState clip_state = g.Save();
            g.SetClip(&band);
            g.SetClip(&path, Gdiplus::CombineModeIntersect);
            Gdiplus::Pen specular(
                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.88f)), 255, 255, 255),
                (std::max)(1.0f, (std::max)(r.Height * 0.024f, 1.4f * scale)));
            g.DrawPath(&specular, &path);
            g.Restore(clip_state);
        }

        g.Restore(state);
    }

    // 液态玻璃的边：一圈内缩的折射亮环（上亮下暗），外加极淡的外缘暗线。
    //
    // 这一圈是"液态"的关键。玻璃是弯的，视线擦过边缘时会看到被折射聚拢的光，
    // 所以边缘比中心亮；而均衡档的边是"外框"，方向恰好相反（中心亮边缘暗）。
    static void DrawLiquidRim(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& path,
                              const Gdiplus::RectF& r, BYTE alpha, float scale) {
        // 外缘：一条几乎看不见的暗线，把玻璃和壁纸分开，防止浅色壁纸上"没边"。
        {
            const Gdiplus::GraphicsState state = g.Save();
            g.SetClip(&path);
            Gdiplus::Pen edge(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.26f)), 8, 10, 16),
                              (std::max)(1.0f, 1.0f * scale));
            g.DrawPath(&edge, &path);
            g.Restore(state);
        }

        // 折射环：内缩 2px 画一圈上亮下暗的细线（外圈亮 → 内圈立刻收暗，
        // 形成"玻璃有厚度"的错觉）。
        const float inset = (std::max)(1.5f, 2.0f * scale);
        const Gdiplus::RectF inner(r.X + inset, r.Y + inset, r.Width - inset * 2.0f,
                                   r.Height - inset * 2.0f);
        const float radius = (std::max)(0.0f, (std::min)(r.Height * 0.5f, 28.0f * scale) - inset);
        Gdiplus::GraphicsPath hp;
        AddRounded(hp, inner, radius);
        // 亮度刻意不过分（0.46 而不是 0.62）：折射是"光被玻璃弯了一下"，
        // 是柔的；拉到 0.62 就变成一圈硬白描边，像霓虹灯管。
        Gdiplus::Color stops[4] = {
            Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.46f)), 255, 255, 255),
            Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.20f)), 255, 255, 255),
            Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.07f)), 236, 236, 240),
            Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.15f)), 255, 255, 255)};
        Gdiplus::REAL at[4] = {0.0f, 0.24f, 0.72f, 1.0f};
        Gdiplus::LinearGradientBrush rim(Gdiplus::PointF(inner.X, inner.Y),
                                        Gdiplus::PointF(inner.X, inner.Y + inner.Height),
                                        Gdiplus::Color(0, 0, 0, 0), Gdiplus::Color(0, 0, 0, 0));
        rim.SetInterpolationColors(stops, at, 4);
        Gdiplus::Pen pen(&rim, (std::max)(1.0f, 0.9f * scale));
        const Gdiplus::GraphicsState state = g.Save();
        g.SetClip(&hp);
        g.DrawPath(&pen, &hp);
        g.Restore(state);
    }

    // 不透明底：实色 + 径向受光（等值线是弧线，dither 打散剩余台阶）。
    static void DrawOpaqueBody(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& path,
                               const Gdiplus::RectF& r, BYTE alpha) {
        const BYTE solid = Clamp8(static_cast<int>(alpha * 0.97f));
        const Gdiplus::Color base_color(solid, 10, 10, 13);

        Gdiplus::SolidBrush fill(base_color);
        g.FillPath(&fill, &path);

        Gdiplus::PathGradientBrush glow(&path);
        glow.SetCenterPoint(Gdiplus::PointF(r.X + r.Width * 0.5f, r.Y + r.Height * 0.24f));
        glow.SetCenterColor(Gdiplus::Color(solid, 36, 36, 44));
        glow.SetFocusScales(0.26f, 0.10f);
        const INT points = path.GetPointCount();
        if (points > 0) {
            std::vector<Gdiplus::Color> ring(static_cast<std::size_t>(points), base_color);
            INT count = points;
            glow.SetSurroundColors(ring.data(), &count);
        }
        g.FillPath(&glow, &path);
    }

    // 玻璃底：模糊背景透出 + 深色染色 + 顶部镜面高光 + 底部内反射。
    // 配方接近 macOS 磨砂：背景只贡献色相与大块明暗（60% 染色压暗），
    // 文字可读性靠染色，玻璃感靠高光与反光的两道亮边。
    static void DrawGlassBody(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& path,
                              const Gdiplus::RectF& r, BYTE alpha, const GlassParams& glass) {
        const Gdiplus::GraphicsState state = g.Save();
        g.SetClip(&path);

        // 1) 透出背景：纹理覆盖的是整个捕获矩形，必须按 offset 平移、按全尺寸
        //    放大对齐，否则胶囊里的"玻璃"和背后真实内容会错位，一看就是贴图。
        if (glass.backdrop != nullptr && glass.full_w > 0.0f && glass.full_h > 0.0f) {
            g.DrawImage(glass.backdrop,
                        Gdiplus::RectF(glass.offset_x, glass.offset_y, glass.full_w, glass.full_h), 0,
                        0, static_cast<float>(glass.backdrop->GetWidth()),
                        static_cast<float>(glass.backdrop->GetHeight()), Gdiplus::UnitPixel);
        }

        // 2) 染色：70% 深色。玻璃的可读性完全靠这一层 —— 60% 时在浅色桌面上
        //    内部会亮到 #AB（实测），白字压不住，看着像灰色塑料而不是玻璃。
        //    70% 之后白底透出约 #54，白字对比回到 5:1 以上。
        {
            Gdiplus::SolidBrush tint(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.70f)), 14, 14, 19));
            g.FillRectangle(&tint, r);
        }

        // 3) 受光：只给顶部一点冷白，且必须在 45% 处衰减到 **0**。
        //    踩过的坑：让"受光"覆盖整个胶囊（哪怕底部还有 0.02）在深色桌面上
        //    会把玻璃整块提亮 —— 黑底实测均值 85，看起来像磨砂白塑料。
        //    白色叠加是加法，而玻璃的明暗应该来自"背后透出什么"，不是自己发光。
        //    所以只有顶缘一圈微光，其余交给背景与染色。
        {
            Gdiplus::Color stops[3] = {Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.12f)), 208, 219,
                                                      244),
                                       Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.05f)), 208, 219,
                                                      244),
                                       Gdiplus::Color(0, 208, 219, 244)};
            Gdiplus::REAL at[3] = {0.0f, 0.18f, 0.45f};
            Gdiplus::LinearGradientBrush sheen(Gdiplus::PointF(r.X, r.Y),
                                              Gdiplus::PointF(r.X, r.Y + r.Height * 0.45f),
                                              Gdiplus::Color(0, 0, 0, 0), Gdiplus::Color(0, 0, 0, 0));
            sheen.SetInterpolationColors(stops, at, 3);
            g.FillRectangle(&sheen, Gdiplus::RectF(r.X, r.Y, r.Width, r.Height * 0.45f));
        }

        // 4) 底部压暗：玻璃有体积，底边比中间更沉。没有这层，染色是"平"的，
        //    看起来像贴了一张半透明色纸。
        {
            Gdiplus::LinearGradientBrush depth(
                Gdiplus::PointF(r.X, r.Y + r.Height * 0.62f), Gdiplus::PointF(r.X, r.Y + r.Height),
                Gdiplus::Color(0, 0, 0, 0),
                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.20f)), 0, 0, 0));
            g.FillRectangle(&depth, Gdiplus::RectF(r.X, r.Y + r.Height * 0.62f, r.Width,
                                                   r.Height * 0.38f));
        }

        // 5) 底部内反射：玻璃底边会接住一点光，很淡（alpha 0.05），
        //    没有它底边就是"被切掉"的，有它才有厚度。
        {
            Gdiplus::LinearGradientBrush reflect(
                Gdiplus::PointF(r.X, r.Y + r.Height * 0.86f), Gdiplus::PointF(r.X, r.Y + r.Height),
                Gdiplus::Color(0, 255, 255, 255),
                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.05f)), 255, 255, 255));
            g.FillRectangle(&reflect, Gdiplus::RectF(r.X, r.Y + r.Height * 0.86f, r.Width,
                                                     r.Height * 0.14f));
        }

        // 6) 顶边镜面线：玻璃最有辨识度的一笔 —— 一条贴着顶弧的细白亮线。
        //    只保留顶部 36%，肩部到侧边自然消失，整圈画会变成镀铬边框。
        //    必须和胶囊路径求交再画：Pen 有一半落在路径外，露出去就是一条毛边。
        {
            Gdiplus::GraphicsPath band;
            band.AddRectangle(Gdiplus::RectF(r.X, r.Y, r.Width, r.Height * 0.36f));
            const Gdiplus::GraphicsState clip_state = g.Save();
            g.SetClip(&band);
            // 与胶囊路径求交：Graphics 没有 path 版的 IntersectClip，
            // 用带 CombineMode 的 SetClip 才是"再裁一刀"的语义。
            g.SetClip(&path, Gdiplus::CombineModeIntersect);
            // 线宽按胶囊高度取，避免把 scale 一路传进来（这个函数只管像素）。
            Gdiplus::Pen specular(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.40f)), 255, 255,
                                                 255),
                                  std::max(1.0f, r.Height * 0.028f));
            g.DrawPath(&specular, &path);
            g.Restore(clip_state);
        }

        g.Restore(state);
    }

    // 一圈玻璃边：上亮下暗，整圈同亮度会像塑料贴纸。
    // 亮度用四点插值而不是两点线性 —— 两点线性会让左右两侧的竖直弧段
    // 停在中间值（约 #2D2D33），在纯黑背景上就是一圈明显的"银边"；
    // 把亮度压进顶部 22%，侧边就自然暗下去，只剩顶部一道受光。
    // glassy：玻璃款的顶边要更亮更"锐"（真实玻璃边缘会折射出一条亮线），
    // 不透明款保持原来的低调边，否则深色胶囊会像镀了铬。
    static void DrawRim(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& path,
                        const Gdiplus::RectF& r, BYTE alpha, float scale, bool glassy) {
        const BYTE solid = Clamp8(static_cast<int>(alpha * 0.97f));
        Gdiplus::LinearGradientBrush rim(
            Gdiplus::PointF(r.X, r.Y), Gdiplus::PointF(r.X, r.Y + r.Height),
            Gdiplus::Color(solid, 74, 74, 86), Gdiplus::Color(solid, 9, 9, 12));
        Gdiplus::Color stops[4];
        Gdiplus::REAL at[4] = {0.0f, 0.20f, 0.60f, 1.0f};
        if (glassy) {
            stops[0] = Gdiplus::Color(solid, 176, 186, 206);
            stops[1] = Gdiplus::Color(solid, 88, 91, 104);
            stops[2] = Gdiplus::Color(solid, 26, 26, 32);
            stops[3] = Gdiplus::Color(solid, 14, 14, 18);
            at[1] = 0.16f;
            at[2] = 0.52f;
        } else {
            stops[0] = Gdiplus::Color(solid, 78, 78, 90);
            stops[1] = Gdiplus::Color(solid, 40, 40, 48);
            stops[2] = Gdiplus::Color(solid, 16, 16, 21);
            stops[3] = Gdiplus::Color(solid, 9, 9, 12);
        }
        rim.SetInterpolationColors(stops, at, 4);
        Gdiplus::Pen pen(&rim, std::max(1.0f, 1.5f * scale));
        // 描边必须裁到胶囊内侧再画。Pen 以路径为中心线，默认有一半落在路径外；
        // 那半个像素在浅色壁纸上就是一圈灰线，看起来像胶囊被切了一刀。
        const Gdiplus::GraphicsState state = g.Save();
        g.SetClip(&path);
        g.DrawPath(&pen, &path);
        g.Restore(state);
    }

    // 内圈再补一条更淡的线：玻璃的「厚度」靠这两圈差出来。
    // 内圈和外圈之间留 2.6px，太近会在 2x 缩放下糊成一条宽亮带。
    static void DrawInnerRim(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, BYTE alpha,
                             float scale, bool glassy) {
        const BYTE solid = Clamp8(static_cast<int>(alpha * 0.97f));
        const float inset = 2.6f * scale;
        Gdiplus::RectF hi(r.X + inset, r.Y + inset, r.Width - inset * 2.0f, r.Height - inset * 2.0f);
        Gdiplus::GraphicsPath hp;
        AddRounded(hp, hi, std::max(0.0f, radius - inset));
        const Gdiplus::Color top_color = glassy ? Gdiplus::Color(solid, 84, 88, 102)
                                               : Gdiplus::Color(solid, 34, 34, 41);
        const Gdiplus::Color mid_color = glassy ? Gdiplus::Color(solid, 26, 27, 33)
                                               : Gdiplus::Color(solid, 17, 17, 21);
        Gdiplus::LinearGradientBrush inner_rim(Gdiplus::PointF(hi.X, hi.Y),
                                              Gdiplus::PointF(hi.X, hi.Y + hi.Height), top_color,
                                              Gdiplus::Color(solid, 10, 10, 13));
        Gdiplus::Color stops[3] = {top_color, mid_color, Gdiplus::Color(solid, 10, 10, 13)};
        Gdiplus::REAL at[3] = {0.0f, 0.30f, 1.0f};
        inner_rim.SetInterpolationColors(stops, at, 3);
        Gdiplus::Pen pen(&inner_rim, std::max(1.0f, 0.8f * scale));
        g.DrawPath(&pen, &hp);
    }

    static Gdiplus::RectF ChipRect(const Gdiplus::RectF& pill, float size) {
        return Gdiplus::RectF(pill.X + pill.Height * 0.5f - size * 0.5f,
                              pill.Y + pill.Height * 0.5f - size * 0.5f, size, size);
    }

    // 各家 Agent 的品牌色。取的是各家产品标识的主色（饱和度压低一点，
    // 因为底是一块深色玻璃，原色直接上会「跳出屏幕」）。
    static Gdiplus::Color AgentAccentFor(const std::string& provider) {
        if (provider == "claude") return Gdiplus::Color(255, 214, 116, 88);     // Anthropic 陶土橙
        if (provider == "codex") return Gdiplus::Color(255, 24, 166, 133);      // OpenAI 青绿
        if (provider == "gemini") return Gdiplus::Color(255, 88, 132, 240);     // Google 蓝
        if (provider == "workbuddy") return Gdiplus::Color(255, 96, 104, 236);  // WorkBuddy 靛蓝
        if (provider == "zcode") return Gdiplus::Color(255, 74, 124, 232);      // ZCode 蓝
        if (provider == "cursor") return Gdiplus::Color(255, 128, 134, 152);    // Cursor 中性灰
        if (provider == "pi") return Gdiplus::Color(255, 152, 122, 240);        // Pi 紫
        return AccentFor("agent");
    }

    static Gdiplus::Color StateColor(const std::string& state) {
        if (state == "running") return Gdiplus::Color(255, 62, 208, 122);
        if (state == "waiting") return Gdiplus::Color(255, 250, 176, 52);
        return Gdiplus::Color(255, 158, 162, 174);
    }

    // Agent 的记号。刻意不用字母（C / W / G 在 30px 上糊成一团）：
    // 每家一个能认出来的几何形状，比缩写可靠。
    void DrawAgentGlyph(Gdiplus::Graphics& g, const std::string& provider, const Gdiplus::RectF& box,
                        const Gdiplus::Color& color) {
        const float cx = box.X + box.Width * 0.5f;
        const float cy = box.Y + box.Height * 0.5f;
        const float s = box.Width;

        if (provider == "claude") {
            // 六向星芒：Anthropic 的记号是一朵放射状的花。
            Gdiplus::Pen pen(color, std::max(1.5f, s * 0.075f));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            for (int i = 0; i < 6; ++i) {
                const double a = 3.14159265358979 * static_cast<double>(i) / 3.0 + 0.30;
                g.DrawLine(&pen, cx + static_cast<float>(std::cos(a)) * s * 0.08f,
                           cy + static_cast<float>(std::sin(a)) * s * 0.08f,
                           cx + static_cast<float>(std::cos(a)) * s * 0.27f,
                           cy + static_cast<float>(std::sin(a)) * s * 0.27f);
            }
            return;
        }
        if (provider == "codex") {
            // >_ 提示符：终端里 agent 的通用符号，和「它在跑命令」的语义一致。
            Gdiplus::Pen pen(color, std::max(1.6f, s * 0.085f));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&pen, cx - s * 0.21f, cy - s * 0.13f, cx - s * 0.05f, cy - s * 0.01f);
            g.DrawLine(&pen, cx - s * 0.05f, cy - s * 0.01f, cx - s * 0.21f, cy + s * 0.11f);
            g.DrawLine(&pen, cx + s * 0.02f, cy + s * 0.11f, cx + s * 0.21f, cy + s * 0.11f);
            return;
        }
        if (provider == "gemini") {
            // 四角星（sparkle）：两枚细长菱形叠成一个星。
            Gdiplus::SolidBrush brush(color);
            Gdiplus::PointF tall[4] = {{cx, cy - s * 0.30f},
                                        {cx + s * 0.105f, cy},
                                        {cx, cy + s * 0.30f},
                                        {cx - s * 0.105f, cy}};
            Gdiplus::PointF wide[4] = {{cx - s * 0.30f, cy},
                                        {cx, cy - s * 0.105f},
                                        {cx + s * 0.30f, cy},
                                        {cx, cy + s * 0.105f}};
            g.FillPolygon(&brush, tall, 4);
            g.FillPolygon(&brush, wide, 4);
            return;
        }
        if (provider == "cursor") {
            // 文本光标：一根竖柱 + 上下两道短横。Cursor 的名字就是它。
            Gdiplus::Pen pen(color, std::max(1.5f, s * 0.08f));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&pen, cx, cy - s * 0.22f, cx, cy + s * 0.22f);
            g.DrawLine(&pen, cx - s * 0.13f, cy - s * 0.22f, cx + s * 0.13f, cy - s * 0.22f);
            g.DrawLine(&pen, cx - s * 0.13f, cy + s * 0.22f, cx + s * 0.13f, cy + s * 0.22f);
            return;
        }
        if (provider == "zcode") {
            // Z：上横 → 斜线 → 下横
            Gdiplus::Pen pen(color, std::max(1.5f, s * 0.085f));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&pen, cx - s * 0.19f, cy - s * 0.19f, cx + s * 0.19f, cy - s * 0.19f);
            g.DrawLine(&pen, cx + s * 0.16f, cy - s * 0.16f, cx - s * 0.16f, cy + s * 0.16f);
            g.DrawLine(&pen, cx - s * 0.19f, cy + s * 0.19f, cx + s * 0.19f, cy + s * 0.19f);
            return;
        }
        if (provider == "pi") {
            // π：一横 + 两条腿（左腿略斜，接近手写体）
            Gdiplus::Pen pen(color, std::max(1.6f, s * 0.09f));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&pen, cx - s * 0.22f, cy - s * 0.16f, cx + s * 0.22f, cy - s * 0.16f);
            g.DrawLine(&pen, cx - s * 0.10f, cy - s * 0.13f, cx - s * 0.13f, cy + s * 0.21f);
            g.DrawLine(&pen, cx + s * 0.10f, cy - s * 0.16f, cx + s * 0.10f, cy + s * 0.21f);
            return;
        }
        // workbuddy / 未知：圆环 + 中心点 —— 「一个在干活的助手」。
        Gdiplus::Pen pen(color, std::max(1.6f, s * 0.085f));
        g.DrawEllipse(&pen, cx - s * 0.19f, cy - s * 0.19f, s * 0.38f, s * 0.38f);
        Gdiplus::SolidBrush brush(color);
        g.FillEllipse(&brush, cx - s * 0.065f, cy - s * 0.065f, s * 0.13f, s * 0.13f);
    }

    // 状态点：画在 chip 右下角，像一枚小徽章。
    // 三种状态三种颜色 + running 会呼吸 —— 用户扫一眼就知道它是在跑还是停了。
    void DrawStateDot(Gdiplus::Graphics& g, const Gdiplus::RectF& chip, BYTE alpha, float scale,
                      const std::string& state, double wave_phase) {
        const float d = chip.Width * 0.34f;
        const Gdiplus::RectF dot(chip.GetRight() - d * 0.86f, chip.GetBottom() - d * 0.86f, d, d);
        // 深色描边把点从彩色方块上「抠」出来，否则同色系会糊在一起。
        Gdiplus::SolidBrush ring(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.92f)), 18, 18, 24));
        g.FillEllipse(&ring, dot.X - 1.6f * scale, dot.Y - 1.6f * scale, dot.Width + 3.2f * scale,
                      dot.Height + 3.2f * scale);
        int a = static_cast<int>(alpha * 0.98f);
        if (state == "running") {
            const double breath = 0.5 + 0.5 * std::sin(wave_phase * 0.9);
            a = static_cast<int>(alpha * (0.55 + 0.45 * breath));
        }
        const Gdiplus::Color c = StateColor(state);
        Gdiplus::SolidBrush fill(Gdiplus::Color(Clamp8(a), c.GetR(), c.GetG(), c.GetB()));
        g.FillEllipse(&fill, dot);
    }

    // 运行中的「心跳」：三个点依次起伏。比进度条更适合「不知道还剩多久」的场景，
    // 而且它一直在动，说明这条数据是活的、不是一帧残留。
    void DrawAgentPulse(Gdiplus::Graphics& g, const Gdiplus::RectF& box, BYTE alpha, float scale,
                        double wave_phase, bool running) {
        const float r = 1.9f * scale;
        const float gap = 5.0f * scale;
        for (int i = 0; i < 3; ++i) {
            const double phase = wave_phase * 1.15 - i * 0.7;
            const float lift = running ? static_cast<float>(1.6 * scale * (0.5 + 0.5 * std::sin(phase)))
                                       : 0.0f;
            const int a = running
                              ? static_cast<int>(alpha * (0.45 + 0.55 * (0.5 + 0.5 * std::sin(phase))))
                              : static_cast<int>(alpha * 0.34f);
            const Gdiplus::Color c = StateColor(running ? "running" : "waiting");
            Gdiplus::SolidBrush brush(Gdiplus::Color(Clamp8(a), c.GetR(), c.GetG(), c.GetB()));
            const float x = box.X + static_cast<float>(i) * gap;
            g.FillEllipse(&brush, x, box.Y + box.Height * 0.5f - r - lift, r * 2.0f, r * 2.0f);
        }
    }

    // 左侧强调色圆角方块 + 白色字形。
    // 方块本身做上亮下暗：iOS 的图标是「有厚度的」，纯平色会显得很平。
    void DrawChip(Gdiplus::Graphics& g, const std::string& kind, const Gdiplus::RectF& box, float size,
                  BYTE alpha, const std::string& cover_path = std::string()) {
        DrawChipEx(g, kind, box, size, alpha, cover_path, AccentFor(kind), std::string());
    }

    // 带品牌色与 provider 图形的版本。编码 Agent 的 chip 必须一眼分清是谁：
    // 三家 agent 都用同一个紫色方块 + 一个点，用户根本认不出它在跑哪个。
    void DrawChipEx(Gdiplus::Graphics& g, const std::string& kind, const Gdiplus::RectF& box,
                    float size, BYTE alpha, const std::string& cover_path,
                    const Gdiplus::Color& accent, const std::string& provider) {
        const float radius = size * 0.34f;

        // 有真实封面就画封面。封面本身就是最好的识别符号，
        // 再在上面叠一个音符字形只会显得脏 —— 所以这条路径直接 return。
        if (Gdiplus::Bitmap* cover = CoverFor(cover_path)) {
            Gdiplus::GraphicsPath cover_path_shape;
            AddRounded(cover_path_shape, box, radius);
            const Gdiplus::GraphicsState state = g.Save();
            g.SetClip(&cover_path_shape);
            // 中心裁剪而不是拉伸：专辑封面绝大多数是方图，但来源不保证
            // （电台、播客、视频缩略图都可能是 16:9）。直接拉伸会把封面压扁，
            // 在 42px 上看不出来，展开成大图就很明显。
            const float target_ar = box.Width / std::max(1.0f, box.Height);
            const float src_w = static_cast<float>(cover->GetWidth());
            const float src_h = static_cast<float>(cover->GetHeight());
            const float src_ar = src_w / std::max(1.0f, src_h);
            float crop_w = src_w;
            float crop_h = src_h;
            if (src_ar > target_ar) {
                crop_w = src_h * target_ar;
            } else {
                crop_h = src_w / target_ar;
            }
            const float crop_x = (src_w - crop_w) * 0.5f;
            const float crop_y = (src_h - crop_h) * 0.5f;
            g.DrawImage(cover, Gdiplus::RectF(box.X, box.Y, box.Width, box.Height), crop_x, crop_y, crop_w,
                        crop_h, Gdiplus::UnitPixel);
            g.Restore(state);
            // 边缘补一圈极淡的暗边，避免浅色封面在深色胶囊上"发飘"。
            Gdiplus::Pen edge(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.30f)), 0, 0, 0),
                              std::max(1.0f, 0.6f * size * 0.34f));
            g.DrawPath(&edge, &cover_path_shape);
            return;
        }

        Gdiplus::GraphicsPath path;
        AddRounded(path, box, radius);

        Gdiplus::LinearGradientBrush brush(
            Gdiplus::PointF(box.X, box.Y), Gdiplus::PointF(box.X, box.Y + box.Height),
            Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)), Clamp8(accent.GetR() + 18),
                           Clamp8(accent.GetG() + 18), Clamp8(accent.GetB() + 18)),
            Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)), Clamp8(accent.GetR() - 16),
                           Clamp8(accent.GetG() - 16), Clamp8(accent.GetB() - 16)));
        g.FillPath(&brush, &path);

        // 方块内的柔光同样用不透明色，不用白色叠加（原因见 DrawPill 的注释）。
        {
            Gdiplus::PathGradientBrush glow(&path);
            glow.SetCenterPoint(Gdiplus::PointF(box.X + box.Width * 0.5f, box.Y + box.Height * 0.20f));
            glow.SetCenterColor(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)),
                                               Clamp8(accent.GetR() + 34), Clamp8(accent.GetG() + 34),
                                               Clamp8(accent.GetB() + 34)));
            glow.SetFocusScales(0.35f, 0.18f);
            const INT points = path.GetPointCount();
            if (points > 0) {
                std::vector<Gdiplus::Color> ring(
                    static_cast<std::size_t>(points),
                    Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)), accent.GetR(), accent.GetG(),
                                   accent.GetB()));
                INT count = points;
                glow.SetSurroundColors(ring.data(), &count);
            }
            g.FillPath(&glow, &path);
        }

        DrawGlyph(g, kind, box, Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.97f)), 255, 255, 255),
                  provider);
    }

    void DrawGlyph(Gdiplus::Graphics& g, const std::string& kind, const Gdiplus::RectF& box,
                   const Gdiplus::Color& color, const std::string& provider = std::string()) {
        const float cx = box.X + box.Width * 0.5f;
        const float cy = box.Y + box.Height * 0.5f;
        const float s = box.Width;
        Gdiplus::SolidBrush brush(color);

        if (kind.rfind("agent", 0) == 0) {
            DrawAgentGlyph(g, provider, box, color);
            return;
        }

        if (kind.rfind("meeting", 0) == 0) {
            // 话筒：胶囊形麦身 + 下方 U 形箍 + 支架。
            // 比"一个红点"多花三笔，但"麦克风"这件事不需要读文字就能认出来 ——
            // 会议岛的全部意义就是提醒「你还在别人耳朵里」。
            const float mw = s * 0.17f;
            const float mh = s * 0.40f;
            Gdiplus::GraphicsPath body;
            AddRounded(body, Gdiplus::RectF(cx - mw * 0.5f, cy - s * 0.30f, mw, mh), mw * 0.5f);
            g.FillPath(&brush, &body);
            Gdiplus::Pen pen(color, (std::max)(1.2f, s * 0.055f));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            const float r = s * 0.22f;
            g.DrawArc(&pen, cx - r, cy - s * 0.06f, r * 2.0f, r * 2.0f, 10.0f, 160.0f);
            g.DrawLine(&pen, cx, cy - s * 0.06f + r, cx, cy + s * 0.28f);
            g.DrawLine(&pen, cx - s * 0.12f, cy + s * 0.28f, cx + s * 0.12f, cy + s * 0.28f);
            return;
        }

        if (kind.rfind("media", 0) == 0) {
            Gdiplus::PointF pts[3] = {
                Gdiplus::PointF(cx - s * 0.11f, cy - s * 0.20f),
                Gdiplus::PointF(cx - s * 0.11f, cy + s * 0.20f),
                Gdiplus::PointF(cx + s * 0.19f, cy - s * 0.01f)};
            g.FillPolygon(&brush, pts, 3);
            return;
        }
        if (kind.rfind("calendar", 0) == 0) {
            Gdiplus::Pen pen(color, std::max(1.4f, s * 0.11f));
            Gdiplus::RectF body(cx - s * 0.20f, cy - s * 0.16f, s * 0.40f, s * 0.34f);
            Gdiplus::GraphicsPath path;
            AddRounded(path, body, s * 0.07f);
            g.DrawPath(&pen, &path);
            g.DrawLine(&pen, cx - s * 0.20f, cy - s * 0.02f, cx + s * 0.20f, cy - s * 0.02f);
            g.DrawLine(&pen, cx - s * 0.09f, cy - s * 0.24f, cx - s * 0.09f, cy - s * 0.14f);
            g.DrawLine(&pen, cx + s * 0.09f, cy - s * 0.24f, cx + s * 0.09f, cy - s * 0.14f);
            return;
        }
        if (kind.rfind("file", 0) == 0 || kind.find("download") != std::string::npos) {
            Gdiplus::Pen pen(color, std::max(1.5f, s * 0.12f));
            g.DrawLine(&pen, cx, cy - s * 0.22f, cx, cy + s * 0.08f);
            Gdiplus::PointF head[3] = {
                Gdiplus::PointF(cx - s * 0.13f, cy - s * 0.02f),
                Gdiplus::PointF(cx, cy + s * 0.12f),
                Gdiplus::PointF(cx + s * 0.13f, cy - s * 0.02f)};
            g.DrawLines(&pen, head, 3);
            g.DrawLine(&pen, cx - s * 0.18f, cy + s * 0.22f, cx + s * 0.18f, cy + s * 0.22f);
            return;
        }
        if (kind.rfind("clipboard", 0) == 0) {
            // 两层叠放的圆角方块 = 剪贴板
            Gdiplus::Pen pen(color, std::max(1.2f, s * 0.085f));
            Gdiplus::RectF back(cx - s * 0.21f, cy - s * 0.24f, s * 0.26f, s * 0.32f);
            Gdiplus::RectF front(cx - s * 0.01f, cy - s * 0.08f, s * 0.28f, s * 0.34f);
            Gdiplus::GraphicsPath p1;
            Gdiplus::GraphicsPath p2;
            AddRounded(p1, back, s * 0.05f);
            AddRounded(p2, front, s * 0.05f);
            g.DrawPath(&pen, &p1);
            g.DrawPath(&pen, &p2);
            return;
        }
        if (kind.rfind("device", 0) == 0) {
            // U 盘：竖长条 + 顶部插头
            Gdiplus::Pen pen(color, std::max(1.3f, s * 0.10f));
            Gdiplus::RectF body(cx - s * 0.09f, cy - s * 0.12f, s * 0.18f, s * 0.34f);
            Gdiplus::GraphicsPath path;
            AddRounded(path, body, s * 0.05f);
            g.DrawPath(&pen, &path);
            g.FillRectangle(&brush, cx - s * 0.05f, cy - s * 0.24f, s * 0.10f, s * 0.12f);
            return;
        }
        if (kind.rfind("power", 0) == 0) {
            Gdiplus::PointF bolt[6] = {
                Gdiplus::PointF(cx + s * 0.05f, cy - s * 0.24f),
                Gdiplus::PointF(cx - s * 0.15f, cy + s * 0.02f),
                Gdiplus::PointF(cx - s * 0.01f, cy + s * 0.02f),
                Gdiplus::PointF(cx - s * 0.05f, cy + s * 0.24f),
                Gdiplus::PointF(cx + s * 0.15f, cy - s * 0.03f),
                Gdiplus::PointF(cx + s * 0.01f, cy - s * 0.03f)};
            g.FillPolygon(&brush, bolt, 6);
            return;
        }
        if (kind.rfind("volume", 0) == 0) {
            Gdiplus::PointF horn[3] = {
                Gdiplus::PointF(cx - s * 0.16f, cy - s * 0.20f),
                Gdiplus::PointF(cx - s * 0.16f, cy + s * 0.20f),
                Gdiplus::PointF(cx + s * 0.02f, cy)};
            g.FillPolygon(&brush, horn, 3);
            Gdiplus::Pen pen(color, std::max(1.2f, s * 0.08f));
            g.DrawArc(&pen, cx + s * 0.00f, cy - s * 0.12f, s * 0.24f, s * 0.24f, -50.0f, 100.0f);
            return;
        }
        if (kind.rfind("notification", 0) == 0) {
            // 消息气泡：圆角方块 + 左下小尾巴
            Gdiplus::Pen pen(color, std::max(1.4f, s * 0.11f));
            Gdiplus::RectF body(cx - s * 0.20f, cy - s * 0.20f, s * 0.40f, s * 0.30f);
            Gdiplus::GraphicsPath path;
            AddRounded(path, body, s * 0.09f);
            g.DrawPath(&pen, &path);
            Gdiplus::PointF tail[3] = {
                Gdiplus::PointF(cx - s * 0.10f, cy + s * 0.10f),
                Gdiplus::PointF(cx + s * 0.00f, cy + s * 0.10f),
                Gdiplus::PointF(cx - s * 0.10f, cy + s * 0.22f)};
            g.FillPolygon(&brush, tail, 3);
            return;
        }
        Gdiplus::RectF dot(cx - s * 0.12f, cy - s * 0.12f, s * 0.24f, s * 0.24f);
        g.FillEllipse(&brush, dot);
    }

    void DrawText(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::RectF& box, float size_px,
                  Gdiplus::FontStyle style, const Gdiplus::Color& color,
                  Gdiplus::StringAlignment align = Gdiplus::StringAlignmentNear) {
        if (text.empty()) return;
        Gdiplus::Font font(family_.get(), size_px, style, Gdiplus::UnitPixel);
        if (font.GetLastStatus() != Gdiplus::Ok) return;
        Gdiplus::StringFormat format;
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        format.SetAlignment(align);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        // 全透明模式下没有底，白字压在亮壁纸上会糊成一片。
        // 做法是两层黑投影：一层贴边（勾轮廓）、一层偏移更大更散（压背景）。
        // 单层 55% 实测在浅色桌面上仍然发飘，读长标题很吃力。
        if (text_shadow_) {
            const BYTE base = color.GetA();
            const Gdiplus::SolidBrush soft(
                Gdiplus::Color(Clamp8(static_cast<int>(base * 0.45f)), 0, 0, 0));
            const Gdiplus::SolidBrush hard(
                Gdiplus::Color(Clamp8(static_cast<int>(base * 0.85f)), 0, 0, 0));
            const float dy = (std::max)(1.0f, size_px * 0.13f);
            g.DrawString(text.c_str(), -1, &font,
                         Gdiplus::RectF(box.X, box.Y + dy * 2.0f, box.Width, box.Height), &format,
                         &soft);
            g.DrawString(text.c_str(), -1, &font, Gdiplus::RectF(box.X, box.Y + dy, box.Width,
                                                                box.Height),
                         &format, &hard);
        }
        Gdiplus::SolidBrush brush(color);
        g.DrawString(text.c_str(), -1, &font, box, &format, &brush);
    }

    static float MeasureText(Gdiplus::Graphics& g, Gdiplus::FontFamily* family, const std::wstring& text,
                             float size_px, Gdiplus::FontStyle style) {
        if (text.empty()) return 0.0f;
        Gdiplus::Font font(family, size_px, style, Gdiplus::UnitPixel);
        Gdiplus::RectF bound;
        Gdiplus::StringFormat format;
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        g.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), &format, &bound);
        return bound.Width;
    }

    // 播放律动条：4 根，相位驱动，纯三角函数，无随机。
    static void DrawWave(Gdiplus::Graphics& g, const Gdiplus::RectF& box, BYTE alpha, float scale,
                         double phase_base) {
        const float bar_w = 2.6f * scale;
        const float gap = 2.6f * scale;
        const float max_h = box.Height;
        for (int i = 0; i < 4; ++i) {
            const double phase = phase_base + i * 0.95;
            const float t = static_cast<float>(0.42 + 0.58 * std::fabs(std::sin(phase)));
            const float bh = std::max(2.0f * scale, max_h * t);
            Gdiplus::RectF bar(box.X + i * (bar_w + gap), box.Y + (max_h - bh) * 0.5f, bar_w, bh);
            FillRounded(g, bar, bar_w * 0.5f,
                        Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.88f)), 255, 255, 255));
        }
    }

    void DrawCompact(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& pill, BYTE alpha,
                     float scale, bool glassy) {
        if (v.agent) {
            DrawCompactAgent(g, v, pill, alpha, scale, glassy);
            return;
        }
        if (v.browser) {
            DrawCompactBrowser(g, v, pill, alpha, scale, glassy);
            return;
        }
        if (v.kind.rfind("meeting", 0) == 0) {
            DrawCompactMeeting(g, v, pill, alpha, scale, glassy);
            return;
        }
        if (v.editor) {
            DrawCompactEditor(g, v, pill, alpha, scale, glassy);
            return;
        }
        const float chip_size = 30.0f * scale;
        const Gdiplus::RectF chip = ChipRect(pill, chip_size);
        DrawChip(g, v.kind, chip, chip_size, alpha, v.cover_path);

        const std::wstring meta = Widen(v.meta);
        const float meta_w = MeasureText(g, family_.get(), meta, 11.5f * scale, Gdiplus::FontStyleRegular);
        const float right_pad = 18.0f * scale;
        float right_edge = pill.X + pill.Width - right_pad;

        // 「还有 N 件」的角标：双指不了，双击才铺开，所以必须让它自己说出来。
        if (v.more_count > 0) {
            const std::wstring label = L"+" + std::to_wstring(v.more_count);
            const float bh = 16.0f * scale;
            const float bw = MeasureText(g, family_.get(), label, 10.5f * scale,
                                         Gdiplus::FontStyleBold) +
                             12.0f * scale;
            const Gdiplus::RectF badge(right_edge - bw, pill.Y + (pill.Height - bh) * 0.5f, bw, bh);
            FillRounded(g, badge, bh * 0.5f,
                        Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.24f)), 255, 255, 255));
            DrawText(g, label, badge, 10.5f * scale, Gdiplus::FontStyleBold,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)), 238, 241, 248),
                     Gdiplus::StringAlignmentCenter);
            right_edge -= bw + 8.0f * scale;
        }

        if (!meta.empty()) {
            Gdiplus::RectF meta_box(right_edge - meta_w, pill.Y, meta_w, pill.Height);
            DrawText(g, meta, meta_box, 11.5f * scale, Gdiplus::FontStyleRegular,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.98f)),
                                    glassy ? 186 : 142, glassy ? 188 : 143, glassy ? 198 : 152),
                     Gdiplus::StringAlignmentFar);
            right_edge -= meta_w + 10.0f * scale;
        }

        if (v.playing) {
            const float wave_w = 4.0f * (2.6f * scale + 2.6f * scale);
            Gdiplus::RectF wave(right_edge - wave_w, pill.Y + pill.Height * 0.5f - 7.0f * scale, wave_w,
                                14.0f * scale);
            DrawWave(g, wave, alpha, scale, v.wave_phase);
            right_edge = wave.X - 13.0f * scale;
        }

        const float text_x = chip.GetRight() + 11.0f * scale;
        const float text_w = std::max(10.0f, right_edge - text_x);
        Gdiplus::RectF title_box(text_x, pill.Y, text_w, pill.Height);
        DrawText(g, Widen(v.title), title_box, 13.5f * scale, Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.98f)), 249, 249, 252));
    }

    // 紧凑态（小岛）：Agent 版。和媒体一样是 46 高的胶囊，但内容结构不同 ——
    // 媒体是「一行歌名 + 右侧时长」，Agent 需要同时说清两件事：
    //   「在哪个项目 / 在干什么」
    // 所以走两行：上行项目名，下行当前动作。右侧留进度或步数。
    void DrawCompactAgent(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& pill,
                          BYTE alpha, float scale, bool glassy) {
        const float chip_size = 30.0f * scale;
        const Gdiplus::RectF chip = ChipRect(pill, chip_size);
        DrawChipEx(g, "agent", chip, chip_size, alpha, std::string(),
                   AgentAccentFor(v.agent_provider), v.agent_provider);
        DrawStateDot(g, chip, alpha, scale, v.agent_state, v.wave_phase);

        const float right_pad = 16.0f * scale;
        float right_edge = pill.X + pill.Width - right_pad;

        // 右侧先放「状态脉冲」（运行中才会动），再放进度/步数文字。
        // 顺序反了会把文字挤到胶囊外面 —— 右边距是固定预算，谁先占谁优先。
        const float pulse_w = 2.0f * scale + 5.0f * scale * 2.0f;
        if (v.agent_state == "running" || v.agent_state == "waiting") {
            Gdiplus::RectF pulse(right_edge - pulse_w, pill.Y, pulse_w, pill.Height);
            DrawAgentPulse(g, pulse, alpha, scale, v.wave_phase, v.agent_state == "running");
            right_edge = pulse.X - 8.0f * scale;
        }

        // 「还有 N 件」：小圆最多两个，第四个及以后只靠这个角标提示。
        // 顺序是 从右往左：心跳 → +N → 进度，视觉上「越靠右越外围」。
        if (v.more_count > 0) {
            const std::wstring label = L"+" + std::to_wstring(v.more_count);
            const float bh = 16.0f * scale;
            const float bw = MeasureText(g, family_.get(), label, 10.5f * scale,
                                         Gdiplus::FontStyleBold) +
                             12.0f * scale;
            const Gdiplus::RectF badge(right_edge - bw, pill.Y + (pill.Height - bh) * 0.5f, bw, bh);
            FillRounded(g, badge, bh * 0.5f,
                        Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.24f)), 255, 255, 255));
            DrawText(g, label, badge, 10.5f * scale, Gdiplus::FontStyleBold,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)), 238, 241, 248),
                     Gdiplus::StringAlignmentCenter);
            right_edge = badge.X - 7.0f * scale;
        }

        if (!v.agent_meta.empty()) {
            const std::wstring meta = Widen(v.agent_meta);
            const float meta_w = MeasureText(g, family_.get(), meta, 10.5f * scale,
                                             Gdiplus::FontStyleBold);
            Gdiplus::RectF meta_box(right_edge - meta_w, pill.Y, meta_w, pill.Height);
            DrawText(g, meta, meta_box, 10.5f * scale, Gdiplus::FontStyleBold,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.94f)), 196, 200, 212),
                     Gdiplus::StringAlignmentFar);
            right_edge = meta_box.X - 9.0f * scale;
        }

        const float text_x = chip.GetRight() + 11.0f * scale;
        const float text_w = std::max(10.0f, right_edge - text_x);
        const std::string title = v.title.empty() ? v.agent_name : v.title;
        DrawText(g, Widen(title),
                 Gdiplus::RectF(text_x, pill.Y + 5.0f * scale, text_w, 18.0f * scale), 12.5f * scale,
                 Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.98f)), 249, 249, 252));
        if (!v.agent_action.empty()) {
            // 动作行保持中性灰：状态已经由 chip 上的点和右侧脉冲用颜色说清了，
            // 再把正文也上色，整条胶囊会花。正文要的是「读得清」。
            DrawText(g, Widen(v.agent_action),
                     Gdiplus::RectF(text_x, pill.Y + 23.0f * scale, text_w, 17.0f * scale),
                     10.2f * scale, Gdiplus::FontStyleRegular,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)),
                                    glassy ? 186 : 142, glassy ? 188 : 143, glassy ? 198 : 152));
        }
    }

    // 一览模式里单个岛的内部：图标 + 两行文字（标题上、副标题下）。    // 岛就是标准紧凑胶囊的尺寸，但内容比 DrawCompact 简单：没有 meta 列、
    // 没有律动条 —— 一览的目的是「一眼认出每件事」，细节留给单岛展开。
    void DrawIsland(Gdiplus::Graphics& g, const OverviewItem& item, const Gdiplus::RectF& pill,
                    BYTE alpha, float scale, bool glassy, double wave_phase = 0.0) {
        const float chip_size = 30.0f * scale;
        const Gdiplus::RectF chip = ChipRect(pill, chip_size);
        const bool is_agent = item.kind.rfind("agent", 0) == 0;
        if (item.kind.rfind("browser", 0) == 0) {
            // 一览里的浏览器岛：站点图标 + 品牌色兜底，和紧凑态同一套画法。
            // 这里没有完整的 CapsuleView，只好把 provider 复用成品牌 id、
            // state 复用成品牌名（没有图标时画首字母）。
            // 注意不能 return —— 后面还要画标题和副标题，早点返回会得到一个
            // 「只有图标、右边一片空」的岛。
            CapsuleView tmp;
            tmp.cover_path = item.cover_path;
            tmp.browser_brand = item.provider;
            tmp.browser_label = item.state;
            DrawBrowserChip(g, tmp, chip, chip_size, alpha);
        } else {
            DrawChipEx(g, item.kind, chip, chip_size, alpha, item.cover_path,
                       is_agent ? AgentAccentFor(item.provider) : AccentFor(item.kind), item.provider);
            if (is_agent) DrawStateDot(g, chip, alpha, scale, item.state, wave_phase);
        }

        const float text_x = chip.GetRight() + 11.0f * scale;
        const float text_w = std::max(10.0f, pill.GetRight() - 14.0f * scale - text_x);
        DrawText(g, Widen(item.title),
                 Gdiplus::RectF(text_x, pill.Y + 5.0f * scale, text_w, 20.0f * scale), 12.5f * scale,
                 Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.98f)), 249, 249, 252));
        if (!item.subtitle.empty()) {
            DrawText(g, Widen(item.subtitle),
                     Gdiplus::RectF(text_x, pill.Y + 24.0f * scale, text_w, 17.0f * scale),
                     10.5f * scale, Gdiplus::FontStyleRegular,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)), glassy ? 186 : 142,
                                    glassy ? 188 : 143, glassy ? 198 : 152));
        }
    }

    // ---------------------------------------------------------------- 浏览器
    //
    // 浏览器是「最常用、却最容易被做成一个没有信息量的方块」的那类活动。
    // 所以这里的取舍是：左侧方块里放**站点真实图标**（浏览器自己的图标库里就有），
    // 标题行是**当前标签页**，第二行是**域名**。三样都是「一眼回答我在哪」的东西。
    static Gdiplus::Color BrowserAccentFor(const std::string& brand) {
        if (brand == "chrome") return Gdiplus::Color(255, 66, 133, 244);
        if (brand == "edge") return Gdiplus::Color(255, 15, 157, 159);
        if (brand == "firefox") return Gdiplus::Color(255, 255, 113, 57);
        if (brand == "brave") return Gdiplus::Color(255, 251, 84, 43);
        if (brand == "vivaldi") return Gdiplus::Color(255, 239, 57, 57);
        if (brand == "opera") return Gdiplus::Color(255, 255, 27, 45);
        if (brand == "360se" || brand == "360chrome") return Gdiplus::Color(255, 25, 185, 85);
        if (brand == "qqbrowser") return Gdiplus::Color(255, 18, 183, 245);
        if (brand == "sogou") return Gdiplus::Color(255, 255, 106, 0);
        if (brand == "quark") return Gdiplus::Color(255, 61, 111, 255);
        if (brand == "uc") return Gdiplus::Color(255, 255, 138, 0);
        if (brand == "2345") return Gdiplus::Color(255, 45, 140, 240);
        if (brand == "yandex") return Gdiplus::Color(255, 252, 63, 29);
        return Gdiplus::Color(255, 91, 141, 239);
    }

    // 没有 favicon 时的品牌字形。只把最常见的三个画成「图形」，
    // 其余用首字母 —— 画得再像也不如真图标，没必要为长尾去雕。
    void DrawBrowserGlyph(Gdiplus::Graphics& g, const std::string& brand, const Gdiplus::RectF& box,
                          BYTE alpha, const std::string& label) {
        const float cx = box.X + box.Width * 0.5f;
        const float cy = box.Y + box.Height * 0.5f;
        const float s = (std::min)(box.Width, box.Height);
        const BYTE a = Clamp8(static_cast<int>(alpha * 0.98f));

        if (brand == "chrome") {
            const Gdiplus::RectF ring(cx - s * 0.33f, cy - s * 0.33f, s * 0.66f, s * 0.66f);
            Gdiplus::SolidBrush red(Gdiplus::Color(a, 234, 67, 53));
            Gdiplus::SolidBrush green(Gdiplus::Color(a, 52, 168, 83));
            Gdiplus::SolidBrush yellow(Gdiplus::Color(a, 251, 188, 5));
            g.FillPie(&red, ring, -90.0f, 120.0f);
            g.FillPie(&green, ring, 30.0f, 120.0f);
            g.FillPie(&yellow, ring, 150.0f, 120.0f);
            Gdiplus::SolidBrush white(Gdiplus::Color(a, 255, 255, 255));
            g.FillEllipse(&white, cx - s * 0.175f, cy - s * 0.175f, s * 0.35f, s * 0.35f);
            Gdiplus::SolidBrush blue(Gdiplus::Color(a, 66, 133, 244));
            g.FillEllipse(&blue, cx - s * 0.135f, cy - s * 0.135f, s * 0.27f, s * 0.27f);
            return;
        }
        if (brand == "edge") {
            const Gdiplus::RectF ring(cx - s * 0.31f, cy - s * 0.31f, s * 0.62f, s * 0.62f);
            Gdiplus::SolidBrush teal(Gdiplus::Color(a, 15, 157, 159));
            Gdiplus::SolidBrush blue(Gdiplus::Color(a, 0, 120, 212));
            g.FillPie(&teal, ring, -30.0f, 150.0f);
            g.FillPie(&blue, ring, 120.0f, 150.0f);
            Gdiplus::SolidBrush white(Gdiplus::Color(a, 255, 255, 255));
            g.FillEllipse(&white, cx - s * 0.16f, cy - s * 0.16f, s * 0.32f, s * 0.32f);
            return;
        }
        if (brand == "firefox") {
            const Gdiplus::RectF ball(cx - s * 0.30f, cy - s * 0.30f, s * 0.60f, s * 0.60f);
            Gdiplus::SolidBrush orange(Gdiplus::Color(a, 255, 113, 57));
            g.FillEllipse(&orange, ball);
            Gdiplus::SolidBrush purple(Gdiplus::Color(Clamp8(static_cast<int>(a) * 3 / 4), 144, 60, 208));
            g.FillPie(&purple, Gdiplus::RectF(cx - s * 0.06f, cy - s * 0.06f, s * 0.42f, s * 0.42f),
                      120.0f, 150.0f);
            return;
        }
        // 首字母：360 / QQ / 夸克 / 搜狗 这些名字本身就足够辨识。
        const std::wstring initial = Widen(label.empty() ? std::string("?") : label.substr(0, 3));
        DrawText(g, initial, box, s * 0.46f, Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.97f)), 255, 255, 255),
                 Gdiplus::StringAlignmentCenter);
    }

    // 左侧方块：有 favicon 就铺满（和媒体封面走同一个内存图片缓存），
    // 没有就用品牌色 + 品牌字形。两条路都必须「看起来像一个 app 图标」。
    void DrawBrowserChip(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& box,
                         float size, BYTE alpha) {
        const float radius = size * 0.34f;
        if (!v.cover_path.empty()) {
            if (Gdiplus::Bitmap* icon = CoverFor(v.cover_path)) {
                Gdiplus::GraphicsPath shape;
                AddRounded(shape, box, radius);
                const Gdiplus::GraphicsState state = g.Save();
                g.SetClip(&shape);
                g.DrawImage(icon, box);
                g.Restore(state);
                Gdiplus::Pen edge(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.26f)), 0, 0, 0),
                                  (std::max)(1.0f, 0.5f * size * 0.34f));
                g.DrawPath(&edge, &shape);
                return;
            }
        }
        FillRounded(g, box, radius,
                    Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)),
                                   BrowserAccentFor(v.browser_brand).GetR(),
                                   BrowserAccentFor(v.browser_brand).GetG(),
                                   BrowserAccentFor(v.browser_brand).GetB()));
        DrawBrowserGlyph(g, v.browser_brand, box, alpha, v.browser_label);
    }

    // 会议紧凑态：话筒 + 「谁」+「开了多久」+ 右侧一个呼吸的红点。
    // 通用的紧凑布局只有一行标题，而会议最关键的是"我还在会里"这个状态，
    // 必须让第二行明确说出来 —— 只写「腾讯会议」和"只是开着客户端"没法区分。
    void DrawCompactMeeting(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& pill,
                            BYTE alpha, float scale, bool glassy) {
        const float chip_size = 30.0f * scale;
        const Gdiplus::RectF chip = ChipRect(pill, chip_size);
        DrawChip(g, "meeting", chip, chip_size, alpha, std::string());

        // 录音点：呼吸节奏比媒体的波形慢一半，避免看起来像"在播音乐"。
        const float dot_r = 3.4f * scale;
        const float cx = pill.X + pill.Width - 18.0f * scale;
        const float cy = pill.Y + pill.Height * 0.5f;
        const double breath = 0.55 + 0.45 * std::sin(v.wave_phase * 0.45);
        Gdiplus::SolidBrush halo(Gdiplus::Color(
            Clamp8(static_cast<int>(alpha * 34.0 * breath)), 255, 90, 90));
        g.FillEllipse(&halo, cx - dot_r * 2.2f, cy - dot_r * 2.2f, dot_r * 4.4f, dot_r * 4.4f);
        Gdiplus::SolidBrush dot(Gdiplus::Color(
            Clamp8(static_cast<int>(alpha * (140.0 + 115.0 * breath))), 240, 80, 80));
        g.FillEllipse(&dot, cx - dot_r, cy - dot_r, dot_r * 2.0f, dot_r * 2.0f);

        const float text_x = chip.GetRight() + 10.0f * scale;
        const float text_w = (std::max)(16.0f, (cx - dot_r * 2.2f) - 10.0f * scale - text_x);
        DrawText(g, Widen(v.title),
                 Gdiplus::RectF(text_x, pill.Y + 7.0f * scale, text_w, 18.0f * scale),
                 12.6f * scale, Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.99f)), 250, 250, 253));
        DrawText(g, Widen(v.subtitle),
                 Gdiplus::RectF(text_x, pill.Y + 24.0f * scale, text_w, 16.0f * scale),
                 10.2f * scale, Gdiplus::FontStyleRegular,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.92f)), glassy ? 232 : 168,
                                glassy ? 178 : 122, glassy ? 178 : 122));
    }

    // 紧凑态：站点图标 + 标签页标题 + 域名（+ 右侧下载百分比）。
    // 两行都是「回答我在哪」，第三样信息（下载进度）放到右边单独一列 ——
    // 它和标题不是一类东西，混在一行会把两边都读不清。
    void DrawCompactBrowser(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& pill,
                            BYTE alpha, float scale, bool glassy) {
        const float chip_size = 30.0f * scale;
        const Gdiplus::RectF chip = ChipRect(pill, chip_size);
        DrawBrowserChip(g, v, chip, chip_size, alpha);

        const float right_pad = 17.0f * scale;
        float right_edge = pill.X + pill.Width - right_pad;
        const bool has_dl = v.browser_dl >= 0.0 || !v.browser_dl_hint.empty();
        if (has_dl) {
            // 右边只放一列：百分比 + 一条 3px 的进度。宽度固定，标题区按它让位。
            const float col_w = 42.0f * scale;
            const std::wstring hint = Widen(v.browser_dl_hint);
            if (!hint.empty()) {
                DrawText(g, hint, Gdiplus::RectF(right_edge - col_w, pill.Y + 7.0f * scale, col_w,
                                                 16.0f * scale),
                         11.0f * scale, Gdiplus::FontStyleBold,
                         Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)), 236, 240, 250),
                         Gdiplus::StringAlignmentFar);
            }
            const Gdiplus::RectF track(right_edge - col_w, pill.Y + 27.0f * scale, col_w,
                                       3.2f * scale);
            FillRounded(g, track, 1.6f * scale,
                        Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.22f)), 255, 255, 255));
            if (v.browser_dl >= 0.0) {
                const float filled = track.Width * static_cast<float>(
                                                       (std::max)(0.0, (std::min)(1.0, v.browser_dl)));
                if (filled > 0.5f) {
                    FillRounded(g, Gdiplus::RectF(track.X, track.Y, filled, track.Height),
                                1.6f * scale,
                                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.95f)), 120, 200, 255));
                }
            }
            right_edge -= col_w + 10.0f * scale;
        }

        const float text_x = chip.GetRight() + 10.0f * scale;
        const float text_w = (std::max)(16.0f, right_edge - text_x);
        // 标题：标签页标题是这里唯一「每几秒就可能变」的东西，字号给足。
        DrawText(g, Widen(v.browser_title),
                 Gdiplus::RectF(text_x, pill.Y + 7.0f * scale, text_w, 18.0f * scale),
                 12.6f * scale, Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.99f)), 250, 250, 253));
        // 第二行：站名 · 域名。站名是给人看的，域名是给「是不是钓鱼站」看的。
        std::string sub = v.browser_site_label;
        if (sub.empty()) sub = v.browser_site;
        if (!v.browser_site.empty() && !v.browser_site_label.empty() &&
            v.browser_site_label != v.browser_site) {
            sub += " · " + v.browser_site;
        }
        if (sub.empty()) sub = v.browser_label;
        DrawText(g, Widen(sub), Gdiplus::RectF(text_x, pill.Y + 24.0f * scale, text_w, 16.0f * scale),
                 10.2f * scale, Gdiplus::FontStyleRegular,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.90f)), glassy ? 186 : 142,
                                glassy ? 192 : 147, glassy ? 206 : 158));
    }

    // 展开态：完整标题 → 完整 URL → 下载列表 → 页脚。
    // 高度按下载条数长（没有下载就是 118 的矮面板），和 agent 面板同一套规则。
    void DrawBrowserExpanded(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& pill,
                             BYTE alpha, float scale, bool glassy) {
        const float pad = 16.0f * scale;
        const float chip_size = 36.0f * scale;
        const ExpandedHit hit = ExpandedHitAreas(pill, scale);
        const Gdiplus::RectF chip(pill.X + pad, pill.Y + pad, chip_size, chip_size);
        DrawBrowserChip(g, v, chip, chip_size, alpha);

        const float text_x = chip.GetRight() + 13.0f * scale;
        const float text_w = (std::max)(20.0f, hit.close.X - 8.0f * scale - text_x);

        DrawText(g, Widen(v.browser_title),
                 Gdiplus::RectF(text_x, chip.Y - 1.0f * scale, text_w, 22.0f * scale), 15.0f * scale,
                 Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.99f)), 250, 250, 253));
        const std::string sub = v.browser_url.empty() ? v.browser_site : v.browser_url;
        if (!sub.empty()) {
            DrawText(g, Widen(sub),
                     Gdiplus::RectF(text_x, chip.Y + 21.0f * scale, text_w, 18.0f * scale),
                     10.8f * scale, Gdiplus::FontStyleRegular,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.92f)), glassy ? 172 : 138,
                                    glassy ? 178 : 143, glassy ? 194 : 154));
        }

        // 分隔线只在真有下载列表时画。没有下载还画一条，下面就是空白 ——
        // 一条线在暗示「下面还有东西」，而下面什么都没有。
        float y = chip.GetBottom() + 12.0f * scale;
        if (!v.browser_rows.empty()) {
            Gdiplus::Pen line(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.16f)), 255, 255, 255),
                              (std::max)(1.0f, 0.8f * scale));
            g.DrawLine(&line, pill.X + pad, y, pill.X + pill.Width - pad, y);
            y += 10.0f * scale;
        }

        const float bar_w = pill.Width - pad * 2.0f;
        for (std::size_t i = 0; i < v.browser_rows.size(); ++i) {
            const BrowserRow& row = v.browser_rows[i];
            const float ry = y + static_cast<float>(i) * Layout::kBrowserRowH * scale;
            DrawText(g, Widen(row.name),
                     Gdiplus::RectF(pill.X + pad, ry, bar_w - 76.0f * scale, 15.0f * scale),
                     11.0f * scale, Gdiplus::FontStyleRegular,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.95f)), glassy ? 214 : 162,
                                    glassy ? 218 : 167, glassy ? 228 : 178));
            DrawText(g, Widen(row.label),
                     Gdiplus::RectF(pill.X + pill.Width - pad - 70.0f * scale, ry, 70.0f * scale,
                                    15.0f * scale),
                     10.4f * scale, Gdiplus::FontStyleBold,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.95f)), 150, 206, 255),
                     Gdiplus::StringAlignmentFar);
            // 条与文件名同宽（右端让出百分比数字）：铺满整行会把面板横向切开，
            // 而且条比它代表的文字还长，看着像两条不同的信息。
            const Gdiplus::RectF track(pill.X + pad, ry + 17.0f * scale, bar_w - 76.0f * scale,
                                       3.0f * scale);
            FillRounded(g, track, 1.5f * scale,
                        Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.18f)), 255, 255, 255));
            if (row.progress >= 0.0) {
                const float filled =
                    track.Width * static_cast<float>((std::max)(0.0, (std::min)(1.0, row.progress)));
                if (filled > 0.5f) {
                    FillRounded(g, Gdiplus::RectF(track.X, track.Y, filled, track.Height),
                                1.5f * scale,
                                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.88f)), 122, 196, 252));
                }
            }
        }

        // 页脚：左边是「哪个浏览器」，右边是操作提示。
        const float foot_y = pill.GetBottom() - 15.0f * scale;
        DrawText(g, Widen(v.browser_label),
                 Gdiplus::RectF(pill.X + pad, foot_y - 8.0f * scale, bar_w * 0.46f, 16.0f * scale),
                 10.0f * scale, Gdiplus::FontStyleRegular,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.80f)), glassy ? 170 : 132,
                                glassy ? 174 : 136, glassy ? 186 : 148));

    }

    // ---------------------------------------------------------------- 编辑器
    //
    // 编辑器岛回答的是「我正在改哪个文件」。所以三样东西按重要性排序：
    // 文件名（最重要，用户认的是这个）、项目（在哪个仓库）、语言（附加信息）。
    // 品牌色只用来区分是哪个编辑器，不再额外画 logo —— 一排 `< >` 已经够表意了。
    static Gdiplus::Color EditorAccentFor(const std::string& brand) {
        if (brand == "vscode") return Gdiplus::Color(255, 0, 122, 204);
        if (brand == "vscode-insiders") return Gdiplus::Color(255, 36, 150, 237);
        if (brand == "cursor") return Gdiplus::Color(255, 128, 134, 152);
        if (brand == "trae") return Gdiplus::Color(255, 108, 99, 255);
        if (brand == "trae-cn") return Gdiplus::Color(255, 94, 92, 230);
        if (brand == "windsurf") return Gdiplus::Color(255, 12, 186, 158);
        if (brand == "vscodium") return Gdiplus::Color(255, 33, 110, 174);
        if (brand == "kiro") return Gdiplus::Color(255, 143, 90, 232);
        if (brand == "qoder") return Gdiplus::Color(255, 66, 133, 244);
        if (brand == "zed") return Gdiplus::Color(255, 24, 92, 230);
        if (brand == "sublime") return Gdiplus::Color(255, 255, 152, 0);
        if (brand == "notepadpp") return Gdiplus::Color(255, 111, 179, 60);
        // JetBrains 全家：统一洋红，靠字形 + 页面上的名字区分具体产品
        if (brand == "idea" || brand == "pycharm" || brand == "webstorm" || brand == "goland" ||
            brand == "clion" || brand == "rider" || brand == "phpstorm" || brand == "rubymine" ||
            brand == "android-studio" || brand == "datagrip") {
            return Gdiplus::Color(255, 254, 49, 93);
        }
        return Gdiplus::Color(255, 0, 122, 204);
    }

    // 字形：一对尖括号 `< >`。所有编辑器共用同一个形状 —— 它们的 logo 在
    // 30px 的方块里根本分不出来，而"这是代码编辑器"这件事一画就懂。
    void DrawEditorGlyph(Gdiplus::Graphics& g, const Gdiplus::RectF& box, BYTE alpha) {
        const float cx = box.X + box.Width * 0.5f;
        const float cy = box.Y + box.Height * 0.5f;
        const float s = (std::min)(box.Width, box.Height);
        Gdiplus::Pen pen(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.98f)), 255, 255, 255),
                         (std::max)(1.6f, s * 0.085f));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        // 两个尖括号之间必须留够空白。间距取 0.10（半个线宽以上），
        // 0.06 的时候 `<` 和 `>` 会连在一起，远看是一个菱形 —— 实测就是这样。
        // < 左
        g.DrawLine(&pen, cx - s * 0.10f, cy - s * 0.19f, cx - s * 0.26f, cy);
        g.DrawLine(&pen, cx - s * 0.26f, cy, cx - s * 0.10f, cy + s * 0.19f);
        // > 右
        g.DrawLine(&pen, cx + s * 0.10f, cy - s * 0.19f, cx + s * 0.26f, cy);
        g.DrawLine(&pen, cx + s * 0.26f, cy, cx + s * 0.10f, cy + s * 0.19f);
    }

    void DrawEditorChip(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& box,
                        float size, BYTE alpha) {
        const float radius = size * 0.34f;
        const Gdiplus::Color accent = EditorAccentFor(v.editor_brand);
        FillRounded(g, box, radius,
                    Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)), accent.GetR(),
                                   accent.GetG(), accent.GetB()));
        DrawEditorGlyph(g, box, alpha);
    }

    // 紧凑态：代码图标 + 文件名 + 「项目 · 语言」+ 右侧未保存圆点。
    void DrawCompactEditor(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& pill,
                           BYTE alpha, float scale, bool glassy) {
        const float chip_size = 30.0f * scale;
        const Gdiplus::RectF chip = ChipRect(pill, chip_size);
        DrawEditorChip(g, v, chip, chip_size, alpha);

        const float right_pad = 16.0f * scale;
        float right_edge = pill.X + pill.Width - right_pad;
        if (v.editor_dirty) {
            // 未保存：一个实心圆点。位置在右上，和"关闭按钮"区分开 ——
            // 它是状态不是控件，所以不画底、不可点。
            const float r = 3.2f * scale;
            Gdiplus::SolidBrush dot(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.92f)), 255,
                                                   214, 102));
            g.FillEllipse(&dot, right_edge - r, pill.Y + 12.0f * scale - r, r * 2.0f, r * 2.0f);
            right_edge -= r * 2.0f + 10.0f * scale;
        }

        const float text_x = chip.GetRight() + 10.0f * scale;
        const float text_w = (std::max)(16.0f, right_edge - text_x);
        DrawText(g, Widen(v.editor_file),
                 Gdiplus::RectF(text_x, pill.Y + 7.0f * scale, text_w, 18.0f * scale),
                 12.6f * scale, Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.99f)), 250, 250, 253));

        // 第二行：项目 · 语言。项目为空时（无工作区）只显示语言。
        std::string sub = v.editor_project;
        if (!v.editor_language.empty()) {
            sub += (sub.empty() ? "" : " · ") + v.editor_language;
        }
        if (sub.empty()) sub = v.editor_label;
        DrawText(g, Widen(sub), Gdiplus::RectF(text_x, pill.Y + 24.0f * scale, text_w, 16.0f * scale),
                 10.2f * scale, Gdiplus::FontStyleRegular,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.90f)), glassy ? 186 : 142,
                                glassy ? 192 : 147, glassy ? 206 : 158));
    }

    // 展开态：大图标 + 文件名 → 项目 / 语言 → 页脚编辑器名。
    // 没有"进度条"这类东西 —— 编辑不是有进度可言的活儿，硬凑一个只会显得空。
    void DrawEditorExpanded(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& pill,
                            BYTE alpha, float scale, bool glassy) {
        const float pad = 16.0f * scale;
        const float chip_size = 36.0f * scale;
        const ExpandedHit hit = ExpandedHitAreas(pill, scale);
        const Gdiplus::RectF chip(pill.X + pad, pill.Y + pad, chip_size, chip_size);
        DrawEditorChip(g, v, chip, chip_size, alpha);

        const float text_x = chip.GetRight() + 13.0f * scale;
        const float text_w = (std::max)(20.0f, hit.close.X - 8.0f * scale - text_x);

        // 文件名后面跟一个「●」，和 VS Code 标题栏同一个语义。
        std::wstring title = Widen(v.editor_file);
        if (v.editor_dirty) title += L" \x25CF";
        DrawText(g, title,
                 Gdiplus::RectF(text_x, chip.Y - 1.0f * scale, text_w, 22.0f * scale),
                 15.0f * scale, Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.99f)),
                                v.editor_dirty ? static_cast<BYTE>(255) : 250,
                                v.editor_dirty ? static_cast<BYTE>(214) : 250,
                                v.editor_dirty ? static_cast<BYTE>(102) : 253));

        std::string sub;
        if (!v.editor_project.empty()) sub = v.editor_project;
        if (!v.editor_language.empty()) sub += (sub.empty() ? "" : " · ") + v.editor_language;
        if (v.editor_remote) sub += (sub.empty() ? "" : " · ") + std::string("远程");
        if (sub.empty()) sub = v.editor_label;
        DrawText(g, Widen(sub),
                 Gdiplus::RectF(text_x, chip.Y + 21.0f * scale, text_w, 18.0f * scale),
                 10.8f * scale, Gdiplus::FontStyleRegular,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.92f)), glassy ? 172 : 138,
                                glassy ? 178 : 143, glassy ? 194 : 154));

        // 页脚：只有编辑器名。原来这里还有一句「点标题切到窗口 · × 关闭」，
        // 那是零信息量的噪音 —— 和媒体/浏览器/agent 一起删掉了。
        const float foot_y = pill.GetBottom() - 15.0f * scale;
        const float bar_w = pill.Width - pad * 2.0f;
        DrawText(g, Widen(v.editor_label),
                 Gdiplus::RectF(pill.X + pad, foot_y - 8.0f * scale, bar_w, 16.0f * scale),
                 10.0f * scale, Gdiplus::FontStyleRegular,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.80f)), glassy ? 170 : 132,
                                glassy ? 174 : 136, glassy ? 186 : 148));
    }

    // 休眠条：**有岛但被藏起来**时的那一条把手。
    // 「没有岛」时这里什么都不该画（上层直接把窗口做成全透明）——
    // 把「无活动」也画成休眠条，会让人以为岛上没内容却一直挂着。
    // 刻意不画文字 / 圆点：一旦放了内容，它就又变成一块挡视线的面板了。
    void DrawSleepBar(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& pill,
                       BYTE alpha, float scale, bool glassy) {
        (void)v;
        (void)glassy;
        // 顶部一道极淡的高光 + 中间略亮的短横：看得出来可点，但不像控件。
        const float inset = pill.Width * 0.18f;
        Gdiplus::RectF gloss(pill.X + inset, pill.Y + 0.9f * scale,
                             pill.Width - inset * 2.0f, 1.2f * scale);
        FillRounded(g, gloss, 0.6f * scale,
                    Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.34f)), 255, 255, 255));
        const float grip_w = 46.0f * scale;
        Gdiplus::RectF grip(pill.X + (pill.Width - grip_w) * 0.5f,
                            pill.Y + pill.Height * 0.5f - 1.6f * scale, grip_w, 3.2f * scale);
        FillRounded(g, grip, 1.6f * scale,
                    Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.55f)), 236, 240, 248));
    }

    // 展开态（大岛）：Agent 版 —— 「点开大岛能看到 agent 在干什么」。
    // 四段式，从上到下分别是：谁在跑 → 在做什么任务 → 现在这一秒在干什么 →
    // 干到哪一步（清单或动作时间线）。顺序不能换：先给身份，再给细节。
    void DrawAgentExpanded(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& pill,
                           BYTE alpha, float scale, bool glassy) {
        const float pad = 18.0f * scale;
        const float chip_size = 42.0f * scale;
        const ExpandedHit hit = ExpandedHitAreas(pill, scale);
        const Gdiplus::RectF chip(pill.X + pad, pill.Y + pad, chip_size, chip_size);
        DrawChipEx(g, "agent", chip, chip_size, alpha, std::string(),
                   AgentAccentFor(v.agent_provider), v.agent_provider);
        DrawStateDot(g, chip, alpha, scale, v.agent_state, v.wave_phase);

        const float text_x = chip.GetRight() + 14.0f * scale;
        const float text_w = std::max(20.0f, hit.close.X - 8.0f * scale - text_x);

        // ---- 身份：品牌 + 项目 + 时长 ----
        DrawText(g, Widen(v.agent_name.empty() ? v.title : v.agent_name),
                 Gdiplus::RectF(text_x, chip.Y - 1.0f * scale, text_w, 22.0f * scale), 15.5f * scale,
                 Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.99f)), 250, 250, 253));
        if (!v.agent_sub.empty()) {
            DrawText(g, Widen(v.agent_sub),
                     Gdiplus::RectF(text_x, chip.Y + 21.0f * scale, text_w, 20.0f * scale),
                     11.5f * scale, Gdiplus::FontStyleRegular,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)),
                                    glassy ? 186 : 146, glassy ? 188 : 147, glassy ? 198 : 156));
        }

        // 状态徽标贴在右上角关闭键左边：状态是全局的，位置也该在「头部」。
        {
            const wchar_t* label = v.agent_state == "running" ? L"运行中"
                                   : v.agent_state == "waiting" ? L"等待输入"
                                                                : L"已完成";
            const std::wstring text(label);
            const float bw = MeasureText(g, family_.get(), text, 10.0f * scale,
                                         Gdiplus::FontStyleBold) +
                             18.0f * scale;
            const float bh = 18.0f * scale;
            const Gdiplus::RectF badge(hit.close.X - 6.0f * scale - bw, pill.Y + pad - 1.0f * scale, bw,
                                       bh);
            const Gdiplus::Color c = StateColor(v.agent_state);
            FillRounded(g, badge, bh * 0.5f,
                        Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.20f)), c.GetR(), c.GetG(),
                                       c.GetB()));
            DrawText(g, text, badge, 10.0f * scale, Gdiplus::FontStyleBold,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)),
                                    static_cast<BYTE>(c.GetR() * 0.5f + 128),
                                    static_cast<BYTE>(c.GetG() * 0.5f + 128),
                                    static_cast<BYTE>(c.GetB() * 0.5f + 128)),
                     Gdiplus::StringAlignmentCenter);
        }

        const float bar_w = pill.Width - pad * 2.0f;
        float y = chip.GetBottom() + 12.0f * scale;

        // ---- 任务：这个会话到底在做什么 ----
        // 用「左侧一道短竖线」而不是标签文字：标签会把本就不多的一行
        // 横向宽度吃掉 60px，而竖线的语义一眼就懂。
        if (!v.agent_task.empty()) {
            Gdiplus::RectF tick(pill.X + pad, y + 2.0f * scale, 2.0f * scale, 13.0f * scale);
            const Gdiplus::Color accent = AgentAccentFor(v.agent_provider);
            FillRounded(g, tick, 1.0f * scale,
                        Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.70f)), accent.GetR(),
                                       accent.GetG(), accent.GetB()));
            DrawText(g, Widen(v.agent_task),
                     Gdiplus::RectF(pill.X + pad + 9.0f * scale, y, bar_w - 9.0f * scale,
                                    18.0f * scale),
                     10.8f * scale, Gdiplus::FontStyleRegular,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)),
                                    glassy ? 202 : 132, glassy ? 206 : 136, glassy ? 216 : 146));
            y += 22.0f * scale;
        }

        // ---- 当前动作：加粗一行，是整块面板的视觉重心 ----
        if (!v.agent_action.empty()) {
            DrawText(g, Widen(v.agent_action),
                     Gdiplus::RectF(pill.X + pad, y, bar_w, 21.0f * scale), 13.0f * scale,
                     Gdiplus::FontStyleBold,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.99f)), 248, 249, 253));
            y += 25.0f * scale;
        }

        // ---- 清单 / 时间线 ----
        // 有 TodoWrite / update_plan 就画清单（能看出「还剩几步」），
        // 没有就画最近动作的时间线（能看出「刚做了什么」）。
        const float row_h = 21.0f * scale;
        for (std::size_t i = 0; i < v.agent_rows.size(); ++i) {
            const AgentRow& row = v.agent_rows[i];
            const float ry = y + static_cast<float>(i) * row_h;
            if (ry + row_h > pill.Y + pill.Height - 34.0f * scale) break;
            if (v.agent_rows_are_todos) {
                const int st = row.status;
                const float bs = 11.0f * scale;
                const Gdiplus::RectF box(pill.X + pad + 1.0f * scale, ry + (row_h - bs) * 0.5f, bs, bs);
                const Gdiplus::Color accent = AgentAccentFor(v.agent_provider);
                if (st == 2) {
                    // 已完成：实心 + 对勾，文字压暗 —— 它已经是背景信息了。
                    FillRounded(g, box, bs * 0.28f,
                                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.85f)), accent.GetR(),
                                               accent.GetG(), accent.GetB()));
                    Gdiplus::Pen check(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.95f)), 16, 16, 20),
                                       std::max(1.2f, 1.5f * scale));
                    check.SetStartCap(Gdiplus::LineCapRound);
                    check.SetEndCap(Gdiplus::LineCapRound);
                    g.DrawLine(&check, box.X + bs * 0.24f, box.Y + bs * 0.52f, box.X + bs * 0.44f,
                               box.Y + bs * 0.72f);
                    g.DrawLine(&check, box.X + bs * 0.44f, box.Y + bs * 0.72f, box.X + bs * 0.78f,
                               box.Y + bs * 0.30f);
                } else if (st == 1) {
                    // 进行中：空心框 + 中心实心点，配高亮文字。
                    Gdiplus::Pen ring(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.90f)),
                                                     accent.GetR(), accent.GetG(), accent.GetB()),
                                      std::max(1.0f, 1.4f * scale));
                    Gdiplus::GraphicsPath p;
                    AddRounded(p, box, bs * 0.28f);
                    g.DrawPath(&ring, &p);
                    Gdiplus::SolidBrush dotbrush(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.95f)),
                                                               accent.GetR(), accent.GetG(),
                                                               accent.GetB()));
                    g.FillEllipse(&dotbrush, box.X + bs * 0.30f, box.Y + bs * 0.30f, bs * 0.40f,
                                  bs * 0.40f);
                } else {
                    Gdiplus::Pen ring(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.42f)), 236, 240, 248),
                                      std::max(1.0f, 1.3f * scale));
                    Gdiplus::GraphicsPath p;
                    AddRounded(p, box, bs * 0.28f);
                    g.DrawPath(&ring, &p);
                }
                const bool done = st == 2;
                const bool active = st == 1;
                DrawText(g, Widen(row.text),
                         Gdiplus::RectF(box.GetRight() + 9.0f * scale, ry,
                                        bar_w - (box.GetRight() - (pill.X + pad)) - 9.0f * scale,
                                        row_h),
                         active ? 11.5f * scale : 11.0f * scale,
                         active ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                         done ? Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.78f)), glassy ? 196 : 140,
                                               glassy ? 198 : 141, glassy ? 208 : 150)
                              : Gdiplus::Color(Clamp8(static_cast<int>(alpha * (active ? 0.99f : 0.84f))),
                                               active ? 246 : (glassy ? 196 : 152),
                                               active ? 248 : (glassy ? 198 : 153),
                                               active ? 252 : (glassy ? 208 : 162)));
            } else {
                // 时间线：一个圆点 + 一行「工具 目标」。最新的那条最亮。
                const bool newest = i == 0;
                const float r = 2.2f * scale;
                Gdiplus::SolidBrush dot(Gdiplus::Color(
                    Clamp8(static_cast<int>(alpha * (newest ? 0.92f : 0.42f))), 236, 240, 248));
                g.FillEllipse(&dot, pill.X + pad + 4.0f * scale, ry + row_h * 0.5f - r, r * 2.0f, r * 2.0f);
                DrawText(g, Widen(row.text),
                         Gdiplus::RectF(pill.X + pad + 18.0f * scale, ry, bar_w - 18.0f * scale, row_h),
                         newest ? 11.4f * scale : 11.0f * scale,
                         newest ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                         Gdiplus::Color(Clamp8(static_cast<int>(alpha * (newest ? 0.96f : 0.62f))),
                                        glassy ? 208 : 158, glassy ? 212 : 160, glassy ? 222 : 170));
            }
        }

        // ---- 页脚 ----
        const float foot_y = pill.Y + pill.Height - pad - 6.0f * scale;
        DrawText(g, Widen(v.agent_footer),
                 Gdiplus::RectF(pill.X + pad, foot_y - 8.0f * scale, bar_w * 0.52f, 16.0f * scale),
                 10.5f * scale, Gdiplus::FontStyleRegular,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.96f)),
                                glassy ? 158 : 108, glassy ? 160 : 109, glassy ? 170 : 118));


        // ---- 右上角关闭 ----
        FillRounded(g, hit.close, hit.close.Height * 0.5f,
                    Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.14f)), 255, 255, 255));
        DrawText(g, L"×", hit.close, 16.0f * scale, Gdiplus::FontStyleRegular,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.92f)), 244, 244, 249),
                 Gdiplus::StringAlignmentCenter);
    }

    void DrawExpanded(Gdiplus::Graphics& g, const CapsuleView& v, const Gdiplus::RectF& pill, BYTE alpha,
                      float scale, bool glassy) {
        if (v.agent) {
            DrawAgentExpanded(g, v, pill, alpha, scale, glassy);
            return;
        }
        if (v.browser) {
            DrawBrowserExpanded(g, v, pill, alpha, scale, glassy);
            return;
        }
        if (v.editor) {
            DrawEditorExpanded(g, v, pill, alpha, scale, glassy);
            return;
        }
        const float pad = 18.0f * scale;
        const float chip_size = 42.0f * scale;
        const ExpandedHit hit = ExpandedHitAreas(pill, scale);
        const Gdiplus::RectF chip(pill.X + pad, pill.Y + pad, chip_size, chip_size);
        DrawChip(g, v.kind, chip, chip_size, alpha, v.cover_path);

        const float text_x = chip.GetRight() + 14.0f * scale;
        // 右上角关闭按钮会占掉一块，标题必须在此之前收尾，否则长歌名会压到 × 上。
        const float text_w = std::max(20.0f, hit.close.X - 8.0f * scale - text_x);

        // 标题与副标题的相对位置锁定在图标上：图标顶对齐标题，图标底对齐副标题，
        // 这样即使只有一行副标题，三者也是一个视觉块，而不是三行各自浮着。
        DrawText(g, Widen(v.title), Gdiplus::RectF(text_x, chip.Y - 1.0f * scale, text_w, 22.0f * scale),
                 15.5f * scale, Gdiplus::FontStyleBold,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.99f)), 250, 250, 253));

        if (!v.subtitle.empty()) {
            DrawText(g, Widen(v.subtitle),
                     Gdiplus::RectF(text_x, chip.Y + 21.0f * scale, text_w, 20.0f * scale), 12.0f * scale,
                     Gdiplus::FontStyleRegular,
                     Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.98f)),
                                     glassy ? 190 : 150, glassy ? 192 : 151, glassy ? 202 : 160));
        }

        // 页脚固定在底边。试过加整条分隔线，但页脚往往只有一句提示，
        // 横贯全宽的线比留白更吵；改成靠间距分层，安静得多。
        const float foot_y = pill.Y + pill.Height - pad - 6.0f * scale;
        const float bar_w = pill.Width - pad * 2.0f;

        // ---- 歌词区：上一句 / 当前句 / 下一句，按播放进度滚动 ----
        if (v.lyric_active) {
            const float lyric_y = pill.Y + pad + 52.0f * scale;
            const float line_h = 21.0f * scale;
            const Gdiplus::Color dim(Clamp8(static_cast<int>(alpha * 0.52f)), glassy ? 198 : 150,
                                     glassy ? 200 : 152, glassy ? 210 : 162);
            const Gdiplus::Color bright(Clamp8(static_cast<int>(alpha * 0.98f)), 250, 250, 253);
            DrawText(g, Widen(v.lyric_prev),
                     Gdiplus::RectF(pill.X + pad, lyric_y, bar_w, line_h), 11.0f * scale,
                     Gdiplus::FontStyleRegular, dim);
            std::string cur = v.lyric_cur;
            if (cur.empty()) {
                if (v.lyric_status == "loading") cur = "歌词加载中…";
                else if (v.lyric_status == "error") cur = "歌词获取失败 · 联网后切歌重试";
                else if (v.lyric_status == "none") cur = "暂无歌词";
                else cur = "♪";
            }
            DrawText(g, Widen(cur), Gdiplus::RectF(pill.X + pad, lyric_y + line_h, bar_w, line_h),
                     13.5f * scale, Gdiplus::FontStyleBold, bright);
            DrawText(g, Widen(v.lyric_next),
                     Gdiplus::RectF(pill.X + pad, lyric_y + 2.0f * line_h, bar_w, line_h),
                     11.0f * scale, Gdiplus::FontStyleRegular, dim);
        }

        if (v.progress >= 0.0) {
            const float bar_y = foot_y - 30.0f * scale;
            const float bar_h = 4.0f * scale;
            Gdiplus::RectF track(pill.X + pad, bar_y, bar_w, bar_h);
            FillRounded(g, track, bar_h * 0.5f,
                        Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.13f)), 255, 255, 255));

            const float filled = static_cast<float>(bar_w * std::clamp(v.progress, 0.0, 1.0));
            if (filled > 1.0f) {
                const Gdiplus::Color accent = AccentFor(v.kind);
                // 先铺一层更宽更淡的同色光晕，进度条才有「亮着」的感觉。
                Gdiplus::RectF halo(pill.X + pad, bar_y - 2.0f * scale, filled, bar_h + 4.0f * scale);
                FillRounded(g, halo, (bar_h + 4.0f * scale) * 0.5f,
                            Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.16f)), accent.GetR(),
                                           accent.GetG(), accent.GetB()));
                Gdiplus::RectF fill(pill.X + pad, bar_y, filled, bar_h);
                FillRounded(g, fill, bar_h * 0.5f,
                            Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.95f)), accent.GetR(),
                                           accent.GetG(), accent.GetB()));
                // 顶端 1px 高光，让圆头看起来是亮的实心而不是平的色带。
                if (filled > 3.0f * scale) {
                    Gdiplus::RectF gloss(pill.X + pad + 1.0f * scale, bar_y + 0.8f * scale,
                                         filled - 2.0f * scale, 1.0f * scale);
                    FillRounded(g, gloss, 0.5f * scale,
                                Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.22f)), 255, 255, 255));
                }
            }
        }

        // 页脚只留「有信息量」的那一行（时间 / 进度）。原来右边还挂着一句
        // 「点标题打开应用 · 拖进度 · × 关闭」—— 那是零信息量的噪音：
        // × 就在右上角看得见，能拖的进度条本身也长得像能拖的。
        DrawText(g, Widen(v.meta), Gdiplus::RectF(pill.X + pad, foot_y - 8.0f * scale, bar_w,
                                                  16.0f * scale),
                 11.0f * scale, Gdiplus::FontStyleRegular,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.98f)),
                                glassy ? 158 : 108, glassy ? 160 : 109, glassy ? 170 : 118));

        // ---- 右上角关闭按钮（点它 dismiss 当前活动）----
        FillRounded(g, hit.close, hit.close.Height * 0.5f,
                    Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.14f)), 255, 255, 255));
        DrawText(g, L"×", hit.close, 16.0f * scale, Gdiplus::FontStyleRegular,
                 Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.92f)), 244, 244, 249),
                 Gdiplus::StringAlignmentCenter);

        // ---- 封面右下角的播放 / 暂停角标（点封面切换）----
        if (v.kind.rfind("media", 0) == 0) {
            const float b = 18.0f * scale;
            const Gdiplus::RectF badge(chip.GetRight() - b * 0.72f, chip.GetBottom() - b * 0.72f, b, b);
            FillRounded(g, badge, b * 0.5f,
                        Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.86f)), 16, 16, 20));
            Gdiplus::SolidBrush fg(Gdiplus::Color(Clamp8(static_cast<int>(alpha * 0.98f)), 245, 245, 249));
            if (v.playing) {
                g.FillRectangle(&fg, badge.X + b * 0.32f, badge.Y + b * 0.26f, 2.0f * scale, 8.0f * scale);
                g.FillRectangle(&fg, badge.X + b * 0.60f, badge.Y + b * 0.26f, 2.0f * scale, 8.0f * scale);
            } else {
                Gdiplus::PointF tri[3] = {
                    {badge.X + b * 0.36f, badge.Y + b * 0.25f},
                    {badge.X + b * 0.36f, badge.Y + b * 0.75f},
                    {badge.X + b * 0.74f, badge.Y + b * 0.50f}};
                g.FillPolygon(&fg, tri, 3);
            }
        }
    }

    // 按路径缓存封面位图。每帧从磁盘加载会让 60fps 主循环变成磁盘读取循环，
    // 而一首歌的封面在整首歌期间是不变的，按路径命中就够了。
    Gdiplus::Bitmap* CoverFor(const std::string& path) {
        if (path.empty()) return nullptr;
        if (path == cover_loaded_) return cover_bitmap_.get();
        cover_loaded_ = path;
        auto bitmap = std::make_unique<Gdiplus::Bitmap>(Widen(path).c_str(), FALSE);
        if (bitmap->GetLastStatus() != Gdiplus::Ok) {
            cover_bitmap_.reset();
            return nullptr;
        }
        cover_bitmap_ = std::move(bitmap);
        return cover_bitmap_.get();
    }

    // 全透明模式下给所有文字加投影（见 DrawText）。用成员而不是逐层传参：
    // DrawText 有几十个调用点，漏一处就在那块文字上糊掉，且很难肉眼发现。
    bool text_shadow_ = false;
    std::unique_ptr<Gdiplus::FontFamily> family_;
    std::unique_ptr<Gdiplus::Bitmap> shadow_;   // 模糊投影缓存（DropShadow 专用）
    std::uint32_t shadow_key_ = 0;              // 0 表示无缓存
    std::unique_ptr<Gdiplus::Bitmap> cover_bitmap_;
    std::string cover_loaded_;
};

}  // namespace win32
}  // namespace diw

#endif  // DIW_SRC_WIN32_RENDERER_HPP
