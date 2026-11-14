// balance_island / 规则引擎（C++17，纯函数，无平台依赖）
//
// 这一层回答一个问题：**这件事该不该出现在岛上**。
// 输入是「事件候选 + 用户配置 + 当前时刻」，输出是「显示 / 不显示 + 原因」。
//
// 为什么把它做成纯函数：
//   1. 规则最容易写错（空 allow 列表的含义、跨零点免打扰、子串大小写），
//      必须能用单元测试覆盖，而不是靠盯着屏幕看有没有弹。
//   2. 渲染层与采集层都不该知道「为什么被拦」，只消费 verdict。
//   3. 「不显示」必须给出原因，否则用户配错了永远查不出来 —— 日志里要写清楚。
//
// 匹配语义（刻意做得可预测）：
//   - 应用：子串匹配，忽略 ASCII 大小写。"wechat" 能命中 "WeChat.exe" 与
//     "com.tencent.wechat" 这类 AUMID。
//   - 文件类型：取扩展名（去掉点、转小写）后**全等**匹配，不做子串，
//     避免 "png" 把 "apng" 一起拦掉。
//   - allow 列表为空 = 不启用白名单；非空 = 只放行命中项。
//   - 事件自身没有应用/文件名（音量、电源、设备）时，对应的应用/扩展规则不参与判定，
//     否则「白名单只留微信」会把音量提示一起干掉。
#ifndef BI_SRC_CORE_RULES_HPP
#define BI_SRC_CORE_RULES_HPP

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mini_json.hpp"

namespace bi {

// ---------------------------------------------------------------- 字符串工具

inline std::string AsciiLower(const std::string& in) {
    std::string out = in;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

inline std::string TrimSpaces(const std::string& in) {
    std::size_t begin = 0;
    std::size_t end = in.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(in[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1])) != 0) --end;
    return in.substr(begin, end - begin);
}

inline bool ContainsCaseless(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    return AsciiLower(haystack).find(AsciiLower(needle)) != std::string::npos;
}

// "报告.PDF" -> "pdf"；".gitignore" -> "gitignore"；无扩展名 -> ""
inline std::string ExtensionOf(const std::string& file_name) {
    const std::size_t slash = file_name.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? file_name : file_name.substr(slash + 1);
    const std::size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= base.size()) return std::string();
    return AsciiLower(base.substr(dot + 1));
}

inline std::string NormalizeExtension(const std::string& raw) {
    std::string out = TrimSpaces(raw);
    while (!out.empty() && (out.front() == '.' || out.front() == '*')) out.erase(out.begin());
    return AsciiLower(out);
}

// "23:05" -> 1385；非法返回 -1
inline int ParseClock(const std::string& text) {
    const std::string t = TrimSpaces(text);
    const std::size_t colon = t.find(':');
    if (colon == std::string::npos) return -1;
    const std::string hh = TrimSpaces(t.substr(0, colon));
    const std::string mm = TrimSpaces(t.substr(colon + 1));
    if (hh.empty() || mm.empty()) return -1;
    for (const char c : hh) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) return -1;
    }
    for (const char c : mm) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) return -1;
    }
    const int h = std::atoi(hh.c_str());
    const int m = std::atoi(mm.c_str());
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

inline std::string FormatClock(int minute_of_day) {
    const int m = ((minute_of_day % 1440) + 1440) % 1440;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", m / 60, m % 60);
    return buf;
}

// 疑似凭据：不显示正文。宁可多藏一次，也不要把密码写到屏幕上。
// 判据刻意保守：显式关键词命中，或「长且无空格 + 同时含字母数字」的串。
inline bool LooksLikeSecret(const std::string& text) {
    if (text.empty()) return false;
    static const char* kKeywords[] = {"password", "passwd", "密码", "密碼", "token",
                                      "secret",   "apikey", "api_key", "-----begin", "私钥"};
    for (const char* keyword : kKeywords) {
        if (ContainsCaseless(text, keyword)) return true;
    }
    if (text.size() >= 24 && text.size() <= 128) {
        bool has_space = false;
        bool has_digit = false;
        bool has_alpha = false;
        for (const char c : text) {
            if (c == ' ' || c == '\t' || c == '\n') has_space = true;
            if (c >= '0' && c <= '9') has_digit = true;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) has_alpha = true;
        }
        if (!has_space && has_digit && has_alpha) return true;
    }
    return false;
}

// ---------------------------------------------------------------- 配置结构

struct EventToggles {
    bool media = true;
    bool notification = true;
    bool clipboard = true;
    bool file = true;
    bool volume = true;
    bool power = true;
    bool device = true;
};

