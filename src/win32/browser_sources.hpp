#pragma once
// 浏览器采集层：把「用户此刻在哪个页面、在下什么」变成岛上的一条活动。
//
// 为什么单独一层：浏览器的信息不在任何统一 API 里 —— SMTC 只有播放中的
// 媒体，通知只有系统弹窗。但浏览器自己把「去过哪个页面」写进了 History，
// 「图标」写进了 Favicons，「下载进度」写进了 downloads 表。这三样合起来
// 足以拼出一个比「Google Chrome」五个字有用得多的岛。
//
// 三条腿，任何一条断了都能降级：
//   1. 窗口标题（GetForegroundWindow + GetWindowText）
//      —— 所有浏览器都有，且是「当前标签页」唯一正确的来源。
//   2. History 反查域名（urls 表按 title 精确匹配）
//      —— 标题里没有域名，而域名才回答「这是哪个站」。
//   3. Favicons 取真实站点图标
//      —— 有了它，岛上的方块是 B 站粉、GitHub 黑、DeepSeek 蓝，
//         而不是又一个一模一样的浏览器 logo。
// 断在哪一层，就退到上一层显示：没有 favicon 用品牌色，没有域名用标题。
//
// 全部只读：不写浏览器任何文件、不联网、不开调试端口。
// 数据库用 immutable 方式打开（浏览器运行期间会独占 History，普通只读会 BUSY）。

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "../core/browser_page.hpp"
#include "sqlite_lite.hpp"

namespace diw {
namespace win32 {

// ---------------------------------------------------------------- 小工具

inline std::wstring BrowserWiden(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline std::string BrowserNarrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

using browser_detail::EndsWith;
using browser_detail::Lower;

// Windows 的 WebKit 时间戳：1601-01-01 起的微秒。浏览器的 downloads / urls
// 表里全是这个纪元，直接当成 epoch ms 比会差出四百年。
inline std::int64_t BrowserWebKitToUnixMs(std::int64_t t) {
    if (t <= 0) return 0;
    return t / 1000 - 11644473600000LL;
}

inline std::int64_t BrowserFileTimeToUnixMs(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart < 116444736000000000ULL) return 0;
    return static_cast<std::int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

inline std::wstring BrowserKnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, 0, nullptr, &raw)) || raw == nullptr) return std::wstring();
    std::wstring out(raw);
    ::CoTaskMemFree(raw);
    return out;
}

inline bool BrowserFileExists(const std::wstring& path) {
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool BrowserDirExists(const std::wstring& path) {
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline std::int64_t BrowserFileMtimeMs(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0) return 0;
    return BrowserFileTimeToUnixMs(data.ftLastWriteTime);
}

// 只读一段字节。浏览器的 History 动辄几十 MB，用不着整个读进来。
inline std::string BrowserReadRange(const std::wstring& path, std::uint64_t offset, std::uint64_t want,
                                    bool* ok) {
    if (ok != nullptr) *ok = false;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) == 0) {
        ::CloseHandle(h);
        return std::string();
    }
    std::string out;
    out.resize(static_cast<std::size_t>(want));
    DWORD got = 0;
    const BOOL read_ok = ::ReadFile(h, out.data(), static_cast<DWORD>(want), &got, nullptr);
    ::CloseHandle(h);
    if (read_ok == 0) return std::string();
    out.resize(got);
    if (ok != nullptr) *ok = true;
    return out;
}

