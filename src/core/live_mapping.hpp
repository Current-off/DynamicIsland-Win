// DynamicIsland-Win / 系统状态 → 实时活动的映射层（C++17，平台无关）
//
// 为什么单独拆出来：
//   采集（live_sources.hpp）要 Win32/WinRT，只能实机验证；
//   映射（本文件）是纯数据变换，可以单元测试 —— 而「该不该弹、弹多久、谁占主位」
//   恰恰是最容易写错、也最容易被用户感知到的部分。
//
// 每个事件都带三样东西：
//   relevance_score  谁占主位（越高越优先）
//   stale_at_ms      什么时候自动退场（没有这个字段 = 常驻，源消失才结束）
//   content_json     渲染层取文案的唯一来源（渲染层不再硬编码任何文案）
//
// 优先级依据：
//   消息通知 75 > 低电量 70 > 复制/截图 68 > 文件落盘 65 > 设备接入 62
//   > 媒体 60 > 电源插拔 58 > 音量 30
// 前四个都是「刚发生、有信息量、需要看一眼」，媒体是「你自己在放，当底噪」。
#ifndef DIW_SRC_CORE_LIVE_MAPPING_HPP
#define DIW_SRC_CORE_LIVE_MAPPING_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "Types.hpp"
#include "agent_session.hpp"
#include "browser_page.hpp"
#include "editor_state.hpp"
#include "mini_json.hpp"
#include "rules.hpp"

