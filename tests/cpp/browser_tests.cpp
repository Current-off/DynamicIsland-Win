// 浏览器采集里那些「纯逻辑」的断言。
//
// 为什么这些值得单测：它们全是「字符串判定」，出错时不会崩、不会报错，
// 只会在界面上显示成一句莫名其妙的话（域名错了、标题少一半、岛莫名不出现）。
// 这类问题靠肉眼截图极难发现，只能靠断言钉住。

#include <cstdio>
#include <string>

#include "browser_page.hpp"

namespace {

int g_failed = 0;
int g_total = 0;

void Check(bool ok, const char* expr, int line) {
    ++g_total;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL line %d: %s\n", line, expr);
    }
}

void CheckEq(const std::string& got, const std::string& want, const char* expr, int line) {
    ++g_total;
    if (got != want) {
        ++g_failed;
        std::printf("FAIL line %d: %s\n  got : \"%s\"\n  want: \"%s\"\n", line, expr, got.c_str(),
                    want.c_str());
    }
}

#define CHECK(x) Check((x), #x, __LINE__)
#define CHECK_EQ(a, b) CheckEq((a), (b), #a " == " #b, __LINE__)

void TestSiteOf() {
    CHECK_EQ(bi::BrowserSiteOf("https://www.bilibili.com/video/BV1xx"), "bilibili.com");
    CHECK_EQ(bi::BrowserSiteOf("https://bilibili.com"), "bilibili.com");
    CHECK_EQ(bi::BrowserSiteOf("http://m.bilibili.com/video"), "bilibili.com");
    CHECK_EQ(bi::BrowserSiteOf("https://chat.deepseek.com/a/chat/s/1"), "chat.deepseek.com");
    CHECK_EQ(bi::BrowserSiteOf("https://example.com:8443/x"), "example.com");
    CHECK_EQ(bi::BrowserSiteOf("https://user:pw@example.com/x"), "example.com");
    CHECK_EQ(bi::BrowserSiteOf("about:blank"), "about");
    CHECK_EQ(bi::BrowserSiteOf(""), "");
    // 端口与查询串都不能漏进站名里：它们会直接影响 favicon 的查表键。
    CHECK_EQ(bi::BrowserSiteOf("https://www.google.com/search?q=a+b&rlz=1"), "google.com");
}

void TestSiteLabel() {
    CHECK_EQ(bi::BrowserSiteLabel("bilibili.com"), "哔哩哔哩");
    CHECK_EQ(bi::BrowserSiteLabel("github.com"), "GitHub");
    // 子域名兜底：lesson 里的名字应该照样认出来。
    CHECK_EQ(bi::BrowserSiteLabel("space.bilibili.com"), "哔哩哔哩");
    // 表里没有的原样返回 —— 编一个错的中文名比显示域名更难理解。
    CHECK_EQ(bi::BrowserSiteLabel("some-unknown-site.dev"), "some-unknown-site.dev");
    CHECK_EQ(bi::BrowserSiteLabel(""), "");
}

void TestCleanTitle() {
    CHECK_EQ(bi::BrowserCleanTitle("【原创】某视频 - 哔哩哔哩"), "【原创】某视频");
    CHECK_EQ(bi::BrowserCleanTitle("octocat/Hello-World · GitHub"), "octocat/Hello-World");
    // 尾部不属于已知站点时不能剪：页面标题自己就带 " - " 的情况非常常见。
    CHECK_EQ(bi::BrowserCleanTitle("问题 - Stack Overflow 中文"), "问题 - Stack Overflow 中文");
    CHECK_EQ(bi::BrowserCleanTitle(""), "");
}

void TestOwnsMedia() {
    // 正例：SMTC 里 Chrome 的两个常见 app_id 形态
    CHECK(bi::BrowserOwnsMedia("Chrome", "Google Chrome", "chrome", "Google Chrome", "chrome"));
    CHECK(bi::BrowserOwnsMedia("msedge.exe", "Microsoft Edge", "edge", "Microsoft Edge", "msedge"));
    // 反例：别的播放器
    CHECK(!bi::BrowserOwnsMedia("QQMusic.exe", "QQ音乐", "chrome", "Google Chrome", "chrome"));
    CHECK(!bi::BrowserOwnsMedia("", "", "chrome", "Google Chrome", "chrome"));
    // 关键反例：WebView2 宿主。它的进程名含 "msedge"，早期版本正是被这条
    // 误判成「Edge 在放媒体」，导致真正的浏览器岛被去重掉、永远不出现。
    CHECK(!bi::BrowserOwnsMedia("msedgewebview2.exe", "", "edge", "Microsoft Edge", "msedge"));
}

void TestPresent() {
    bi::BrowserPage p;
    CHECK(!p.Present());            // 什么都没有
    p.title = "某页面";             // 有标题但不在前台
    CHECK(!p.Present());
    p.foreground = true;
    CHECK(p.Present());
    // 后台但有下载：要出现（用户正在等它）
    bi::BrowserPage q;
    q.downloads.push_back(bi::BrowserDownload{});
    q.downloads.front().name = "a.zip";
    CHECK(q.Present());
    CHECK(q.Interesting());
}

}  // namespace

int main() {
    TestSiteOf();
    TestSiteLabel();
    TestCleanTitle();
    TestOwnsMedia();
    TestPresent();
    std::printf("%s: %d assertions, %d failed\n", g_failed == 0 ? "PASS" : "FAILED", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