struct QuietHours {
    bool enabled = false;
    int from_min = 23 * 60;
    int to_min = 8 * 60;

    // 支持跨零点（from > to 时表示 23:00–08:00 这种跨夜区间）。
    bool Contains(int minute_of_day) const {
        if (!enabled) return false;
        if (from_min == to_min) return false;
        const int m = ((minute_of_day % 1440) + 1440) % 1440;
        if (from_min < to_min) return m >= from_min && m < to_min;
        return m >= from_min || m < to_min;
    }
};

struct ClipboardRule {
    int min_chars = 2;      // 少于这么长不分发（避免复制单字符刷屏）
    int max_chars = 60;     // 展示截断
    bool hide_secrets = true;
};

struct RuleConfig {
    int version = 1;
    EventToggles events;
    std::vector<std::string> app_block;
    std::vector<std::string> app_allow;
    std::vector<std::string> ext_block{"tmp", "crdownload", "part", "download"};
    std::vector<std::string> ext_allow;
    ClipboardRule clipboard;
    QuietHours quiet;
    bool fullscreen_hold = true;
    bool glass = true;  // 磨砂玻璃效果（关闭则回到不透明深色胶囊）
    // 胶囊本体画成什么样（三档）。默认「均衡」= 现在的磨砂玻璃。
    //   0 液态玻璃：透，但仍有玻璃结构 —— 32% 冷色染色 + 边缘折射亮环 +
    //             顶部镜面线 + 左上反光。浅色壁纸上也读得清，比"全透明"好看得多。
    //   1 均衡  ：磨砂玻璃（或 glass=false 时的深色实心）。
    //   2 不透明：永远是深色实心，看不清背后内容，但任何壁纸下都可读。
    int opacity = 1;
    // 胶囊停在哪里。0 靠左 / 1 居中 / 2 靠右（都按工作区算，不会越界）。
    int anchor = 1;
    // 休眠条：有岛但让路（全屏 / 长时间没人理）时显示的那一条把手。
    // 注意它和「没有岛」无关 —— 没有活动时窗口本来就应该全透明。
    bool sleep_bar = true;
    // 多久没交互自动睡成一条（秒）。0 = 不自动休眠，只在全屏避让时睡。
    int auto_sleep_sec = 60;
    bool lyrics = true;      // 展开态在线歌词（LRCLIB，关闭则不发网络请求）
    // ---- 编码 Agent ----
    // 读各家 Agent 落盘的会话文件，把它「正在干什么」显示成岛。
    // 关闭后完全不碰这些文件（不扫描、不 stat），给「不想让我读我的对话」留口子。
    bool agents = true;
    // 浏览器：读不读 History / Favicons / downloads。
    // 关掉后连这些库都不打开 —— 用户关它往往就是不想让程序碰浏览记录，
    // 「扫了但不显示」不符合这个意图。
    bool browsers = true;
    // 会议：检测「正在用麦克风」的会议软件并上岛。
    bool meetings = true;
    // 编辑器：显示「此刻在改哪个文件」（只读前台窗口标题）。
    bool editors = true;
    int agent_max = 3;        // 最多同时显示几个 Agent（1..4）
    int agent_idle_sec = 480; // 多久没有新内容就从岛上撤掉（秒）
    // 繁转简：歌词与媒体标题/歌手统一显示简体（歌词库按繁体标题匹配最准，
    // 所以查询用原文、显示用简体）。关闭后原样显示。
    bool simplify_chinese = true;
    // 歌词请求走的代理。空 = 自动探测（环境变量 HTTPS_PROXY/HTTP_PROXY → Internet 选项）。
    // 本机实测：直连会优先 IPv6 而超时（12002），只有代理能出网，所以要给显式入口。
    std::string lyrics_proxy;

    static RuleConfig Defaults() { return RuleConfig(); }

    bool EventEnabled(const std::string& kind) const {
        if (kind == "media") return events.media;
        if (kind == "notification") return events.notification;
        if (kind == "clipboard") return events.clipboard;
        if (kind == "file") return events.file;
        if (kind == "volume") return events.volume;
        if (kind == "power") return events.power;
        if (kind == "device") return events.device;
        if (kind == "agent") return agents;
        if (kind == "browser") return browsers;
        if (kind == "meeting") return meetings;
        if (kind == "editor") return editors;
        return true;  // 未知类型不因配置被吞掉，交给上层判断
    }