namespace diw {

// ---------------------------------------------------------------- 快照结构
// 全部是纯数据，采集层填、映射层读、渲染层不碰。

struct MediaState {
    bool present = false;
    bool playing = false;
    std::string title;
    std::string artist;
    std::string album;
    std::string app_id;     // SMTC 的 SourceAppUserModelId
    std::string app_label;  // 友好名（AUMID 的末段）
    std::int64_t position_ms = 0;
    std::int64_t end_ms = 0;
    // 专辑封面在磁盘上的缓存路径（采集层解码后落盘，渲染层按路径加载）。
    // 走字符串而不是内存位图，是为了让 Activity 继续是一个纯数据、
    // 可序列化的结构 —— 它是跨层契约，不该掺进 GDI+ 对象。
    std::string cover_path;
    // 播放器自己的应用图标，封面缺失时的兜底。B 站 / VLC 这类视频应用
    // 不提供缩略图，只画一个纯色占位方块完全认不出是哪个 App。
    std::string icon_path;
    // 用户刚刚用 × 关掉了这一条。**只在采集层打标记，不改数据**：
    // 早先的做法是「抑制时把 MediaState 清空」，结果 title 一起被清掉，
    // 下一帧「属性不用重读」的分支又把这个空标题拷回去 —— 日志里表现为
    // `none (paused)` 与 `(no title) (playing)` 交替抖动，界面上就是
    // 「叉掉之后再也不出现岛」。抑制只该影响「要不要展示」，不该污染数据。
    bool suppressed = false;
};

struct NotificationState {
    bool ok = false;
    std::uint64_t seq = 0;
    std::string app;
    std::string app_id;
    std::string title;
    std::string body;
    // 来源应用图标（Logo）的缓存路径，语义与 MediaState::cover_path 一致：
    // 采集层解码落盘、content_json 里传路径、渲染层按路径加载。
    std::string icon_path;
};

// 复制/截图。kind 区分 text / image / screenshot / files，
// 因为「复制了一行字」和「截了一张图」在岛上的文案与图形完全不同。
struct ClipState {
    std::uint64_t seq = 0;
    std::string kind;   // text | image | screenshot | files
    std::string text;   // 文本预览，或文件名 / 分辨率，用于展示
    std::string app_id;
    std::string app_label;
    // 来源应用的完整 exe 路径（与 app_id 的「文件名」互补）：
    // 一览模式点击岛直接拉起对应应用时，路径才是可执行的句柄。
    std::string app_path;
    int item_count = 0;  // 复制的文件个数
    int width = 0;       // 图片/截图尺寸
    int height = 0;
};

struct FileState {
    std::string name;
    std::string ext;
    std::int64_t age_ms = -1;  // 距今毫秒，<0 表示还没有
};

struct PowerState {
    bool has_battery = false;
    bool ac_online = true;
    int pct = 100;
    bool known = false;  // 至少成功读过一次，否则首次会把「插入电源」误报成事件
};

struct DeviceState {
    std::uint64_t seq = 0;
    std::string label;  // 例如 "E: 已接入"
};

// 会议的一拍快照。只留「谁在开会 + 开了多久」——
// 麦克风电平、参会人这些拿不到，也不该假装拿得到。
struct MeetingSnapshot {
    std::string brand;
    std::string label;
    std::string exe;
    std::int64_t elapsed_ms = 0;
};

struct Snapshot {
    MediaState media;                      // 「主」媒体（列表第一项，命令/歌词用）
    // 全部可见的媒体会话。多个播放器同时开着时（QQ 音乐 + 腾讯视频 + B 站…）
    // SMTC 里是多个 session，以前只取 GetCurrentSession 一个，于是「同时开了
    // 两个播放器却只有一个岛」。这里全量上报，由映射层一应用一条活动。
    std::vector<MediaState> media_all;
    // 正在跑的编码 Agent（Claude Code / Codex / Gemini / WorkBuddy…）。
    // 与媒体同构：采集层产出结构化会话，映射层一会话一条活动。
    // 不放在 MediaState 里是因为它们的字段几乎不重叠（进度 vs 任务清单），
    // 硬塞进去会让两边都变成一半空的字段。
    std::vector<AgentSession> agents;
    // 浏览器：前台那个（在看什么）与后台有下载的那些（在下什么）。
    // 和 agent 一样是「采集层产结构化数据、映射层一条活动」，不塞进 MediaState ——
    // 媒体那几个字段（artist/album/position）跟浏览器毫无关系。
    std::vector<BrowserPage> browsers;
    // 正在用麦克风的会议（腾讯会议 / Zoom / Teams / Discord…）。
    std::vector<MeetingSnapshot> meetings;
    // 编辑器：此刻在前台改哪个文件。只会有零或一条 —— 前台只能有一个窗口。
    std::vector<EditorState> editors;
    // 采集层的「墙上时钟」（Unix epoch 毫秒）。BuildActivities 的 now_ms 是
    // 从进程启动算起的相对毫秒（stale_at 用它做窗口，语义更简单），
    // 而会话文件里的时间戳是绝对的 —— 两者相减会得到一个巨大负数，
    // 表现是「所有 agent 永远显示运行中、时长永远为空」。必须分开传。
    std::int64_t wall_now_ms = 0;
    NotificationState notif;
    ClipState clipboard;
    FileState file;
    PowerState power;
    DeviceState device;
    int volume_pct = 0;
    bool muted = false;
    bool volume_known = false;  // 音频端点读成功过；未读到时 0% 不是真实音量
    bool fullscreen = false;
    std::string last_error;
};

// ---------------------------------------------------------------- 基线
// 「变化检测」的状态必须显式传递，不能藏在全局变量里：
// 藏在全局里就没法测「同一次复制只弹一次」。

struct LiveBaselines {
    std::uint64_t notif_seq = 0;
    std::uint64_t clip_seq = 0;
    std::uint64_t device_seq = 0;
    int vol_pct = -1;
    bool muted = false;
    std::int64_t vol_change_ms = -1;
    bool ac_known = false;
    bool ac_online = true;
};

struct BlockedNote {
    std::string kind;
    std::string reason;
    std::string matched;
};

struct LiveActivities {
    std::vector<Activity> list;
    std::vector<BlockedNote> blocked;
    bool suspend = false;
    LiveBaselines next;
};

namespace detail {

// 活动 id 只允许安全字符：把 app_id 归一成小写、非字母数字换 '-'。
// 「一个应用一条媒体活动」时 id 必须由 app 决定，而且必须稳定 ——
// 带序号的话，播放/暂停一变排序就变，id 跟着变，活动被当成新的重建，岛会闪。
inline std::string Slug(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += c;
        } else if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
        if (out.size() >= 40) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

inline std::string Quote(const std::string& v) { return json::Quote(v); }

// 把「事件候选」送进规则引擎；被拦下就记一笔原因，方便用户自查配置。
inline bool Admit(const RuleConfig& cfg, const RuleInput& input, LiveActivities& out) {
    const RuleVerdict verdict = EvaluateRules(cfg, input);
    if (verdict.show) return true;
    BlockedNote note;
    note.kind = input.kind;
    note.reason = verdict.reason;
    note.matched = verdict.matched;
    out.blocked.push_back(note);
    return false;
}

inline std::string ClipTitle(const ClipState& clip) {
    if (clip.kind == "screenshot") return "已截图";
    if (clip.kind == "image") return "已复制图片";
    if (clip.kind == "files") {
        return "已复制 " + std::to_string(clip.item_count) + " 个文件";
    }
    return "已复制";
}

// AUMID 末段： "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify" -> "Spotify"
inline std::string FriendlyAppName(const std::string& aumid) {
    if (aumid.empty()) return std::string();
    const std::size_t bang = aumid.find_last_of('!');
    std::string tail = bang == std::string::npos ? aumid : aumid.substr(bang + 1);
    if (tail.empty()) tail = aumid;
    return tail;
}

}  // namespace detail

// ---------------------------------------------------------------- 映射主体

inline LiveActivities BuildActivities(const Snapshot& s, std::int64_t now_ms, int minute_of_day,
                                      const LiveBaselines& base, const RuleConfig& cfg) {
    LiveActivities out;
    out.suspend = cfg.fullscreen_hold && s.fullscreen;

    // ---- 媒体：常驻，源消失才退场。每个会话一条活动 ----
    // suppressed = 刚被用户用 × 关掉（只在采集层打标记，数据本身是完好的）。
    std::vector<const MediaState*> medias;
    if (!s.media_all.empty()) {
        for (const auto& m : s.media_all) medias.push_back(&m);
    } else if (s.media.present) {
        medias.push_back(&s.media);
    }
    for (const MediaState* mp : medias) {
        const MediaState& media = *mp;
        if (!media.present || media.title.empty() || media.suppressed) continue;
        RuleInput input;
        input.kind = "media";
        input.app_id = media.app_id;
        input.app_label = media.app_label;
        input.minute_of_day = minute_of_day;
        if (detail::Admit(cfg, input, out)) {
            Activity a;
            // id 必须只跟应用绑定（不能带序号）：序号会随播放/暂停排序变化，
            // 一变 id 就变，活动被当作「新的」重建，岛会闪。
            a.id = media.app_id.empty() ? std::string("live-media")
                                        : ("live-media-" + detail::Slug(media.app_id));
            a.kind = "media";
            a.title = media.title;
            // 暂停时降权：正在放歌比暂停的会话更值得占主位。
            a.relevance_score = media.playing ? 60.0 : 45.0;
            a.created_at_ms = now_ms;
            // 进度必须写进 content_json：BuildView 靠它决定「展开态要不要进度条、
            // 用多高的面板」。早先只写了 position_ms / end_ms，渲染层解析不到 progress，
            // 于是媒体展开后是没有进度条的矮面板 —— 截图里只有时间，没有条。
            const double progress =
                media.end_ms > 0
                    ? (std::max)(0.0, (std::min)(1.0, static_cast<double>(media.position_ms) /
                                                          static_cast<double>(media.end_ms)))
                    : -1.0;
            char progress_text[24];
            std::snprintf(progress_text, sizeof(progress_text), "%.3f", progress);
            a.content_json = "{\"artist\":" + detail::Quote(media.artist) + ",\"album\":" +
                             detail::Quote(media.album) + ",\"app\":" +
                             detail::Quote(media.app_label) + ",\"app_id\":" +
                             detail::Quote(media.app_id) + ",\"playing\":" +
                             (media.playing ? "true" : "false") + ",\"position_ms\":" +
                             std::to_string(media.position_ms) + ",\"end_ms\":" +
                             std::to_string(media.end_ms) + ",\"progress\":" + progress_text +
                             ",\"cover\":" + detail::Quote(media.cover_path) + ",\"icon\":" +
                             detail::Quote(media.icon_path) + "}";
            out.list.push_back(a);
        }
    }

    // ---- 编码 Agent：一个会话一条活动 ----
    // 常驻（不设 stale_at）：只要会话还在写，岛就该在；会话文件不再更新
    // 之后由采集层把它从快照里摘掉，这条活动随之退场。
    //
    // 权重刻意分两档：正在跑 74（高于媒体 60，低于一次通知 75）、
    // 等输入 50（低于媒体）。这样「放歌时 agent 在等你」不会把歌挤走，
    // 而「agent 正在改代码」时它占主位 —— 那才是用户此刻真正关心的事。
    // agent 的所有时间判定都必须用 epoch：会话文件里的 timestamp 是绝对的，
    // 而 now_ms 是「进程启动至今」的相对值。
    const std::int64_t agent_now = s.wall_now_ms > 0 ? s.wall_now_ms : now_ms;
    for (const auto& agent : s.agents) {
        if (!agent.Present()) continue;
        RuleInput input;
        input.kind = "agent";
        input.app_id = agent.provider;
        input.app_label = agent.provider_label;
        input.minute_of_day = minute_of_day;
        if (!detail::Admit(cfg, input, out)) continue;

        const std::string agent_state = AgentStateAt(agent, agent_now);
        const bool running = agent_state == "running";
        Activity a;
        // id 由 provider + 会话 id 决定，与会话内的事件无关 —— 每多一次
        // 工具调用就换一个 id 的话，界面上这条岛会被当成新活动重建，一直闪。
        a.id = "live-agent-" + detail::Slug(agent.provider) + "-" + detail::Slug(agent.session_id);
        a.kind = "agent";
        a.title = agent.project.empty() ? agent.provider_label : agent.project;
        // 目录名只是时间戳（WorkBuddy 的 workspace 就是）时，换成会话标题，
        // 否则标题栏会是一串「2026-11-12-14-12-37」，占满宽度却零信息量。
        if (IsGenericProjectName(agent.project) && !agent.task.empty()) {
            a.title = EllipsizeUtf8(agent.task, 22);
        }
        a.relevance_score = running ? 74.0 : 50.0;
        a.created_at_ms = now_ms;  // 相对时钟：核心域只用它算「显示多久了」

        std::string todos = "[";
        for (std::size_t i = 0; i < agent.todos.size(); ++i) {
            if (i != 0) todos += ",";
            todos += "{\"t\":" + detail::Quote(agent.todos[i].text) + ",\"s\":" +
                     detail::Quote(agent.todos[i].status) + "}";
        }
        todos += "]";
        std::string timeline = "[";
        for (std::size_t i = 0; i < agent.timeline.size(); ++i) {
            if (i != 0) timeline += ",";
            timeline += detail::Quote(agent.timeline[i]);
        }
        timeline += "]";

        a.content_json =
            "{\"provider\":" + detail::Quote(agent.provider) + ",\"name\":" +
            detail::Quote(agent.provider_label) + ",\"state\":" + detail::Quote(agent_state) +
            ",\"project\":" + detail::Quote(agent.project) + ",\"cwd\":" + detail::Quote(agent.cwd) +
            ",\"task\":" + detail::Quote(agent.task) + ",\"action\":" + detail::Quote(agent.action) +
            ",\"detail\":" + detail::Quote(agent.detail) + ",\"reply\":" +
            detail::Quote(agent.last_text) + ",\"model\":" + detail::Quote(agent.model) +
            ",\"elapsed\":" + detail::Quote(AgentElapsedText(agent, agent_now)) + ",\"started_ms\":" +
            std::to_string(agent.started_ms) + ",\"steps\":" + std::to_string(agent.tool_calls) +
            ",\"done\":" + std::to_string(agent.done_count) + ",\"total\":" +
            std::to_string(agent.todo_count) + ",\"todos\":" + todos + ",\"timeline\":" + timeline +
            ",\"log\":" + detail::Quote(agent.log_path) + "}";
        out.list.push_back(a);
    }

    // ---- 浏览器：前台在看什么 / 后台在下什么 ----
    //
    // 有一条必须挡住的重复：在 B 站看视频时，SMTC 给一条「媒体」活动
    // （app_id = Chrome），浏览器采集又给一条「浏览器」活动（页面 = 那个视频页）。
    // 不判这一下，屏幕上就是两个岛讲同一件事 —— 和当初「两个一模一样的岛」
    // 是同一类错误，只是这次两个岛长得不一样、内容却一样。
    // 有下载时例外：下载进度是媒体那条完全没有的信息，值得单开一个岛。
    for (const auto& b : s.browsers) {
        if (!b.Present()) continue;
        bool media_covers = false;
        if (b.downloads.empty()) {
            for (const MediaState* mp : medias) {
                if (!mp->present) continue;
                if (BrowserOwnsMedia(mp->app_id, mp->app_label, b.brand, b.label, b.exe_stem)) {
                    media_covers = true;
                    break;
                }
            }
        }
        if (media_covers) continue;

        RuleInput input;
        input.kind = "browser";
        input.app_id = b.brand;
        input.app_label = b.label;
        input.minute_of_day = minute_of_day;
        if (!detail::Admit(cfg, input, out)) continue;

        Activity a;
        a.id = "live-browser-" + detail::Slug(b.brand) + "-" + detail::Slug(b.profile);
        a.kind = "browser";
        if (!b.title.empty()) {
            a.title = b.title;
        } else if (!b.downloads.empty()) {
            a.title = b.downloads.front().name;
        } else {
            a.title = b.label;
        }
        // 权重：前台浏览器 64 > 播放中的媒体 60。
        // 理由是「焦点在哪」：用户正盯着一个页面时，后台那张专辑在放什么
        // 是背景信息；反过来把音乐放主位、把「我在看什么」塞进小圆，才是错位。
        // 有下载则再抬一档 —— 那是用户正在等、且只有浏览器自己知道的信息。
        // （浏览器自己在放视频时不出这条岛，所以不会跟媒体抢同一个视频。）
        if (!b.foreground) {
            a.relevance_score = 62.0;   // 后台但仍在下东西
        } else {
            a.relevance_score = b.downloads.empty() ? 64.0 : 66.0;
        }
        a.created_at_ms = now_ms;

        std::string downloads = "[";
        for (std::size_t i = 0; i < b.downloads.size(); ++i) {
            if (i != 0) downloads += ",";
            const auto& d = b.downloads[i];
            char pct[24];
            std::snprintf(pct, sizeof(pct), "%.3f", d.progress);
            downloads += "{\"n\":" + detail::Quote(d.name) + ",\"p\":" + pct + ",\"got\":" +
                         std::to_string(d.received) + ",\"total\":" + std::to_string(d.total) + "}";
        }
        downloads += "]";

        char dprogress[24];
        std::snprintf(dprogress, sizeof(dprogress), "%.3f", b.download_progress);
        a.content_json =
            "{\"brand\":" + detail::Quote(b.brand) + ",\"label\":" + detail::Quote(b.label) +
            ",\"title\":" + detail::Quote(b.title) + ",\"url\":" + detail::Quote(b.url) +
            ",\"site\":" + detail::Quote(b.site) + ",\"site_label\":" +
            detail::Quote(b.site_label) + ",\"favicon\":" + detail::Quote(b.favicon) +
            ",\"profile\":" + detail::Quote(b.profile) + ",\"fg\":" +
            (b.foreground ? "true" : "false") + ",\"dcount\":" + std::to_string(b.download_count) +
            ",\"dprogress\":" + dprogress + ",\"dhint\":" + detail::Quote(b.download_hint) +
            ",\"downloads\":" + downloads + "}";
        out.list.push_back(std::move(a));
    }

    // ---- 编辑器：我正在改哪个文件 ----
    //
    // 这条岛和 agent 岛是互补的：agent 是"AI 在改"，editor 是"我在改"。
    // 两个可以同时在岛上，各自回答不同的问题。
    //
    // 权重 56：低于媒体（60）和浏览器前台（64）—— 正在编辑是个**持续状态**，
    // 不是事件；但它高于"等待输入的 agent"（50），因为那是用户此刻眼睛盯着的窗口。
    for (const auto& ed : s.editors) {
        if (ed.file.empty()) continue;
        RuleInput input;
        input.kind = "editor";
        input.app_id = ed.brand;
        input.app_label = ed.label;
        input.minute_of_day = minute_of_day;
        if (!detail::Admit(cfg, input, out)) continue;

        Activity a;
        a.id = "live-editor-" + detail::Slug(ed.brand);
        a.kind = "editor";
        a.title = ed.file;
        a.relevance_score = 56.0;
        a.created_at_ms = now_ms;
        a.content_json =
            "{\"brand\":" + detail::Quote(ed.brand) + ",\"label\":" + detail::Quote(ed.label) +
            ",\"file\":" + detail::Quote(ed.file) + ",\"project\":" + detail::Quote(ed.project) +
            ",\"language\":" + detail::Quote(ed.language) + ",\"dirty\":" +
            (ed.dirty ? "true" : "false") + ",\"remote\":" + (ed.remote ? "true" : "false") +
            ",\"exe\":" + detail::Quote(ed.exe_stem) + "}";
        out.list.push_back(std::move(a));
    }

    // ---- 会议：正在用麦克风 ----
    // 这条岛的价值不在「显示什么」，而在「提醒你在开会」：很多人开着会切去干活，
    // 忘了自己还在别人耳朵里。所以权重压在媒体之上，仅次于通知。
    for (const auto& mt : s.meetings) {
        RuleInput input;
        input.kind = "meeting";
        input.app_id = mt.brand;
        input.app_label = mt.label;
        input.minute_of_day = minute_of_day;
        if (!detail::Admit(cfg, input, out)) continue;

        const std::int64_t sec = mt.elapsed_ms / 1000;
        char clock[32];
        if (sec >= 3600) {
            std::snprintf(clock, sizeof(clock), "%lld:%02lld", static_cast<long long>(sec / 3600),
                          static_cast<long long>((sec % 3600) / 60));
        } else {
            std::snprintf(clock, sizeof(clock), "%02lld:%02lld", static_cast<long long>(sec / 60),
                          static_cast<long long>(sec % 60));
        }

        Activity a;
        a.id = "live-meeting-" + detail::Slug(mt.brand);
        a.kind = "meeting";
        a.title = mt.label;
        a.relevance_score = 72.0;
        a.created_at_ms = now_ms;
        a.content_json = "{\"brand\":" + detail::Quote(mt.brand) + ",\"label\":" +
                         detail::Quote(mt.label) + ",\"exe\":" + detail::Quote(mt.exe) +
                         ",\"elapsed\":" + detail::Quote(clock) + "}";
        out.list.push_back(std::move(a));
    }

    // ---- 通知：只在序号变化时产生，6 秒退场 ----
    if (s.notif.ok && s.notif.seq != 0 && s.notif.seq != base.notif_seq) {
        RuleInput input;
        input.kind = "notification";
        input.app_id = s.notif.app_id;
        input.app_label = s.notif.app;
        input.minute_of_day = minute_of_day;
        if (detail::Admit(cfg, input, out)) {
            Activity a;
            a.id = "live-notif";
            a.kind = "notification";
            a.title = s.notif.title.empty() ? (s.notif.app.empty() ? "新通知" : s.notif.app)
                                            : s.notif.title;
            a.relevance_score = 75.0;
            a.created_at_ms = now_ms;
            a.stale_at_ms = now_ms + 6000;
            a.content_json = "{\"app\":" + detail::Quote(s.notif.app) + ",\"app_id\":" +
                             detail::Quote(s.notif.app_id) + ",\"body\":" +
                             detail::Quote(s.notif.body) + ",\"icon\":" +
                             detail::Quote(s.notif.icon_path) + "}";
            out.list.push_back(a);
        }
    }

    // ---- 复制 / 截图：用户明确按了 Ctrl+C，值得给一次即时反馈 ----
    if (s.clipboard.seq != 0 && s.clipboard.seq != base.clip_seq && !s.clipboard.kind.empty()) {
        RuleInput input;
        input.kind = "clipboard";
        input.app_id = s.clipboard.app_id;
        input.app_label = s.clipboard.app_label;
        input.text = s.clipboard.text;
        input.minute_of_day = minute_of_day;
        if (detail::Admit(cfg, input, out)) {
            Activity a;
            a.id = "live-clipboard";
            a.kind = "clipboard";
            a.title = detail::ClipTitle(s.clipboard);
            a.relevance_score = 68.0;
            a.created_at_ms = now_ms;
            a.stale_at_ms = now_ms + 3200;

            // 疑似密码/密钥：只报「复制了」，正文一律不上屏。
            std::string preview = s.clipboard.text;
            bool secret = false;
            if (cfg.clipboard.hide_secrets && s.clipboard.kind == "text" &&
                LooksLikeSecret(preview)) {
                preview = "疑似敏感内容，已隐藏";
                secret = true;
            } else if (static_cast<int>(preview.size()) > cfg.clipboard.max_chars) {
                preview = preview.substr(0, static_cast<std::size_t>(cfg.clipboard.max_chars)) + "…";
            }

            a.content_json = "{\"text\":" + detail::Quote(preview) + ",\"clip_kind\":" +
                             detail::Quote(s.clipboard.kind) + ",\"app\":" +
                             detail::Quote(s.clipboard.app_label) + ",\"app_path\":" +
                             detail::Quote(s.clipboard.app_path) + ",\"count\":" +
                             std::to_string(s.clipboard.item_count) + ",\"w\":" +
                             std::to_string(s.clipboard.width) + ",\"h\":" +
                             std::to_string(s.clipboard.height) + ",\"secret\":" +
                             (secret ? "true" : "false") + "}";
            out.list.push_back(a);
        }
    }

    // ---- 文件落盘：8 秒内算「刚完成」 ----
    if (!s.file.name.empty() && s.file.age_ms >= 0 && s.file.age_ms < 8000) {
        RuleInput input;
        input.kind = "file";
        input.file_name = s.file.name;
        input.minute_of_day = minute_of_day;
        if (detail::Admit(cfg, input, out)) {
            Activity a;
            a.id = "live-file";
            a.kind = "file";
            a.title = s.file.name;
            a.relevance_score = 65.0;
            a.created_at_ms = now_ms;
            a.stale_at_ms = now_ms + 8000;
            a.content_json = "{\"name\":" + detail::Quote(s.file.name) + ",\"ext\":" +
                             detail::Quote(s.file.ext) + ",\"event\":\"saved\"}";
            out.list.push_back(a);
        }
    }

    // ---- 设备接入（U 盘 / 移动硬盘）----
    if (s.device.seq != 0 && s.device.seq != base.device_seq && !s.device.label.empty()) {
        RuleInput input;
        input.kind = "device";
        input.minute_of_day = minute_of_day;
        if (detail::Admit(cfg, input, out)) {
            Activity a;
            a.id = "live-device";
            a.kind = "device";
            a.title = s.device.label;
            a.relevance_score = 62.0;
            a.created_at_ms = now_ms;
            a.stale_at_ms = now_ms + 4500;
            a.content_json = "{\"label\":" + detail::Quote(s.device.label) + "}";
            out.list.push_back(a);
        }
    }

    // ---- 低电量：低于 20% 且未充电 ----
    if (s.power.has_battery && !s.power.ac_online && s.power.pct <= 20) {
        RuleInput input;
        input.kind = "power";
        input.minute_of_day = minute_of_day;
        if (detail::Admit(cfg, input, out)) {
            Activity a;
            a.id = "live-battery";
            a.kind = "power";
            a.title = "电量低";
            a.relevance_score = 70.0;
            a.created_at_ms = now_ms;
            a.content_json = "{\"percent\":" + std::to_string(s.power.pct) +
                             ",\"charging\":false,\"sub\":\"low\"}";
            out.list.push_back(a);
        }
    }

    // ---- 电源插拔：只在「状态翻转」时提示 ----
    if (s.power.known && base.ac_known && s.power.ac_online != base.ac_online) {
        RuleInput input;
        input.kind = "power";
        input.minute_of_day = minute_of_day;
        if (detail::Admit(cfg, input, out)) {
            Activity a;
            a.id = "live-power";
            a.kind = "power";
            a.title = s.power.ac_online ? "已接通电源" : "已断开电源";
            a.relevance_score = 58.0;
            a.created_at_ms = now_ms;
            a.stale_at_ms = now_ms + 4000;
            a.content_json = std::string("{\"percent\":") + std::to_string(s.power.pct) +
                             ",\"charging\":" + (s.power.ac_online ? "true" : "false") +
                             ",\"sub\":\"plug\"}";
            out.list.push_back(a);
        }
    }

    // ---- 音量 / 静音：变化后 2.5 秒短提示 ----
    if (s.volume_known && base.vol_pct >= 0 &&
        (s.volume_pct != base.vol_pct || s.muted != base.muted)) {
        RuleInput input;
        input.kind = "volume";
        input.minute_of_day = minute_of_day;
        if (detail::Admit(cfg, input, out)) {
            Activity a;
            a.id = "live-volume";
            a.kind = "volume";
            a.title = s.muted ? "已静音" : "音量 " + std::to_string(s.volume_pct) + "%";
            a.relevance_score = 30.0;
            a.created_at_ms = now_ms;
            a.stale_at_ms = now_ms + 2500;
            a.content_json = "{\"percent\":" + std::to_string(s.volume_pct) + ",\"muted\":" +
                             (s.muted ? "true" : "false") + "}";
            out.list.push_back(a);
        }
    }

    // ---- 基线推进：无条件写入，保证「同一次变化只产出一次活动」----
    out.next = base;
    if (s.notif.seq != 0) out.next.notif_seq = s.notif.seq;
    if (s.clipboard.seq != 0) out.next.clip_seq = s.clipboard.seq;
    if (s.device.seq != 0) out.next.device_seq = s.device.seq;
    if (s.power.known) {
        out.next.ac_online = s.power.ac_online;
        out.next.ac_known = true;
    }
    if (!s.volume_known) {
        // 音频端点还没就绪：保持基线不动，等第一次读到真实值再建立。
    } else if (base.vol_pct < 0) {
        out.next.vol_pct = s.volume_pct;
        out.next.muted = s.muted;
        out.next.vol_change_ms = -1;
    } else if (s.volume_pct != base.vol_pct || s.muted != base.muted) {
        out.next.vol_pct = s.volume_pct;
        out.next.muted = s.muted;
        out.next.vol_change_ms = now_ms;
    }

    return out;
}

}  // namespace diw

#endif  // DIW_SRC_CORE_LIVE_MAPPING_HPP
