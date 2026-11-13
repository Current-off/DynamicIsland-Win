// DynamicIsland-Win / 桌面胶囊可执行程序入口（Win32 + GDI+，GUI 子系统）
//
// 这一层只做四件事：
//   1. 把采集到的系统状态交给核心域映射成实时活动（core/live_mapping.hpp）
//   2. 按用户规则过滤（core/rules.hpp，由 rules_io.hpp 提供热加载）
//   3. 把核心域输出变成几何（renderer.hpp）
//   4. 把几何画到桌面（layered_surface.hpp）
// 所有业务判定都在 core/，渲染层不含任何调度逻辑。
//
// 运行方式：
//   diw_overlay.exe                        正常跑：真实系统事件 + 规则过滤
//   diw_overlay.exe --duration=20000       20 秒后自动退出（便于脚本化验证）
//   diw_overlay.exe --snapshot=out.png --snapshot-at=4000
//   diw_overlay.exe --grab=desktop.png --snapshot-at=4000   从屏幕 DC 抓取，证明窗口在桌面上
//   diw_overlay.exe --settings             打开设置窗口
//   diw_overlay.exe --settings --shot=ui.png   打开设置窗口并截图后关闭（视觉回归用）
//   diw_overlay.exe --script               回到内置剧本（仅用于视觉回归）
//   右键胶囊 → 菜单（设置 / 暂停 / 退出）；左键 → 展开收起
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// 必须在 windows.h 之前定义，否则 min/max 宏会破坏 core 里的 std::min/std::max。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbt.h>
#include <shellapi.h>
// 只用到 MINIDUMP_TYPE 等类型定义；MiniDumpWriteDump 走 GetProcAddress 动态解析，
// 因此不需要链接 dbghelp.lib（见 CrashFilter）。
#include <dbghelp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "../core/activity.hpp"
#include "../core/live_mapping.hpp"
#include "backdrop.hpp"
#include "layered_surface.hpp"
#include "hanzi.hpp"
#include "ipc_server.hpp"
#include "live_sources.hpp"
#include "lyrics.hpp"
#include "renderer.hpp"
#include "rules_io.hpp"
#include "settings_ui.hpp"

#include <shellapi.h>       // ShellExecuteW
#include <shobjidl_core.h>  // IApplicationActivationManager：按 AUMID 激活应用

namespace {

using diw::Activity;
using diw::ActivityRuntime;
using diw::CapsuleEvent;
using diw::CapsuleState;
using diw::Frame;
using diw::ScheduledActivity;
using diw::win32::CapsuleRenderer;
using diw::win32::CapsuleView;
using diw::win32::GdiPlusSession;
using diw::win32::GlassParams;
using diw::win32::LayeredSurface;
using diw::win32::Layout;
using diw::win32::OverviewItem;
using diw::win32::ParseProgress;
using diw::win32::Widen;
using diw::win32::Backdrop;

using Clock = std::chrono::steady_clock;

bool g_quit = false;
bool g_click = false;
POINT g_click_pt = {-1, -1};  // WM_LBUTTONUP 的客户区坐标（一览按岛命中测试用）
bool g_overview_req = false;  // 双击：切换一览模式
bool g_suppress_click = false;  // 双击序列的第二击 WM_LBUTTONUP 不再当单击
bool g_menu = false;
bool g_launch_req = false;  // 展开态点了「图标 + 标题」= 打开对应应用
// 点的是第几个小圆（-1 = 没点）。主循环用它查 detached_ids 决定展开哪一件。
int g_detach_click = -1;
// 显示器拓扑 / 每显示器 DPI 变了（拔插屏幕、改缩放、窗口被挪到别的屏）：
// 主循环重算 scale / 工作区 / 玻璃捕获矩形，否则窗口还按旧坐标 present，
// 玻璃也可能采样到已经不存在的屏幕区域。
bool g_display_changed = false;
// 用户又双击了一次 exe：已有实例收到这条消息后把胶囊亮出来。
// 目的不是「防重复」本身，而是解决一个更实际的问题：
// 胶囊闲时 alpha 全 0，鼠标点不到，也没有托盘图标 —— 用户既看不到它
// 在不在跑，也找不到退出的入口。再点一次 exe 就让它现身，是最短的路径。
bool g_presence = false;
constexpr UINT kMsgPresence = WM_APP + 2;
constexpr UINT kMsgTray = WM_APP + 3;
// Explorer 重启后所有托盘图标都会消失。系统会给每个顶层窗口广播一条
// "TaskbarCreated" 注册消息，收到就必须重新 NIM_ADD，否则用户会以为
// 程序没在跑（图标没了、胶囊又可能正处于休眠态）。
UINT g_taskbar_created = 0;
// 定义在文件下方的托盘段；窗口过程要先用（TaskbarCreated 广播与托盘点击）。
bool TrayAdd(HWND hwnd);

// ---- 展开态交互状态（全部只在窗口线程读写，无需加锁）----
struct BoxD {
    double x = 0, y = 0, w = 0, h = 0;
    bool Contains(int px, int py) const {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};
inline BoxD ToBox(const Gdiplus::RectF& r) {
    return BoxD{r.X, r.Y, r.Width, r.Height};
}
inline double ProgressAtX(const BoxD& bar, int x) {
    if (bar.w <= 0.0) return 0.0;
    return std::clamp((x - bar.x) / bar.w, 0.0, 1.0);
}
struct HitState {
    BoxD close, play, progress, app;
    // 主胶囊右侧的每个小圆。以前没有这一项，点小圆会被当成「点了胶囊」，
    // 于是展开的是主活动 —— 用户报的「点小的却路由到大的」就是这个。
    std::vector<BoxD> detached;
    bool expanded = false;
    // 没有时长的媒体（直播、部分视频播放器）根本不显示进度条，
    // 那就也不该允许拖动 —— 否则拖的时候会凭空冒出一根条，松手又消失。
    bool progress_draggable = false;
};
HitState g_hit;               // 由主循环每帧按真实胶囊几何刷新
bool g_dragging = false;      // 正在拖进度条
double g_drag_progress = -1.0;  // 拖动中的预览进度（-1 = 没在拖）
bool g_seek_req = false;
double g_seek_progress = 0.0;
bool g_play_req = false;
bool g_close_req = false;
POINT g_press_pt = {-1, -1};
bool g_press_in_button = false;

// 窗口线程与采集线程之间唯一的共享入口：WM_DEVICECHANGE 是异步广播，
// 收到时直接推进采集层的设备序号，主循环下一帧就能读到。
diw::live::LiveSources* g_sources = nullptr;

constexpr int kMenuSettings = 3001;
constexpr int kMenuPause = 3002;
constexpr int kMenuQuit = 3003;
constexpr int kMenuOverview = 3004;

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), &out[0], need, nullptr, nullptr);
    return out;
}

std::wstring ExeDirectoryWide() {
    wchar_t buf[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const std::wstring path(buf);
    const auto pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::string ExeDirectory() { return Narrow(ExeDirectoryWide()); }

std::string Timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                  tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

// 免打扰判定要的是「墙上时间」，不是程序运行毫秒，所以单独取一次。
int MinuteOfDay() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return static_cast<int>(st.wHour) * 60 + static_cast<int>(st.wMinute);
}

const char* StateName(CapsuleState s) {
    switch (s) {
        case CapsuleState::Idle: return "Idle";
        case CapsuleState::Compact: return "Compact";
        case CapsuleState::Minimal: return "Minimal";
        case CapsuleState::Expanded: return "Expanded";
        case CapsuleState::Dismissed: return "Dismissed";
    }
    return "?";
}

struct Options {
    long long duration_ms = 0;
    // 状态行默认关闭：它是诊断信息（state=Compact · vis 1 …），
    // 挂在桌面上像调试转储，不像产品。要看时加 --hud。
    bool hud = false;
    // 默认走真实数据（媒体 / 通知 / 复制 / 文件 / 电源 / 音量 / 设备 / 全屏）。
    // 剧本模式降级为显式选项 --script，只用于视觉回归和演示。
    bool scripted = false;
    bool settings = false;
    bool settings_selftest = false;
    long long snapshot_at_ms = -1;
    std::string snapshot_path;
    std::string grab_path;      // 屏幕抓取：从桌面 DC BitBlt，用于证明胶囊真的在桌面上
    std::string settings_shot;  // 设置窗口截图
    std::string force_cover;    // 强制指定封面图（视觉验证用，不参与真实链路）
    long long expand_at_ms = -1;  // 到点自动展开主活动（视觉回归用，等价于用户左键点一下）
    long long overview_at_ms = -1;  // 到点自动切一览（视觉回归用，等价于用户双击一下）
    // ---- 无人值守回归注入（不传则完全不生效）----
    long long click_at_ms = -1;      // 到点模拟一次左键点击
    int click_island = 0;            // 点一览里第几个岛（按当前 scale 换算成客户区坐标）
    int click_detached = -1;         // 点主胶囊旁边第几个小圆（>=0 才生效）
    std::string browser_brand;       // 调试：假装某个浏览器在前台（截图回归用）
    int browser_fake_dl = 0;         // 调试：伪造几条下载，验证「有下载」的布局
    int opacity = -1;                // 调试：覆盖透明度档位（-1 = 用配置）
    int anchor = -1;                 // 调试：覆盖位置档位（-1 = 用配置）
    std::string fake_meeting;        // 调试：伪造一个进行中的会议
    std::string fake_editor;         // 调试：伪造编辑器状态，格式 "exe|窗口标题"
    long long display_at_ms = -1;    // 到点模拟一次 WM_DISPLAYCHANGE
    std::string overview_launch;     // 覆盖一览项的拉起目标，验证 LaunchApp 全链路
    long long mouse_at_ms = -1;      // 到点模拟一次真实鼠标序列（展开态按钮 / 拖进度）
    std::string mouse_target;        // close | play | progress
    double mouse_progress = 0.42;   // progress 拖到哪个位置
    std::string lyric_lrc_path;      // 本地 LRC fixture：确定性歌词回归
    long long lyric_pos_ms = -1;     // fixture 模式下注入播放位置（<0 = 用运行时钟）
    long long seek_end_ms = -1;      // 剧本模式没有真实时长，注入一个用于验证 seek 换算
    bool no_duration = false;        // 强制「无时长」：验证无进度条时不可拖
    long long auto_sleep_after_ms = -1;  // 覆盖 auto_sleep_sec（回归：几秒内就能睡着）
    std::string launch_test;         // 启动时直接试一次 LaunchApp，验证目标能否拉起
    bool launch_dry_run = false;     // 只解析不启动（验证用，避免真的弹出应用）
    // Agent 会话文件的假 home：拿一份固定 fixture 验证「有任务清单的展开态」，
    // 不必真去跑一个会写 TodoWrite 的会话。
    std::wstring agent_home;
    std::string log_path;
};

Options ParseArgs() {
    Options opt;
    opt.log_path = ExeDirectory() + "\\diw-overlay.log";
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) return opt;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&a](const wchar_t* key) -> std::wstring {
            const std::wstring k(key);
            return a.size() > k.size() ? a.substr(k.size()) : std::wstring();
        };
        if (a.rfind(L"--duration=", 0) == 0) {
            opt.duration_ms = _wtoi64(value(L"--duration=").c_str());
        } else if (a.rfind(L"--snapshot=", 0) == 0) {
            opt.snapshot_path = Narrow(value(L"--snapshot="));
        } else if (a.rfind(L"--snapshot-at=", 0) == 0) {
            opt.snapshot_at_ms = _wtoi64(value(L"--snapshot-at=").c_str());
        } else if (a.rfind(L"--grab=", 0) == 0) {
            opt.grab_path = Narrow(value(L"--grab="));
        } else if (a.rfind(L"--cover=", 0) == 0) {
            opt.force_cover = Narrow(value(L"--cover="));
        } else if (a.rfind(L"--agent-home=", 0) == 0) {
            opt.agent_home = value(L"--agent-home=");
        } else if (a.rfind(L"--expand-at=", 0) == 0) {
            opt.expand_at_ms = _wtoi64(value(L"--expand-at=").c_str());
        } else if (a.rfind(L"--overview-at=", 0) == 0) {
            opt.overview_at_ms = _wtoi64(value(L"--overview-at=").c_str());
        } else if (a.rfind(L"--click-at=", 0) == 0) {
            opt.click_at_ms = _wtoi64(value(L"--click-at=").c_str());
        } else if (a.rfind(L"--click-island=", 0) == 0) {
            opt.click_island = _wtoi(value(L"--click-island=").c_str());
        } else if (a.rfind(L"--click-detached=", 0) == 0) {
            opt.click_detached = _wtoi(value(L"--click-detached=").c_str());
        } else if (a.rfind(L"--browser-brand=", 0) == 0) {
            opt.browser_brand = Narrow(value(L"--browser-brand="));
        } else if (a.rfind(L"--browser-fake-dl=", 0) == 0) {
            opt.browser_fake_dl = _wtoi(value(L"--browser-fake-dl=").c_str());
        } else if (a.rfind(L"--opacity=", 0) == 0) {
            opt.opacity = _wtoi(value(L"--opacity=").c_str());
        } else if (a.rfind(L"--anchor=", 0) == 0) {
            opt.anchor = _wtoi(value(L"--anchor=").c_str());
        } else if (a.rfind(L"--meeting=", 0) == 0) {
            opt.fake_meeting = Narrow(value(L"--meeting="));
        } else if (a.rfind(L"--editor=", 0) == 0) {
            opt.fake_editor = Narrow(value(L"--editor="));
        } else if (a.rfind(L"--display-at=", 0) == 0) {
            opt.display_at_ms = _wtoi64(value(L"--display-at=").c_str());
        } else if (a.rfind(L"--overview-launch=", 0) == 0) {
            opt.overview_launch = Narrow(value(L"--overview-launch="));
        } else if (a.rfind(L"--mouse-at=", 0) == 0) {
            opt.mouse_at_ms = _wtoi64(value(L"--mouse-at=").c_str());
        } else if (a.rfind(L"--mouse=", 0) == 0) {
            opt.mouse_target = Narrow(value(L"--mouse="));
        } else if (a.rfind(L"--mouse-progress=", 0) == 0) {
            opt.mouse_progress = _wtof(value(L"--mouse-progress=").c_str());
        } else if (a.rfind(L"--lyric-lrc=", 0) == 0) {
            opt.lyric_lrc_path = Narrow(value(L"--lyric-lrc="));
        } else if (a.rfind(L"--lyric-pos-ms=", 0) == 0) {
            opt.lyric_pos_ms = _wtoi64(value(L"--lyric-pos-ms=").c_str());
        } else if (a.rfind(L"--seek-end-ms=", 0) == 0) {
            opt.seek_end_ms = _wtoi64(value(L"--seek-end-ms=").c_str());
        } else if (a.rfind(L"--shot=", 0) == 0) {
            opt.settings_shot = Narrow(value(L"--shot="));
        } else if (a.rfind(L"--log=", 0) == 0) {
            opt.log_path = Narrow(value(L"--log="));
        } else if (a == L"--no-hud") {
            opt.hud = false;
        } else if (a == L"--hud") {
            opt.hud = true;
        } else if (a == L"--no-duration") {
            opt.no_duration = true;  // 回归用：模拟直播 / B 站这种没有总时长的源
        } else if (a == L"--launch-dry-run") {
            opt.launch_dry_run = true;
        } else if (a.rfind(L"--launch-test=", 0) == 0) {
            opt.launch_test = Narrow(value(L"--launch-test="));
        } else if (a.rfind(L"--sleep-after-ms=", 0) == 0) {
            // 回归用：把「多久没交互就睡」压到几秒，否则要等一分钟。
            opt.auto_sleep_after_ms = _wtoi64(value(L"--sleep-after-ms=").c_str());
        } else if (a == L"--script") {
            opt.scripted = true;   // 显式回到剧本模式（视觉回归用）
        } else if (a == L"--no-script") {
            opt.scripted = false;  // 兼容旧参数
        } else if (a == L"--settings") {
            opt.settings = true;
        } else if (a == L"--selftest") {
            opt.settings = true;
            opt.settings_selftest = true;
        }
    }
    ::LocalFree(argv);
    return opt;
}

void EnableDpiAwareness() {
    using SetCtx = BOOL(WINAPI*)(HANDLE);
    auto fn = (SetCtx)::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (fn != nullptr) {
        fn((HANDLE)(static_cast<intptr_t>(-4)));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    } else {
        ::SetProcessDPIAware();
    }
}

double WindowScale(HWND hwnd) {
    using GetDpi = UINT(WINAPI*)(HWND);
    auto fn = (GetDpi)::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    const UINT dpi = (fn != nullptr && hwnd != nullptr) ? fn(hwnd) : 96u;
    return static_cast<double>(dpi == 0 ? 96u : dpi) / 96.0;
}