inline std::string BrowserHexDecode(const std::string& hex) {
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() < 2 || (hex.size() % 2) != 0) return std::string();
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = val(hex[i]);
        const int lo = val(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::string();
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

// ---------------------------------------------------------------- 数据

// ---------------------------------------------------------------- 品牌表

// root: 'L' = %LOCALAPPDATA%，'R' = %APPDATA%
// depth: 0 = data_dir 本身就是 profile 目录；1 = data_dir 下的一级子目录才是
// chromium: 是否用 Chromium 的 History/Favicons 结构（Firefox 要用 places.sqlite）
struct BrowserBrand {
    const char* id;
    const char* label;
    const char* exes;      // '|' 分隔，小写
    const char* suffixes;  // '|' 分隔，窗口标题后缀
    char root;
    const char* data_dir;
    int depth;
    bool chromium;
};

inline const BrowserBrand* BrowserBrandTable(int* count) {
    static const BrowserBrand kBrands[] = {
        {"chrome", "Google Chrome", "chrome.exe", "Google Chrome", 'L', "Google\\Chrome\\User Data", 1, true},
        {"edge", "Microsoft Edge", "msedge.exe", "Microsoft Edge|Microsoft Edge Beta|Microsoft Edge Dev",
         'L', "Microsoft\\Edge\\User Data", 1, true},
        {"brave", "Brave", "brave.exe", "Brave", 'L', "BraveSoftware\\Brave-Browser\\User Data", 1, true},
        {"vivaldi", "Vivaldi", "vivaldi.exe", "Vivaldi", 'L', "Vivaldi\\User Data", 1, true},
        {"chromium", "Chromium", "chromium.exe", "Chromium", 'L', "Chromium\\User Data", 1, true},
        {"opera", "Opera", "opera.exe", "Opera", 'R', "Opera Software\\Opera Stable", 0, true},
        {"operagx", "Opera GX", "opera.exe", "Opera GX", 'R', "Opera Software\\Opera GX Stable", 0, true},
        {"firefox", "Firefox", "firefox.exe", "Mozilla Firefox|Firefox", 'R', "Mozilla\\Firefox\\Profiles",
         1, false},
        {"waterfox", "Waterfox", "waterfox.exe", "Waterfox", 'R', "Waterfox\\Profiles", 1, false},
        {"librewolf", "LibreWolf", "librewolf.exe", "LibreWolf", 'R', "librewolf\\Profiles", 1, false},
        {"yandex", "Yandex", "browser.exe", "Yandex", 'L', "Yandex\\YandexBrowser\\User Data", 1, true},
        // 国产壳：都是 Chromium，标题后缀各不相同，很多用户就这么用。
        {"360se", "360 安全浏览器", "360se.exe|360se6.exe", "360安全浏览器|360安全浏览器6", 'L',
         "360Chrome\\Chrome\\User Data", 1, true},
        {"360chrome", "360 极速浏览器", "360chrome.exe", "360极速浏览器", 'L',
         "360Chrome\\Chrome\\User Data", 1, true},
        {"qqbrowser", "QQ 浏览器", "qqbrowser.exe", "QQ浏览器", 'L', "Tencent\\QQBrowser\\User Data", 1,
         true},
        {"sogou", "搜狗高速浏览器", "sogouexplorer.exe", "搜狗高速浏览器", 'L', "SogouExplorer\\User Data", 1,
         true},
        {"2345", "2345 加速浏览器", "2345explorer.exe", "2345加速浏览器", 'L', "2345Explorer\\User Data", 1,
         true},
        {"uc", "UC 浏览器", "ucbrowser.exe", "UC浏览器", 'L', "UCBrowser\\User Data", 1, true},
        {"quark", "夸克", "quarkpc.exe|quark.exe", "夸克", 'L', "Quark\\User Data", 1, true},
        {"arc", "Arc", "arc.exe", "Arc", 'L', "", 0, true},
        {"ie", "Internet Explorer", "iexplore.exe", "Internet Explorer", 'L', "", 0, false},
    };
    if (count != nullptr) *count = static_cast<int>(sizeof(kBrands) / sizeof(kBrands[0]));
    return kBrands;
}

// 品牌识别：先按进程名，再按窗口标题后缀（壳浏览器经常换 exe 名）。
inline const BrowserBrand* BrowserBrandByExe(const std::wstring& exe) {
    int count = 0;
    const BrowserBrand* table = BrowserBrandTable(&count);
    const std::string name = Lower(BrowserNarrow(exe));
    for (int i = 0; i < count; ++i) {
        const std::string list = table[i].exes;
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t bar = list.find('|', start);
            const std::string one = list.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (!one.empty() && one == name) return &table[i];
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
    }
    return nullptr;
}

inline const BrowserBrand* BrowserBrandById(const std::string& id) {
    int count = 0;
    const BrowserBrand* table = BrowserBrandTable(&count);
    for (int i = 0; i < count; ++i) {
        if (id == table[i].id) return &table[i];
    }
    return nullptr;
}

inline const BrowserBrand* BrowserBrandByTitle(const std::string& title) {
    int count = 0;
    const BrowserBrand* table = BrowserBrandTable(&count);
    const std::string lower = Lower(title);
    for (int i = 0; i < count; ++i) {
        const std::string list = table[i].suffixes;
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t bar = list.find('|', start);
            const std::string one = list.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (!one.empty() && EndsWith(lower, Lower(one))) return &table[i];
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
    }
    return nullptr;
}

// 从窗口标题里剥掉浏览器后缀。
// 只在「右段确实等于某个已知品牌名」时才剥 —— 页面标题自己也可能带 " - "
// （"问题 - Stack Overflow"），宽松地从右切会把标题切掉一半。
inline std::string BrowserStripSuffix(const std::string& window_title, const BrowserBrand* brand) {
    if (window_title.empty()) return window_title;
    static const char* kSeparators[] = {" - ", " \xE2\x80\x94 ", " \xE2\x80\x93 ", " | "};
    for (const char* sep : kSeparators) {
        const std::string s(sep);
        if (window_title.size() <= s.size()) continue;
        // 找到最后一个分隔符
        const std::size_t pos = window_title.rfind(s);
        if (pos == std::string::npos || pos + s.size() >= window_title.size()) continue;
        const std::string tail = window_title.substr(pos + s.size());
        bool known = false;
        if (brand != nullptr) {
            const std::string list = brand->suffixes;
            std::size_t start = 0;
            while (start <= list.size()) {
                const std::size_t bar = list.find('|', start);
                const std::string one =
                    list.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                if (!one.empty() && Lower(one) == Lower(tail)) {
                    known = true;
                    break;
                }
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
        }
        if (!known) known = BrowserBrandByTitle(tail) != nullptr;
        if (!known) continue;
        std::string head = window_title.substr(0, pos);
        if (head.empty()) continue;
        return head;
    }
    return window_title;
}

// ---------------------------------------------------------------- 采集器

class BrowserWatcher {
public:
    void SetEnabled(bool on) { enabled_ = on; }
    void SetLogger(std::function<void(const std::string&)> fn) { log_fn_ = std::move(fn); }
    void SetSuppressed(bool on) { suppressed_ = on; }
    void SetMaxPages(int n) { max_pages_ = std::max(1, std::min(n, 2)); }
    // 调试：假装这个品牌在前台。用来把「前台检测」和「数据库链路」分开验证 ——
    // 跑测试时前台往往是终端，不这么做就没法单独验证采集那一半。
    void SetForceBrand(const std::string& id) { force_brand_ = id; }

    void Poll(std::int64_t now_ms) {
        if (!enabled_ || suppressed_) {
            // 只在「从开着变成关着」时报一次：每 100ms 刷一行会把日志淹掉。
            if (!reported_idle_) {
                reported_idle_ = true;
                Log("browser: idle (enabled=" + std::string(enabled_ ? "1" : "0") +
                    " suppressed=" + std::string(suppressed_ ? "1" : "0") + ")");
            }
            std::lock_guard<std::mutex> lock(mutex_);
            pages_.clear();
            return;
        }
        reported_idle_ = false;

        diag_.clear();
        // 前台标记的「离开宽限」：直接清零的话，Alt+Tab 的那一帧岛就消失，
        // 用户想点它永远点不到。宽限 2.5 秒，够点一下、也够读完一行标题。
        constexpr std::int64_t kForegroundGraceMs = 2500;
        for (auto& e : entries_) {
            e.page.foreground = now_ms - e.page.last_fg_ms < kForegroundGraceMs;
        }

        // ---- 1. 前台窗口 ----
        HWND fg = ::GetForegroundWindow();
        std::wstring fg_exe;
        std::string fg_title;
        if (fg != nullptr) {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(fg, &pid);
            fg_exe = ProcessImageName(pid);
            wchar_t buf[512] = {0};
            const int len = ::GetWindowTextW(fg, buf, 512);
            if (len > 0) fg_title = BrowserNarrow(std::wstring(buf, static_cast<std::size_t>(len)));
        }
        const BrowserBrand* brand = BrowserBrandByExe(fg_exe);
        if (brand == nullptr && !fg_title.empty()) brand = BrowserBrandByTitle(fg_title);
        if (brand == nullptr && force_brand_.empty() && !reported_nobrand_) {
            // 「浏览器没出岛」最常见的原因是前台压根不是浏览器，而这件事
            // 界面上完全看不出来。把当前前台是谁打一次，省掉一整轮瞎猜。
            reported_nobrand_ = true;
            Log("browser: foreground is not a browser — exe=" +
                BrowserNarrow(fg_exe) + " title=" + fg_title.substr(0, 40));
        }
        if (brand != nullptr) reported_nobrand_ = false;
        if (!force_brand_.empty()) {
            const BrowserBrand* forced = BrowserBrandById(force_brand_);
            if (forced != nullptr) {
                // 强制模式下前台往往还停在终端：那个窗口标题（"WorkBuddy"）
                // 绝不能被当成标签页标题，否则后面拿它去查 History 必空。
                if (brand != forced) fg_title.clear();
                brand = forced;
            }
        }

        if (brand != nullptr) {
            Entry* e = EntryFor(*brand, now_ms);
            if (e != nullptr) {
                BrowserPage& p = e->page;
                p.brand = brand->id;
                p.label = brand->label;
                p.foreground = true;
                p.last_fg_ms = now_ms;
                p.fullscreen = IsFullscreen(fg);
                p.seen_ms = now_ms;
                if (!fg_title.empty()) {
                    p.title = BrowserCleanTitle(BrowserStripSuffix(fg_title, brand));
                } else if (!force_brand_.empty() && p.title.empty() && !e->history_path.empty()) {
                    // 调试模式没有真实窗口：拿 History 最近一条当标题，
                    // 否则「标题 → URL」这一步永远走不到，等于没验证。
                    SqliteLite db;
                    if (OpenReadOnly(e->history_path, &db)) {
                        db.Query("select title, url from urls where hidden = 0 "
                                 "order by last_visit_time desc limit 1",
                                 {}, [&p](const std::vector<std::string>& row, const std::vector<bool>&) {
                                     if (row.size() > 1 && !row[0].empty()) {
                                         p.title = BrowserCleanTitle(row[0]);
                                         p.url = row[1];
                                     }
                                     return false;
                                 });
                    }
                }
                // 数据库 3 秒一查：改一个页面的时间远长于 3 秒，
                // 而这个库里躺着几十万行历史，没必要每帧都碰。
                if (now_ms - e->db_ms >= 3000 || p.url.empty()) {
                    e->db_ms = now_ms;
                    RefreshPage(*brand, &p, e->history_path, e->favicons_path);
                }
                EnsureDownloads(*brand, &p, e->history_path);
            }
        }

        // ---- 2. 后台浏览器：只看下载 ----
        // 「前台在看什么」切走了就不该继续占岛；但「后台在下东西」要留着，
        // 因为用户此刻正等着它下完，而这恰恰是唯一看不到的地方。
        for (auto& e : entries_) {
            if (e.page.foreground) continue;
            if (now_ms - e.db_ms < 6000) continue;
            e.db_ms = now_ms;
            EnsureDownloads(*e.brand_def, &e.page, e.history_path);
        }

        // ---- 3. 清理 + 发布 ----
        std::vector<BrowserPage> out;
        for (auto it = entries_.begin(); it != entries_.end();) {
            BrowserPage& p = it->page;
            const bool alive = p.foreground || !p.downloads.empty();
            if (!alive && now_ms - p.seen_ms > 20000) {
                it = entries_.erase(it);
                continue;
            }
            if (alive && p.Interesting()) out.push_back(p);
            ++it;
        }
        if (out.empty() && !entries_.empty()) {
            // 出岛为空时把「卡在哪一步」说出来：前台没识别到 / 有品牌没 profile /
            // 有 profile 但标题空。三种原因在同一条日志里能一眼分开。
            Entry& e0 = entries_.front();
            Log("browser: no page — brand=" + std::string(e0.brand_def ? e0.brand_def->id : "?") +
                " profile=" + std::string(e0.history_path.empty() ? "none" : "ok") +
                " fg=" + (e0.page.foreground ? "1" : "0") +
                " title=" + (e0.page.title.empty() ? "-" : e0.page.title.substr(0, 24)));
        }
        std::sort(out.begin(), out.end(), [](const BrowserPage& a, const BrowserPage& b) {
            if (a.foreground != b.foreground) return a.foreground;
            if (a.downloads.size() != b.downloads.size()) return a.downloads.size() > b.downloads.size();
            return a.seen_ms > b.seen_ms;
        });
        if (static_cast<int>(out.size()) > max_pages_) out.resize(static_cast<std::size_t>(max_pages_));
        std::lock_guard<std::mutex> lock(mutex_);
        pages_ = std::move(out);
    }

    std::vector<BrowserPage> Active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pages_;
    }

    void Invalidate() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        pages_.clear();
    }

    // 调试：这一拍看到了什么（前台 exe / 品牌 / 数据库是否可读）。
    std::string Diag() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return diag_;
    }

