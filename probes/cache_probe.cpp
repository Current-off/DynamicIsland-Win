// 缓存 LRU 回归探针：直接调用产品里的 bi::live::TrimCache，
// 验证「按最旧修改时间淘汰、保留最新 keep 个、未超限不动作」。
//
// 之前 covers/ 目录只增不减：听歌多 / 通知杂的机器上会无限膨胀。
// TrimCache 在每次写入新 PNG 后调用，按 ftLastWriteTime 排序删除最旧的。
// 本探针造 120 个时间严格递增的假 PNG，逐档验证淘汰结果。
//
// 用法：cache_probe.exe   （无需参数，自带临时目录并自清理）
//   PASS / FAIL 打印到 stdout，退出码 0 = 全通过。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <knownfolders.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>

#include <gdiplus.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../../src/win32/live_sources.hpp"

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

std::wstring TempDir() {
    wchar_t tmp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"diw_cache_probe_" +
                       std::to_wstring(::GetCurrentProcessId());
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

int CountPng(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) ++n;
    } while (::FindNextFileW(find, &fd) != FALSE);
    ::FindClose(find);
    return n;
}

bool Exists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring Name(const std::wstring& dir, int i) {
    wchar_t buf[32];
    std::swprintf(buf, 32, L"f%03d.png", i);
    return dir + L"\\" + buf;
}

// 造 n 个文件，第 i 个的最后写入时间 = 基准 + i 秒（严格递增）。
void CreateFiles(const std::wstring& dir, int n) {
    SYSTEMTIME st{};
    st.wYear = 2020; st.wMonth = 1; st.wDay = 1;
    FILETIME base{};
    ::SystemTimeToFileTime(&st, &base);
    LONGLONG base64 = (static_cast<LONGLONG>(base.dwHighDateTime) << 32) | base.dwLowDateTime;
    for (int i = 0; i < n; ++i) {
        const std::wstring path = Name(dir, i);
        HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        const char payload[] = "fake-png";
        DWORD written = 0;
        ::WriteFile(f, payload, sizeof(payload) - 1, &written, nullptr);
        const LONGLONG t = base64 + static_cast<LONGLONG>(i) * 10000000LL;  // 1 秒 = 1e7 * 100ns
        FILETIME ft{static_cast<DWORD>(t & 0xFFFFFFFFu), static_cast<DWORD>(t >> 32)};
        ::SetFileTime(f, nullptr, nullptr, &ft);
        ::CloseHandle(f);
    }
}

void Purge(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                ::DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            }
        } while (::FindNextFileW(find, &fd) != FALSE);
        ::FindClose(find);
    }
    ::RemoveDirectoryW(dir.c_str());
}

}  // namespace

int main() {
    const std::wstring dir = TempDir();
    const int keep = bi::live::kCacheKeep;
    std::printf("cache LRU probe: keep=%d, fixture=120 files\n", keep);

    Purge(dir);  // 复用上次残留的目录时先清空
    ::CreateDirectoryW(dir.c_str(), nullptr);
    CreateFiles(dir, 120);
    Check(CountPng(dir) == 120, "fixture: 120 png created with increasing mtime");

    // ① 超上限：应裁到 keep 个，删最旧的 120-keep 个。
    bi::live::TrimCache(dir, keep);
    const int after1 = CountPng(dir);
    Check(after1 == keep, "120 -> 96 (trim to keep)");
    bool oldest_gone = true, newest_kept = true;
    for (int i = 0; i < 120 - keep; ++i) {
        if (Exists(Name(dir, i))) oldest_gone = false;
    }
    for (int i = 120 - keep; i < 120; ++i) {
        if (!Exists(Name(dir, i))) newest_kept = false;
    }
    Check(oldest_gone, "oldest 24 files deleted");
    Check(newest_kept, "newest 96 files kept");

    // ② 恰好在上限：不应删任何文件（幂等，避免每次写缓存都扫目录误删）。
    bi::live::TrimCache(dir, keep);
    Check(CountPng(dir) == keep, "at limit: no file deleted (idempotent)");

    // ③ 更紧的上限：继续删最旧的，只留最后 10 个。
    bi::live::TrimCache(dir, 10);
    Check(CountPng(dir) == 10, "96 -> 10 (tighter limit)");
    bool tail_kept = true;
    for (int i = 110; i < 120; ++i) {
        if (!Exists(Name(dir, i))) tail_kept = false;
    }
    Check(tail_kept, "only newest 10 remain (f110..f119)");

    // ④ 空目录 / 不存在的目录：不崩溃。
    bi::live::TrimCache(dir + L"\\does_not_exist", keep);
    Check(true, "missing dir: returns without crash");

    Purge(dir);
    std::printf("cache_probe: failed=%d\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