    // 供设置界面显示：kind -> 中文名
    static const char* LabelOf(const std::string& kind) {
        if (kind == "media") return "媒体播放";
        if (kind == "notification") return "消息通知";
        if (kind == "clipboard") return "复制 / 截图";
        if (kind == "file") return "文件落盘";
        if (kind == "volume") return "音量变化";
        if (kind == "power") return "电源 / 电量";
        if (kind == "device") return "设备接入";
        if (kind == "agent") return "编码 Agent";
        return kind.c_str();
    }

    std::string ToJson() const {
        std::string out;
        out += "{\n";
        out += "  \"version\": " + std::to_string(version) + ",\n";
        out += "  \"_help\": \"保存即生效，运行中的实例 1 秒内热加载。改坏了也不影响运行，会回落到内置默认值。\",\n";
        out += "  \"events\": {\n";
        out += std::string("    \"media\": ") + (events.media ? "true" : "false") + ",\n";
        out += std::string("    \"notification\": ") + (events.notification ? "true" : "false") + ",\n";
        out += std::string("    \"clipboard\": ") + (events.clipboard ? "true" : "false") + ",\n";
        out += std::string("    \"file\": ") + (events.file ? "true" : "false") + ",\n";
        out += std::string("    \"volume\": ") + (events.volume ? "true" : "false") + ",\n";
        out += std::string("    \"power\": ") + (events.power ? "true" : "false") + ",\n";
        out += std::string("    \"device\": ") + (events.device ? "true" : "false") + "\n";
        out += "  },\n";
        out += "  \"apps\": {\n";
        out += "    \"_help\": \"block 命中即静默；allow 非空时只放行命中项。子串匹配，忽略大小写。\",\n";
        out += "    \"block\": " + StringArray(app_block) + ",\n";
        out += "    \"allow\": " + StringArray(app_allow) + "\n";
        out += "  },\n";
        out += "  \"extensions\": {\n";
        out += "    \"_help\": \"按扩展名（不带点）精确匹配，例如 zip、tmp、iso。\",\n";
        out += "    \"block\": " + StringArray(ext_block) + ",\n";
        out += "    \"allow\": " + StringArray(ext_allow) + "\n";
        out += "  },\n";
        out += "  \"clipboard\": {\n";
        out += "    \"min_chars\": " + std::to_string(clipboard.min_chars) + ",\n";
        out += "    \"max_chars\": " + std::to_string(clipboard.max_chars) + ",\n";
        out += std::string("    \"hide_secrets\": ") + (clipboard.hide_secrets ? "true" : "false") + "\n";
        out += "  },\n";
        out += "  \"quiet_hours\": {\n";
        out += std::string("    \"enabled\": ") + (quiet.enabled ? "true" : "false") + ",\n";
        out += "    \"from\": \"" + FormatClock(quiet.from_min) + "\",\n";
        out += "    \"to\": \"" + FormatClock(quiet.to_min) + "\"\n";
        out += "  },\n";
        out += std::string("  \"fullscreen_hold\": ") + (fullscreen_hold ? "true" : "false") + ",\n";
        out += std::string("  \"glass\": ") + (glass ? "true" : "false") + ",\n";
        out += "  \"opacity\": " + std::to_string(opacity) + ",\n";
        out += "  \"anchor\": " + std::to_string(anchor) + ",\n";
        out += std::string("  \"sleep_bar\": ") + (sleep_bar ? "true" : "false") + ",\n";
        out += "  \"auto_sleep_sec\": " + std::to_string(auto_sleep_sec) + ",\n";
        out += std::string("  \"lyrics\": ") + (lyrics ? "true" : "false") + ",\n";
        out += std::string("  \"agents\": ") + (agents ? "true" : "false") + ",\n";
        out += std::string("  \"browsers\": ") + (browsers ? "true" : "false") + ",\n";
        out += std::string("  \"meetings\": ") + (meetings ? "true" : "false") + ",\n";
        out += std::string("  \"editors\": ") + (editors ? "true" : "false") + ",\n";
        out += "  \"agent_max\": " + std::to_string(agent_max) + ",\n";
        out += "  \"agent_idle_sec\": " + std::to_string(agent_idle_sec) + ",\n";
        out += std::string("  \"simplify_chinese\": ") +
               (simplify_chinese ? "true" : "false") + ",\n";
        // 必须 Quote：这是 ToJson 里唯一不经转义的字符串字段。值里出现一个反斜杠
        //（局域网代理串很常见）就会产出非法 JSON，下次启动 Parse 失败 → 整份回退
        // 默认 → 用户所有设置丢失（issue #2 第 6 条）。
        // json::Quote **自带两侧引号**，这里不能再包一层（否则是 ""...""）。
        out += "  \"lyrics_proxy\": " + json::Quote(lyrics_proxy) + "\n";
        out += "}\n";
        return out;
    }

