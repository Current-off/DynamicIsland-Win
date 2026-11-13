#pragma once
// 浏览器页面的「纯数据 + 纯函数」部分。
//
// 单独一层的原因和 agent_session.hpp 一样：采集（读 SQLite、查窗口）是
// 平台相关的脏活，但「这个 URL 属于哪个站」「窗口标题里哪一段才是页面标题」
// 「这个媒体会话是不是同一个浏览器」全是纯逻辑，可以被单测，也可以被映射层
// 直接消费。混在一起的话，core 层就要反过来依赖 win32 —— 分层就烂了。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace diw {

struct BrowserDownload {
    std::string name;        // 文件名（去掉路径）
    long long received = 0;
    long long total = 0;
    double progress = -1.0;  // 0..1；总量未知时留 -1，界面上画成不确定条
    bool done = false;
    std::int64_t started_ms = 0;
};

struct BrowserPage {
    std::string id;           // browser:<brand>:<profile>
    std::string brand;        // chrome / edge / firefox / 360se …
    std::string label;        // "Google Chrome"（品牌名）
    std::string title;        // 当前标签页标题（已剥掉浏览器后缀）
    std::string url;
    std::string site;         // bilibili.com
    std::string site_label;   // 哔哩哔哩（无映射时等于 site）
    std::string favicon;      // png 路径；空 = 用品牌色
    std::string exe_stem;     // chrome / msedge / firefox（判「这个媒体是不是它在放」用）
    std::string profile;      // Default / Profile 1
    bool foreground = false;  // 这个浏览器此刻是前台
    bool fullscreen = false;
    int download_count = 0;
    double download_progress = -1.0;
    std::string download_hint;   // "62%" 或 "3.4 MB"
    std::vector<BrowserDownload> downloads;
    std::int64_t seen_ms = 0;
    // 最后一次确认「它是前台」的时刻。切走之后岛还要留一小会儿 ——
    // 否则用户 Alt+Tab 回来点岛，岛正好在那一帧消失，永远点不到。
    std::int64_t last_fg_ms = 0;

    bool Present() const {
        if (!downloads.empty()) return true;
        return foreground && !title.empty();
    }
    bool Interesting() const { return !title.empty() || !downloads.empty(); }
};