// 卷设备到达：DEV_BROADCAST_VOLUME 的 unitmask 直接给出盘符位，不需要 SetupAPI。
void NoteVolumeArrival(LPARAM lparam) {
    if (g_sources == nullptr || lparam == 0) return;
    auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lparam);
    if (hdr == nullptr || hdr->dbch_devicetype != DBT_DEVTYP_VOLUME) return;
    auto* volume = reinterpret_cast<DEV_BROADCAST_VOLUME*>(hdr);
    const DWORD mask = volume->dbcv_unitmask;
    for (int bit = 0; bit < 26; ++bit) {
        if ((mask & (1UL << bit)) != 0) {
            char label[32];
            std::snprintf(label, sizeof(label), "%c: 已接入", static_cast<char>('A' + bit));
            g_sources->PushDeviceEvent(label);
            break;
        }
    }
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // Explorer 重启的广播：必须在 switch 之前判断，它是运行时注册的消息号。
    if (g_taskbar_created != 0 && msg == g_taskbar_created) {
        TrayAdd(hwnd);
        return 0;
    }
    switch (msg) {
        case WM_LBUTTONDOWN: {
            // 按下就要判定命中：进度条要立刻进入拖动并捕获鼠标，
            // 否则快速拖拽会在窗口边缘丢失 MOUSEMOVE / LBUTTONUP。
            const POINT pt = {static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            g_press_pt = pt;
            g_press_in_button = false;
            if (g_hit.expanded) {
                if (g_hit.progress_draggable && g_hit.progress.Contains(pt.x, pt.y)) {
                    g_dragging = true;
                    g_drag_progress = ProgressAtX(g_hit.progress, pt.x);
                    ::SetCapture(hwnd);
                } else if (g_hit.close.Contains(pt.x, pt.y) || g_hit.play.Contains(pt.x, pt.y) ||
                           g_hit.app.Contains(pt.x, pt.y)) {
                    g_press_in_button = true;
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g_dragging) {
                const POINT pt = {static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
                g_drag_progress = ProgressAtX(g_hit.progress, pt.x);
            }
            return 0;
        case WM_LBUTTONUP: {
            // 客户区坐标必须在这里取：主循环消费 g_click 时窗口可能已被
            // present 挪动过，GetCursorPos 换算回来会错位。lparam 是点击
            // 瞬间的坐标，short 强转处理多显示器负坐标。
            const POINT pt = {static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            g_click_pt = pt;
            if (g_dragging) {
                g_dragging = false;
                ::ReleaseCapture();
                g_seek_progress = ProgressAtX(g_hit.progress, pt.x);
                g_seek_req = true;
                g_drag_progress = -1.0;
                return 0;
            }
            if (g_hit.expanded && g_press_in_button) {
                if (g_hit.close.Contains(pt.x, pt.y)) g_close_req = true;
                else if (g_hit.play.Contains(pt.x, pt.y)) g_play_req = true;
                else if (g_hit.app.Contains(pt.x, pt.y)) g_launch_req = true;
                g_press_in_button = false;
                return 0;  // 按钮点击不产生普通单击（否则会顺手收起/展开）
            }
            // 小圆优先：它是紧凑态里唯一「点谁就该是谁」的元素。
            if (!g_hit.expanded && !g_hit.detached.empty()) {
                for (std::size_t i = 0; i < g_hit.detached.size(); ++i) {
                    if (g_hit.detached[i].Contains(pt.x, pt.y)) {
                        g_detach_click = static_cast<int>(i);
                        g_suppress_click = false;
                        return 0;  // 不算普通单击：不能顺带展开主活动
                    }
                }
            }
            if (g_suppress_click) {
                g_suppress_click = false;  // 双击的第二击：不重复算单击
            } else {
                g_click = true;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            // 双击的时序是 up(click) → down(dblclk) → up。这里清掉第一击的
            // click（多数时候还没被主循环消费），并压住第二击的 up。
            g_dragging = false;
            g_press_in_button = false;
            g_overview_req = true;
            g_click = false;
            g_suppress_click = true;
            return 0;
        case WM_RBUTTONUP:
            g_menu = true;
            return 0;
        case kMsgPresence:
            g_presence = true;
            return 0;
        case kMsgTray:
            // 左键和右键弹同一个菜单：托盘只有一个入口，不制造第二套交互。
            // 菜单本身按 GetCursorPos 定位，所以直接复用主循环那条路径。
            if (LOWORD(lparam) == WM_LBUTTONUP || LOWORD(lparam) == WM_RBUTTONUP) {
                g_menu = true;
            }
            return 0;
        case WM_DEVICECHANGE:
            if (wparam == DBT_DEVICEARRIVAL) NoteVolumeArrival(lparam);
            return TRUE;
        case WM_DISPLAYCHANGE:
            // 显示器拓扑变了。位置不用管（主循环每帧 present），几何要重算。
            g_display_changed = true;
            return 0;
        case WM_DPICHANGED:
            // 窗口跨到了不同缩放的显示器。系统给的建议矩形对我们无意义
            //（位置由主循环决定），只当作「该重算几何」的信号。
            g_display_changed = true;
            return 0;
        case WM_PAINT:
            ::ValidateRect(hwnd, nullptr);  // 内容由 UpdateLayeredWindow 推送
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

// 点击一览里的岛：拉起 / 前置对应应用。
// 目标有两类：AUMID（媒体的 SMTC 来源、通知的发送方）和完整 exe 路径
// （复制活动的来源应用）。AUMID 优先走 ApplicationActivationManager ——
// 打包应用与在开始菜单注册过 AUMID 的 Win32 应用（QQ/微信/PowerShell…）
// 都能激活，而且「已在运行」时是前置到前台而不是新开实例；失败再回落
// 隐式 AUMID → exe 路径直接 ShellExecute。
// dry_run：只解析「会用什么去启动」，不真的启动 —— 回归验证用。
// resolved：解析结果（exe 路径或 shell: 虚拟路径），写进日志便于定位。
bool LaunchAppImpl(const std::string& target, bool dry_run, std::string* resolved) {
    if (target.empty()) return false;
    const std::wstring w = Widen(target);
    auto shell_open = [&](const std::wstring& path, bool optimistic = false) {
        if (resolved != nullptr) *resolved = Narrow(path);
        if (dry_run) return true;
        const INT_PTR r = reinterpret_cast<INT_PTR>(
            ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (r > 32) return true;
        // shell: 虚拟路径（AppsFolder 里的应用）无法用返回值判断：实测计算器
        // 明明被拉起来了，ShellExecute 仍返回 <=32。这类目标只能乐观处理，
        // 否则每次点 UWP 应用都会记一条假 FAILED，反过来掩盖真问题。
        return optimistic;
    };

    // ① 真实存在的 exe 路径（剪贴板来源应用给的就是完整路径）：直接开。
    //    判断依据是「文件是否存在」，不是「字符串里有没有反斜杠」—— 隐式
    //    AUMID 恰好也带反斜杠（{GUID}\relpath\app.exe），但那不是路径，
    //    直接 ShellExecute 会失败，导致 Chrome / Spotify / foobar 这类未打包
    //    播放器的岛点了没反应。
    if (::GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES) return shell_open(w);

    // ② 只给了进程名（QQ 的 "QQ.exe"、QQ 音乐的 "QQMusic.exe"）：SMTC 和通知
    //    经常只报进程名，而这两条最常用。直接找正在运行的同名进程，取它的
    //    完整路径再 ShellExecute —— 已在跑时 ShellExecute 就是「切到前台」，
    //    也顺带解决了「AUMID 查不到、AppsFolder 里也没有」的经典应用。
    //    这条比后面的 AUMID 路线可靠得多，所以放在前面。
    {
        std::wstring proc = w;
        if (proc.size() > 4 && _wcsicmp(proc.c_str() + proc.size() - 4, L".exe") == 0) {
            proc.resize(proc.size() - 4);
        }
        std::wstring running;
        if (diw::live::RunningExePathByProcessName(proc, &running) &&
            ::GetFileAttributesW(running.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // 有些应用（腾讯视频这类 WebView 壳）在 SMTC 里报的是宿主进程
            // msedgewebview2.exe —— 把用户「打开」到这个进程毫无意义，
            // 还会凭空多开一个渲染进程。这类宿主一律不当跳转目标。
            const std::size_t slash = running.find_last_of(L"\\/");
            const std::wstring base =
                slash == std::wstring::npos ? running : running.substr(slash + 1);
            static const wchar_t* kHosts[] = {L"msedgewebview2.exe", L"applicationframehost.exe",
                                              L"runtimebroker.exe", L"wmiprvse.exe",
                                              L"explorer.exe"};
            bool is_host = false;
            for (const wchar_t* h : kHosts) {
                if (_wcsicmp(base.c_str(), h) == 0) {
                    is_host = true;
                    break;
                }
            }
            if (!is_host) return shell_open(running);
        }
    }

    // ② 隐式 AUMID：系统为未打包应用合成的 {GUID}\相对路径，开始菜单里
    //    往往根本没有对应 .lnk。用已知文件夹 GUID 还原出真实 exe 再开。
    std::wstring exe;
    if (diw::live::ExeFromImplicitAumid(target, &exe) &&
        ::GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return shell_open(exe);
    }

    // ③ 打包应用 AUMID（PFN!AppId）：ApplicationActivationManager 优先，
    //    已在运行时是「前置到前台」而不是新开实例。主线程默认没有 COM
    //    apartment（只有采集线程 init 了 MTA），CoCreateInstance 会直接失败，
    //    表现为「点打包应用的岛毫无反应」。这里按调用临时起一个 STA。
    struct ComApartment {
        bool owned = false;
        ComApartment() {
            // S_OK 与 S_FALSE（本线程已初始化同 apartment）都要配一次 Uninitialize。
            owned = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                         COINIT_DISABLE_OLE1DDE));
        }
        ~ComApartment() {
            if (owned) ::CoUninitialize();
        }
    } apartment;

    // ④ AppsFolder 里找真正的 AppID：覆盖「进程名」「包族名」两类
    //    （QQ 的 AppID 是 "QQ"、B 站是 "BiliBiliPC"，都不是上报的那串）。
    //    **必须放在 apartment 之后**：SHCreateItemFromParsingName 需要 COM，
    //    否则会静默失败，表现为「明明 Get-StartApps 里有，就是解析不到」。
    {
        std::wstring appid;
        if (diw::live::FindAppsFolderAppId(target, &appid)) {
            if (shell_open(std::wstring(L"shell:AppsFolder\\") + appid, true)) return true;
        }
    }

    try {
        auto manager = winrt::create_instance<IApplicationActivationManager>(
            __uuidof(ApplicationActivationManager), CLSCTX_LOCAL_SERVER);
        DWORD pid = 0;
        if (SUCCEEDED(manager->ActivateApplication(w.c_str(), L"", AO_NONE, &pid))) {
            if (resolved != nullptr) *resolved = "AUMID:" + target;
            if (dry_run) return true;
            return true;
        }
    } catch (...) {
        // COM 不可用 / AUMID 未注册：走下面的 Shell 回落
    }
    // ⑤ 兜底：只在 target 看起来**本身就是一个 AppID**（没有点、路径分隔符、
    //    盘符）时才盲目拼一次 shell:AppsFolder。像 "msedgewebview2.exe" 这种
    //    宿主进程名拼上去只会让 Shell 到处乱试，宁可直接报失败。
    const bool looks_like_appid = target.find('.') == std::string::npos &&
                                  target.find('\\') == std::string::npos &&
                                  target.find('/') == std::string::npos &&
                                  target.find(':') == std::string::npos;
    if (!looks_like_appid || dry_run) {
        if (resolved != nullptr) *resolved = "(unresolved)";
        return false;
    }
    return shell_open(std::wstring(L"shell:AppsFolder\\") + w, true);
}

bool LaunchApp(const std::string& target) { return LaunchAppImpl(target, false, nullptr); }

// ---------------------------------------------------------------- Agent 焦点
// 点 agent 岛的标题 = 「带我去看它在干什么」。目标不是「启动那个应用」
// （它已经在跑了），而是把承载它的那个终端 / 编辑器窗口提到前台。
//
// 没有 API 能问「这个会话属于哪个窗口」，所以按窗口标题匹配：
// 终端标题里几乎总是带着项目目录名（`DynamicIsland-Win — claude`），
// 再叠加一层 provider 关键词，足够区分同时开着的几个终端。
struct FocusCandidate {
    HWND hwnd = nullptr;
    int score = 0;
};

// 真正干活的那一层。EnumWindows 的 lpParam 只能给一个指针，跨回调累积结果
// 必须自己搭容器；这里用「文件内静态指针」而不是把 vector 塞进上下文结构，
// 因为只有窗口线程会调它，不存在并发。
std::vector<FocusCandidate>* g_focus_out = nullptr;
HWND g_focus_self = nullptr;
std::string g_focus_project;
std::string g_focus_provider;

BOOL CALLBACK FocusCollectProc(HWND hwnd, LPARAM) {
    if (g_focus_out == nullptr || hwnd == g_focus_self) return TRUE;
    if (::IsWindowVisible(hwnd) == FALSE) return TRUE;
    if (::IsIconic(hwnd) != FALSE) return TRUE;
    wchar_t title[512] = {0};
    const int len = ::GetWindowTextW(hwnd, title, 512);
    if (len <= 0) return TRUE;

    const std::string narrow = Narrow(title);
    const std::string lower = diw::AsciiLower(narrow);
    int score = 0;
    if (!g_focus_project.empty() && diw::ContainsCaseless(lower, diw::AsciiLower(g_focus_project))) {
        score += 4;
    }
    // provider 关键词：Claude Code 的终端标题常带 "claude"，Codex 带 "codex"。
    static const std::pair<const char*, const char*> kHints[] = {
        {"claude", "claude"}, {"codex", "codex"}, {"gemini", "gemini"}, {"workbuddy", "workbuddy"}};
    for (const auto& hint : kHints) {
        if (g_focus_provider == hint.first && diw::ContainsCaseless(lower, hint.second)) {
            score += 2;
            break;
        }
    }
    if (score == 0) return TRUE;
    // 终端类窗口优先：同一个标题可能同时出现在编辑器的标签栏和终端标题里，
    // 用户要的是「能看见 agent 输出的那个窗口」。
    wchar_t cls[128] = {0};
    ::GetClassNameW(hwnd, cls, 128);
    const std::wstring klass(cls);
    if (klass == L"ConsoleWindowClass" || klass == L"CASCADIA_HOSTING_WINDOW_CLASS" ||
        klass == L"mintty" || klass == L"PuTTY") {
        score += 2;
    }
    // 前台窗口天然更可能是「刚才在用的那个」。
    if (hwnd == ::GetForegroundWindow()) score += 1;
    g_focus_out->push_back(FocusCandidate{hwnd, score});
    return TRUE;
}

// 前台锁定：本进程不是前台时 SetForegroundWindow 会被系统拒绝。
// 临时共享输入队列是唯一稳定的绕过方式（不注入、不改注册表）。
bool BringToForeground(HWND target) {
    if (target == nullptr) return false;
    const DWORD fg_tid = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
    const DWORD self_tid = ::GetCurrentThreadId();
    if (fg_tid != 0 && fg_tid != self_tid) ::AttachThreadInput(self_tid, fg_tid, TRUE);
    ::AllowSetForegroundWindow(ASFW_ANY);
    ::ShowWindow(target, SW_RESTORE);
    const BOOL ok = ::SetForegroundWindow(target);
    ::BringWindowToTop(target);
    if (fg_tid != 0 && fg_tid != self_tid) ::AttachThreadInput(self_tid, fg_tid, FALSE);
    return ok != FALSE;
}

bool FocusAgentWindow(HWND self, const std::string& project, const std::string& provider) {
    std::vector<FocusCandidate> found;
    g_focus_out = &found;
    g_focus_self = self;
    g_focus_project = project;
    g_focus_provider = provider;
    ::EnumWindows(FocusCollectProc, 0);
    g_focus_out = nullptr;
    if (found.empty()) return false;
    std::sort(found.begin(), found.end(),
              [](const FocusCandidate& a, const FocusCandidate& b) { return a.score > b.score; });
    return BringToForeground(found.front().hwnd);
}

// 编辑器版：按「进程 exe 名」找窗口。
// 这是三种聚焦里最准的一个 —— 编辑器进程的 exe 是唯一的，不像浏览器要猜品牌后缀、
// 也不像 agent 要猜项目名。同一个编辑器开了多个窗口时，取第一个可见的（用户
// 要的是"回到那个编辑器"，不是"回到某个特定的标签页"）。
struct EditorFocusCtx {
    const std::string* exe = nullptr;
    HWND hit = nullptr;
};

BOOL CALLBACK EditorFocusProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<EditorFocusCtx*>(param);
    if (ctx == nullptr || ctx->exe == nullptr || ctx->exe->empty()) return FALSE;
    if (ctx->hit != nullptr) return FALSE;  // 已经找到了
    if (::IsWindowVisible(hwnd) == FALSE) return TRUE;
    wchar_t title[512] = {0};
    if (::GetWindowTextW(hwnd, title, 512) <= 0) return TRUE;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) return TRUE;
    wchar_t path[1024] = {0};
    DWORD size = 1024;
    bool match = false;
    if (::QueryFullProcessImageNameW(proc, 0, path, &size) != 0) {
        std::wstring full(path, size);
        const std::size_t slash = full.find_last_of(L"\\/");
        std::wstring base = slash == std::wstring::npos ? full : full.substr(slash + 1);
        if (base.size() > 4 && base.compare(base.size() - 4, 4, L".exe") == 0) {
            base.resize(base.size() - 4);
        }
        match = diw::AsciiLower(Narrow(base)) == diw::AsciiLower(*ctx->exe);
    }
    ::CloseHandle(proc);
    if (match) {
        ctx->hit = hwnd;
        return FALSE;  // 停
    }
    return TRUE;
}

bool FocusEditorWindow(HWND self, const std::string& exe_stem) {
    (void)self;
    if (exe_stem.empty()) return false;
    EditorFocusCtx ctx;
    ctx.exe = &exe_stem;
    ::EnumWindows(EditorFocusProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.hit == nullptr) return false;
    return BringToForeground(ctx.hit);
}

// 浏览器版：按窗口标题里的品牌后缀找窗口。
// 不用「标签页标题」匹配 —— 它每个页面都不同，而我们要找的是「那个浏览器」。
bool FocusBrowserWindow(HWND self, const std::string& label) {
    if (label.empty()) return false;
    std::vector<FocusCandidate> found;
    g_focus_out = &found;
    g_focus_self = self;
    g_focus_project = label;  // 复用同一个匹配字段：标题里一定带品牌名
    g_focus_provider.clear();
    ::EnumWindows(FocusCollectProc, 0);
    g_focus_out = nullptr;
    if (found.empty()) return false;
    std::sort(found.begin(), found.end(),
              [](const FocusCandidate& a, const FocusCandidate& b) { return a.score > b.score; });
    return BringToForeground(found.front().hwnd);
}

void OpenFolderInExplorer(const std::string& folder) {
    if (folder.empty()) return;
    ::ShellExecuteW(nullptr, L"open", Widen(folder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// 一览命中测试：客户区坐标 → 岛下标（-1 = 没点在任何岛上）。
// 岛画在画布左边距起、等宽等距横排（与 renderer Draw 的一览分支同一套
// 几何）。展开动画只改画布宽度，岛在画布内的槽位固定，所以命中矩形
// 与动画进度无关，可以静态计算。
int OverviewHitIndex(POINT pt, double scale) {
    const double m = Layout::kMargin * scale;
    const double iw = Layout::kCompactW * scale;
    const double ih = Layout::kCompactH * scale;
    const double step = (Layout::kCompactW + Layout::kGap) * scale;
    if (pt.y < m || pt.y > m + ih) return -1;
    for (int i = 0; i < Layout::kOverviewMax; ++i) {
        const double x = m + i * step;
        if (pt.x >= x && pt.x <= x + iw) return i;
    }
    return -1;
}

// 右键菜单。返回命令 id，0 表示点了空白处。
int ShowContextMenu(HWND hwnd, bool paused, int visible_count, bool overview) {
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) return 0;
    ::AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置…");
    // 这一项永远在：只有 1 件时也给出「展开」入口，否则用户完全没有
    // 「怎么把岛打开 / 怎么看到多任务」的线索（菜单里突然少一项只会更困惑）。
    {
        std::wstring label;
        if (overview) {
            label = L"收起全部任务";
        } else if (visible_count >= 2) {
            label = L"展开全部任务（" + std::to_wstring(visible_count) + L" 个）";
        } else if (visible_count == 1) {
            label = L"展开当前任务";
        } else {
            label = L"暂无任务";
        }
        ::AppendMenuW(menu, MF_STRING | (visible_count == 0 ? MF_GRAYED : 0), kMenuOverview,
                      label.c_str());
    }
    ::AppendMenuW(menu, MF_STRING, kMenuPause, paused ? L"恢复显示" : L"暂停 30 分钟");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuQuit, L"退出");

    POINT pt{};
    ::GetCursorPos(&pt);
    // TrackPopupMenu 要求先设置前台窗口，否则点别处菜单不会消失。
    ::SetForegroundWindow(hwnd);
    const int cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
    return cmd;
}

// ---------- 托盘图标 ----------
// 胶囊是一个置顶的透明窗口：没有任务栏按钮，也不进 Alt-Tab。没有托盘时，
// 用户唯一能让它停下来的方式就是右键那颗胶囊 —— 而它可能正处于休眠（只剩
// 一条极淡的细把手）甚至全屏避让（完全不显示）的状态，等于无路可走。
//
// 图标用系统通用图标兜底：仓库里没有 .ico 资源，宁可用一个不漂亮的图标，
// 也好过注册不上 —— 这只影响观感，不影响「能不能退出/进设置」。
constexpr UINT kTrayId = 1;

NOTIFYICONDATAW MakeTrayData(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kMsgTray;
    nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    ::lstrcpynW(nid.szTip, L"DynamicIsland-Win（左键/右键打开菜单）",
                static_cast<int>(std::size(nid.szTip)));
    return nid;
}

bool TrayAdd(HWND hwnd) {
    NOTIFYICONDATAW nid = MakeTrayData(hwnd);
    return ::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void TrayRemove(HWND hwnd) {
    NOTIFYICONDATAW nid = MakeTrayData(hwnd);
    nid.uFlags = 0;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ---------- 场景脚本：驱动真实 ActivityRuntime，而不是直接摆弄动画 ----------

enum class Action {
    StartMedia,
    StartCalendar,
    StartDownload,
    EndCalendar,
    BridgeOffline,
    BridgeOnline,
    SuspendOn,
    SuspendOff,
    ExpandOn,
    ExpandOff,
    EndAll,
    Reset
};

struct ScriptEvent {
    double at_sec;
    Action action;
};

const std::vector<ScriptEvent>& Script() {
    static const std::vector<ScriptEvent> kScript = {
        {0.6, Action::StartMedia},
        {2.6, Action::StartCalendar},
        {3.6, Action::ExpandOn},
        {5.8, Action::ExpandOff},
        {7.0, Action::StartDownload},
        {8.6, Action::BridgeOffline},
        {10.4, Action::BridgeOnline},
        {11.6, Action::EndCalendar},
        {13.2, Action::SuspendOn},
        {14.4, Action::SuspendOff},
        {15.6, Action::EndAll},
        {17.6, Action::Reset},
    };
    return kScript;
}

constexpr double kLoopSeconds = 18.0;
constexpr const char* kMediaId = "media-1";
constexpr const char* kCalendarId = "calendar-1";
constexpr const char* kDownloadId = "download-1";

Activity MakeMedia() {
    Activity a;
    a.id = kMediaId;
    a.kind = "media";
    a.title = "晴天";
    a.relevance_score = 60.0;
    a.content_json = R"({"artist":"周杰伦","album":"叶惠美","playing":true,"progress":0.38})";
    return a;
}

Activity MakeCalendar() {
    Activity a;
    a.id = kCalendarId;
    a.kind = "calendar";
    a.title = "产品站会";
    a.relevance_score = 90.0;
    a.content_json = R"({"start":"15:00","end":"15:30","place":"会议室 A"})";
    return a;
}

Activity MakeDownload(std::int64_t now_ms) {
    Activity a;
    a.id = kDownloadId;
    a.kind = "file";
    a.title = "Windows11_24H2.iso";
    a.relevance_score = 85.0;
    a.created_at_ms = now_ms;
    a.stale_at_ms = now_ms + 5000;  // 5 秒后过期，用于演示 staleDate 自动结束
    a.content_json = R"({"progress":0.62,"speed":"3.1 MB/s","ext":"iso"})";
    return a;
}

struct Animation {
    double w = Layout::kCompactW;
    double h = Layout::kCompactH;
    double detached = 0.0;
    double alpha = 0.0;
    // 速度必须跨帧保留。之前用纯指数逼近（每帧 x += (target-x)*k），
    // 那不是弹簧，是"越来越慢地爬过去"——看起来像长出来，不像甩出来。
    double vw = 0.0;
    double vh = 0.0;
    double vd = 0.0;

    void Reset() {
        w = Layout::kCompactW;
        h = Layout::kCompactH;
        detached = 0.0;
        alpha = 0.0;
        vw = vh = vd = 0.0;
    }

    // 返回值表示「这一帧有没有真的动」：几何（弹簧）与透明度任一项变化即为 true。
    // 主循环用它决定要不要跑 60fps —— 见 frame_budget。
    bool Approach(double target_w, double target_h, double target_detached, double target_alpha, double dt) {
        const double w0 = w;
        const double h0 = h;
        const double d0 = detached;
        const double a0 = alpha;
        // iOS 灵动岛的手感来自「冲过头一点再回落」，ζ = c/(2√k) ≈ 0.68，
        // 过冲约 5%：肉眼能感觉到"弹"，但不会晃。
        constexpr double kStiffness = 420.0;
        constexpr double kDamping = 28.0;

        double remaining = std::clamp(dt, 0.0, 0.12);
        while (remaining > 1e-6) {
            const double step = std::min(remaining, 1.0 / 240.0);  // 子步进：dt 抖动时也不会炸
            Spring(w, vw, target_w, kStiffness, kDamping, step);
            Spring(h, vh, target_h, kStiffness, kDamping, step);
            Spring(detached, vd, target_detached, kStiffness, kDamping, step);
            remaining -= step;
        }

        // 透明度不参与弹簧：过冲会让它冲过 1 再回落，屏幕上表现为闪一下。
        const double k_alpha = 1.0 - std::exp(-dt / 0.05);
        alpha += (target_alpha - alpha) * k_alpha;
        if (std::fabs(alpha - target_alpha) < 0.002) alpha = target_alpha;
        return w != w0 || h != h0 || detached != d0 || alpha != a0;
    }

private:
    static void Spring(double& x, double& v, double target, double k, double c, double dt) {
        const double a = (target - x) * k - v * c;
        v += a * dt;
        x += v * dt;
        // 吸附：残余速度会让胶囊永远在 target 附近抖 0.05px，白白多画无数帧。
        if (std::fabs(x - target) < 0.08 && std::fabs(v) < 2.0) {
            x = target;
            v = 0.0;
        }
    }
};

// 一条坏活动不该让整颗胶囊消失：ActivityStore::Upsert 对空 id / 空 kind /
// stale_at 早于 created_at 会抛异常，而 StartActivity 不吞它 —— 抛到 wWinMain
// 只剩一行 FATAL，用户看到的就是「胶囊莫名不见了」。这里就地兜住、把原因交回
// 调用点记录，代价是跳过这一条活动，而不是整个进程。
bool StartActivitySafe(ActivityRuntime& runtime, const Activity& activity, std::string* error) {
    try {
        runtime.StartActivity(activity);
        return true;
    } catch (const std::exception& e) {
        if (error != nullptr) *error = e.what();
        return false;
    }
}

struct Demo {
    // unique_ptr 而非值成员：ActivityRuntime 内含 const 转移表，不可赋值。
    std::unique_ptr<ActivityRuntime> runtime{std::make_unique<ActivityRuntime>()};
    std::size_t cursor = 0;
    bool bridge_online = true;
    bool suspended = false;
    std::string expand_id;
    bool overview = false;  // 一览：并行任务全部铺成独立岛（双击 / 右键菜单切换）

    void Reset() {
        runtime = std::make_unique<ActivityRuntime>();
        runtime->SetBridgeOnline(true);
        runtime->SetSuspended(false);
        bridge_online = true;
        suspended = false;
        expand_id.clear();
        cursor = 0;
    }

    void Apply(Action action, std::int64_t now_ms) {
        switch (action) {
            case Action::StartMedia: StartActivitySafe(*runtime, MakeMedia(), nullptr); break;
            case Action::StartCalendar: StartActivitySafe(*runtime, MakeCalendar(), nullptr); break;
            case Action::StartDownload:
                StartActivitySafe(*runtime, MakeDownload(now_ms), nullptr);
                break;
            case Action::EndCalendar: runtime->EndActivity(kCalendarId); break;
            case Action::BridgeOffline:
                bridge_online = false;
                runtime->SetBridgeOnline(false);
                break;
            case Action::BridgeOnline:
                bridge_online = true;
                runtime->SetBridgeOnline(true);
                break;
            case Action::SuspendOn:
                suspended = true;
                runtime->SetSuspended(true);
                break;
            case Action::SuspendOff:
                suspended = false;
                runtime->SetSuspended(false);
                break;
            case Action::ExpandOn: expand_id = kCalendarId; break;
            case Action::ExpandOff: expand_id.clear(); break;
            case Action::EndAll:
                runtime->EndActivity(kCalendarId);
                runtime->EndActivity(kMediaId);
                runtime->EndActivity(kDownloadId);
                expand_id.clear();
                break;
            case Action::Reset: Reset(); break;
        }
    }
};

// ---- content_json 轻量读取 ----
// 不引入完整 JSON 解析器：content_json 全部由本项目生成，键名与转义都可控。
// 但必须处理转义，否则歌名里的引号会把副标题截断。
std::string JsonString(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string out;
    for (std::size_t i = pos; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            const char n = json[++i];
            switch (n) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                default: out += n;
            }
            continue;
        }
        if (c == '"') break;
        out += c;
    }
    return out;
}

std::int64_t JsonInt(const std::string& json, const char* key, std::int64_t fallback = 0) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    return std::strtoll(json.c_str() + pos + needle.size(), nullptr, 10);
}

bool JsonBool(const std::string& json, const char* key, bool fallback = false) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    const char* p = json.c_str() + pos + needle.size();
    while (*p == ' ' || *p == '\t') ++p;
    if (std::strncmp(p, "true", 4) == 0) return true;
    if (std::strncmp(p, "false", 5) == 0) return false;
    return fallback;
}

// 毫秒 → m:ss，用于媒体进度
std::string MmSs(std::int64_t ms) {
    const std::int64_t total_s = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(total_s / 60),
                  static_cast<long long>(total_s % 60));
    return buf;
}

// 活动的一行副标题，来自 content_json（真实数据），渲染层不硬编码业务文本。
// 单岛（BuildView）与一览岛共用，保证两种形态下文案一致。
std::string SubtitleOf(const std::string& kind, const std::string& json) {
    if (kind.rfind("media", 0) == 0) {
        const std::string artist = JsonString(json, "artist");
        const std::string album = JsonString(json, "album");
        if (!artist.empty() && !album.empty()) return artist + " · " + album;
        return artist.empty() ? album : artist;
    }
    if (kind.rfind("notification", 0) == 0) {
        const std::string app = JsonString(json, "app");
        const std::string body = JsonString(json, "body");
        return body.empty() ? app : body;
    }
    if (kind.rfind("clipboard", 0) == 0) {
        const std::string app = JsonString(json, "app");
        const std::string text = JsonString(json, "text");
        if (!app.empty() && JsonInt(json, "count") > 1) return app + " · " + text;
        return text;
    }
    if (kind.rfind("file", 0) == 0 || kind.rfind("download", 0) == 0) {
        return "已保存到下载文件夹";
    }
    if (kind.rfind("browser", 0) == 0) {
        // 副标题（一览 / 紧凑态共用）：站名 · 域名，有下载再补一句。
        const std::string site = JsonString(json, "site");
        const std::string label = JsonString(json, "site_label");
        std::string sub = label.empty() ? site : label;
        if (!site.empty() && !label.empty() && label != site) sub += " · " + site;
        const std::string hint = JsonString(json, "dhint");
        if (!hint.empty()) sub += (sub.empty() ? "" : " · ") + ("下载 " + hint);
        return sub;
    }
    if (kind.rfind("device", 0) == 0) return JsonString(json, "label");
    if (kind.rfind("power", 0) == 0) {
        const std::int64_t pct = JsonInt(json, "percent", -1);
        const std::string sub = JsonString(json, "sub");
        return sub == "plug" ? (pct >= 0 ? "当前电量 " + std::to_string(pct) + "%" : "")
                             : "剩余 " + std::to_string(pct) + "% · 建议接通电源";
    }
    if (kind.rfind("volume", 0) == 0) return "音量 " + std::to_string(JsonInt(json, "percent")) + "%";
    if (kind.rfind("calendar", 0) == 0) {
        const std::string start = JsonString(json, "start");
        const std::string end_t = JsonString(json, "end");
        const std::string place = JsonString(json, "place");
        return start + " – " + end_t + (place.empty() ? "" : " · " + place);
    }
    if (kind.rfind("agent", 0) == 0) {
        // 一览里每个 agent 岛只有两行位置：项目名 + 这一句。
        const std::string action = JsonString(json, "action");
        if (!action.empty()) return action;
        const std::string reply = JsonString(json, "reply");
        return reply.empty() ? JsonString(json, "name") : reply;
    }
    return std::string();
}

// 当前「被展开的那一件」。界面上所有针对单个活动的操作（关闭、播放、拖进度、
// 切窗口）都必须用它，而不是 frame.visible.front() —— 否则从一览里点开
// 第二个岛之后，每个按钮都在操作第一个。
const Activity* LeadOf(const Frame& f, const std::string& lead_id) {
    if (f.visible.empty()) return nullptr;
    if (!lead_id.empty()) {
        for (const auto& s : f.visible) {
            if (s.activity.id == lead_id) return &s.activity;
        }
    }
    return &f.visible.front().activity;
}

// 调试：伪造下载条目。真实下载不可控（要等它下、还要刚好在截图时进行到一半），
// 而「有下载」是这块布局里唯一没法靠等就等到的分支。
int g_browser_fake_dl = 0;
// -1 表示「按配置」。两个覆盖值放在文件级全局，是为了让截图回归一次跑遍
// 「三档透明度 × 三个位置」，不用反复改 rules.json（那会污染用户配置）。
int g_force_opacity = -1;
int g_force_anchor = -1;

// 把核心域输出翻译成一次绘制所需的一切（纯数据，便于截图回归）。
CapsuleView BuildView(const Frame& frame, CapsuleState state, bool bridge_online, bool suspended,
                      double wave_phase, bool with_hud, int blocked_count,
                      const std::string& force_cover = std::string(),
                      bool overview = false,
                      bool sleeping = false,
                      bool lyrics_on = false,
                      bool lyric_ready = false,
                      bool simplify = false,
                      const std::string& lead_id = std::string()) {
    CapsuleView view;
    // 一览优先于展开态：两者互斥（一览是「看全部」，展开是「看一个」）。
    // 剧本 / 自动展开动作可能把 runtime 推到 Expanded，若不在这里压住，
    // 下面的 overview 分支会被 !expanded 挡掉，view.items 为空 → 点岛没反应。
    const bool expanded = !overview && !sleeping && state == CapsuleState::Expanded;
    view.expanded = expanded;

    // 休眠：**有岛但让路**，收成一条细条。真正的「没有岛」不走这里 ——
    // 那种情况窗口全透明（上层 hidden），连一条把手都不该画。
    view.sleep_bar = sleeping;
    if (sleeping) {
        view.content_w = Layout::kSleepBarW;
        view.content_h = Layout::kSleepBarH;
        view.kind = "sleep";
        return view;  // 不填任何活动内容：它不是面板，是一条可点的把手
    }
    // 展开的是「哪一件」由 lead_id 决定，不能无脑取 front()：
    // 一览里点第二个岛时，front() 仍然是第一个 —— 用户点 Codex，弹出来的
    // 却是 Claude，这是最容易被当成「点错了」的那种错。
    const ScheduledActivity* lead_ptr = nullptr;
    if (!frame.visible.empty()) {
        lead_ptr = &frame.visible.front();
        if (!lead_id.empty()) {
            for (const auto& s0 : frame.visible) {
                if (s0.activity.id == lead_id) {
                    lead_ptr = &s0;
                    break;
                }
            }
        }
    }
    const bool media_lead = lead_ptr != nullptr && lead_ptr->activity.kind.rfind("media", 0) == 0;
    const bool agent_lead = lead_ptr != nullptr && lead_ptr->activity.kind.rfind("agent", 0) == 0;
    const bool browser_lead = lead_ptr != nullptr && lead_ptr->activity.kind.rfind("browser", 0) == 0;
    const bool editor_lead = lead_ptr != nullptr && lead_ptr->activity.kind.rfind("editor", 0) == 0;
    {
        view.content_w = expanded ? Layout::kExpandedW : Layout::kCompactW;
        // 只有「真有歌词」时才留出三行歌词区。没有歌词（视频 / 纯音乐 / 网络失败）
        // 就把高度收回常规展开态 —— 否则会剩一大片空白加一句「暂无歌词」，
        // 视觉上很诡异，也白白挡住后面的窗口。
        const bool lyric_zone = expanded && media_lead && lyrics_on && lyric_ready;
        view.lyric_active = lyric_zone;
        // 高度按「内容形态」逐层判断。这里原来是嵌套三元，加到第四层时括号
        // 已经数不清了 —— 少一个右括号，编译器只会甩一行谁也看不懂的错。
        view.content_h = Layout::kCompactH;
        if (expanded) {
            if (agent_lead) {
                view.content_h = Layout::kAgentExpandedH;  // 下面按清单行数覆盖
            } else if (browser_lead) {
                view.content_h = Layout::kBrowserExpandedH;  // 下面按下载行数覆盖
            } else if (editor_lead) {
                view.content_h = Layout::kEditorExpandedH;
            } else if (lyric_zone) {
                view.content_h = Layout::kExpandedLyricH;
            } else if (lead_ptr != nullptr &&
                       lead_ptr->activity.kind.rfind("calendar", 0) == 0) {
                // 日历没有进度条也没有页脚，用通用短档(96)仍会空出下面一截。
                view.content_h = Layout::kCalendarExpandedH;
            } else if (ParseProgress(lead_ptr == nullptr ? std::string()
                                                         : lead_ptr->activity.content_json) >= 0.0) {
                view.content_h = Layout::kExpandedH;
            } else {
                view.content_h = Layout::kExpandedShortH;
            }
        }
    }

    if (lead_ptr != nullptr) {
        const ScheduledActivity& lead = *lead_ptr;
        const std::string& json = lead.activity.content_json;
        const std::string& kind = lead.activity.kind;
        view.title = lead.activity.title;
        view.kind = kind;
        // force_cover 只服务视觉回归：真实链路的封面由采集层解码后写进 content_json。
        if (!force_cover.empty()) {
            view.cover_path = force_cover;
        } else {
            view.cover_path = JsonString(json, "cover");
            // 封面缺失时用播放器图标兜底。B 站 / VLC 这类视频应用不给缩略图，
            // 以前只能画一个纯色方块，完全认不出是哪个 App。
            if (view.cover_path.empty()) view.cover_path = JsonString(json, "icon");
        }
        view.progress = expanded ? ParseProgress(json) : -1.0;
        // 之前写死成「是媒体活动就当正在播放」，暂停时角标永远显示 ⏸。
        // 真实状态在 content_json 里（SMTC 的 PlaybackStatus）。
        view.playing = kind.rfind("media", 0) == 0 && JsonBool(json, "playing", true);
        view.wave_phase = wave_phase;

        // 所有文案都来自 content_json（真实数据），渲染层不硬编码任何业务文本。
        if (kind.rfind("media", 0) == 0) {
            // 繁转简只作用于「播放器给的元数据」：很多老歌的 tag 与歌词都是繁体，
            // 直接显示会和系统其它界面不一致。歌词在拉取时就转了，这里只管标题/歌手。
            if (simplify) {
                view.title = diw::win32::ToSimplified(view.title);
            }
            view.subtitle = SubtitleOf(kind, json);
            if (simplify) view.subtitle = diw::win32::ToSimplified(view.subtitle);
            const std::int64_t pos_ms = JsonInt(json, "position_ms");
            const std::int64_t end_ms = JsonInt(json, "end_ms");
            if (end_ms > 0) {
                view.meta = MmSs(pos_ms) + " / " + MmSs(end_ms);
            } else if (pos_ms > 0) {
                // 没有总时长（直播 / 部分视频播放器）：别退化成显示 AUMID，
                // 那串 com.bilibili.bilibiliPC 放在这里很怪。
                view.meta = MmSs(pos_ms) + " / --:--";
            }
            if (view.meta.empty()) view.meta = JsonString(json, "app");
        } else if (kind.rfind("notification", 0) == 0) {
            view.subtitle = SubtitleOf(kind, json);
            view.meta = JsonString(json, "app");
            // 来源应用图标（采集层从 AppInfo Logo 解码缓存）；为空时 DrawChip
            // 自动回落到通知默认图形。
            const std::string icon = JsonString(json, "icon");
            if (!icon.empty()) view.cover_path = icon;
        } else if (kind.rfind("clipboard", 0) == 0) {
            const std::int64_t count = JsonInt(json, "count");
            view.subtitle = SubtitleOf(kind, json);
            view.meta = count > 1 ? std::to_string(count) + " 项" : JsonString(json, "app");
        } else if (kind.rfind("file", 0) == 0 || kind.rfind("download", 0) == 0) {
            const std::string ext = JsonString(json, "ext");
            view.subtitle = SubtitleOf(kind, json);
            view.meta = ext.empty() ? "文件" : ext;
        } else if (kind.rfind("device", 0) == 0) {
            view.subtitle = SubtitleOf(kind, json);
            view.meta = "设备";
        } else if (kind.rfind("power", 0) == 0) {
            const std::int64_t pct = JsonInt(json, "percent", -1);
            view.meta = pct >= 0 ? std::to_string(pct) + "%" : "";
            view.subtitle = SubtitleOf(kind, json);
        } else if (kind.rfind("volume", 0) == 0) {
            view.subtitle = SubtitleOf(kind, json);
            view.meta = std::to_string(JsonInt(json, "percent")) + "%";
        } else if (kind.rfind("calendar", 0) == 0) {
            view.subtitle = SubtitleOf(kind, json);
        } else if (kind.rfind("agent", 0) == 0) {
            // 编码 Agent：所有文案都来自 content_json（采集层解析会话文件得到），
            // 这里只做「塞进哪个槽位」的排版决策。
            view.agent = true;
            view.agent_provider = JsonString(json, "provider");
            view.agent_name = JsonString(json, "name");
            view.agent_state = JsonString(json, "state");
            view.agent_task = JsonString(json, "task");
            view.agent_action = JsonString(json, "action");
            if (view.agent_action.empty()) view.agent_action = JsonString(json, "reply");
            if (view.agent_action.empty()) view.agent_action = "等待下一条指令";

            const std::int64_t done = JsonInt(json, "done");
            const std::int64_t total = JsonInt(json, "total");
            const std::int64_t steps = JsonInt(json, "steps");
            // 紧凑态右侧只放一个数字：有清单就「3/7」（一眼看出还剩多少），
            // 没清单就退成「78 步」（至少说明它没闲着）。
            if (total > 0) {
                view.agent_meta = std::to_string(done) + "/" + std::to_string(total);
            } else if (steps > 0) {
                view.agent_meta = std::to_string(steps) + " 步";
            }

            // 副标题只放「项目 · 已运行」：模型名很长，塞进来会把项目名挤掉，
            // 而项目名是用户认会话的第一线索。模型和步数挪到页脚，那里宽度富余。
            // 副标题用「真项目名」而不是活动标题：标题可能是被换成任务摘要的
            // 显示名，而副标题要回答「这是哪个仓库」。
            std::string project = JsonString(json, "project");
            if (project.empty()) project = view.title;
            std::string sub = project;
            const std::string elapsed = JsonString(json, "elapsed");
            if (!elapsed.empty()) sub += (sub.empty() ? "" : " · ") + elapsed;
            view.agent_sub = sub;

            std::string model = JsonString(json, "model");
            if (!model.empty()) {
                // 模型名可能带渠道前缀（custom-local:deepseek-flash），只留末段。
                const std::size_t colon = model.find_last_of(':');
                if (colon != std::string::npos && colon + 1 < model.size()) {
                    model = model.substr(colon + 1);
                }
            }
            std::string foot = model;
            if (steps > 0) {
                foot += (foot.empty() ? "" : " · ") + std::to_string(steps) + " 步";
            }
            view.agent_footer = foot;

            // 清单优先，没清单就画动作时间线 —— 两者都是「干到哪一步」的答案。
            diw::json::Value root;
            if (diw::json::Parse(json, root, nullptr)) {
                const auto& todos = root.At("todos");
                if (todos.IsArray() && !todos.items.empty()) {
                    view.agent_rows_are_todos = true;
                    for (const auto& t : todos.items) {
                        if (static_cast<int>(view.agent_rows.size()) >= Layout::kAgentRows) break;
                        const std::string status = t.At("s").AsString();
                        diw::win32::AgentRow row;
                        row.status = status == "completed" ? 2 : (status == "in_progress" ? 1 : 0);
                        row.text = t.At("t").AsString();
                        if (!row.text.empty()) view.agent_rows.push_back(std::move(row));
                    }
                } else {
                    const auto& timeline = root.At("timeline");
                    for (const auto& item : timeline.items) {
                        if (static_cast<int>(view.agent_rows.size()) >= Layout::kAgentRows) break;
                        if (!item.IsString() || item.text.empty()) continue;
                        diw::win32::AgentRow row;
                        row.status = 2;
                        row.text = item.text;
                        view.agent_rows.push_back(std::move(row));
                    }
                }
            }
            // 面板高度跟着实际行数走：只有两条时间线时就该是个矮面板，
            // 而不是底下挂着一条空带。高度变化有弹簧动画，不会突兀。
            // 只在展开态生效 —— 紧凑态的高度是固定的 46，套上这个公式
            // 会把小胶囊撑成一块空面板（实测就是这样翻车的）。
            if (expanded) {
                view.content_h = Layout::kAgentBaseH +
                                 static_cast<double>(view.agent_rows.size()) * Layout::kAgentRowH;
            }
        } else if (kind.rfind("meeting", 0) == 0) {
            // 会议复用通用的「两行 + 右侧」布局：它能表达的信息就两个
            // —— 哪个会、开了多久，不值得再写一套绘制。
            // 只需要一个专属颜色和话筒字形（见 AccentFor / DrawGlyph）。
            const std::string label = JsonString(json, "label");
            const std::string elapsed = JsonString(json, "elapsed");
            view.title = label.empty() ? "会议中" : label;
            view.subtitle = elapsed.empty() ? "正在使用麦克风" : ("正在开会 · " + elapsed);
            view.meta.clear();
            view.progress = -1.0;
        } else if (kind.rfind("editor", 0) == 0) {
            // 编辑器：三样信息按重要性排 —— 文件名、项目、语言。
            // 语言放在副标题末尾而不是单独一行：它只在"这是个什么文件"上有用，
            // 单独占一行会把本来就矮的面板撑开。
            view.editor = true;
            view.editor_brand = JsonString(json, "brand");
            view.editor_label = JsonString(json, "label");
            view.editor_file = JsonString(json, "file");
            view.editor_project = JsonString(json, "project");
            view.editor_language = JsonString(json, "language");
            // 这两个字段在 JSON 里是**布尔**，不是字符串 —— 用 JsonString 取会
            // 得到空串，于是"有未保存修改"永远显示不出来（实测踩过）。
            view.editor_dirty = JsonBool(json, "dirty", false);
            view.editor_remote = JsonBool(json, "remote", false);
            view.title = view.editor_file;
            view.subtitle = view.editor_project;
            view.progress = -1.0;
        } else if (kind.rfind("browser", 0) == 0) {
            // 浏览器：三样东西决定「我在哪」—— 站点图标、标签页标题、域名。
            // 下载进度是第四样，单独放右侧一列，因为它回答的是另一个问题。
            view.browser = true;
            view.browser_brand = JsonString(json, "brand");
            view.browser_label = JsonString(json, "label");
            view.browser_title = JsonString(json, "title");
            view.browser_url = JsonString(json, "url");
            view.browser_site = JsonString(json, "site");
            view.browser_site_label = JsonString(json, "site_label");
            view.browser_dl = ParseProgress(json, -1.0, "dprogress");
            view.browser_dl_hint = JsonString(json, "dhint");
            // favicon 走「封面」通道：渲染层只认一个图片路径，媒体封面也是这么来的，
            // 复用它就不必再造一套图片缓存。
            view.cover_path = JsonString(json, "favicon");
            if (simplify) {
                view.browser_title = diw::win32::ToSimplified(view.browser_title);
                view.browser_site_label = diw::win32::ToSimplified(view.browser_site_label);
            }
            if (view.browser_title.empty()) view.browser_title = view.browser_label;

            diw::json::Value root;
            if (diw::json::Parse(json, root, nullptr)) {
                for (const auto& d : root.At("downloads").items) {
                    if (static_cast<int>(view.browser_rows.size()) >= Layout::kBrowserRows) break;
                    diw::win32::BrowserRow row;
                    row.name = d.At("n").AsString();
                    row.progress = d.At("p").AsDouble(-1.0);
                    if (!(row.progress >= 0.0)) row.progress = -1.0;
                    const long long got = static_cast<long long>(d.At("got").AsInt(0));
                    char buf[48] = {0};
                    if (row.progress >= 0.0) {
                        std::snprintf(buf, sizeof(buf), "%d%%",
                                      static_cast<int>(row.progress * 100.0));
                    } else if (got > 0) {
                        std::snprintf(buf, sizeof(buf), "%.1f MB",
                                      static_cast<double>(got) / (1024.0 * 1024.0));
                    }
                    row.label = buf;
                    if (!row.name.empty()) view.browser_rows.push_back(std::move(row));
                }
                if (g_browser_fake_dl > 0) {
                    struct FakeDl {
                        const char* name;
                        double p;
                    };
                    static const FakeDl kFake[] = {
                        {"TRAE_CN-Setup-x64.exe", 0.62},
                        {"WorkBuddy-win32-x64-user.exe", 0.28},
                        {"deepseek-harness-0.2.2.zip", -1.0},
                    };
                    view.browser_rows.clear();
                    const int cap = (std::min)(g_browser_fake_dl, Layout::kBrowserRows);
                    for (int i = 0; i < cap; ++i) {
                        diw::win32::BrowserRow row;
                        row.name = kFake[i].name;
                        row.progress = kFake[i].p;
                        char buf[48] = {0};
                        if (row.progress >= 0.0) {
                            std::snprintf(buf, sizeof(buf), "%d%%",
                                          static_cast<int>(row.progress * 100.0));
                        } else {
                            std::snprintf(buf, sizeof(buf), "12.4 MB");
                        }
                        row.label = buf;
                        view.browser_rows.push_back(std::move(row));
                    }
                    view.browser_dl = 0.62;
                    view.browser_dl_hint = "62%";
                }
                if (expanded) {
                    view.content_h = view.browser_rows.empty()
                                         ? Layout::kBrowserBaseH
                                         : Layout::kBrowserWithRowsH +
                                               static_cast<double>(view.browser_rows.size()) *
                                                   Layout::kBrowserRowH;
                }
            }
        }
    }

    // ---- 一览模式：并行任务全部铺成独立岛 ----
    // 只在「确实有多件事」时才有意义：不足 2 个时上层会直接退出一览。
    if (overview && !expanded && frame.visible.size() >= 2) {
        const std::size_t n = std::min(frame.visible.size(),
                                       static_cast<std::size_t>(Layout::kOverviewMax));
        for (std::size_t i = 0; i < n; ++i) {
            const ScheduledActivity& s = frame.visible[i];
            const std::string& sj = s.activity.content_json;
            OverviewItem item;
            item.kind = s.activity.kind;
            item.title = s.activity.title;
            if (simplify && s.activity.kind.rfind("media", 0) == 0) {
                item.title = diw::win32::ToSimplified(item.title);  // 一览里也要简体
            }
            item.subtitle = SubtitleOf(s.activity.kind, sj);
            item.cover_path = JsonString(sj, "cover");
            // 任何类型都走同一套兜底：没有封面就用应用图标（媒体/通知都有 icon 字段）。
            if (item.cover_path.empty()) item.cover_path = JsonString(sj, "icon");
            // 点击拉起目标：媒体的 SMTC 来源 / 通知发送方是 AUMID；
            // 复制活动带的是完整 exe 路径（优先于只有文件名的 app_id）。
            // 没有 target 的岛（电源/音量/文件…）点击不动作，一览保持。
            item.launch = JsonString(sj, "app_id");
            const std::string app_path = JsonString(sj, "app_path");
            if (!app_path.empty()) item.launch = app_path;
            item.provider = JsonString(sj, "provider");
            item.state = JsonString(sj, "state");
            if (s.activity.kind.rfind("browser", 0) == 0) {
                // 一览里的浏览器岛用站点图标；provider 复用成品牌 id、
                // state 复用成品牌名（没有图标时画首字母用）。
                item.provider = JsonString(sj, "brand");
                item.state = JsonString(sj, "label");
                const std::string fav = JsonString(sj, "favicon");
                if (!fav.empty()) item.cover_path = fav;
            }
            view.items.push_back(std::move(item));
        }
        view.overview = true;
        view.content_w = static_cast<double>(n) * Layout::kCompactW +
                         static_cast<double>(n - 1) * Layout::kGap;
        view.content_h = Layout::kCompactH;
        view.detached_w = 0.0;  // 全都铺开了，不再需要分离小圆点
        view.detached_items.clear();
    }
    // 展开态是大视图，主位已占用全部注意力，不再并列分离小胶囊。
    // 最多 kDetachedMax 个：一个主胶囊 + 两个小圆是这块区域的极限，
    // 第三个之后只靠「+N」告诉用户还有多少。
    if (frame.visible.size() >= 2 && !expanded) {
        const int slots = std::min<int>(static_cast<int>(frame.visible.size()) - 1,
                                        Layout::kDetachedMax);
        for (int i = 0; i < slots; ++i) {
            const ScheduledActivity& item = frame.visible[static_cast<std::size_t>(1 + i)];
            diw::win32::DetachedItem d;
            d.kind = item.activity.kind;
            d.provider = JsonString(item.activity.content_json, "provider");
            d.state = JsonString(item.activity.content_json, "state");
            d.cover_path = JsonString(item.activity.content_json, "cover");
            if (d.cover_path.empty()) {
                d.cover_path = JsonString(item.activity.content_json, "icon");
            }
            d.activity_id = item.activity.id;
            view.detached_items.push_back(std::move(d));
        }
        view.detached_w = static_cast<double>(slots) * Layout::kDetachedSize +
                          static_cast<double>(slots - 1) * Layout::kGap;
    }

    // 紧凑态右侧的「+N」：告诉用户后面还排着几件事（双击可铺开）。
    // 必须放在 detached 定稿之后算：已经画出来的小圆各代表 1 件，
    // 再写个 +N 就是重复噪音。
    {
        const int detached_slots = static_cast<int>(view.detached_items.size());
        view.more_count = static_cast<int>(frame.visible.size()) - 1 - detached_slots;
        if (view.more_count < 0) view.more_count = 0;
    }

    if (with_hud) {
        // 状态行必须短到能放进胶囊宽度内，否则会被省略号截断。
        std::string hud_text = std::string(StateName(state)) + " · vis " + std::to_string(frame.visible.size());
        if (blocked_count > 0) hud_text += " · blk " + std::to_string(blocked_count);
        hud_text += " · sup " + std::to_string(frame.suppressed_count) + " · exp " +
                    std::to_string(frame.expired_ids.size());
        if (!bridge_online) hud_text += " · 桥接断开";
        if (suspended) hud_text += " · 已暂停";
        view.hud = hud_text;
    }
    return view;
}

// 日志滚动：追加写入而不是每次截断，否则上一次崩溃的现场会被这次启动擦掉，
// 而「崩溃前最后几行」恰恰是唯一有用的东西。超过 2MB 才滚动成 .old 保留一份。
void RotateLogIfNeeded(const std::string& path) {
    if (path.empty()) return;
    const std::wstring wpath = Widen(path);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (::GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &info) == FALSE) return;
    const unsigned long long size =
        (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size < 2ull * 1024ull * 1024ull) return;
    // 覆盖式移动而不是「先 DeleteFile 再 MoveFile」：那两步之间有个窗口，
    // 一旦移动失败（例如另一个实例还开着日志），旧 .old 已经被删、这次也没滚成，
    // 上一份现场就白丢了。
    ::MoveFileExW(wpath.c_str(), (wpath + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
}

int Run() {
    const Options opt = ParseArgs();
    RotateLogIfNeeded(opt.log_path);
    std::ofstream log(opt.log_path, std::ios::out | std::ios::app | std::ios::binary);
    if (log) {
        log << "\n===== start " << std::time(nullptr) << " =====\n";
        log.flush();
    }
    // 采集线程会通过 SetLogger 跨线程写这个流（notif:/media: 诊断行），
    // ofstream 本身不是线程安全的，必须加锁；否则两线程交错写会撕裂行。
    static std::mutex log_mutex;
    auto note = [&log](const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        if (log) {
            log << line << "\n";
            log.flush();
        }
    };

    EnableDpiAwareness();
    GdiPlusSession gdiplus;

    // ---- 规则：先落地配置文件，再读进内存。放在最前面，设置窗口也要用。----
    diw::win32::RulesStore rules;
    rules.Init(ExeDirectoryWide());
    note("rules: " + Narrow(rules.Path()));

    // ---- 设置模式：开一个原生窗口，保存后写回配置文件并退出 ----
    if (opt.settings) {
        diw::RuleConfig updated;
        bool saved = false;
        bool shot_done = false;
        std::string selftest_note;
        diw::win32::ShowSettings(rules.Config(), rules.Path(), Widen(opt.settings_shot), &updated,
                                 &saved, &shot_done, opt.settings_selftest, &selftest_note);

        if (opt.settings_selftest) {
            // 自检结果单独落一份，绝不覆盖用户真实配置。
            const std::string out_path = ExeDirectory() + "\\settings_selftest.json";
            std::ofstream out(out_path, std::ios::out | std::ios::trunc | std::ios::binary);
            const std::string text = updated.ToJson();
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.close();
            note("selftest: " + selftest_note);
            note("selftest: wrote " + out_path);
            return saved ? 0 : 1;
        }

        if (saved) {
            if (rules.Save(updated)) {
                note("rules saved: " + Narrow(rules.Path()));
            } else {
                note("rules save failed: " + rules.LastError());
            }
        }
        if (!opt.settings_shot.empty()) note(std::string("settings shot: ") + (shot_done ? "saved" : "failed"));
        note("settings closed, exit");
        return 0;
    }

    LayeredSurface surface;
    CapsuleRenderer renderer;

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;  // 没有它收不到 WM_LBUTTONDBLCLK
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"DIW_OverlayWindow";
    if (::RegisterClassExW(&wc) == 0) {
        note("ERROR: RegisterClassExW failed");
        return 1;
    }

    // 单实例：否则两个进程各画一个胶囊，像素级重叠，动画不同步，
    // 而且两套采集线程都在读剪贴板/通知。实测同时跑两个实例确认过。
    // 第二个进程不静默退出，而是通知已有实例现身 —— 见 g_presence。
    ::CreateMutexW(nullptr, FALSE, L"DynamicIsland-Win-Overlay-SingleInstance");
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = ::FindWindowW(L"DIW_OverlayWindow", nullptr)) {
            ::PostMessageW(existing, kMsgPresence, 0, 0);
            note("already running: presence ping sent, this process exits");
        } else {
            note("already running: mutex held but window not found");
        }
        return 0;
    }

    HWND hwnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                  wc.lpszClassName, L"DynamicIsland-Win", WS_POPUP, 0, 0, 10, 10, nullptr,
                                  nullptr, instance, nullptr);
    if (hwnd == nullptr) {
        note("ERROR: CreateWindowExW failed");
        return 1;
    }

    // ---- 几何：DPI 缩放 / 所在显示器 / 玻璃捕获矩形 ----
    // 全部可重算：WM_DISPLAYCHANGE / WM_DPICHANGED（拔插显示器、改缩放、
    // 拓扑重排把窗口挤到别的屏）会置 g_display_changed，主循环重算一遍。
    // 玻璃捕获矩形固定为「一览 + HUD」的最大可能窗口 —— 固定几何的意义：
    // epoch（干净捕获）与刷新捕获永远像素对齐，修补才能直接按行 memcpy。
    // 一览（双击铺开所有并行任务）比展开态宽得多：4 个岛 = 1224 逻辑像素，
    // 捕获矩形必须盖住它，否则铺开瞬间玻璃透出的是黑边。
    double scale = WindowScale(hwnd);
    MONITORINFO monitor{};
    int screen_cx = 0;
    int screen_top = 0;
    int glass_w = 0, glass_h = 0, glass_x = 0, glass_y = 0;
    auto relayout = [&]() {
        scale = WindowScale(hwnd);
        monitor = MONITORINFO{};
        monitor.cbSize = sizeof(monitor);
        ::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        screen_cx = (monitor.rcWork.left + monitor.rcWork.right) / 2;
        screen_top = monitor.rcWork.top + static_cast<int>(6 * scale);

        // 直接按整个工作区宽度取，不再「估一个够用的宽度」。
        // 估宽度的版本算错过两次：加小圆时漏了 detached 的宽度，加第 4 个岛时
        // 漏了画布把 margin 也算进内容宽 —— 每次都表现为「最右边那个岛是纯黑的」。
        // 玻璃只在这一步和尺寸变化时重拍，铺满一屏的代价是几十毫秒，
        // 比「猜宽度」这种会反复出错的省法划算得多。
        const int screen_w = monitor.rcWork.right - monitor.rcWork.left;
        glass_w = screen_w - 8;
        // 高度按最高的形态取，否则超出玻璃捕获矩形的那一截会透出黑边。
        // 现在是 Agent 面板（清单 4 行 = 254）比歌词态（196）更高 ——
        // 加它的时候漏改这里，实测底部会出现一条没有模糊的纯黑带。
        const float tallest = std::max(Layout::kExpandedLyricH, Layout::kAgentExpandedH);
        glass_h = static_cast<int>(std::lround(
            (tallest + Layout::kMargin * 2.0f + Layout::kHudHeight) * scale));
        glass_x = screen_cx - glass_w / 2;
        glass_y = screen_top;
    };
    relayout();

    // 岛的水平位置。三个档位都按**工作区**算，并且一律夹在可视范围内：
    // 靠右时如果宽度大于工作区（4 个岛铺开在窄屏上），右对齐会把左边推出屏幕，
    // 用户就只能看到半个岛、还点不到关闭按钮。
    auto anchor_x = [&](int width) -> int {
        const int left = monitor.rcWork.left;
        const int right = monitor.rcWork.right;
        const int edge = static_cast<int>(std::lround(28.0 * scale));  // 靠边时的留白
        int x = (left + right) / 2 - width / 2;
        const int mode = g_force_anchor >= 0 ? g_force_anchor : rules.Config().anchor;
        if (mode == 0) x = left + edge;
        if (mode == 2) x = right - width - edge;
        if (width >= right - left) return left;       // 比屏幕还宽：只能从左贴边
        if (x < left) x = left;
        if (x + width > right) x = right - width;
        return x;
    };

    note("DIW overlay start " + Timestamp());
    note("scale=" + std::to_string(scale) + "  workarea=" +
         std::to_string(monitor.rcWork.right - monitor.rcWork.left) + "x" +
         std::to_string(monitor.rcWork.bottom - monitor.rcWork.top));
    note("rules load: " + std::string(rules.LastLoadOk() ? "ok" : "fallback") +
         (rules.LastError().empty() ? "" : " (" + rules.LastError() + ")"));
    note("events: media=" + std::string(rules.Config().events.media ? "on" : "off") +
         " notif=" + std::string(rules.Config().events.notification ? "on" : "off") +
         " clip=" + std::string(rules.Config().events.clipboard ? "on" : "off") +
         " file=" + std::string(rules.Config().events.file ? "on" : "off") +
         " vol=" + std::string(rules.Config().events.volume ? "on" : "off") +
         " power=" + std::string(rules.Config().events.power ? "on" : "off") +
         " device=" + std::string(rules.Config().events.device ? "on" : "off"));
    note("glass: " + std::string(rules.Config().glass ? "on (backdrop blur)" : "off (opaque)"));

    bool present_failed = false;
    // 提交统计：胶囊静止时绝大多数帧的像素与上一帧完全相同，跳过提交是常态。
    // 把 skip/total 写进周期日志，便于判断"重绘是否真的降下来了"。
    std::uint64_t present_total = 0;
    std::uint64_t present_skipped = 0;
    auto present = [&](int x, int y, BYTE alpha) {
        if (present_failed) return;
        try {
            ++present_total;
            if (!surface.PresentIfChanged(hwnd, x, y, alpha)) ++present_skipped;
        } catch (const std::exception& e) {
            present_failed = true;
            note(std::string("WARN: present failed, continue headless: ") + e.what());
        }
    };

    // 每帧的 GDI+ 质量档位（两段绘制都要设一遍，因为中间会重建 Graphics）。
    auto set_quality = [](Gdiplus::Graphics& g) {
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    };

    // 铬层的缓存键：凡是会影响「底/受光/描边/投影」的输入都要进来。
    // 尺寸类用千分之一像素取整，避免弹簧末端的浮点抖动导致每帧假失配。
    auto chrome_key_of = [](int cw, int ch, const CapsuleView& v, const GlassParams& gl) {
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&h](std::uint64_t x) {
            h ^= x;
            h *= 1099511628211ull;
        };
        auto mix_milli = [&mix](double value) {
            mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(value * 1000.0))));
        };
        mix(static_cast<std::uint64_t>(cw));
        mix(static_cast<std::uint64_t>(ch));
        mix_milli(v.content_w);
        mix_milli(v.content_h);
        mix_milli(v.detached_w);
        mix(static_cast<std::uint64_t>(std::lround(std::clamp(v.alpha, 0.0, 1.0) * 255.0)));
        mix(static_cast<std::uint64_t>(v.opacity_mode));
        mix(v.expanded ? 1u : 0u);
        mix(v.overview ? 1u : 0u);
        mix(v.sleep_bar ? 1u : 0u);
        mix(static_cast<std::uint64_t>(v.items.size()));
        mix(static_cast<std::uint64_t>(v.detached_items.size()));
        // 玻璃纹理每换一个纪元就是新对象，指针变化即可作为失效信号。
        mix(reinterpret_cast<std::uint64_t>(gl.backdrop));
        mix_milli(gl.offset_x);
        mix_milli(gl.offset_y);
        mix_milli(gl.full_w);
        mix_milli(gl.full_h);
        return h;
    };

    // 铬层缓存：命中时直接把上次的像素 memcpy 回来，只重画内容。
    std::vector<std::uint8_t> chrome_cache;
    std::uint64_t chrome_key_cached = 0;
    std::uint64_t chrome_hits = 0;
    std::uint64_t chrome_calls = 0;

    // 首次提交必须在 ShowWindow 之前，否则会闪一块黑矩形。
    surface.EnsureSize(static_cast<int>(Layout::kCompactW * scale + 2 * Layout::kMargin * scale),
                       static_cast<int>(Layout::kCompactH * scale + 2 * Layout::kMargin * scale));
    surface.Clear();
    present(anchor_x(surface.Width()), screen_top, 0);
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    // 托盘在进入主循环前注册。注册失败不算致命：菜单仍可从胶囊右键进入，
    // 但要在日志里说清楚，否则"没有托盘"会被当成 bug 反复排查。
    if (TrayAdd(hwnd)) {
        note("tray: registered");
    } else {
        note("tray: unavailable (shell not ready?) - use capsule right-click");
    }
    g_taskbar_created = ::RegisterWindowMessageW(L"TaskbarCreated");

    // ---- 外部推送上岛（命名管道）----
    // 一次连接一条消息；解码在工作线程，主循环只从队列取现成的请求。
    // 这样别的程序（CI、下载器、脚本、IDE）不必弹自己的通知，直接上岛。
    diw::win32::IpcServer ipc;
    ipc.SetLogger([&note](const std::string& line) { note(line); });
    ipc.Start();
    note("ipc: listening on \\\\.\\pipe\\DynamicIslandWin-v1");

    Demo demo;
    Animation anim;

    // 在线歌词：独立 worker 线程拉 LRCLIB，主线程只投歌名与读结果。
    diw::win32::LyricFetcher lyric_fetcher;
    // 配置里指定的歌词代理（空 = 自动探测环境变量 / Internet 选项）。
    lyric_fetcher.SetProxy(Widen(rules.Config().lyrics_proxy));
    lyric_fetcher.SetSimplify(rules.Config().simplify_chinese);
    // 关闭按钮：记住被 dismiss 的活动指纹，直到内容变化（换歌 / 新通知）再显示。
    // 列表而非单个槽：并行任务有两条时用户会连着叉两个，单槽会把前一条挤掉。
    struct DismissedItem {
        std::string fp;
        std::int64_t until_ms = 0;
    };
    std::vector<DismissedItem> dismissed;
    bool was_sleeping = false;  // 只为日志去重：休眠进出各记一行
    std::string last_lead_fp;   // 上一帧的主活动指纹：变了就算「有新内容」，用来唤醒
    std::int64_t last_lyric_fill_ms = 0;
    // 歌词三行按 100ms 节流更新，帧间直接复用，避免每帧拷贝整首歌词。
    std::string lyric_prev_cache, lyric_cur_cache, lyric_next_cache, lyric_status_cache;
    std::string last_lyric_status;
    // seek 乐观更新：松手后进度条先停在目标位置，等采集层追上来再交还真实值。
    // 播放器 seek 有延迟，不这么做进度条会立刻跳回原位，用户以为「拖不动」。
    double seek_hold_progress = -1.0;
    std::int64_t seek_hold_until_ms = 0;

    // 本地 LRC fixture：用真实 API 拉下来的数据存盘，做确定性歌词回归。
    std::vector<diw::win32::LyricLine> lyric_fixture;
    if (!opt.lyric_lrc_path.empty()) {
        std::ifstream in(opt.lyric_lrc_path, std::ios::in | std::ios::binary);
        if (in) {
            std::ostringstream buf;
            buf << in.rdbuf();
            // fixture 也要走同一套繁转简，否则「回归里是繁体、真实链路是简体」
            std::string lrc_text = buf.str();
            if (rules.Config().simplify_chinese) {
                lrc_text = diw::win32::ToSimplified(lrc_text);
            }
            lyric_fixture = diw::win32::ParseLrc(lrc_text);
        }
        note("lyric fixture: " + std::to_string(lyric_fixture.size()) + " lines from " +
             opt.lyric_lrc_path);
    }
    const bool has_fixture = !lyric_fixture.empty();

    Backdrop glass_epoch;       // 胶囊不可见时截的干净背景
    Backdrop glass_refresh;     // 可见期间的定期刷新（用 epoch 修补胶囊挡住的区域）
    Backdrop* glass_current = nullptr;  // 渲染当前用的那份
    std::int64_t glass_last_ms = -100000;
    bool was_hidden = true;
    // 上一次捕获时的画布尺寸。画布一变（岛数变化 / 展开收起），旧 epoch 的
    // 覆盖范围就不够用了，必须重拍 —— 否则修补区右侧贴不到干净像素。
    int glass_canvas_w = 0;
    int glass_canvas_h = 0;

    // 真实数据采集：跑在独立工作线程，渲染主线程只读快照。
    diw::live::LiveSources sources;
    diw::LiveBaselines baselines;
    std::unordered_set<std::string> persistent_ids;
    std::string last_live_log;
    std::string last_block_log;
    int blocked_shown = 0;
    std::int64_t pause_until_ms = -1;
    std::int64_t last_rules_check_ms = -1000;
    int last_visible_count = 0;  // 上一帧可见活动数（右键菜单的「展开全部（N）」用）
    // 一览各岛的拉起目标（上一帧 BuildView 的快照）。点击发生在两帧之间，
    // 消费时用最近一次构建的结果做命中 —— 岛集合变化极慢（秒级），一帧
    // 的陈旧不会命中错岛。
    std::vector<std::string> overview_launch;
    std::vector<std::string> detached_ids;  // 本帧每个小圆代表的活动 id

    // 回归：只验「给定目标能不能拉起来」，不进入主循环。
    // 点击跳转的失败模式几乎都在 LaunchApp 内部（AUMID 类型 / 进程名 / 宿主进程），
    // 用它就能逐条验证，不必真的去点。
    if (!opt.launch_test.empty()) {
        std::string resolved;
        const bool ok = LaunchAppImpl(opt.launch_test, opt.launch_dry_run, &resolved);
        note("launch-test: target=" + opt.launch_test +
             (ok ? " ok" : " FAILED") + (resolved.empty() ? "" : " via=" + resolved) +
             (opt.launch_dry_run ? " (dry-run)" : ""));
        return ok ? 0 : 1;
    }

    if (!opt.scripted) {
        g_sources = &sources;
        sources.SetLogger(note);  // 采集线程诊断行（notif:/media:），必须在 Start 之前
        if (!opt.agent_home.empty()) {
            sources.SetAgentHome(opt.agent_home);
            note("agent: home override -> " + Narrow(opt.agent_home));
        }
        sources.ConfigureAgents(rules.Config().agents, rules.Config().agent_max,
                                rules.Config().agent_idle_sec, false);
        sources.ConfigureBrowsers(rules.Config().browsers, false);
        sources.ConfigureMeetings(rules.Config().meetings, false);
        sources.SetMeetingLogger(note);
        sources.ConfigureEditors(rules.Config().editors, false);
        sources.SetEditorLogger(note);
        if (!opt.fake_editor.empty()) {
            // 格式 "exe|标题"。真实链路要求"用户正开着编辑器并打开了一个文件"，
            // 截图回归不能依赖这个，所以留一条注入。
            const std::size_t bar = opt.fake_editor.find('|');
            sources.SetForceEditor(bar == std::string::npos ? std::string() :
                                                              opt.fake_editor.substr(0, bar),
                                   bar == std::string::npos ? opt.fake_editor
                                                            : opt.fake_editor.substr(bar + 1));
        }
        if (!opt.fake_meeting.empty()) sources.SetFakeMeeting(opt.fake_meeting);
        if (!opt.browser_brand.empty()) sources.SetForceBrowserBrand(opt.browser_brand);
        g_browser_fake_dl = opt.browser_fake_dl;
        g_force_opacity = opt.opacity;
        g_force_anchor = opt.anchor;
        sources.SetBrowserLogger(note);
        if (!opt.browser_brand.empty()) note("browser: force=" + opt.browser_brand);
        sources.Start();
        note("live mode: media / notifications / clipboard / files / power / volume / device / agent");
        if (rules.Config().agents) {
            note("editor: watching foreground window title " +
                 std::string(rules.Config().editors ? "(on)" : "(off)"));
            note("browser: watching foreground window + History / Favicons / downloads " +
                 std::string(rules.Config().browsers ? "(on)" : "(off)"));
            note("agent: watching .claude / .workbuddy / .codex / .gemini / .pi / .zcode / Cursor "
                 "(max=" + std::to_string(rules.Config().agent_max) +
                 ", idle=" + std::to_string(rules.Config().agent_idle_sec) + "s, sqlite=" +
                 (diw::win32::SqliteLite::ApiReady() ? "winsqlite3" : "unavailable") + ")");
        }
    } else {
        note("script mode: 使用内置剧本（仅用于视觉回归与演示）");
    }

    const auto start = Clock::now();
    auto last = start;
    long long overview_at = opt.overview_at_ms;  // 可变副本：触发一次后失效（opt 是 const）
    long long expand_at = opt.expand_at_ms;
    long long click_at = opt.click_at_ms;
    int pending_detach = opt.click_detached;  // 可变副本：触发一次后置 -1
    long long display_at = opt.display_at_ms;
    long long mouse_at = opt.mouse_at_ms;
    std::string last_log;
    bool snapshot_taken = false;
    bool grab_taken = false;
    bool hidden_presented = false;
    // 最近一次「用户动了它」的时刻。长时间没人理就自动睡成一条细条，
    // 免得一块面板一直挂在屏幕顶上挡东西。
    std::int64_t last_interaction_ms = 0;
    HWND last_foreground = nullptr;  // 上一帧的前台窗口，用来检测「打开/切换了新界面」

    while (!g_quit) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_quit = true;
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (g_quit) break;

        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        const double elapsed = std::chrono::duration<double>(now - start).count();
        const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

        // ---- 显示器 / DPI 变化：重算全部几何 ----
        // 触发源：WM_DISPLAYCHANGE（拔插屏、改分辨率）与 WM_DPICHANGED
        //（缩放比例变化、拓扑重排把窗口挤到别的屏）。位置每帧 present
        // 自动跟上新 screen_cx；玻璃的 epoch 缓冲按旧几何截的，作废重截。
        if (g_display_changed) {
            g_display_changed = false;
            relayout();
            glass_current = nullptr;  // 旧纹理尺寸/位置可能不再匹配，重截前回落不透明
            was_hidden = true;        // 下一帧按新几何截干净 epoch
            note("display change: re-layout scale=" + std::to_string(scale) +
                 " workarea=" + std::to_string(monitor.rcWork.right - monitor.rcWork.left) + "x" +
                 std::to_string(monitor.rcWork.bottom - monitor.rcWork.top) +
                 " glass=" + std::to_string(glass_w) + "x" + std::to_string(glass_h) + " at (" +
                 std::to_string(glass_x) + "," + std::to_string(glass_y) + ")");
        }

        // ---- 配置热加载：500ms 查一次 mtime，改了立刻生效 ----
        if (now_ms - last_rules_check_ms >= 500) {
            last_rules_check_ms = now_ms;
            if (rules.ReloadIfChanged()) {
                note(std::string("rules reloaded: ") + (rules.LastLoadOk() ? "ok" : "ignored") +
                     (rules.LastError().empty() ? "" : " (" + rules.LastError() + ")"));
                // 代理改了要立刻生效，不必等下一首歌。
                lyric_fetcher.SetProxy(Widen(rules.Config().lyrics_proxy));
                lyric_fetcher.SetSimplify(rules.Config().simplify_chinese);
                // 浏览器采集开关也要跟着走：关掉后下一拍就不再打开浏览器数据库。
                sources.ConfigureBrowsers(rules.Config().browsers, demo.suspended);
                sources.ConfigureEditors(rules.Config().editors, demo.suspended);
                // Agent 采集：关掉要立刻停止读文件，别等重启。
                sources.ConfigureAgents(rules.Config().agents, rules.Config().agent_max,
                                        rules.Config().agent_idle_sec, demo.suspended);
                if (!rules.Config().agents) {
                    // 真关掉时把已有会话也清空，否则界面上会残留最后几条岛
                    // 直到它们自然超时 —— 用户会以为开关没生效。
                    for (const auto& id : persistent_ids) {
                        if (id.rfind("live-agent-", 0) == 0) demo.runtime->EndActivity(id);
                    }
                }
            }
        }

        // ---- 右键菜单 ----
        if (g_menu) {
            g_menu = false;
            const bool paused = pause_until_ms > now_ms;
            const int cmd = ShowContextMenu(hwnd, paused, last_visible_count, demo.overview);
            if (cmd == kMenuQuit) {
                g_quit = true;
                break;
            }
            if (cmd == kMenuOverview) {
                const Frame& mf = demo.runtime->LastResult().frame;
                if (demo.overview) {
                    demo.overview = false;
                } else if (mf.visible.size() >= 2) {
                    demo.overview = true;
                    demo.expand_id.clear();
                } else if (!mf.visible.empty()) {
                    demo.expand_id = mf.visible.front().activity.id;
                }
                last_interaction_ms = now_ms;
                note(std::string("[t=+") + std::to_string(elapsed).substr(0, 5) + "s] overview=" +
                     (demo.overview ? "on" : "off"));
            }
            if (cmd == kMenuPause) {
                pause_until_ms = paused ? -1 : now_ms + 30 * 60 * 1000;
                note(std::string("[t=+") + std::to_string(elapsed).substr(0, 5) +
                     "s] pause=" + (paused ? "off" : "30min"));
            }
            if (cmd == kMenuSettings) {
                diw::RuleConfig updated;
                bool saved = false;
                diw::win32::ShowSettings(rules.Config(), rules.Path(), L"", &updated, &saved, nullptr);
                if (saved) {
                    note(std::string("rules saved: ") + (rules.Save(updated) ? "ok" : rules.LastError()));
                }
            }
        }

        const bool user_paused = pause_until_ms > now_ms;

        // 脚本推进（循环播放）
        if (opt.scripted) {
            const double loop_t = std::fmod(elapsed, kLoopSeconds);
            const auto& script = Script();
            while (demo.cursor < script.size() && script[demo.cursor].at_sec <= loop_t) {
                demo.Apply(script[demo.cursor].action, now_ms);
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] action=" +
                     std::to_string(static_cast<int>(script[demo.cursor].action)));
                ++demo.cursor;
            }
            if (loop_t < 0.05 && demo.cursor >= script.size()) demo.Reset();
        } else {
            // ── 真实模式：系统状态 → 规则过滤 → 实时活动 → 核心域 ──
            const auto snap = sources.Get();
            const diw::LiveActivities built =
                diw::BuildActivities(snap, now_ms, MinuteOfDay(), baselines, rules.Config());
            baselines = built.next;
            blocked_shown = static_cast<int>(built.blocked.size());

            // 换歌就投一个歌词请求（非阻塞）。配置关掉时完全不发网络请求。
            // 取「当前主活动」那首歌而不是 snap.media：同时开着两个播放器时，
            // 屏上显示的不一定是 SMTC 的「当前会话」，歌词会串到另一个应用。
            if (rules.Config().lyrics && snap.media.present && !snap.media.title.empty()) {
                std::string lyric_artist = snap.media.artist;
                std::string lyric_title = snap.media.title;
                const auto& lf = demo.runtime->LastResult().frame;
                const Activity* lyric_act = LeadOf(lf, demo.expand_id);
                if (lyric_act != nullptr && lyric_act->kind.rfind("media", 0) == 0) {
                    lyric_title = lyric_act->title;
                    const std::string a = JsonString(lyric_act->content_json, "artist");
                    if (!a.empty()) lyric_artist = a;
                }
                if (lyric_fetcher.SetSong(lyric_artist, lyric_title)) {
                    note("lyrics: fetch \"" + lyric_artist + " - " + lyric_title + "\"");
                }
            }

            std::unordered_set<std::string> want_persistent;
            // 抑制的指纹必须带上「哪个应用」：只按 kind|标题 匹配时，
            // 两个应用碰巧同名（视频站的通用标题、同类通知）会互相顶掉；
            // 而没有应用维度的抑制又会让「叉掉 A 后打开 B」永远看不到 B。
            std::vector<bool> dismissed_seen(dismissed.size(), false);
            for (const auto& a : built.list) {
                // 被 × 关掉的活动在本条内容变化前不再插入，否则下一帧又被
                // StartActivity 重建，看起来「关不掉」。换歌 / 新通知会改指纹。
                const std::string fp =
                    a.kind + "|" + JsonString(a.content_json, "app_id") + "|" + a.title;
                bool skip_dismissed = false;
                for (std::size_t i = 0; i < dismissed.size(); ++i) {
                    if (dismissed[i].fp != fp) continue;
                    dismissed_seen[i] = true;
                    skip_dismissed = true;
                }
                if (skip_dismissed) continue;
                if (a.stale_at_ms.has_value()) {
                    // 瞬时活动：只在「需要」时插入。
                    // 绝不能每帧 Upsert —— ActivityStore::Upsert 是整条覆盖，
                    // 会不断把 stale_at 往后推，结果通知永不退场。
                    //
                    // 三种需要插入的情况：
                    //   1) 当前没有这条提示；
                    //   2) 上一条同类提示已经过期（只标 Stale 不删除，Get 仍非空）；
                    //   3) 提示还在显示，但内容变了 —— 用户连续复制时，
                    //      岛上不该停在上一次那段文本上。
                    // 情况 2 是实测抓到的致命 bug：固定 id 的事件在同一进程里
                    // 只提示第一次，第二次起完全静默。
                    const diw::Activity* existing = demo.runtime->Store().Get(a.id);
                    const bool already_stale =
                        existing != nullptr && existing->state == diw::ActivityState::Stale;
                    const bool content_changed =
                        existing != nullptr &&
                        (existing->title != a.title || existing->content_json != a.content_json);

                    if (existing == nullptr || already_stale || content_changed) {
                        if (existing != nullptr) demo.runtime->Store().Remove(a.id);
                        std::string err;
                        if (StartActivitySafe(*demo.runtime, a, &err)) {
                            note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] " +
                                 (content_changed ? "~" : "+") + a.kind + " 「" + a.title + "」");
                        } else {
                            note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] !skip " +
                                 a.kind + ": " + err);
                        }
                    }
                } else {
                    // 持续类（媒体/低电量）：每帧刷新，才能同步进度与百分比。
                    std::string err;
                    if (StartActivitySafe(*demo.runtime, a, &err)) {
                        want_persistent.insert(a.id);
                    } else {
                        note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] !skip " + a.kind +
                             ": " + err);
                    }
                }
            }
            // 源消失就结束：歌停了、插电了、通知过期了。
            for (const auto& id : persistent_ids) {
                if (want_persistent.find(id) == want_persistent.end()) {
                    demo.runtime->EndActivity(id);
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] -" + id);
                }
            }
            persistent_ids = std::move(want_persistent);

            // 被叉掉的那条已经不在数据里了（关了软件 / 换歌 / 通知退场）：
            // 立刻解除那一条的抑制。否则它会一直挂着，下一次「同应用同标题」
            // 的事件永远不再出现 —— 这类「关了以后再也不弹」最难自查。
            for (std::size_t i = dismissed.size(); i-- > 0;) {
                if (!dismissed_seen[i] || now_ms > dismissed[i].until_ms) {
                    dismissed.erase(dismissed.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }

            // 全屏 / 演示模式 / 手动暂停：真实避让，不再靠手工开关
            demo.suspended = built.suspend || user_paused;
            demo.runtime->SetSuspended(demo.suspended);
            // 全屏 / 手动暂停时连文件都不扫：agent 的会话文件在被持续写入，
            // 全屏看电影时还在后台解析它没有意义。松开后第一拍会自动补上。
            if (!opt.scripted) {
                sources.ConfigureAgents(rules.Config().agents, rules.Config().agent_max,
                                        rules.Config().agent_idle_sec, demo.suspended);
            }

            // 规则拦截摘要：只在变化时记一行，用于确认「确实拦下来了」
            if (!built.blocked.empty()) {
                std::string block_line = "blocked:";
                for (const auto& b : built.blocked) {
                    block_line += " " + b.kind + "/" + b.reason;
                    if (!b.matched.empty()) block_line += "(" + b.matched + ")";
                }
                if (block_line != last_block_log) {
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] " + block_line);
                    last_block_log = block_line;
                }
            } else {
                last_block_log.clear();
            }

            // 采集摘要：只在状态变化时记一行，用于确认「确实读到了真实数据」
            char live_line[900];
            std::snprintf(live_line, sizeof(live_line),
                          "live media=%s (%s%s) vol=%d%% batt=%d%% ac=%d fullscreen=%d notif_seq=%llu "
                          "clip=%s/%llu file=%s err=%s",
                          snap.media.present ? (snap.media.title.empty() ? "(no title)"
                                                                         : snap.media.title.c_str())
                                             : "none",
                          snap.media.playing ? "playing" : "paused",
                          snap.media.suppressed ? ",suppressed" : "", snap.volume_pct, snap.power.pct,
                          snap.power.ac_online ? 1 : 0, snap.fullscreen ? 1 : 0,
                          static_cast<unsigned long long>(snap.notif.seq),
                          snap.clipboard.kind.empty() ? "-" : snap.clipboard.kind.c_str(),
                          static_cast<unsigned long long>(snap.clipboard.seq),
                          snap.file.name.empty() ? "-" : snap.file.name.c_str(),
                          snap.last_error.empty() ? "-" : snap.last_error.c_str());
            if (last_live_log != live_line) {
                note(live_line);
                last_live_log = live_line;
            }
        }

        // ---- 休眠语义：有岛但被藏起来 ≠ 没有岛 ----
        // 「没有岛」（没有任何活动）时窗口必须彻底透明、点不到；
        // 「有岛但该让路」（全屏避让 / 长时间没人理 / 手动收起）才是休眠：
        // 留一条极淡的细条，点它把岛叫回来。
        // 这里用 store 里的活动数判断「有没有岛」—— 全屏避让时 Tick 返回的
        // frame.visible 是空的，但活动仍然保留在 store 里。
        auto live_count = [&]() {
            int n = 0;
            for (const auto& a : demo.runtime->Store().All()) {
                if (a.state == diw::ActivityState::Ended ||
                    a.state == diw::ActivityState::Dismissed) {
                    continue;
                }
                ++n;
            }
            return n;
        };
        auto has_island = [&](const Frame& f) {
            return !f.visible.empty() || (demo.suspended && live_count() > 0);
        };
        // 前台窗口一换（打开新界面 / 切到别的应用）就让路：
        // 岛挂在那里正好压在新窗口的标题栏上，是最碍事的时候。
        // 用户点一下休眠条就能叫回来，直到下一次切窗口。
        {
            HWND fg = ::GetForegroundWindow();
            if (fg != nullptr && fg != hwnd) {
                if (last_foreground != nullptr && fg != last_foreground) {
                    last_interaction_ms = -1000000;  // 视为「刚换了界面」→ 立刻睡
                }
                last_foreground = fg;
            }
        }
        auto is_sleeping = [&](const Frame& f) {
            if (!has_island(f)) return false;
            if (demo.suspended) return true;  // 全屏 / 避让
            const std::int64_t auto_ms =
                opt.auto_sleep_after_ms > 0
                    ? opt.auto_sleep_after_ms
                    : static_cast<std::int64_t>(rules.Config().auto_sleep_sec) * 1000;
            return auto_ms > 0 && now_ms - last_interaction_ms >= auto_ms;
        };

        // 双击：切换一览 —— 所有并行任务各自铺成一个独立岛。
        // 与展开互斥（一览的意图就是"看全部"，不是"放大一个"）。
        // 只有 1 件任务时「铺开」没有意义：此时双击按「展开那一件」处理。
        // 以前是无条件 overview=true 再被 <2 的检查撤销，但 expand_id 已经
        // 被清掉了 —— 于是双击的结果是「什么都没发生，原来展开的还收起来了」。
        if (g_overview_req) {
            g_overview_req = false;
            // 同 g_click：先判断睡没睡，再刷新交互时间（顺序反了 slept 恒为 false）
            const bool slept = is_sleeping(demo.runtime->LastResult().frame);
            last_interaction_ms = now_ms;
            const Frame& f = demo.runtime->LastResult().frame;
            const std::size_t live_n = f.visible.size();
            const char* what = "";
            if (demo.overview) {
                demo.overview = false;
                what = "off (dblclick)";
            } else if (slept) {
                what = "wake (dblclick on sleep bar)";
            } else if (live_n >= 2) {
                demo.overview = true;
                demo.expand_id.clear();
                what = "on";
            } else if (live_n == 1) {
                // 只有一件：双击 = 展开它，别让用户白点一下
                demo.expand_id = f.visible.front().activity.id;
                what = "on (single task -> expand)";
            } else {
                what = "ignored (no task)";
            }
            note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] overview=" + what);
        }

        // 手动点击：一览中点在岛上 = 拉起那个应用，点在空白处 = 收一览；
        // 非一览则在当前主活动上切换展开/收起
        // 点小圆：展开它代表的那一件，而不是主胶囊。
        // 这一段必须排在 g_click 之前 —— 两件事如果都吃掉这次点击，
        // 就会出现「点小的顺手把大的也展开」。
        if (g_detach_click >= 0) {
            const int idx = g_detach_click;
            g_detach_click = -1;
            last_interaction_ms = now_ms;
            if (idx >= 0 && idx < static_cast<int>(detached_ids.size()) && !detached_ids[idx].empty()) {
                demo.expand_id = detached_ids[idx];
                demo.overview = false;
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] click detached #" +
                     std::to_string(idx) + " -> expand " + demo.expand_id);
            }
        }
        if (g_click) {
            g_click = false;
            // 必须先判断「刚才是不是睡着的」再刷新交互时间：
            // 先刷新的话 now - last_interaction 立刻变成 0，is_sleeping 恒为 false，
            // 这一下点击就被当成普通点击，直接把岛展开了 —— 实测就是这样翻车的。
            const bool clicked_while_sleeping = is_sleeping(demo.runtime->LastResult().frame);
            last_interaction_ms = now_ms;
            if (clicked_while_sleeping) {
                // 点在休眠条上：先把岛「蹦出来」（紧凑态），不直接展开 ——
                // 直接展开会一下子铺满半个屏幕，比原来更突兀。
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] wake: click on sleep bar");
            } else if (demo.overview) {
                const int hit = OverviewHitIndex(g_click_pt, scale);
                const Frame& of = demo.runtime->LastResult().frame;
                const bool is_agent =
                    hit >= 0 && hit < static_cast<int>(of.visible.size()) &&
                    of.visible[hit].activity.kind.rfind("agent", 0) == 0;
                const bool has_target =
                    hit >= 0 && hit < static_cast<int>(overview_launch.size()) &&
                    !overview_launch[hit].empty();
                if (is_agent) {
                    // Agent 岛点开的是「详情」而不是「应用」：它的宿主窗口
                    // 可能就是这个终端，用户想看的是它在干什么。
                    // 要跳到窗口，展开态里再点标题。
                    demo.expand_id = of.visible[hit].activity.id;
                    demo.overview = false;
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] overview -> expand " +
                         demo.expand_id);
                } else if (hit >= 0 && has_target) {
                    const bool ok = LaunchApp(overview_launch[hit]);
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] overview launch #" +
                         std::to_string(hit) + " target=" + overview_launch[hit] +
                         (ok ? " ok" : " FAILED"));
                } else if (hit < 0) {
                    demo.overview = false;
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] overview=off (click)");
                }
                // 点在岛上但没有可拉起目标：不动作，一览保持
            } else if (demo.expand_id.empty()) {
                const Frame& f = demo.runtime->LastResult().frame;
                if (!f.visible.empty()) demo.expand_id = f.visible.front().activity.id;
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] click expand=" +
                     (demo.expand_id.empty() ? std::string("off") : demo.expand_id));
            } else {
                demo.expand_id.clear();
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] click expand=off");
            }
        }

        // ---- 展开态按钮 / 拖动进度：窗口线程投递，这里统一消费 ----
        if (g_seek_req) {
            g_seek_req = false;
            // 时长与 app_id 都取自「屏上那条活动」：多播放器并行时 SMTC 的
            // 当前会话可能已经换成别人，拿 snap.media 会把进度发给另一个播放器。
            std::int64_t end_ms = 0;
            std::string seek_app;
            const Frame& sf = demo.runtime->LastResult().frame;
            const Activity* seek_act = LeadOf(sf, demo.expand_id);
            if (seek_act != nullptr && seek_act->kind.rfind("media", 0) == 0) {
                end_ms = JsonInt(seek_act->content_json, "end_ms");
                seek_app = JsonString(seek_act->content_json, "app_id");
            }
            if (end_ms <= 0 && opt.seek_end_ms > 0) end_ms = opt.seek_end_ms;
            if (end_ms > 0) {
                const std::int64_t target =
                    static_cast<std::int64_t>(g_seek_progress * static_cast<double>(end_ms));
                sources.SeekToMs(target, seek_app);
                // 先让进度条停在目标位置，避免「松手立刻弹回」被当成没拖动。
                seek_hold_progress = g_seek_progress;
                seek_hold_until_ms = now_ms + 1500;
                note("seek: " + std::to_string(target) + "ms / " +
                     std::to_string(end_ms) + "ms app=" +
                     (seek_app.empty() ? std::string("(current)") : seek_app));
            } else {
                note("seek: ignored (no duration)");
            }
        }
        if (g_play_req) {
            g_play_req = false;
            std::string play_app;
            const Frame& pf = demo.runtime->LastResult().frame;
            if (!pf.visible.empty()) {
                const Activity* play_act = LeadOf(pf, demo.expand_id);
                play_app = play_act == nullptr ? std::string()
                                               : JsonString(play_act->content_json, "app_id");
            }
            sources.TogglePlayPause(play_app);
            note("media: toggle play/pause app=" +
                 (play_app.empty() ? std::string("(current)") : play_app));
        }
        if (g_launch_req) {
            g_launch_req = false;
            last_interaction_ms = now_ms;
            const Frame& f = demo.runtime->LastResult().frame;
            if (!f.visible.empty()) {
                const auto& act = *LeadOf(f, demo.expand_id);
                if (act.kind.rfind("agent", 0) == 0) {
                    // Agent 不是「打开应用」，而是「切到它在的那个窗口」。
                    // 找不到窗口时**不再退化成打开项目目录**：用户点的是岛，
                    // 期待的是回到那个会话，而不是让资源管理器弹出来盖住半个屏幕
                    //（实机反馈："agent 点击进去是文件夹"）。找不到就什么都不做，
                    // 只记一行日志，让人能从日志判断是标题匹配不上还是真没窗口。
                    const std::string project = act.title;
                    const std::string provider = JsonString(act.content_json, "provider");
                    const bool focused = FocusAgentWindow(hwnd, project, provider);
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] focus agent \"" +
                         project + "\"" + (focused ? " ok" : " (no matching window, no-op)"));
                    last_interaction_ms = now_ms;
                } else if (act.kind.rfind("editor", 0) == 0) {
                    // 编辑器：把它拉回前台。找窗口用「exe 名」而不是文件名 ——
                    // 文件名每个标签页都不同，而我们要找的是"那个编辑器"。
                    const std::string exe = JsonString(act.content_json, "exe");
                    const bool ok = FocusEditorWindow(hwnd, exe);
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] focus editor " +
                         (exe.empty() ? std::string("?") : exe) + (ok ? " ok" : " FAILED"));
                    last_interaction_ms = now_ms;
                } else if (act.kind.rfind("browser", 0) == 0) {
                    // 浏览器：把它拉回前台。不「打开应用」—— 它本来就开着，
                    // 用户要的是回到那个窗口，而不是再开一个。
                    const std::string label = JsonString(act.content_json, "label");
                    const bool ok = FocusBrowserWindow(hwnd, label);
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] focus browser " +
                         label + (ok ? " ok" : " FAILED"));
                    last_interaction_ms = now_ms;
                } else {
                    // 剪贴板带完整 exe 路径；媒体 / 通知只有 app_id（AUMID 或进程名）。
                    std::string target = JsonString(act.content_json, "app_path");
                    if (target.empty()) target = JsonString(act.content_json, "app_id");
                    const bool ok = LaunchApp(target);
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] launch \"" +
                         act.title + "\" target=" + (target.empty() ? "(none)" : target) +
                         (ok ? " ok" : " FAILED"));
                }
            }
        }
        if (g_close_req) {
            g_close_req = false;
            const Frame& f = demo.runtime->LastResult().frame;
            if (!f.visible.empty()) {
                const auto& lead = *LeadOf(f, demo.expand_id);
                const std::string app_id = JsonString(lead.content_json, "app_id");
                const std::string fp = lead.kind + "|" + app_id + "|" + lead.title;
                bool replaced = false;
                for (auto& d : dismissed) {
                    if (d.fp != fp) continue;
                    d.until_ms = now_ms + 90 * 1000;
                    replaced = true;
                }
                if (!replaced) {
                    if (dismissed.size() >= 8) dismissed.erase(dismissed.begin());
                    // 90 秒：叉掉之后不会立刻弹回来，但也绝不「永久消失」。
                    // 之前是 30 分钟 —— 用户「关掉岛 → 再打开同一个播放器」
                    // 会发现岛再也不出现，只能靠杀进程解决。
                    dismissed.push_back({fp, now_ms + 90 * 1000});
                }
                // 媒体还要告诉采集层「这个应用的这一条被叉掉了」：
                // 采集层据此把当前会话让给别的应用 —— 否则叉掉 A 再打开 B 时，
                // B 因为在暂停中抢不到「当前会话」，界面上一直没有新岛。
                if (lead.kind == "media" && !app_id.empty()) {
                    sources.SuppressMedia(app_id, lead.title);
                }
                demo.runtime->EndActivity(lead.id);
                note("close: dismissed \"" + lead.title + "\"");
            }
            demo.expand_id.clear();
            demo.overview = false;
        }

        // 自动展开：与「左键点一下」走同一条路径（设置 expand_id），不绕过状态机。
        // 一次性触发：否则关掉活动后 expand_id 变空，它会立刻又把新活动展开，
        // 回归日志里全是「auto expand」，反而盖掉真正要看的东西。
        if (expand_at >= 0 && now_ms >= expand_at && demo.expand_id.empty()) {
            const Frame& last_frame = demo.runtime->LastResult().frame;
            if (!last_frame.visible.empty()) {
                expand_at = -1;
                demo.expand_id = last_frame.visible.front().activity.id;
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] auto expand=" + demo.expand_id);
            }
        }

        // 自动一览：等价于用户双击一下（视觉回归用）。
        if (overview_at >= 0 && now_ms >= overview_at) {
            overview_at = -1;  // 一次性触发
            demo.overview = !demo.overview;
            demo.expand_id.clear();
            note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] auto overview=" +
                 (demo.overview ? "on" : "off"));
        }

        // 回归注入：模拟左键点一览里第 N 个岛的中心。坐标按当前 scale 换算，
        // 与 OverviewHitIndex 同一套几何，等价于真实鼠标落在岛上。
        // --click-detached 与 --click-at 共用同一个时间点，二者互斥：
        // 否则会先按「点岛」处理一次、再按「点小圆」处理一次，
        // 第二次点击又把刚展开的岛收起来。
        if (click_at >= 0 && now_ms >= click_at && opt.click_detached < 0) {
            click_at = -1;  // 一次性触发
            const double m = Layout::kMargin * scale;
            const double step = (Layout::kCompactW + Layout::kGap) * scale;
            const double cx = m + opt.click_island * step + Layout::kCompactW * scale / 2.0;
            const double cy = m + Layout::kCompactH * scale / 2.0;
            g_click_pt = {static_cast<short>(std::lround(cx)), static_cast<short>(std::lround(cy))};
            g_click = true;
            note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] inject click island=" +
                 std::to_string(opt.click_island) + " pt=(" + std::to_string(g_click_pt.x) + "," +
                 std::to_string(g_click_pt.y) + ") scale=" + std::to_string(scale));
        }

        // 回归注入：点主胶囊旁边的小圆。走真实的命中矩形，证明「点小的展开小的」。
        if (pending_detach >= 0 && now_ms >= click_at) {
            const std::size_t idx = static_cast<std::size_t>(pending_detach);
            pending_detach = -1;  // 一次性
            if (idx < g_hit.detached.size()) {
                const BoxD& b = g_hit.detached[idx];
                g_click_pt = {static_cast<short>(std::lround(b.x + b.w * 0.5)),
                              static_cast<short>(std::lround(b.y + b.h * 0.5))};
                g_detach_click = static_cast<int>(idx);
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] inject click detached=" +
                     std::to_string(idx) + " pt=(" + std::to_string(g_click_pt.x) + "," +
                     std::to_string(g_click_pt.y) + ")");
            } else {
                note("[t=+" + std::to_string(elapsed).substr(0, 5) +
                     "s] inject click detached=" + std::to_string(idx) + " (no such circle, n=" +
                     std::to_string(g_hit.detached.size()) + ")");
            }
        }

        // 回归注入：模拟一次显示器/DPI 变化，验证几何重排与玻璃 epoch 作废。
        if (display_at >= 0 && now_ms >= display_at) {
            display_at = -1;  // 一次性触发
            g_display_changed = true;
            note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] inject display change");
        }

        // 回归注入：向自己投递真实的鼠标消息序列，走完整 WndProc 命中测试。
        // 比直接置 g_close_req 更可信 —— 验证的是「按钮画在哪就能点在哪」。
        // dbl / tap 针对「紧凑态 / 休眠条」这种没有按钮几何的状态，
        // 所以不要求 g_hit.expanded；坐标取胶囊（或休眠条）中心。
        const bool capsule_target = opt.mouse_target == "dbl" || opt.mouse_target == "tap";
        if (mouse_at >= 0 && now_ms >= mouse_at && (g_hit.expanded || capsule_target)) {
            mouse_at = -1;
            auto center = [](const BoxD& b) {
                return POINT{static_cast<LONG>(b.x + b.w * 0.5), static_cast<LONG>(b.y + b.h * 0.5)};
            };
            POINT down = center(g_hit.progress);
            POINT up = down;
            if (opt.mouse_target == "close") {
                down = up = center(g_hit.close);
            } else if (opt.mouse_target == "play") {
                down = up = center(g_hit.play);
            } else if (opt.mouse_target == "app") {
                down = up = center(g_hit.app);  // 点「图标 + 标题」= 打开对应应用
            } else if (capsule_target) {
                down = up = POINT{static_cast<LONG>(surface.Width() / 2),
                                  static_cast<LONG>(std::lround(Layout::kMargin * scale +
                                                                anim.h * scale * 0.5))};
            } else {  // progress：从 10% 拖到指定位置
                down = POINT{static_cast<LONG>(g_hit.progress.x + g_hit.progress.w * 0.10),
                             center(g_hit.progress).y};
                up = POINT{static_cast<LONG>(g_hit.progress.x + g_hit.progress.w *
                                             std::clamp(opt.mouse_progress, 0.0, 1.0)),
                           center(g_hit.progress).y};
            }
            ::PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(down.x, down.y));
            if (opt.mouse_target == "progress") {
                ::PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(up.x, up.y));
            }
            ::PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(up.x, up.y));
            if (opt.mouse_target == "dbl") {
                // 真实双击的第二击是 WM_LBUTTONDBLCLK（类里有 CS_DBLCLKS）
                ::PostMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(down.x, down.y));
                ::PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(up.x, up.y));
            }
            note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] inject mouse=" +
                 opt.mouse_target + " (" + std::to_string(down.x) + "," +
                 std::to_string(down.y) + ") -> (" + std::to_string(up.x) + "," +
                 std::to_string(up.y) + ")");
        }

        // 第二次启动：让已有实例亮一下，顺便告诉用户怎么退出。
        // 单看「防止两个胶囊重叠」也成立，但真正的价值是给用户一个
        // 在胶囊隐藏状态下「找到它、关掉它」的入口。
        if (g_presence) {
            g_presence = false;
            // 同时当作「恢复被我叉掉的提示」的入口：
            // 岛被叉掉后是完全不显示的，用户没有任何可点的地方去反悔，
            // 只能等抑制超时或换歌。双击一次 exe 正好是「找它」的直觉动作，
            // 顺手把抑制全清了，比让用户猜多久恢复靠谱得多。
            // 注意要**无条件**清采集层：主循环侧的 dismissed 记录在「这条已经
            // 不出现在数据里」时就被删了，此时真正压着它的是采集层的抑制。
            dismissed.clear();
            sources.ClearSuppressedMedia();
            note("[t=+" + std::to_string(elapsed).substr(0, 5) +
                 "s] presence: cleared dismissed (hidden) items");
            diw::Activity hint;
            hint.id = "live-presence";
            hint.kind = "presence";
            hint.title = "灵动岛正在运行";
            hint.content_json = "{\"body\":\"左键展开 · 右键可设置或退出\"}";
            hint.relevance_score = 95.0;
            hint.created_at_ms = now_ms;
            hint.stale_at_ms = now_ms + 4500;
            demo.runtime->Store().RemoveIfStale(hint.id);
            if (demo.runtime->Store().Get(hint.id) == nullptr) {
                std::string err;
                if (StartActivitySafe(*demo.runtime, hint, &err)) {
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] presence hint shown");
                } else {
                    note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] !skip presence: " + err);
                }
            }
        }

        // 外部推送：先落地，再让 Tick 把它算进这一帧 —— 否则会白等一帧才显示。
        std::vector<diw::ipc::Request> ipc_requests;
        ipc.TakeAll(&ipc_requests);
        for (const auto& request : ipc_requests) {
            if (request.type == diw::ipc::Request::Type::End) {
                demo.runtime->EndActivity(request.activity.id);
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] [ipc] -" +
                     request.activity.id);
                continue;
            }
            diw::Activity incoming = request.activity;
            if (incoming.created_at_ms == 0) incoming.created_at_ms = now_ms;
            // 没给过期时间就按 8 秒算：外部推送绝大多数是「通知型」事件，
            // 不设上限它会永远占着主位（Activity 的 stale 语义就是干这个的）。
            if (!incoming.stale_at_ms.has_value()) incoming.stale_at_ms = now_ms + 8000;
            std::string err;
            if (StartActivitySafe(*demo.runtime, incoming, &err)) {
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] [ipc] +" + incoming.kind +
                     " 「" + incoming.title + "」");
            } else {
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] [ipc] !skip " +
                     incoming.kind + ": " + err);
            }
        }

        const Frame frame = demo.runtime->Tick(now_ms, demo.expand_id).frame;

        // 展开目标已经退场（过期 / 被更高优先级抢占）时，必须把展开意图一起清掉。
        // 否则残留的 expand_id 会一直非空，下一条同类活动插入后立刻被下面的
        // Expand 事件铺开 —— 表现是「点开过一次之后，之后每条通知都直接满屏展开」。
        // 实测日志：第二次复制没有经历 Compact，直接 state=Expanded。
        if (!demo.expand_id.empty()) {
            bool expand_alive = false;
            for (const auto& v : frame.visible) {
                if (v.activity.id == demo.expand_id) {
                    expand_alive = true;
                    break;
                }
            }
            if (!expand_alive) {
                note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] expand target gone: " +
                     demo.expand_id);
                demo.expand_id.clear();
            }
        }

        // 新内容出现也算一次「交互」：睡着时来了新通知 / 换了歌，岛应该醒过来。
        // 否则用户会以为通知没提示 —— 其实是被一条细条挡在后面。
        // 全屏避让不受影响：is_sleeping 对 suspended 直接返回 true。
        {
            const std::string lead_fp =
                frame.visible.empty()
                    ? std::string()
                    : (frame.visible.front().activity.id + "|" + frame.visible.front().activity.title);
            if (lead_fp != last_lead_fp) {
                last_lead_fp = lead_fp;
                if (!lead_fp.empty()) last_interaction_ms = now_ms;
            }
        }

        // 展开/收起通过显式事件驱动状态机，保证转移表被真实使用而非绕过
        auto& machine = demo.runtime->StateMachine();
        if (!demo.expand_id.empty() && machine.State() != CapsuleState::Expanded &&
            machine.Can(CapsuleEvent::Expand)) {
            machine.Send(CapsuleEvent::Expand);
        } else if (demo.expand_id.empty() && machine.State() == CapsuleState::Expanded &&
                   machine.Can(CapsuleEvent::Collapse)) {
            machine.Send(CapsuleEvent::Collapse);
        }
        const CapsuleState state = machine.State();

        // 「没有岛」才算隐藏；有岛但睡着只是收成一条细条。
        // 配置关掉 sleep_bar 时，睡着就等于完全隐藏（给「全屏时什么都别显示」的人留的口子）。
        const bool want_sleep = is_sleeping(frame);
        const bool bar_enabled = rules.Config().sleep_bar;
        const bool sleeping = want_sleep && bar_enabled;
        const bool hidden = !has_island(frame) || (want_sleep && !bar_enabled);

        // 「没有任务」也要能看出它还在跑：给一枚与休眠条同高的小圆（直径 =
        // kSleepBarH），材质与休眠条一致。没有它，胶囊完全透明时用户无法区分
        // "没事可显示"和"程序已经退了"——这是实机试用里最直接的困惑。
        // 跟随 sleep_bar 开关：明确关掉"待机提示"的人，同样不希望屏幕上留一个点。
        // 具体的尺寸覆盖在 view 构建之后做（view 在这里还没声明）。
        const bool alive_dot = hidden && bar_enabled;
        // 入睡时把展开 / 一览收掉：醒来应该是紧凑态，而不是「一睁眼就是半个屏幕」。
        if (sleeping != was_sleeping) {
            if (sleeping) {
                demo.expand_id.clear();
                demo.overview = false;
            } else {
                last_interaction_ms = now_ms;  // 被动唤醒（新事件）也算一次交互
            }
            note(std::string("[t=+") + std::to_string(elapsed).substr(0, 5) + "s] sleep=" +
                 (sleeping ? "on" : "off") +
                 (sleeping ? (demo.suspended ? " (fullscreen)"
                                             : (last_interaction_ms < 0 ? " (new window)" : " (idle)"))
                           : (hidden ? " (no island)" : std::string())));
            was_sleeping = sleeping;
        }
        // 一览只在「确实有多件事」时有意义：活动退到只剩一件就自动收起，
        // 否则用户关掉一条消息后屏幕上会一直摊着一条孤岛。
        if (demo.overview && (frame.visible.size() < 2 || sleeping)) {
            demo.overview = false;
            note("[t=+" + std::to_string(elapsed).substr(0, 5) + "s] overview=off (fewer tasks)");
        }
        const bool lyrics_on = rules.Config().lyrics && (!opt.scripted || has_fixture);
        // 先问一句「到底有没有歌词」，再决定展开态要不要留歌词区。
        const bool lyric_ready =
            has_fixture ? !lyric_fixture.empty() : (lyrics_on && lyric_fetcher.Status() == "ok");
        CapsuleView view =
            BuildView(frame, state, demo.bridge_online, demo.suspended, elapsed * 6.0, opt.hud, blocked_shown,
                      opt.force_cover, demo.overview, sleeping, lyrics_on, lyric_ready,
                      rules.Config().simplify_chinese, demo.expand_id);
        // 外观两档直接来自用户配置：透明度决定本体怎么画（透/玻璃/实心），
        // 位置在 anchor_x() 里决定窗口停在哪。
        view.opacity_mode = g_force_opacity >= 0 ? g_force_opacity : rules.Config().opacity;
        // 采集层的真实进度（>=0 才代表这个源有时长）。必须在这里取：
        // 下面的拖动预览会覆盖 view.progress，用被覆盖后的值判断会把
        // 「正在拖」误判成「有时长」，松手后命中区来不及失效。
        if (opt.no_duration) view.progress = -1.0;  // 回归：模拟无时长源
        const bool has_progress = view.progress >= 0.0;

        // 歌词：100ms 节流取一次当前三行，帧间复用缓存。
        if (view.lyric_active) {
            if (now_ms - last_lyric_fill_ms >= 100) {
                last_lyric_fill_ms = now_ms;
                const std::int64_t pos_ms = has_fixture
                    ? (opt.lyric_pos_ms >= 0 ? opt.lyric_pos_ms : now_ms)
                    : (frame.visible.empty()
                           ? 0
                           : JsonInt(LeadOf(frame, demo.expand_id) == nullptr
                                         ? std::string()
                                         : LeadOf(frame, demo.expand_id)->content_json,
                                     "position_ms"));
                const std::vector<diw::win32::LyricLine> lines =
                    has_fixture ? lyric_fixture : lyric_fetcher.Lines();
                const auto triple = diw::win32::SelectLyricLines(lines, pos_ms);
                lyric_prev_cache = triple.prev;
                lyric_cur_cache = triple.cur;
                lyric_next_cache = triple.next;
                lyric_status_cache = has_fixture ? "ok" : lyric_fetcher.Status();
                // 状态一变就记一行：失败时带上代理/直连各自的原因，
                // 否则界面上只有一句「获取失败」，日志里查不到为什么。
                if (!has_fixture && lyric_status_cache != last_lyric_status) {
                    last_lyric_status = lyric_status_cache;
                    note("lyrics: status=" + lyric_status_cache +
                         (lyric_status_cache == "error"
                              ? " (" + lyric_fetcher.LastError() + ")"
                              : "") +
                         " lines=" + std::to_string(lines.size()));
                }
            }
            view.lyric_prev = lyric_prev_cache;
            view.lyric_cur = lyric_cur_cache;
            view.lyric_next = lyric_next_cache;
            view.lyric_status = lyric_status_cache;
        }
        // 拖动中本地预览；松手后保持目标位置 1.5s，等播放器真的 seek 过去。
        if (g_drag_progress >= 0.0 && view.expanded) {
            view.progress = g_drag_progress;
        } else if (view.expanded && seek_hold_progress >= 0.0) {
            if (now_ms < seek_hold_until_ms) {
                view.progress = seek_hold_progress;
            } else {
                seek_hold_progress = -1.0;
            }
        }
        last_visible_count = static_cast<int>(frame.visible.size());
        overview_launch.clear();
        // 点击发生在两帧之间，用小圆列表的快照做路由（与 overview_launch 同思路）。
        detached_ids.clear();
        for (const auto& d : view.detached_items) detached_ids.push_back(d.activity_id);
        // 回归注入：--overview-launch 覆盖每个岛的目标，用于验证「点岛 → 拉起应用」
        // 全链路（剧本模式的 content_json 本来没有 app_id）。不传时走真实目标。
        for (const auto& item : view.items) {
            overview_launch.push_back(opt.overview_launch.empty() ? item.launch : opt.overview_launch);
        }
        // 休眠时钟常驻：无活动也不把 alpha 目标压到 0。
        // 存活态：把胶囊缩成一枚与休眠条同高的圆点（正方形 + DrawPill 的
        // min(h*0.5, 28) 圆角 = 天然正圆），内容层整段跳过。
        if (alive_dot) {
            view.alive_dot = true;
            view.content_w = Layout::kSleepBarH;
            view.content_h = Layout::kSleepBarH;
        }
        const bool want_visible = !hidden || view.sleep_bar || alive_dot;
        const bool anim_moved =
            anim.Approach(view.content_w, view.content_h, view.detached_w,
                          want_visible ? 1.0 : 0.0, std::min(dt, 0.1));

        if (anim.alpha > 0.004) {
            hidden_presented = false;
            const float m = static_cast<float>(Layout::kMargin * scale);
            const float detach = anim.detached > 1.0 ? static_cast<float>(anim.detached) : 0.0f;
            const float group_w = static_cast<float>(anim.w) * static_cast<float>(scale) +
                                  (detach > 0.0f ? static_cast<float>(Layout::kGap) * static_cast<float>(scale) +
                                                       detach * static_cast<float>(scale)
                                                 : 0.0f);
            const int canvas_w = static_cast<int>(std::lround(group_w + m * 2.0f));
            const int canvas_h = static_cast<int>(std::lround(
                anim.h * scale + m * 2.0f + (opt.hud ? Layout::kHudHeight * scale : 0.0f)));

            surface.EnsureSize(canvas_w, canvas_h);
            surface.Clear();

            CapsuleView drawn = view;
            drawn.content_w = anim.w;
            drawn.content_h = anim.h;
            drawn.detached_w = detach;
            // 休眠态整体半透明：它不是要你看的内容，只是一条待机提示。
            // 存活点用极慢的呼吸（wave_phase = elapsed*6，再乘 0.35 → 约 3s 一周期）：
            // 完全静止的 10px 圆点容易被当成屏幕上的坏点，会呼吸的不会。
            // 范围压在 kSleepBarAlpha 的 0.72~1.0 倍，肉眼是"若隐若现"而不是明暗闪烁。
            const float alive_breath =
                view.alive_dot
                    ? (0.72f + 0.28f *
                                   static_cast<float>(0.5 + 0.5 * std::sin(view.wave_phase * 0.35)))
                    : 1.0f;
            drawn.alpha = anim.alpha * (view.alive_dot
                                            ? Layout::kSleepBarAlpha * alive_breath
                                            : (view.sleep_bar ? Layout::kSleepBarAlpha : 1.0f));

            // 命中几何用「本帧真正画出来的」胶囊尺寸，和 renderer 同源函数，
            // 保证按钮画在哪就能点在哪。窗口消息与主循环同线程，无需加锁。
            g_hit.expanded = drawn.expanded && !drawn.sleep_bar;
            g_hit.detached.clear();
            if (!g_hit.expanded && !drawn.sleep_bar) {
                // 与 renderer 的布局公式逐字一致：圆固定 46、间距 kGap、
                // 主胶囊宽度用「本帧真正画出来的」anim.w，动画中途也不会错位。
                const float dsize = static_cast<float>(Layout::kDetachedSize * scale);
                const float pill_right = m + static_cast<float>(drawn.content_w * scale);
                const float dsize_f = dsize;
                const float dy =
                    m + (static_cast<float>(drawn.content_h * scale) - dsize_f) * 0.5f;
                for (std::size_t i = 0; i < drawn.detached_items.size(); ++i) {
                    const float gap = static_cast<float>(Layout::kGap * scale);
                    const float dx =
                        pill_right + gap + static_cast<float>(i) * (dsize + gap);
                    g_hit.detached.push_back(BoxD{dx, dy, dsize, dsize});
                }
            }
            if (g_hit.expanded) {
                const float pill_xy = static_cast<float>(Layout::kMargin * scale);
                const Gdiplus::RectF pill(pill_xy, pill_xy,
                                          static_cast<float>(drawn.content_w * scale),
                                          static_cast<float>(drawn.content_h * scale));
                const auto areas = diw::win32::ExpandedHitAreas(pill, static_cast<float>(scale));
                g_hit.close = ToBox(areas.close);
                g_hit.play = ToBox(areas.play);
                g_hit.progress = ToBox(areas.progress);
                g_hit.app = ToBox(areas.app);
                g_hit.progress_draggable = has_progress;
            }

            // ---- 玻璃背景维护 ----
            // 出现 / 重新出现：胶囊此刻还不可见（上一帧 alpha=0），截到的就是
            // 干净背景，作为 epoch。可见期间每 2s 刷新一次，胶囊挡住的区域
            // 用 epoch 修补 —— 全程不需要"隐藏一帧再截"，无闪烁。
            // 休眠把手用纯色半透明，不需要背景模糊：省掉每 2s 一次的抓屏。
            const bool glass_on = rules.Config().glass && !view.sleep_bar;
            if (glass_on) {
                // 画布尺寸变了（2 个岛变 3 个、展开/收起）也必须重拍 epoch：
                // 旧 epoch 的覆盖范围是上一次的窗口大小，修补当前更大的窗口时
                // 右侧贴不到干净像素，只能拿抓屏结果顶上 —— 那里面有胶囊自己，
                // 表现为「一半玻璃、一半漆黑」。实测就是这么发现的。
                const bool size_changed = canvas_w != glass_canvas_w || canvas_h != glass_canvas_h;
                const bool need_epoch = (glass_current == nullptr) || was_hidden || size_changed;
                const bool need_refresh = !need_epoch && now_ms - glass_last_ms >= 2000;
                if (need_epoch || need_refresh) {
                    // 修补矩形按「当前实际窗口」算：可见期间刷新时，胶囊自己
                    // 会出现在抓屏结果里，必须用 epoch 的干净像素把它抹掉。
                    const int win_x = anchor_x(canvas_w);
                    Backdrop* target = need_epoch ? &glass_epoch : &glass_refresh;
                    // 修补区比画布再向外扩一圈：正好卡在画布边界上的话，
                    // 模糊后边界两边会差半个像素，界面上就是一条竖直的浅色拼接线
                    // （Agent 面板右边缘实测能看到）。扩出去之后边界落在看不见的地方。
                    const int patch_pad = static_cast<int>(std::lround(Layout::kMargin * scale));
                    const bool ok =
                        target->Capture(glass_x, glass_y, glass_w, glass_h,
                                        win_x - glass_x - patch_pad, screen_top - glass_y - patch_pad,
                                        canvas_w + patch_pad * 2, canvas_h + patch_pad * 2,
                                        need_epoch ? nullptr : &glass_epoch);
                    glass_last_ms = now_ms;  // 失败也别每帧重试，下次整点再试
                    if (ok) {
                        glass_current = target;
                        glass_canvas_w = canvas_w;
                        glass_canvas_h = canvas_h;
                        char gline[240];
                        std::snprintf(gline, sizeof(gline),
                                      "glass: %s %dx%d at (%d,%d) raw=%.1f blur=%.1f",
                                      need_epoch ? "epoch" : "refresh", glass_w, glass_h, glass_x,
                                      glass_y, target->MeanLuminanceRaw(), target->MeanLuminance());
                        note(gline);
                    } else if (need_epoch) {
                        glass_current = nullptr;  // 无桌面会话等，回落不透明样式
                    }
                }
            } else {
                glass_current = nullptr;
            }
            was_hidden = false;

            GlassParams glass;
            if (glass_current != nullptr) {
                // 液态玻璃要「轻模糊 + 保色」的那份纹理：重模糊 + 去饱和会把它
                // 变成一块均匀灰板，无论再怎么调染色都救不回来。
                Gdiplus::Bitmap* bm = drawn.opacity_mode == 0 ? glass_current->Liquid()
                                                              : glass_current->Blurred();
                if (bm != nullptr) {
                    glass.backdrop = bm;
                    // win_x 是上面那个 if 块里的局部量，这里已经出作用域了。
                    // anchor_x 是纯函数（同一宽度必然同一个结果），重算一次更清楚。
                    glass.offset_x =
                        static_cast<float>(glass_current->X() - anchor_x(canvas_w));
                    glass.offset_y = static_cast<float>(glass_current->Y() - screen_top);
                    glass.full_w = static_cast<float>(glass_current->FullWidth());
                    glass.full_h = static_cast<float>(glass_current->FullHeight());
                }
            }

            {
                // 铬层：命中缓存就把像素抹回来，只重画会变的内容。
                // 两段各用一个 Graphics 作用域，是为了让 GDI+ 在切换前把绘制刷进 DIB
                // —— 否则存下来的"铬层像素"可能还缺最后一笔。
                // 存像素必须发生在**画内容之前**，否则存进去的就是铬层+内容。
                const std::uint64_t key = chrome_key_of(canvas_w, canvas_h, drawn, glass);
                const bool hit = (chrome_key_cached == key) && !chrome_cache.empty();
                ++chrome_calls;
                if (hit) {
                    ++chrome_hits;
                    surface.LoadPixels(chrome_cache);
                } else {
                    {
                        Gdiplus::Graphics g(surface.Dc());
                        set_quality(g);
                        renderer.Draw(g, drawn, static_cast<float>(scale), glass,
                                      CapsuleRenderer::Phase::Chrome);
                    }
                    surface.SavePixels(chrome_cache);
                    chrome_key_cached = key;
                }
                {
                    Gdiplus::Graphics g(surface.Dc());
                    set_quality(g);
                    renderer.Draw(g, drawn, static_cast<float>(scale), glass,
                                  CapsuleRenderer::Phase::Content);
                }
            }

            if (!opt.snapshot_path.empty() && !snapshot_taken && opt.snapshot_at_ms >= 0 &&
                now_ms >= opt.snapshot_at_ms) {
                try {
                    surface.SavePng(Widen(opt.snapshot_path));
                    note("snapshot saved: " + opt.snapshot_path + " at t=+" +
                         std::to_string(elapsed).substr(0, 5) + "s");
                    snapshot_taken = true;
                } catch (const std::exception& e) {
                    note(std::string("snapshot failed: ") + e.what());
                }
            }

            // 休眠把手要真正贴住屏幕顶边：canvas 顶部有 kMargin 的透明留白，
            // 把窗口整体上移这么多，细条的顶边就正好压在 rcWork.top 上。
            const int present_y =
                view.sleep_bar
                    ? monitor.rcWork.top - static_cast<int>(std::lround(Layout::kMargin * scale))
                    : screen_top;
            present(anchor_x(canvas_w), present_y, 255);

            // 桌面实证：从屏幕 DC 抓回「胶囊 + 背景」的实际合成结果。
            // 这一步抓的是显示器内容，不是自己的绘制缓冲，因此能证明窗口真的在桌面上。
            if (!opt.grab_path.empty() && !grab_taken && opt.snapshot_at_ms >= 0 &&
                now_ms >= opt.snapshot_at_ms) {
                try {
                    const int gw = 1200;
                    const int gh = 420;
                    const int gx = anchor_x(gw);
                    const int gy = monitor.rcWork.top;
                    surface.EnsureSize(gw, gh);
                    HDC screen_dc = ::GetDC(nullptr);
                    if (screen_dc == nullptr || !::BitBlt(surface.Dc(), 0, 0, gw, gh, screen_dc, gx, gy, SRCCOPY)) {
                        note("WARN: BitBlt screen grab failed");
                    } else {
                        surface.ForceOpaqueAlpha();
                        surface.SavePng(Widen(opt.grab_path));
                        note("desktop grab saved: " + opt.grab_path);
                    }
                    if (screen_dc != nullptr) ::ReleaseDC(nullptr, screen_dc);
                    grab_taken = true;
                } catch (const std::exception& e) {
                    note(std::string("grab failed: ") + e.what());
                    grab_taken = true;
                }
            }
        } else {
            g_hit.expanded = false;  // 岛已隐藏，别再让旧几何吞掉点击
            g_hit.progress_draggable = false;
            if (!hidden_presented) {
                surface.Clear();
                present(anchor_x(surface.Width()), screen_top, 0);
                hidden_presented = true;
                was_hidden = true;  // 下次出现时重新干净捕获玻璃背景
            }
        }

        // 只在「可观察状态」变化时记一行；时间戳参与文本但不参与比较，否则每帧都会刷一行。
        char key[320];
        std::snprintf(key, sizeof(key), "state=%s visible=%d blocked=%d suppressed=%d expired=%d bridge=%s suspend=%s",
                      StateName(state), static_cast<int>(frame.visible.size()), blocked_shown,
                      frame.suppressed_count, static_cast<int>(frame.expired_ids.size()),
                      demo.bridge_online ? "on" : "off", demo.suspended ? "on" : "off");
        if (last_log != key) {
            char line[512];
            std::snprintf(line, sizeof(line), "[t=+%.2fs] %s", elapsed, key);
            note(line);
            last_log = key;
        }

        if (opt.duration_ms > 0 && now_ms >= opt.duration_ms) break;

        // 帧预算分三档，依据是「这一帧到底有什么在动」：
        //
        //  · 隐藏 / 休眠条：10fps。休眠时钟每分钟才变一次，没有画 60fps 的理由。
        //  · 几何或透明度在动（弹簧、展开、隐去），或媒体在播（律动条要跟拍）：60fps。
        //  · 只剩呼吸点一类的慢动画：20fps。这类动画的频率在 0.86Hz 量级
        //    （wave_phase 以 6 rad/s 推进，再乘 0.9），20fps 下每个周期仍有 24 帧，
        //    肉眼与 60fps 无差别；而单帧 draw 实测 8.5ms，60fps 常驻等于白烧约 30% 单核。
        //
        // 交互后 1.2s 内强制 60fps：点击、拖进度条、悬停高亮都要跟手，
        // 不能让一次点击等 50ms 才在画面上出现。
        const bool interactive = now_ms - last_interaction_ms < 1200;
        const bool fast = anim_moved || view.playing || interactive;
        double frame_budget = 1.0 / 20.0;
        if (view.sleep_bar || hidden) {
            frame_budget = 1.0 / 10.0;
        } else if (fast) {
            frame_budget = 1.0 / 60.0;
        }
        const double used = std::chrono::duration<double>(Clock::now() - now).count();
        if (used < frame_budget) {
            ::Sleep(static_cast<DWORD>((frame_budget - used) * 1000.0));
        }
    }

    // 先停采集线程再收尾：它还在调 WinRT / COM，进程直接退会留下悬挂的异步操作。
    g_sources = nullptr;
    sources.Stop();

    // 托盘图标必须在进程退出前主动删除，否则会在托盘区留一个"幽灵图标"，
    // 直到用户把鼠标划过去才消失。
    TrayRemove(hwnd);

    ipc.Stop();
    note("ipc: accepted " + std::to_string(ipc.Accepted()) + ", rejected " +
         std::to_string(ipc.Rejected()));

    note("clean exit at t=+" +
         std::to_string(std::chrono::duration<double>(Clock::now() - start).count()).substr(0, 5) +
         "s");
    // 跳过提交的比例：静止画面下应接近 100%，动画/播放时接近 0。
    // 这个数字比"感觉流畅"更能说明重绘有没有真的降下来。
    note("present: skipped " + std::to_string(present_skipped) + "/" +
         std::to_string(present_total) + " frames");
    note("chrome: cached " + std::to_string(chrome_hits) + "/" + std::to_string(chrome_calls) +
         " frames");
    if (log) log.close();
    ::DestroyWindow(hwnd);
    return 0;
}

}  // namespace