    // 容错解析：任何字段坏了都退回默认值，绝不因为用户手抖让程序起不来。
    static RuleConfig FromJson(const json::Value& root) {
        RuleConfig cfg;
        if (!root.IsObject()) return cfg;

        cfg.version = root.At("version").AsInt(cfg.version);

        const json::Value& events = root.At("events");
        cfg.events.media = events.At("media").AsBool(cfg.events.media);
        cfg.events.notification = events.At("notification").AsBool(cfg.events.notification);
        cfg.events.clipboard = events.At("clipboard").AsBool(cfg.events.clipboard);
        cfg.events.file = events.At("file").AsBool(cfg.events.file);
        cfg.events.volume = events.At("volume").AsBool(cfg.events.volume);
        cfg.events.power = events.At("power").AsBool(cfg.events.power);
        cfg.events.device = events.At("device").AsBool(cfg.events.device);

        const json::Value& apps = root.At("apps");
        if (apps.At("block").IsArray()) cfg.app_block = ReadStringArray(apps.At("block"));
        if (apps.At("allow").IsArray()) cfg.app_allow = ReadStringArray(apps.At("allow"));

        const json::Value& exts = root.At("extensions");
        if (exts.At("block").IsArray()) cfg.ext_block = ReadExtensions(exts.At("block"));
        if (exts.At("allow").IsArray()) cfg.ext_allow = ReadExtensions(exts.At("allow"));

        const json::Value& clip = root.At("clipboard");
        cfg.clipboard.min_chars = clip.At("min_chars").AsInt(cfg.clipboard.min_chars);
        cfg.clipboard.max_chars = clip.At("max_chars").AsInt(cfg.clipboard.max_chars);
        cfg.clipboard.hide_secrets = clip.At("hide_secrets").AsBool(cfg.clipboard.hide_secrets);
        if (cfg.clipboard.min_chars < 0) cfg.clipboard.min_chars = 0;
        if (cfg.clipboard.max_chars < 8) cfg.clipboard.max_chars = 8;

        const json::Value& quiet = root.At("quiet_hours");
        cfg.quiet.enabled = quiet.At("enabled").AsBool(cfg.quiet.enabled);
        const int from = ParseClock(quiet.At("from").AsString(FormatClock(cfg.quiet.from_min)));
        const int to = ParseClock(quiet.At("to").AsString(FormatClock(cfg.quiet.to_min)));
        if (from >= 0) cfg.quiet.from_min = from;
        if (to >= 0) cfg.quiet.to_min = to;

        cfg.fullscreen_hold = root.At("fullscreen_hold").AsBool(cfg.fullscreen_hold);
        cfg.glass = root.At("glass").AsBool(cfg.glass);
        {
            // 越界值直接夹住而不是忽略：配置是手写的，写个 5 不该让整个外观回退到默认。
            const int opacity = root.At("opacity").AsInt(cfg.opacity);
            cfg.opacity = opacity < 0 ? 0 : (opacity > 2 ? 2 : opacity);
            const int anchor = root.At("anchor").AsInt(cfg.anchor);
            cfg.anchor = anchor < 0 ? 0 : (anchor > 2 ? 2 : anchor);
        }
        cfg.sleep_bar = root.At("sleep_bar").AsBool(cfg.sleep_bar);
        // 老配置里的 idle_clock 当作 sleep_bar 的别名，避免升级后行为突然变了。
        cfg.sleep_bar = root.At("idle_clock").AsBool(cfg.sleep_bar);
        const int sleep_sec = static_cast<int>(root.At("auto_sleep_sec").AsInt(cfg.auto_sleep_sec));
        if (sleep_sec >= 0 && sleep_sec <= 3600) cfg.auto_sleep_sec = sleep_sec;
        cfg.lyrics = root.At("lyrics").AsBool(cfg.lyrics);
        cfg.agents = root.At("agents").AsBool(cfg.agents);
        cfg.browsers = root.At("browsers").AsBool(cfg.browsers);
        cfg.meetings = root.At("meetings").AsBool(cfg.meetings);
        cfg.editors = root.At("editors").AsBool(cfg.editors);
        const int agent_max = root.At("agent_max").AsInt(cfg.agent_max);
        if (agent_max >= 1 && agent_max <= 4) cfg.agent_max = agent_max;
        const int agent_idle = root.At("agent_idle_sec").AsInt(cfg.agent_idle_sec);
        if (agent_idle >= 30 && agent_idle <= 7200) cfg.agent_idle_sec = agent_idle;
        cfg.simplify_chinese = root.At("simplify_chinese").AsBool(cfg.simplify_chinese);
        cfg.lyrics_proxy = root.At("lyrics_proxy").AsString(cfg.lyrics_proxy);
        return cfg;
    }

private:
    static std::string StringArray(const std::vector<std::string>& items) {
        std::string out = "[";
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i != 0) out += ", ";
            out += json::Quote(items[i]);
        }
        out += "]";
        return out;
    }

    static std::vector<std::string> ReadStringArray(const json::Value& array) {
        std::vector<std::string> out;
        for (const auto& item : array.items) {
            if (!item.IsString()) continue;
            const std::string trimmed = TrimSpaces(item.text);
            if (!trimmed.empty()) out.push_back(trimmed);
        }
        return out;
    }

    static std::vector<std::string> ReadExtensions(const json::Value& array) {
        std::vector<std::string> out;
        for (const auto& item : array.items) {
            if (!item.IsString()) continue;
            const std::string ext = NormalizeExtension(item.text);
            if (!ext.empty()) out.push_back(ext);
        }
        return out;
    }
};

