// 新增交互的纯逻辑回归：在线歌词解析 / 时间轴选行 / URL 与 JSON 转义 /
// 展开态命中几何。I/O（WinHTTP）与绘制不在本测试范围，只测可判定部分。
// 包含顺序有讲究：renderer.hpp 会先补 objidl.h 再引 gdiplus.h（GDI+ 需要
// IStream / PROPID）。在这里抢先 include <gdiplus.h> 会让 include guard 生效，
// 后面补的 objidl.h 就来不及了 —— 一整屏 C2065/C2146。
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/win32/lyrics.hpp"
#include "../../src/win32/renderer.hpp"

using bi::win32::ExpandedHitAreas;
using bi::win32::ExtractFirstNonEmptyJsonString;
using bi::win32::LyricLine;
using bi::win32::ParseLrc;
using bi::win32::ProgressFromX;
using bi::win32::SelectLyricLines;
using bi::win32::UrlEncodeUtf8;

static int g_run = 0;
static int g_failed = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        ++g_run;                                                                 \
        if (!(expr)) {                                                           \
            ++g_failed;                                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
        }                                                                        \
    } while (0)

void TestParseLrc() {
    const std::string lrc =
        "[ti:晴天]\n[ar:周杰伦]\n[00:00.00]故事的小黄花\n"
        "[00:12.50]从出生那年就飘着\n[00:25.00][01:10.00]童年的荡秋千\n";
    const auto lines = ParseLrc(lrc);
    CHECK(lines.size() == 4);
    CHECK(lines[0].ms == 0);
    CHECK(lines[0].text == "故事的小黄花");
    CHECK(lines[1].ms == 12500);
    CHECK(lines[2].ms == 25000);
    CHECK(lines[3].ms == 70000);
    CHECK(lines[2].text == lines[3].text);
    CHECK(lines[2].text == "童年的荡秋千");

    const auto shifted = ParseLrc("[offset:500]\n[00:10.00]晚一点出现\n");
    CHECK(shifted.size() == 1);
    CHECK(shifted[0].ms == 9500);

    CHECK(ParseLrc("").empty());
    CHECK(ParseLrc("纯文本没有时间戳").empty());

    const auto unsorted = ParseLrc("[00:20.00]第二句\n[00:05.00]第一句\n");
    CHECK(unsorted.size() == 2);
    CHECK(unsorted[0].ms == 5000);
    CHECK(unsorted[0].text == "第一句");
}

void TestSelectLines() {
    const std::vector<LyricLine> lines = {{0, "A"}, {10000, "B"}, {20000, "C"}};
    CHECK(SelectLyricLines({}, 0).available == false);

    const auto intro = SelectLyricLines(lines, -1);
    CHECK(intro.available);
    CHECK(intro.cur.empty());
    CHECK(intro.next == "A");

    const auto mid = SelectLyricLines(lines, 12000);
    CHECK(mid.prev == "A");
    CHECK(mid.cur == "B");
    CHECK(mid.next == "C");

    const auto last = SelectLyricLines(lines, 999999);
    CHECK(last.cur == "C");
    CHECK(last.next.empty());
}

void TestUrlAndJson() {
    CHECK(UrlEncodeUtf8("abc-_.~") == "abc-_.~");
    CHECK(UrlEncodeUtf8("a b") == "a%20b");
    CHECK(UrlEncodeUtf8("周杰伦") == "%E5%91%A8%E6%9D%B0%E4%BC%A6");

    const std::string arr =
        R"([{"syncedLyrics":""},{"syncedLyrics":"[00:01.00]hello\nworld"}])";
    CHECK(ExtractFirstNonEmptyJsonString(arr, "syncedLyrics") == "[00:01.00]hello\nworld");

    const std::string unicode = R"({"s":"\u5468\u6770\u4F26"})";
    CHECK(ExtractFirstNonEmptyJsonString(unicode, "s") == "周杰伦");

    const std::string escaped = R"({"s":"a\"b\\c\/d\ttab"})";
    CHECK(ExtractFirstNonEmptyJsonString(escaped, "s") == "a\"b\\c/d\ttab");

    const std::string surrogate = R"({"s":"\uD83C\uDFB5"})";
    CHECK(ExtractFirstNonEmptyJsonString(surrogate, "s") == "\xF0\x9F\x8E\xB5");

    CHECK(ExtractFirstNonEmptyJsonString("{}", "s").empty());
    CHECK(ExtractFirstNonEmptyJsonString(R"({"s":null})", "s").empty());
}

void TestHitGeometry() {
    const float scale = 2.0f;
    const Gdiplus::RectF pill(32.0f, 32.0f, 420.0f * scale, 196.0f * scale);
    const auto h = ExpandedHitAreas(pill, scale);

    CHECK(h.close.X > pill.X + pill.Width * 0.5f);  // 关闭在右上角
    CHECK(h.close.Y >= pill.Y);
    CHECK(h.play.X == pill.X + 18.0f * scale);
    CHECK(h.play.Y == pill.Y + 18.0f * scale);
    CHECK(h.progress.Width == pill.Width - 36.0f * scale);
    CHECK(h.progress.Height > 4.0f * scale);  // 命中带比可见条高，便于拖

    CHECK(ProgressFromX(h.progress, h.progress.X) == 0.0);
    CHECK(ProgressFromX(h.progress, h.progress.X + h.progress.Width) == 1.0);
    CHECK(ProgressFromX(h.progress, -10000.0) == 0.0);
    CHECK(ProgressFromX(h.progress, 100000.0) == 1.0);

    // 1:1 缩放也要成立（100% DPI 的机器）。
    const auto h1 = ExpandedHitAreas(Gdiplus::RectF(16.0f, 16.0f, 420.0f, 196.0f), 1.0f);
    CHECK(h1.close.X > 300.0f);
    CHECK(h1.progress.Width == 420.0f - 36.0f);

    // ---- 收起键 ----
    // 它必须和 × 一样大、同一行、在它左边且不重叠 —— 三个条件缺一个就会
    // 出现「两个键叠在一起，点哪个看运气」。
    CHECK(h.collapse.Width == h.close.Width);
    CHECK(h.collapse.Height == h.close.Height);
    CHECK(h.collapse.Y == h.close.Y);
    CHECK(h.collapse.GetRight() < h.close.X);
    CHECK(h.collapse.GetRight() > h.close.X - 10.0f * scale);  // 紧邻，不散开
    CHECK(h.collapse.X > pill.X + pill.Width * 0.5f);
    // 收起键不能被 × 或「打开应用」盖住：三个区域两两不重叠。
    CHECK(!h.collapse.IntersectsWith(h.close));
    CHECK(!h.app.IntersectsWith(h.collapse));
    CHECK(!h.app.IntersectsWith(h.close));
    // 点开岛身仍要能打开应用：标题那一块不能被两个键吃掉。
    CHECK(h.app.Width > pill.Width * 0.5f);
    // 两个键都得在胶囊里，否则会被窗口裁掉一半（点了没反应）。
    CHECK(h.collapse.X >= pill.X);
    CHECK(h.collapse.GetBottom() <= pill.GetBottom());

    const auto h2 = ExpandedHitAreas(Gdiplus::RectF(16.0f, 16.0f, 420.0f, 196.0f), 2.0f);
    CHECK(h2.collapse.GetRight() < h2.close.X);
    CHECK(!h2.app.IntersectsWith(h2.collapse));
}

int main() {
    TestParseLrc();
    TestSelectLines();
    TestUrlAndJson();
    TestHitGeometry();
    std::printf("interaction_tests: run=%d failed=%d\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}