namespace browser_detail {

inline std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace browser_detail

// url -> 主机名 -> 去掉 www./m./mobile./wap. 的站点名。
inline std::string BrowserSiteOf(const std::string& url) {
    if (url.empty()) return std::string();
    std::size_t start = url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    std::size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    std::string host = url.substr(start, end - start);
    const std::size_t at = host.rfind('@');  // user:pass@host
    if (at != std::string::npos) host = host.substr(at + 1);
    const std::size_t colon = host.find(':');  // 端口
    if (colon != std::string::npos) host = host.substr(0, colon);
    if (host.empty()) return std::string();
    static const char* kPrefixes[] = {"www.", "m.", "mobile.", "wap."};
    for (const char* p : kPrefixes) {
        const std::string prefix(p);
        if (host.size() > prefix.size() && host.compare(0, prefix.size(), prefix) == 0) {
            host = host.substr(prefix.size());
            break;
        }
    }
    return host;
}

// 域名 -> 站名。只收「看到域名就能想起是谁」的那些；其余原样显示域名。
// 编一个错误的站名比显示域名更难理解，所以这张表宁缺毋滥。
inline std::string BrowserSiteLabel(const std::string& site) {
    struct Pair {
        const char* site;
        const char* label;
    };
    static const Pair kNames[] = {
        {"bilibili.com", "哔哩哔哩"},
        {"youtube.com", "YouTube"},
        {"github.com", "GitHub"},
        {"google.com", "Google"},
        {"zhihu.com", "知乎"},
        {"weibo.com", "微博"},
        {"taobao.com", "淘宝"},
        {"tmall.com", "天猫"},
        {"jd.com", "京东"},
        {"douyin.com", "抖音"},
        {"xiaohongshu.com", "小红书"},
        {"chat.deepseek.com", "DeepSeek"},
        {"platform.deepseek.com", "DeepSeek 开放平台"},
        {"deepseek.com", "DeepSeek"},
        {"chatgpt.com", "ChatGPT"},
        {"openai.com", "OpenAI"},
        {"claude.ai", "Claude"},
        {"stackoverflow.com", "Stack Overflow"},
        {"bing.com", "Bing"},
        {"baidu.com", "百度"},
        {"weixin.qq.com", "微信公众号"},
        {"mp.weixin.qq.com", "微信公众号"},
        {"music.163.com", "网易云音乐"},
        {"163.com", "网易"},
        {"douban.com", "豆瓣"},
        {"x.com", "X"},
        {"twitter.com", "X"},
        {"reddit.com", "Reddit"},
        {"notion.so", "Notion"},
        {"feishu.cn", "飞书"},
        {"yuque.com", "语雀"},
        {"gitee.com", "Gitee"},
        {"gitlab.com", "GitLab"},
        {"npmjs.com", "npm"},
        {"developer.mozilla.org", "MDN"},
        {"learn.microsoft.com", "Microsoft Learn"},
        {"leetcode.cn", "力扣"},
        {"leetcode.com", "LeetCode"},
        {"coursera.org", "Coursera"},
        {"twitch.tv", "Twitch"},
        {"netflix.com", "Netflix"},
        {"iqiyi.com", "爱奇艺"},
        {"v.qq.com", "腾讯视频"},
        {"youku.com", "优酷"},
        {"aliyun.com", "阿里云"},
        {"pan.baidu.com", "百度网盘"},
        {"chromewebstore.google.com", "Chrome 应用商店"},
        {"agensi.io", "Agensi"},
    };
    for (const auto& p : kNames) {
        if (site == p.site) return p.label;
    }
    // 子域名兜底：chat.deepseek.com 命中过上面，这里再按「后缀匹配」兜一层，
    // 让 a.bilibili.com 也能显示「哔哩哔哩」。
    for (const auto& p : kNames) {
        const std::string suffix = std::string(".") + p.site;
        if (browser_detail::EndsWith(site, suffix)) return p.label;
    }
    return site;
}

// 标签页标题 -> 适合上岛的短标题。去掉站点已经在域名那行显示过的尾巴。
inline std::string BrowserCleanTitle(const std::string& title) {
    static const char* kTails[] = {
        " - 哔哩哔哩", " - YouTube", " · GitHub", " - 知乎", " - 微博",
        " - Stack Overflow", " - 掘金", " - CSDN博客", " - 简书", " | 菜鸟教程",
    };
    for (const char* t : kTails) {
        const std::string tail(t);
        if (title.size() > tail.size() && browser_detail::EndsWith(title, tail)) {
            return title.substr(0, title.size() - tail.size());
        }
    }
    return title;
}

// 这个媒体会话是不是就来自这个浏览器。
//
// 用途只有一个，但非常关键：**避免同一个视频出两个岛**。
// 用户在 B 站看视频时，SMTC 会给一条「媒体」活动（app_id = Chrome），
// 浏览器采集又会给一条「浏览器」活动（页面 = 那个视频页）。
// 不判这一下，屏幕上就是两个岛讲同一件事 —— 和当初「两个一模一样的岛」
// 是同一类错误，只不过这次是两个看起来不同的岛讲同一件事。
inline bool BrowserOwnsMedia(const std::string& media_app_id, const std::string& media_label,
                             const std::string& brand_id, const std::string& brand_label,
                             const std::string& exe_stem) {
    const std::string id = browser_detail::Lower(media_app_id);
    const std::string label = browser_detail::Lower(media_label);
    if (id.empty() && label.empty()) return false;
    // 先排掉「名字里含浏览器名、但不是浏览器」的宿主进程。
    // 实测踩到的：WorkBuddy / 各类 Electron 壳用 msedgewebview2.exe 播放内嵌音视频，
    // 它的 app_id 含 "msedge"，于是「Edge 在放媒体」成立，真正的浏览器岛被去重掉 ——
    // 表现是「浏览器明明开着，却永远不出岛」。包含匹配必须配一条排除规则才安全。
    if (id.find("webview") != std::string::npos) return false;
    const std::string stem = browser_detail::Lower(exe_stem);
    const std::string brand = browser_detail::Lower(brand_label);
    auto hit = [](const std::string& hay, const std::string& needle) {
        return !needle.empty() && hay.find(needle) != std::string::npos;
    };
    for (const std::string* hay : {&id, &label}) {
        if (hit(*hay, stem)) return true;
        if (hit(*hay, brand)) return true;
        if (hit(*hay, brand_id)) return true;
    }
    return false;
}

}  // namespace diw