private:
    struct Entry {
        const BrowserBrand* brand_def = nullptr;
        BrowserPage page;
        std::wstring profile_dir;
        std::wstring history_path;
        std::wstring favicons_path;
        std::int64_t db_ms = 0;
    };

    void Log(const std::string& line) {
        if (log_fn_) log_fn_(line);
    }

    static std::wstring ProcessImageName(DWORD pid) {
        if (pid == 0) return std::wstring();
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h == nullptr) return std::wstring();
        wchar_t buf[1024] = {0};
        DWORD size = 1024;
        std::wstring out;
        if (::QueryFullProcessImageNameW(h, 0, buf, &size) != 0) {
            out.assign(buf, size);
        }
        ::CloseHandle(h);
        const std::size_t slash = out.find_last_of(L"\\/");
        if (slash != std::wstring::npos) out = out.substr(slash + 1);
        return out;
    }

    // 铺满整个屏幕（含任务栏区）算全屏：F11 看视频、全屏编辑文档都是。
    static bool IsFullscreen(HWND hwnd) {
        if (hwnd == nullptr) return false;
        // 最大化窗口的 GetWindowRect 会含一圈看不见的边框，比显示器还大 ——
        // 不加这一条，任何最大化窗口都会被当成全屏，岛就永远处于「浏览器全屏」。
        if (::IsZoomed(hwnd) != 0) return false;
        RECT r;
        if (::GetWindowRect(hwnd, &r) == 0) return false;
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        const HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (::GetMonitorInfoW(mon, &mi) == 0) return false;
        const RECT& m = mi.rcMonitor;
        return r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
    }

    // 找一个品牌下「最近用过的 profile」。
    // Chromium 的多用户是 User Data\Default / Profile 1 / Profile 2 …
    // Firefox 是 Profiles\<随机>.default-release。
    bool FindProfile(const BrowserBrand& brand, std::wstring* dir, std::wstring* db, std::wstring* favicons) {
        const std::wstring root = brand.root == 'L' ? BrowserKnownFolder(FOLDERID_LocalAppData)
                                                   : BrowserKnownFolder(FOLDERID_RoamingAppData);
        if (root.empty()) return false;
        const std::wstring data =
            brand.data_dir[0] == '\0' ? root : root + L"\\" + BrowserWiden(brand.data_dir);
        const wchar_t* db_name = brand.chromium ? L"History" : L"places.sqlite";

        if (brand.depth == 0) {
            if (BrowserFileExists(data + L"\\" + db_name)) {
                *dir = data;
                *db = data + L"\\" + db_name;
                *favicons = brand.chromium ? (data + L"\\Favicons") : std::wstring();
                return true;
            }
            return false;
        }

        WIN32_FIND_DATAW fd;
        HANDLE h = ::FindFirstFileW((data + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        std::wstring best_dir;
        std::int64_t best_ms = -1;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if (fd.cFileName[0] == L'.') continue;
            // 跳过 Chromium 的非 profile 目录（缓存、组件等一堆）
            if (brand.chromium) {
                const std::wstring name = fd.cFileName;
                if (name != L"Default" && name.rfind(L"Profile ", 0) != 0 && name != L"Guest Profile") {
                    continue;
                }
            }
            const std::wstring candidate = data + L"\\" + fd.cFileName;
            const std::wstring file = candidate + L"\\" + db_name;
            if (!BrowserFileExists(file)) continue;
            const std::int64_t ms = BrowserFileMtimeMs(file);
            if (ms > best_ms) {
                best_ms = ms;
                best_dir = candidate;
            }
        } while (::FindNextFileW(h, &fd) != 0);
        ::FindClose(h);
        if (best_dir.empty()) return false;
        *dir = best_dir;
        *db = best_dir + L"\\" + db_name;
        *favicons = brand.chromium ? (best_dir + L"\\Favicons") : std::wstring();
        const std::size_t slash = best_dir.find_last_of(L'\\');
        (void)slash;
        return true;
    }

    Entry* EntryFor(const BrowserBrand& brand, std::int64_t now_ms) {
        for (auto& e : entries_) {
            if (e.brand_def != nullptr && std::strcmp(e.brand_def->id, brand.id) == 0) return &e;
        }
        Entry e;
        e.brand_def = &brand;
        std::wstring dir, db, fav;
        if (FindProfile(brand, &dir, &db, &fav)) {
            e.profile_dir = dir;
            e.history_path = db;
            e.favicons_path = fav;
        } else {
            // 没有库也能显示（标题 + 品牌色），只是少了域名和图标 ——
            // 这正是「降级但要出岛」的意义，不该因为找不到数据库就整块消失。
            diag_ += std::string(brand.id) + ":no-profile ";
        }
        e.page.brand = brand.id;
        e.page.label = brand.label;
        // 窗口标题里可能带的是 exe 主名（Chrome/msedge），也可能只有 SMTC 的
        // AUMID。两个都留着，判重时一起比。
        const std::string exes = brand.exes;
        e.page.exe_stem = exes.substr(0, exes.find('|'));
        if (e.page.exe_stem.size() > 4 && e.page.exe_stem.compare(e.page.exe_stem.size() - 4, 4, ".exe") == 0) {
            e.page.exe_stem.resize(e.page.exe_stem.size() - 4);
        }
        e.page.seen_ms = now_ms;
        const std::size_t slash = e.profile_dir.find_last_of(L'\\');
        e.page.profile = slash == std::wstring::npos ? std::string("Default")
                                                     : BrowserNarrow(e.profile_dir.substr(slash + 1));
        e.page.id = "browser:" + std::string(brand.id) + ":" + e.page.profile;
        entries_.push_back(std::move(e));
        return &entries_.back();
    }

    // 标题 → URL → 站点 → favicon。四步都可能失败，每一步失败都只是少一点信息。
    void RefreshPage(const BrowserBrand& brand, BrowserPage* page, const std::wstring& history_path,
                     const std::wstring& favicons_path) {
        if (!brand.chromium || page->title.empty() || history_path.empty()) return;
        SqliteLite db;
        if (!OpenReadOnly(history_path, &db)) {
            diag_ += std::string(brand.id) + ":locked ";
            return;
        }
        std::string url;
        db.Query("select url from urls where title = ? and hidden = 0 "
                 "order by last_visit_time desc limit 1",
                 {page->title},
                 [&url](const std::vector<std::string>& row, const std::vector<bool>&) {
                     if (!row.empty()) url = row[0];
                     return false;  // 只要第一条
                 });
        if (url.empty()) {
            // 精确匹配不上是常态：刚打开的页面还没写历史、标题里有零宽字符、
            // 或者页面自己改了 title。退一步在最近 24 条里找「互相包含」的那条，
            // 命中率高得多，而错配的代价只是域名差一点，远小于整行空着。
            db.Query("select url, title from urls where hidden = 0 "
                     "order by last_visit_time desc limit 24",
                     {}, [&url, page](const std::vector<std::string>& row, const std::vector<bool>&) {
                         if (row.size() < 2 || row[1].empty()) return true;
                         const std::string& t = row[1];
                         const bool contains = t.find(page->title) != std::string::npos ||
                                               page->title.find(t) != std::string::npos;
                         if (!contains) return true;
                         url = row[0];
                         return false;
                     });
        }
        if (url.empty()) return;
        page->url = url;
        page->site = BrowserSiteOf(url);
        page->site_label = page->site.empty() ? page->site : BrowserSiteLabel(page->site);
        if (!favicons_path.empty() && !page->site.empty()) {
            page->favicon = FaviconFor(favicons_path, url);
        }
    }

    // 从浏览器的 Favicons 库里把站点图标抠出来，落成 png。
    // 存文件而不是常驻内存：渲染层已经有一套「按路径加载并缓存图片」的机制
    // （媒体封面用的就是它），复用比再造一份图片缓存省事，也不会漏生命周期。
    std::string FaviconFor(const std::wstring& favicons_path, const std::string& url) {
        const std::wstring dir = FaviconCacheDir();
        if (dir.empty()) return std::string();
        // 文件名用 url 的哈希：同一站点不同页面通常同一图标，但换 logo 后
        // url 不变就还是旧图 —— 图标本身带 last_updated，值得按它算哈希。
        std::uint64_t hash = 1469598103934665603ULL;
        for (const char c : url) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            hash *= 1099511628211ULL;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "%016llx.png", static_cast<unsigned long long>(hash));
        const std::wstring file = dir + L"\\" + BrowserWiden(name);
        if (BrowserFileExists(file)) return BrowserNarrow(file);

        SqliteLite fav_db;
        if (!OpenReadOnly(favicons_path, &fav_db)) return std::string();
        // image_data 是 BLOB。现有 Query 接口按文本取，PNG 里的 \0 会把值截断，
        // 所以这里用 hex() 转文本再自己解回来 —— 比给 sqlite 层加整套 blob
        // 取值分支省事，代价是内存翻倍（32×32 图标也就几 KB）。
        std::string hex;
        const char* sql =
            "select hex(b.image_data) from icon_mapping m "
            "join favicon_bitmaps b on b.icon_id = m.icon_id "
            "where m.page_url = ? order by abs(b.width - 32) limit 1";
        fav_db.Query(sql, {url}, [&hex](const std::vector<std::string>& row, const std::vector<bool>&) {
            if (!row.empty()) hex = row[0];
            return false;
        });
        if (hex.empty()) return std::string();
        const std::string png = BrowserHexDecode(hex);
        if (png.size() < 8 || png.compare(0, 4, "\x89PNG") != 0) return std::string();
        HANDLE h = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::string();
        DWORD wrote = 0;
        ::WriteFile(h, png.data(), static_cast<DWORD>(png.size()), &wrote, nullptr);
        ::CloseHandle(h);
        return BrowserNarrow(file);
    }

    std::wstring FaviconCacheDir() {
        if (!favicon_dir_.empty()) return favicon_dir_;
        const std::wstring local = BrowserKnownFolder(FOLDERID_LocalAppData);
        if (local.empty()) return std::wstring();
        const std::wstring dir = local + L"\\DynamicIsland-Win\\favicons";
        ::CreateDirectoryW((local + L"\\DynamicIsland-Win").c_str(), nullptr);
        ::CreateDirectoryW(dir.c_str(), nullptr);
        favicon_dir_ = dir;
        return favicon_dir_;
    }

    // 下载进度。这是浏览器岛上唯一「会动」的东西，也是最值得占一个岛的信息：
    // 用户在等它，却只能开着下载面板盯着。
    void EnsureDownloads(const BrowserBrand& brand, BrowserPage* page, const std::wstring& history_path) {
        (void)brand;
        if (history_path.empty()) return;
        SqliteLite db;
        if (!OpenReadOnly(history_path, &db)) return;
        std::vector<BrowserDownload> list;
        const char* sql =
            "select target_path, received_bytes, total_bytes, state, start_time from downloads "
            "where state = 0 order by start_time desc limit 4";
        db.Query(sql, {}, [&list](const std::vector<std::string>& row, const std::vector<bool>&) {
            if (row.size() < 5) return true;
            BrowserDownload d;
            const std::string& path = row[0];
            const std::size_t slash = path.find_last_of("\\/");
            d.name = slash == std::string::npos ? path : path.substr(slash + 1);
            d.received = std::strtoll(row[1].c_str(), nullptr, 10);
            d.total = std::strtoll(row[2].c_str(), nullptr, 10);
            d.progress = d.total > 0 ? static_cast<double>(d.received) / static_cast<double>(d.total) : -1.0;
            d.started_ms = BrowserWebKitToUnixMs(std::strtoll(row[4].c_str(), nullptr, 10));
            if (!d.name.empty()) list.push_back(std::move(d));
            return true;
        });
        page->downloads = std::move(list);
        page->download_count = static_cast<int>(page->downloads.size());
        if (!page->downloads.empty()) {
            double sum = 0.0;
            int known = 0;
            long long got = 0;
            long long total = 0;
            for (const auto& d : page->downloads) {
                if (d.progress >= 0.0) {
                    sum += d.progress;
                    ++known;
                }
                got += d.received;
                if (d.total > 0) total += d.total;
            }
            page->download_progress =
                known > 0 ? sum / static_cast<double>(known)
                          : (total > 0 ? static_cast<double>(got) / static_cast<double>(total) : -1.0);
            char hint[32];
            if (page->download_progress >= 0.0) {
                std::snprintf(hint, sizeof(hint), "%d%%", static_cast<int>(page->download_progress * 100.0));
            } else if (got > 0) {
                std::snprintf(hint, sizeof(hint), "%.1f MB", static_cast<double>(got) / (1024.0 * 1024.0));
            }
            page->download_hint = hint;
        } else {
            page->download_progress = -1.0;
            page->download_hint.clear();
        }
    }

    // Chromium 的 History 在浏览器运行期间被独占：普通 mode=ro 会 SQLITE_BUSY。
    // 先试 immutable（不打扰浏览器，最省事），失败再把文件拷到临时目录读副本。
    bool OpenReadOnly(const std::wstring& path, SqliteLite* db) {
        if (db->OpenImmutable(path)) return true;
        const std::wstring tmp = TempCopy(path);
        if (tmp.empty()) return false;
        return db->OpenImmutable(tmp);
    }

    std::wstring TempCopy(const std::wstring& path) {
        const std::wstring temp = BrowserKnownFolder(FOLDERID_LocalAppData);
        if (temp.empty()) return std::wstring();
        const std::wstring dir = temp + L"\\DynamicIsland-Win\\browser-tmp";
        ::CreateDirectoryW(dir.c_str(), nullptr);
        std::uint64_t hash = 1469598103934665603ULL;
        for (const wchar_t c : path) {
            hash ^= static_cast<std::uint64_t>(c);
            hash *= 1099511628211ULL;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "%016llx.db", static_cast<unsigned long long>(hash));
        const std::wstring dst = dir + L"\\" + BrowserWiden(name);
        if (::CopyFileW(path.c_str(), dst.c_str(), FALSE) == 0) return std::wstring();
        return dst;
    }

    bool enabled_ = true;
    bool suppressed_ = false;
    int max_pages_ = 2;
    std::string force_brand_;
    bool reported_idle_ = false;
    bool reported_nobrand_ = false;
    std::function<void(const std::string&)> log_fn_;

    std::vector<Entry> entries_;
    std::vector<BrowserPage> pages_;
    mutable std::mutex mutex_;
    std::string diag_;
    std::wstring favicon_dir_;
};

}  // namespace win32
}  // namespace diw
