// browser_probe：把采集层在「本机真实浏览器」上的结果原样打出来。
//
// 这个探针存在的理由和 agent_probe 一样：岛上看不到东西时，必须能立刻分清
// 「没采到」和「采到了但没渲染」。它不需要开浏览器窗口，也不需要管理员权限。
//
//   browser_probe                 用当前前台窗口（可能不是浏览器）
//   browser_probe --wait=3        轮询 3 秒，边等在浏览器里切页面边看输出
//   browser_probe --check         只打印「哪些浏览器装了 + 库能不能读」

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "../src/win32/browser_sources.hpp"

namespace {

void PrintPage(const bi::BrowserPage& p, int index) {
    std::printf("  [%d] %s  (%s / %s)%s%s\n", index, p.label.c_str(), p.brand.c_str(),
                p.profile.c_str(), p.foreground ? "  [前台]" : "", p.fullscreen ? " [全屏]" : "");
    std::printf("      title : %s\n", p.title.empty() ? "-" : p.title.c_str());
    std::printf("      site  : %s %s\n", p.site.empty() ? "-" : p.site.c_str(),
                p.site_label.empty() || p.site_label == p.site
                    ? ""
                    : ("(" + p.site_label + ")").c_str());
    std::printf("      url   : %s\n", p.url.empty() ? "-" : p.url.c_str());
    std::printf("      icon  : %s\n", p.favicon.empty() ? "-（用品牌色）" : p.favicon.c_str());
    if (!p.downloads.empty()) {
        std::printf("      下载  : %d 项  主进度=%s\n", p.download_count,
                    p.download_hint.empty() ? "-" : p.download_hint.c_str());
        for (const auto& d : p.downloads) {
            std::printf("              %-42s %5.1f%%  %lld/%lld\n", d.name.c_str(), d.progress * 100.0,
                        d.received, d.total);
        }
    }
}

// 直接把「装了哪些浏览器、库能不能读」列出来。用户问「为什么我的浏览器没上岛」
// 时，这一步能省掉一半的来回。
void Check() {
    int count = 0;
    const bi::win32::BrowserBrand* table = bi::win32::BrowserBrandTable(&count);
    const std::wstring local = bi::win32::BrowserKnownFolder(FOLDERID_LocalAppData);
    const std::wstring roaming = bi::win32::BrowserKnownFolder(FOLDERID_RoamingAppData);
    std::printf("已知品牌 %d 个：\n", count);
    for (int i = 0; i < count; ++i) {
        const auto& b = table[i];
        const std::wstring root = b.root == 'L' ? local : roaming;
        if (root.empty() || b.data_dir[0] == '\0') continue;
        const std::wstring data = root + L"\\" + bi::win32::BrowserWiden(b.data_dir);
        if (!bi::win32::BrowserDirExists(data)) continue;
        std::printf("  %-12s %-22s %s\n", b.id, b.label,
                    bi::win32::BrowserNarrow(data).c_str());
    }
    std::printf("\n%ls\\balance_island\\favicons 里的图标：\n", local.c_str());
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW((local + L"\\balance_island\\favicons\\*.png").c_str(), &fd);
    int n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            LARGE_INTEGER sz;
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            std::printf("  %-20ls %lld B\n", fd.cFileName, sz.QuadPart);
            ++n;
        } while (::FindNextFileW(h, &fd) != 0);
        ::FindClose(h);
    }
    if (n == 0) std::printf("  （空）\n");
}

// 前台窗口是谁、被判成哪个品牌。和「库能不能读」分开查，
// 因为「没出岛」有两种完全不同的原因：前台根本不是浏览器 / 是浏览器但没采到。
void Front() {
    HWND h = ::GetForegroundWindow();
    std::printf("hwnd : %p\n", reinterpret_cast<void*>(h));
    if (h == nullptr) return;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    wchar_t buf[512] = {0};
    const int n = ::GetWindowTextW(h, buf, 512);
    const std::string title = bi::win32::BrowserNarrow(std::wstring(buf, static_cast<std::size_t>(n)));
    std::printf("pid  : %lu\n", static_cast<unsigned long>(pid));
    std::printf("title: %s\n", title.c_str());
    std::printf("zoomed: %d\n", ::IsZoomed(h) != 0 ? 1 : 0);

    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::wstring exe;
    if (proc != nullptr) {
        wchar_t path[1024] = {0};
        DWORD size = 1024;
        if (::QueryFullProcessImageNameW(proc, 0, path, &size) != 0) {
            exe.assign(path, size);
        }
        ::CloseHandle(proc);
    }
    std::printf("exe  : %ls\n", exe.c_str());
    const std::size_t slash = exe.find_last_of(L"\\/");
    const std::wstring base = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
    const bi::win32::BrowserBrand* by_exe = bi::win32::BrowserBrandByExe(base);
    const bi::win32::BrowserBrand* by_title = bi::win32::BrowserBrandByTitle(title);
    std::printf("brand: exe=%s  title=%s\n", by_exe ? by_exe->id : "-", by_title ? by_title->id : "-");
    if (by_exe != nullptr) {
        std::printf("strip: %s\n",
                    bi::win32::BrowserStripSuffix(title, by_exe).c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    int wait_sec = 0;
    bool check = false;
    bool front = false;
    std::string force_brand;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--check") == 0) check = true;
        if (std::strcmp(argv[i], "--front") == 0) front = true;
        if (std::strncmp(argv[i], "--wait=", 7) == 0) wait_sec = std::atoi(argv[i] + 7);
        if (std::strncmp(argv[i], "--brand=", 8) == 0) force_brand = argv[i] + 8;
    }

    if (check) {
        Check();
        return 0;
    }
    if (front) {
        Front();
        return 0;
    }

    bi::win32::BrowserWatcher watcher;
    if (!force_brand.empty()) watcher.SetForceBrand(force_brand);
    watcher.SetLogger([](const std::string& line) { std::printf("  log: %s\n", line.c_str()); });

    const auto start = std::chrono::steady_clock::now();
    int round = 0;
    while (true) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        watcher.Poll(now);
        const auto pages = watcher.Active();
        std::printf("\n===== round %d  pages=%zu =====\n", ++round, pages.size());
        const std::string diag = watcher.Diag();
        if (!diag.empty()) std::printf("  diag : %s\n", diag.c_str());
        for (std::size_t i = 0; i < pages.size(); ++i) {
            PrintPage(pages[i], static_cast<int>(i));
        }
        if (pages.empty()) std::printf("  （前台不是浏览器，也没有进行中的下载）\n");

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (elapsed >= wait_sec) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    return 0;
}