// ---------------------------------------------------------------- 判定

struct RuleInput {
    std::string kind;
    std::string app_id;     // 进程名 / AUMID
    std::string app_label;  // 展示名（也可参与匹配，用户更可能写中文名）
    std::string file_name;
    std::string text;
    int minute_of_day = -1;  // <0 表示不检查免打扰
};

struct RuleVerdict {
    bool show = true;
    const char* reason = "ok";
    std::string matched;  // 命中的规则项，用于日志
};

inline bool MatchAny(const std::vector<std::string>& needles, const std::string& haystack,
                     std::string* matched) {
    for (const auto& needle : needles) {
        if (ContainsCaseless(haystack, needle)) {
            if (matched != nullptr) *matched = needle;
            return true;
        }
    }
    return false;
}

inline RuleVerdict EvaluateRules(const RuleConfig& cfg, const RuleInput& in) {
    RuleVerdict verdict;

    if (!cfg.EventEnabled(in.kind)) {
        verdict.show = false;
        verdict.reason = "event-disabled";
        verdict.matched = in.kind;
        return verdict;
    }

    // 免打扰时段拦截「弹一下就走」的提醒；媒体是常驻底噪，不在此列
    //（否则夜里放歌会让胶囊整夜消失，那不是免打扰，是失联）。
    if (in.minute_of_day >= 0 && in.kind != "media" && cfg.quiet.Contains(in.minute_of_day)) {
        verdict.show = false;
        verdict.reason = "quiet-hours";
        verdict.matched = FormatClock(cfg.quiet.from_min) + "-" + FormatClock(cfg.quiet.to_min);
        return verdict;
    }

    // 应用规则只在事件带应用身份时参与，否则音量/电源会被白名单误伤。
    if (!in.app_id.empty() || !in.app_label.empty()) {
        const std::string haystack = in.app_id + " " + in.app_label;
        if (!cfg.app_allow.empty() && !MatchAny(cfg.app_allow, haystack, &verdict.matched)) {
            verdict.show = false;
            verdict.reason = "app-not-allowed";
            return verdict;
        }
        if (MatchAny(cfg.app_block, haystack, &verdict.matched)) {
            verdict.show = false;
            verdict.reason = "app-blocked";
            return verdict;
        }
    }

    // 扩展名同理：只对带文件的事件生效。
    if (!in.file_name.empty()) {
        const std::string ext = ExtensionOf(in.file_name);
        if (!ext.empty()) {
            const bool allowed =
                cfg.ext_allow.empty() ||
                std::find(cfg.ext_allow.begin(), cfg.ext_allow.end(), ext) != cfg.ext_allow.end();
            if (!allowed) {
                verdict.show = false;
                verdict.reason = "ext-not-allowed";
                verdict.matched = ext;
                return verdict;
            }
            if (std::find(cfg.ext_block.begin(), cfg.ext_block.end(), ext) != cfg.ext_block.end()) {
                verdict.show = false;
                verdict.reason = "ext-blocked";
                verdict.matched = ext;
                return verdict;
            }
        }
    }

    if (in.kind == "clipboard" && !in.text.empty() &&
        static_cast<int>(in.text.size()) < cfg.clipboard.min_chars) {
        verdict.show = false;
        verdict.reason = "too-short";
        return verdict;
    }

    return verdict;
}

}  // namespace bi

#endif  // BI_SRC_CORE_RULES_HPP