namespace {

// ---- 硬崩溃现场 ----
// C++ 的 try/catch 抓不到访问违例（SEH 不参与 C++ 栈展开），进程会直接消失，
// 日志里连一行都不会留下。这正是「胶囊莫名不见了」里最难查的那种形态。
// 这里装一个未处理异常过滤器：异常码 + 出错地址追加进**独立**的 crash 日志，
// 另落一份 minidump；两个文件都不参与滚动，不会被下次启动擦掉。
// dbghelp 按需动态加载：只为崩溃路径给发布包引入静态链接依赖不划算。
const char* ExceptionCodeName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        default: return "UNKNOWN";
    }
}

void WriteCrashDump(EXCEPTION_POINTERS* info, const std::wstring& path) {
    HMODULE dbghelp = ::LoadLibraryW(L"dbghelp.dll");
    if (dbghelp == nullptr) return;
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                              PMINIDUMP_EXCEPTION_INFORMATION,
                                              PMINIDUMP_USER_STREAM_INFORMATION,
                                              PMINIDUMP_CALLBACK_INFORMATION);
    auto write_dump =
        reinterpret_cast<MiniDumpWriteDumpFn>(::GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (write_dump != nullptr) {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = ::GetCurrentThreadId();
            mei.ExceptionPointers = info;
            mei.ClientPointers = FALSE;
            write_dump(::GetCurrentProcess(), ::GetCurrentProcessId(), file,
                       static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithHandleData),
                       info != nullptr ? &mei : nullptr, nullptr, nullptr);
            ::CloseHandle(file);
        }
    }
    ::FreeLibrary(dbghelp);
}

void NoteFatal(const char* text) {
    std::ofstream f(ExeDirectory() + "\\diw-overlay.crash.log",
                    std::ios::out | std::ios::app | std::ios::binary);
    if (!f) return;
    f << "===== crash " << std::time(nullptr) << " =====\n" << text << "\n";
    f.flush();
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = (info != nullptr) ? info->ExceptionRecord : nullptr;
    const DWORD code = (record != nullptr) ? record->ExceptionCode : 0;
    const void* address = (record != nullptr) ? record->ExceptionAddress : nullptr;
    char line[256] = {0};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "FATAL(hard): code=0x%08lX (%s) addr=%p",
                static_cast<unsigned long>(code), ExceptionCodeName(code), address);
    NoteFatal(line);
    WriteCrashDump(info, ExeDirectoryWide() + L"\\diw-overlay.crash.dmp");
    // 已经留证，直接终止；不要落到 Windows 错误报告对话框上。
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    ::SetUnhandledExceptionFilter(CrashFilter);
    try {
        return Run();
    } catch (const std::exception& e) {
        // 追加而不是截断：崩溃前的那几行日志是唯一线索，
        // 用 trunc 打开等于自己把现场擦掉。
        std::ofstream f(ExeDirectory() + "\\diw-overlay.log", std::ios::out | std::ios::app);
        f << "FATAL: " << e.what() << "\n";
        NoteFatal((std::string("FATAL(std): ") + e.what()).c_str());
        return 2;
    } catch (...) {
        // 非 std::exception：表现和硬崩溃一样是「静默消失」，必须至少留一行。
        std::ofstream f(ExeDirectory() + "\\diw-overlay.log", std::ios::out | std::ios::app);
        f << "FATAL: unknown exception\n";
        NoteFatal("FATAL(unknown): non-std exception thrown out of Run()");
        return 3;
    }
}
