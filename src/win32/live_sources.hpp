// DynamicIsland-Win / 真实数据采集层（Win32 + C++/WinRT）
//
// 职责：只负责「把系统状态读成 Snapshot」，不做任何取舍与文案。
// 该不该弹、弹多久、谁能盖住谁，全部在 core/live_mapping.hpp + core/rules.hpp。
//
// 工程约束（每一条都是踩过坑之后加的）：
//   1. 采集必须跑在独立工作线程：SMTC 的 TryGetMediaPropertiesAsync().get() 会阻塞，
//      放在渲染主线程会直接掉帧。主线程只读快照，不做任何 WinRT 调用。
//   2. 每个数据源独立失败隔离：某个源抛异常（音频设备被独占、通知服务未启动）
//      不影响其他源，也不让整个程序退出。
//   3. 只在「内容真的变了」时更新快照，避免把轮询抖动传给状态机。
//   4. 剪贴板要先比序列号再 OpenClipboard：后者会抢系统剪贴板锁，
//      每 100ms 抢一次会让其他程序复制失败。
//
// 已实测可用（2026-11-12，本机 Win11 + MSVC 14.51 + SDK 10.0.26100）：
//   - GlobalSystemMediaTransportControlsSessionManager  未打包可用
//   - UserNotificationListener                          未打包返回 Allowed
//   - 剪贴板 CF_UNICODETEXT / CF_DIB / CF_HDROP         免管理员可用
//   - WM_DEVICECHANGE + DEV_BROADCAST_VOLUME            拿到盘符
#ifndef DIW_SRC_WIN32_LIVE_SOURCES_HPP
#define DIW_SRC_WIN32_LIVE_SOURCES_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <appmodel.h>
#include <commctrl.h>     // 必须在 commoncontrols.h 之前（IImageList 依赖其类型）
#include <commoncontrols.h>  // IImageList：按系统图像列表拿 48px 图标
#include <knownfolders.h>
#include <propkey.h>  // PKEY_AppUserModel_ID：.lnk 里存 AUMID 的属性键
#include <propsys.h>  // IPropertyStore：读 .lnk 属性
#include <shellapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shobjidl_core.h>  // IShellItem2 / IShellFolder / IEnumIDList：枚举 AppsFolder
#include <shlguid.h>        // BHID_SFObject
#include <shlwapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>  // UserNotification.AppInfo().DisplayInfo() 需要完整类型
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <mutex>
#include <string>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

// 与 core 保持同一约定：依赖 -Icontracts -Isrc/core，不写相对路径。
#include "agent_sources.hpp"
#include "browser_sources.hpp"
#include "editor_sources.hpp"
#include "meeting_sources.hpp"
#include "live_mapping.hpp"

namespace diw {
namespace live {

// ---------------------------------------------------------------- 字符串

inline std::string Narrow(const wchar_t* w, int len) {
    if (w == nullptr || len <= 0) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), need, nullptr, nullptr);
    return out;
}

inline std::string Narrow(const std::wstring& w) {
    return Narrow(w.c_str(), static_cast<int>(w.size()));
}

// winrt::hstring 不能隐式转 wstring，必须单独重载，否则每个调用点都 C2664。
inline std::string Narrow(const winrt::hstring& h) {
    return Narrow(h.c_str(), static_cast<int>(h.size()));
}

inline std::wstring WidenOf(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                           nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

// FILETIME 是 1601 纪元，渲染层用的是「从启动起的相对毫秒」，两者不能直接相减。
inline std::int64_t WallNowFileTimeMs() {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<std::int64_t>(u.QuadPart / 10000ULL);
}

inline std::int64_t FileTimeToMs(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<std::int64_t>(u.QuadPart / 10000ULL);
}

// ---------------------------------------------------------------- 专辑封面
//
// SMTC 暴露的是 IRandomAccessStreamReference（不透明句柄），必须自己解码。
// 解码结果落到 %LOCALAPPDATA%\DynamicIsland-Win\covers\<hash>.png：
//   - 不放进 content_json：那是文本协议，塞二进制要么 base64 膨胀 33%，
//     要么让 Activity 依赖平台类型（它是跨层契约，必须保持纯数据）。
//   - 按 app+曲名 做文件名哈希：同一首歌反复读会话时命中缓存，不重复解码。

inline bool FileExistsW(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline std::uint64_t Fnv1a(const std::string& text) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

inline bool CoverCacheDir(std::wstring* out) {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) || raw == nullptr) {
        return false;
    }
    std::wstring base(raw);
    ::CoTaskMemFree(raw);
    const std::wstring app_dir = base + L"\\DynamicIsland-Win";
    const std::wstring dir = app_dir + L"\\covers";
    ::CreateDirectoryW(app_dir.c_str(), nullptr);
    ::CreateDirectoryW(dir.c_str(), nullptr);
    *out = dir;
    return true;
}

// 缓存 LRU：目录里的 PNG 只增不减，听歌多/通知杂的机器上会无限膨胀。
// 每次写入新文件后按「最旧修改时间」淘汰，保持总量 ≤ keep。
// 上限 96 个：封面 ≈40KB、图标 ≈10KB，96 个 ≈ 2-4MB，「最近常听 +
// 常见应用」全部命中的同时磁盘占用可控。只在真正写入新文件时才扫目录
// （缓存命中直接 return，不触发），扫描是 FindFirstFile 一趟，毫秒级。
inline void TrimCache(const std::wstring& dir, int keep) {
    std::vector<std::pair<FILETIME, std::wstring>> files;
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        files.emplace_back(fd.ftLastWriteTime, std::wstring(fd.cFileName));
    } while (::FindNextFileW(find, &fd) != FALSE);
    ::FindClose(find);
    if (static_cast<int>(files.size()) <= keep) return;

    // 最旧的排前面，超出 keep 的删掉。失败不打断（竞争删除等），下次再清。
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        const LONG64 ta = (static_cast<LONG64>(a.first.dwHighDateTime) << 32) | a.first.dwLowDateTime;
        const LONG64 tb = (static_cast<LONG64>(b.first.dwHighDateTime) << 32) | b.first.dwLowDateTime;
        return ta < tb;
    });
    const std::size_t remove = files.size() - static_cast<std::size_t>(keep);
    for (std::size_t i = 0; i < remove; ++i) {
        ::DeleteFileW((dir + L"\\" + files[i].second).c_str());
    }
}

inline constexpr int kCacheKeep = 96;

inline bool PngEncoderClsid(CLSID* out) {
    UINT num = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0) return false;
    std::vector<std::uint8_t> raw(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(raw.data());
    if (Gdiplus::GetImageEncoders(num, size, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (std::wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            *out = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

// 解码并缓存封面，返回可在 GDI+ 里直接打开的 PNG 路径；任何一步失败都返回空串
// （调用方据此回退到「色块 + 音符」图标，不会因为封面读不到就不显示媒体）。
inline std::string SaveCoverToCache(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& ref, const std::string& key) {
    try {
        if (!ref) return std::string();
        std::wstring dir;
        if (!CoverCacheDir(&dir)) return std::string();
        wchar_t name[40] = {};
        std::swprintf(name, 40, L"%016llx.png", static_cast<unsigned long long>(Fnv1a(key)));
        const std::wstring path = dir + L"\\" + name;
        if (FileExistsW(path)) return Narrow(path);

        auto stream = ref.OpenReadAsync().get();
        if (!stream) return std::string();
        const auto raw_size = stream.Size();
        if (raw_size == 0 || raw_size > 16ull * 1024ull * 1024ull) return std::string();
        winrt::Windows::Storage::Streams::DataReader reader(stream);
        reader.LoadAsync(static_cast<std::uint32_t>(raw_size)).get();
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(raw_size));
        winrt::array_view<std::uint8_t> view(bytes);
        reader.ReadBytes(view);

        IStream* mem = ::SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
        if (mem == nullptr) return std::string();
        std::string result;
        {
            Gdiplus::Bitmap src(mem, FALSE);
            if (src.GetLastStatus() == Gdiplus::Ok && src.GetWidth() > 0 && src.GetHeight() > 0) {
                // 存 160px：胶囊里最大只画到 42 逻辑像素，200% DPI 下 84px，
                // 留一倍余量给未来放大；再大只是白占内存。
                constexpr int kSide = 160;
                Gdiplus::Bitmap dst(kSide, kSide, PixelFormat32bppARGB);
                CLSID png{};
                if (dst.GetLastStatus() == Gdiplus::Ok && PngEncoderClsid(&png)) {
                    Gdiplus::Graphics g(&dst);
                    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                    const float w = static_cast<float>(src.GetWidth());
                    const float h = static_cast<float>(src.GetHeight());
                    const float side = std::min(w, h);  // 中心裁成正方形，交给渲染层直接用
                    g.DrawImage(&src, Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(kSide), static_cast<float>(kSide)),
                                (w - side) * 0.5f, (h - side) * 0.5f, side, side, Gdiplus::UnitPixel);
                    if (dst.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok) {
                        result = Narrow(path);
                        TrimCache(dir, kCacheKeep);  // 只在写入新文件时清理
                    }
                }
            }
        }
        mem->Release();  // Bitmap 析构时已释放它自己那份引用
        return result;
    } catch (...) {
        return std::string();
    }
}

// ---------------------------------------------------------------- 通知应用图标
//
// 为什么不用 WinRT（全部实测过，全堵死）：
//   - AppDisplayInfo::GetLogo(Size)：16/24/32/44/48/96/256 全尺寸一律返回 null 引用，
//     连打包应用（有完整 DisplayInfo）也是；
//   - AppInfo::GetFromAppUserModelId(AUMID)：返回 0x80070490（ERROR_NOT_FOUND）；
//   - 通知的 Visual/Binding 只暴露文本（GetTextElements），没有图片元素，
//     Notification() 也转不成 IToastNotification（E_NOINTERFACE）拿不到 XML。
//
// 可行路线是 Win32：应用要在通知中心显示 toast，必须注册 AUMID；
// AUMID 存在开始菜单 .lnk 的 System.AppUserModel.ID 属性里；
// 命中 .lnk 后取目标 exe 的 shell 图标（系统图像列表 48px），转 PNG 落缓存。
// QQ / 微信 / 打包应用 / 未打包应用统一走这一条。

inline void CollectShortcutFiles(const std::wstring& dir, std::vector<std::wstring>* out) {
    // 开始菜单通常 2~3 层、几百个 .lnk；只在某个 AUMID 首次出现（缓存未命中）时跑一遍。
    const std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (std::wcscmp(fd.cFileName, L".") == 0 || std::wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            CollectShortcutFiles(full, out);
        } else {
            const std::size_t len = std::wcslen(fd.cFileName);
            if (len > 4 && _wcsicmp(fd.cFileName + len - 4, L".lnk") == 0) {
                out->push_back(full);
            }
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

// HICON → 48×48 PNG。DrawIconEx 写进 32bpp DIB 的是**预乘** ARGB，
// GDI+ 的 PixelFormat32bppARGB 期望非预乘，直接存会把半透明边缘存成深色描边。
// HBITMAP → PNG。IShellItemImageFactory::GetImage 返回的是预乘 ARGB，
// 而 GDI+ 要的是直通 alpha，必须先反预乘，否则图标边缘会发黑。
inline bool SaveHBitmapPng(HBITMAP hbmp, const std::wstring& png_path) {
    if (hbmp == nullptr) return false;
    BITMAP bm{};
    if (::GetObjectW(hbmp, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    if (w > 1024 || h > 1024) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    HDC dc = ::GetDC(nullptr);
    if (dc == nullptr) return false;
    const int lines = ::GetDIBits(dc, hbmp, 0, static_cast<UINT>(h), px.data(), &bi, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);
    if (lines == 0) return false;

    for (int i = 0; i < w * h * 4; i += 4) {
        const std::uint8_t a = px[static_cast<std::size_t>(i) + 3];
        if (a > 0 && a < 255) {
            px[static_cast<std::size_t>(i) + 0] = static_cast<std::uint8_t>(px[i + 0] * 255 / a);
            px[static_cast<std::size_t>(i) + 1] = static_cast<std::uint8_t>(px[i + 1] * 255 / a);
            px[static_cast<std::size_t>(i) + 2] = static_cast<std::uint8_t>(px[i + 2] * 255 / a);
        }
    }

    Gdiplus::Bitmap out(w, h, w * 4, PixelFormat32bppARGB, px.data());
    CLSID png{};
    if (out.GetLastStatus() != Gdiplus::Ok || !PngEncoderClsid(&png)) return false;
    return out.Save(png_path.c_str(), &png, nullptr) == Gdiplus::Ok;
}

inline bool SaveIconPng(HICON icon, const std::wstring& png_path) {
    constexpr int kSide = 48;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kSide;
    bi.bmiHeader.biHeight = -kSide;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    if (screen == nullptr) return false;
    HDC mem = ::CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (mem == nullptr || dib == nullptr || bits == nullptr) {
        if (mem) ::DeleteDC(mem);
        if (dib) ::DeleteObject(dib);
        return false;
    }
    HGDIOBJ old = ::SelectObject(mem, dib);
    ::DrawIconEx(mem, 0, 0, icon, kSide, kSide, 0, nullptr, DI_NORMAL);
    ::SelectObject(mem, old);
    ::DeleteDC(mem);

    // 反预乘：c' = c * 255 / a（a=0 时留 0，GDI+ 全透明像素不显示颜色）
    auto* px = static_cast<std::uint8_t*>(bits);
    for (int i = 0; i < kSide * kSide * 4; i += 4) {
        const std::uint8_t a = px[i + 3];
        if (a > 0 && a < 255) {
            px[i + 0] = static_cast<std::uint8_t>(px[i + 0] * 255 / a);
            px[i + 1] = static_cast<std::uint8_t>(px[i + 1] * 255 / a);
            px[i + 2] = static_cast<std::uint8_t>(px[i + 2] * 255 / a);
        }
    }

    bool ok = false;
    {
        Gdiplus::Bitmap bmp(kSide, kSide, kSide * 4, PixelFormat32bppARGB,
                            static_cast<BYTE*>(bits));
        CLSID png{};
        if (bmp.GetLastStatus() == Gdiplus::Ok && PngEncoderClsid(&png)) {
            ok = bmp.Save(png_path.c_str(), &png, nullptr) == Gdiplus::Ok;
        }
    }
    ::DeleteObject(dib);
    return ok;
}

// 打包应用 AUMID 的两种形态：通知中心给 "PFN"（WorkBuddy.WorkBuddy），
// 开始菜单 .lnk 存 "PFN!AppId"（WorkBuddy.WorkBuddy!App）。按前缀 + '!' 容忍。
inline bool AumidMatches(const wchar_t* lnk_aumid, const std::wstring& want) {
    if (_wcsicmp(lnk_aumid, want.c_str()) == 0) return true;
    const std::size_t n = want.size();
    if (_wcsnicmp(lnk_aumid, want.c_str(), n) != 0) return false;
    // 两种合法前缀：
    //   want 后再跟 '!'  —— 通知给的 "PFN!AppId" 少写了 AppId；
    //   want 后再跟 '_'  —— UWP 的 SMTC 只报包族名（com.bilibili.bilibiliPC），
    //                       而开始菜单里的 AUMID 是
    //                       "com.bilibili.bilibiliPC_8wekyb3d8bbwe!App"。
    // 之前只认 '!'，所以 B 站这类 UWP 播放器的图标永远是空。
    const wchar_t next = lnk_aumid[n];
    return next == L'!' || next == L'_';
}

// 按进程名（不含 .exe，忽略大小写）找到正在运行的那个进程，问出它的完整路径。
// 为什么需要这条：SMTC / 通知经常只报进程名（QQ 是 "QQ.exe"，QQ 音乐是
// "QQMusic.exe"）。这种名字既不是路径、也不是 AUMID，前面几条路线全部落空，
// 用户点岛就是「毫无反应」。而点击的那一刻这些应用通常正在运行 —— 直接问
// 系统要它的 ImagePath 最靠谱，ShellExecute 过去就是「切到前台」。
inline bool RunningExePathByProcessName(const std::wstring& proc_no_ext, std::wstring* out) {
    if (proc_no_ext.empty() || out == nullptr) return false;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool ok = false;
    if (::Process32FirstW(snap, &pe)) {
        do {
            std::wstring name(pe.szExeFile);
            if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) {
                name.resize(name.size() - 4);
            }
            if (_wcsicmp(name.c_str(), proc_no_ext.c_str()) != 0) continue;
            HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (proc == nullptr) continue;
            wchar_t buf[MAX_PATH * 2] = {0};
            DWORD len = static_cast<DWORD>(std::size(buf));
            if (::QueryFullProcessImageNameW(proc, 0, buf, &len)) {
                *out = buf;
                ok = true;
            }
            ::CloseHandle(proc);
            if (ok) break;
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return ok;
}

// 在 shell:AppsFolder 里找「这个应用对应的 AppID」。
// 为什么需要：三类常见的 target 都不是启动器认识的字符串 ——
//   1) 通知 / SMTC 报进程名："QQ.exe"、"QQMusic.exe"
//   2) 报包族名："com.bilibili.bilibiliPC"（UWP 的 AUMID 其实是 PFN!AppId）
//   3) 而 AppsFolder 里它们的 AppID 分别是 "QQ"、"BiliBiliPC"
// 光靠 AUMID 匹配永远对不上，表现为「点岛毫无反应」。这里按三种等价写法
// （原名 / 去掉 .exe / 包族名末段）在 AppsFolder 里找一遍，拿到真正的 AppID。
inline bool FindAppsFolderAppId(const std::string& target, std::wstring* out) {
    if (target.empty() || out == nullptr) return false;
    std::wstring want = WidenOf(target);
    std::wstring noext = want;
    if (noext.size() > 4 && _wcsicmp(noext.c_str() + noext.size() - 4, L".exe") == 0) {
        noext.resize(noext.size() - 4);
    }
    // 包族名末段：com.bilibili.bilibiliPC → bilibiliPC
    std::wstring tail = noext;
    const std::size_t dot = tail.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < tail.size()) tail = tail.substr(dot + 1);

    // 绑定方式必须和 SaveAppsFolderIcon 一致：SHParseDisplayName + SHBindToObject
    // 在这里会失败（虚拟命名空间要先拿 IShellItem 再 BindToHandler）。
    winrt::com_ptr<IShellItem> apps;
    if (FAILED(::SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr,
                                             IID_PPV_ARGS(apps.put())))) {
        return false;
    }
    winrt::com_ptr<IShellFolder> folder;
    if (FAILED(apps->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(folder.put())))) {
        return false;
    }
    winrt::com_ptr<IEnumIDList> ids;
    if (FAILED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, ids.put()))) {
        return false;
    }
    wchar_t buf[512] = {};
    LPITEMIDLIST pidl = nullptr;
    ULONG fetched = 0;
    bool ok = false;
    while (!ok && ids->Next(1, &pidl, &fetched) == S_OK && fetched == 1) {
        STRRET strret{};
        std::wstring appid;
        if (SUCCEEDED(folder->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &strret)) &&
            SUCCEEDED(::StrRetToBufW(&strret, pidl, buf, static_cast<UINT>(std::size(buf))))) {
            appid = buf;
            const std::size_t slash = appid.find_last_of(L'\\');
            if (slash != std::wstring::npos) appid = appid.substr(slash + 1);
        }
        ::CoTaskMemFree(pidl);
        pidl = nullptr;
        if (appid.empty()) continue;
        if (_wcsicmp(appid.c_str(), want.c_str()) == 0 ||
            _wcsicmp(appid.c_str(), noext.c_str()) == 0 ||
            _wcsicmp(appid.c_str(), tail.c_str()) == 0) {
            *out = appid;
            ok = true;
        }
    }
    return ok;
}

// 隐式 AUMID："{KNOWNFOLDER-GUID}\相对路径\app.exe"。系统对没有显式 AUMID 的
// 开始菜单快捷方式自动合成这种形式（Get-StartApps 能看到，但对应的 .lnk
// 可能根本不在文件系统里 —— Windows PowerShell 就是实例）。GUID 直接喂
// SHGetKnownFolderPath 就能得到基目录，拼上相对路径就是 exe。
inline bool ExeFromImplicitAumid(const std::string& aumid, std::wstring* out_exe) {
    if (aumid.empty() || aumid[0] != '{') return false;
    const std::size_t close = aumid.find('}');
    if (close == std::string::npos || close + 2 > aumid.size()) return false;
    const std::wstring guid_w = WidenOf(aumid.substr(0, close + 1));
    const std::wstring rel = WidenOf(aumid.substr(close + 2));
    GUID guid{};
    if (FAILED(::CLSIDFromString(guid_w.c_str(), &guid))) return false;
    PWSTR base = nullptr;
    if (FAILED(::SHGetKnownFolderPath(guid, 0, nullptr, &base)) || base == nullptr) {
        return false;
    }
    std::wstring full = std::wstring(base) + L"\\" + rel;
    ::CoTaskMemFree(base);
    const DWORD attr = ::GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    *out_exe = full;
    return true;
}

// exe 路径 → 系统图像列表 48px 图标 → PNG 落盘。
inline bool SaveExeIconPng(const std::wstring& exe, const std::wstring& png_path) {
    SHFILEINFOW sfi{};
    if (::SHGetFileInfoW(exe.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX) == 0) {
        return false;
    }
    winrt::com_ptr<IImageList> images;
    if (FAILED(::SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(images.put())))) {
        return false;
    }
    HICON icon = nullptr;
    if (FAILED(images->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon)) || icon == nullptr) {
        return false;
    }
    const bool ok = SaveIconPng(icon, png_path);
    ::DestroyIcon(icon);
    return ok;
}

// UWP 应用：快捷方式不在文件系统里（shell:AppsFolder 是虚拟命名空间），
// 扫开始菜单 .lnk 永远扫不到 —— B 站、计算器这类只看得到「开始菜单 249 个
// lnk、匹配 0」。这里直接枚举 AppsFolder，按包族名前缀匹配，再用
// SHGetFileInfoW 对虚拟路径取图标（Shell 有权限读 WindowsApps 里的资源）。
inline bool SaveAppsFolderIcon(const std::wstring& want, const std::wstring& png_path) {
    winrt::com_ptr<IShellItem> apps;
    if (FAILED(::SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr,
                                             IID_PPV_ARGS(apps.put())))) {
        return false;
    }
    winrt::com_ptr<IShellFolder> folder;
    if (FAILED(apps->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(folder.put())))) {
        return false;
    }
    winrt::com_ptr<IEnumIDList> ids;
    if (FAILED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, ids.put()))) {
        return false;
    }
    // SMTC 对 UWP 报的是包族名（com.bilibili.bilibiliPC），而 AppsFolder 里的
    // AppID 是应用名（BiliBiliPC）——**两个字符串毫无关系**，精确/前缀匹配都没戏。
    // 能对上的只有「包族名去掉 publisher 后的末段」与 AppID（忽略大小写相等）。
    std::wstring tail = want;
    const auto dot = tail.find_last_of(L'.');
    if (dot != std::wstring::npos) tail = tail.substr(dot + 1);

    // AppsFolder 的项既没有 PKEY_AppUserModel_ID（实测 314 项全部取不到），
    // SHGetNameFromIDList 也返回失败；唯一稳定的是 IShellFolder::GetDisplayNameOf
    // 配 SHGDN_FORPARSING —— 它返回的解析名就是 AUMID。
    WCHAR name_buf[512] = {};
    LPITEMIDLIST pidl = nullptr;
    ULONG fetched = 0;
    bool ok = false;
    while (!ok && ids->Next(1, &pidl, &fetched) == S_OK && fetched == 1) {
        STRRET strret{};
        std::wstring aumid;
        if (SUCCEEDED(folder->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &strret)) &&
            SUCCEEDED(::StrRetToBufW(&strret, pidl, name_buf,
                                     static_cast<UINT>(std::size(name_buf))))) {
            aumid = name_buf;
            // 个别 Shell 会带上 "shell:AppsFolder\" 前缀，取最后一段。
            const auto slash = aumid.find_last_of(L'\\');
            if (slash != std::wstring::npos) aumid = aumid.substr(slash + 1);
        }
        // EnumObjects 给的是**相对 pidl**（相对于 AppsFolder），必须配父文件夹
        // 才能构成一个完整项。用 SHCreateItemFromIDList 会得到错的东西 ——
        // 图标取出来是一张通用的「白纸」。
        winrt::com_ptr<IShellItem2> item;
        if (!aumid.empty()) {
            ::SHCreateItemWithParent(nullptr, folder.get(), pidl, IID_PPV_ARGS(item.put()));
        }
        ::CoTaskMemFree(pidl);
        pidl = nullptr;
        if (aumid.empty()) continue;
        const bool matched = AumidMatches(aumid.c_str(), want) ||
                             (tail != want && _wcsicmp(aumid.c_str(), tail.c_str()) == 0);
        if (!matched) continue;
        // 虚拟路径喂 SHGetFileInfoW 拿不到图标索引，直接向 Shell 图像工厂要位图。
        winrt::com_ptr<IShellItemImageFactory> factory;
        if (item && SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(factory.put())))) {
            SIZE sz{48, 48};
            HBITMAP bmp = nullptr;
            if (SUCCEEDED(factory->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp)) &&
                bmp != nullptr) {
                ok = SaveHBitmapPng(bmp, png_path);
                ::DeleteObject(bmp);
            }
        }
    }
    return ok;
}

// AUMID → 应用图标 PNG 路径。与封面共用缓存目录和 Fnv1a 命名规则，
// 缓存键 "appicon|<AUMID>"：同一应用只解析一次。
// 四级路线，命中任何一级即落盘返回：
//   1) 隐式 AUMID 解码（"{GUID}\relpath" → 直接得到 exe）——经典 Win32 应用；
//   2) AppsFolder 虚拟枚举 —— UWP 应用（B 站 / 计算器 / 照片…），它们没有实体 .lnk；
//   3) 开始菜单 .lnk 显式 AUMID → 目标 exe ——QQ/微信等注册过 AUMID 的 Win32 应用；
//   4) 都失败 → 空（渲染层回落默认图形）。
inline std::string AppIconForAumid(const std::string& aumid) {
    try {
        if (aumid.empty()) return std::string();
        std::wstring dir;
        if (!CoverCacheDir(&dir)) return std::string();
        wchar_t name[40] = {};
        std::swprintf(name, 40, L"%016llx.png",
                      static_cast<unsigned long long>(Fnv1a("appicon|" + aumid)));
        const std::wstring png_path = dir + L"\\" + name;
        if (FileExistsW(png_path)) return Narrow(png_path);  // 缓存命中，不再碰开始菜单

        // 路线 0：有些 Win32 播放器（腾讯视频 QQLive 等）把 SourceAppUserModelId
        // 直接报成 exe 完整路径（"D:\...\QQLive.exe"），根本不是 AUMID。
        // 四条 AUMID 路线对它全部无效，会一直画占位色块。
        if (aumid.find('\\') != std::string::npos || aumid.find(':') != std::string::npos) {
            if (SaveExeIconPng(WidenOf(aumid), png_path)) {
                TrimCache(dir, kCacheKeep);
                return Narrow(png_path);
            }
        }

        // 路线 1：隐式 AUMID（成本极低，先试）
        std::wstring exe;
        if (ExeFromImplicitAumid(aumid, &exe) && SaveExeIconPng(exe, png_path)) {
            TrimCache(dir, kCacheKeep);
            return Narrow(png_path);
        }

        const std::wstring aumid_w = WidenOf(aumid);

        // 路线 2：UWP 应用（AppsFolder 虚拟枚举）。放在 .lnk 扫描之前，
        // 因为 .lnk 里根本没有它们，扫 249 个文件纯属浪费。
        if (SaveAppsFolderIcon(aumid_w, png_path)) {
            TrimCache(dir, kCacheKeep);
            return Narrow(png_path);
        }

        // 路线 3：开始菜单 .lnk 的显式 AUMID

        std::vector<std::wstring> lnks;
        for (bool common : {true, false}) {
            PWSTR root = nullptr;
            if (SUCCEEDED(::SHGetKnownFolderPath(
                    common ? FOLDERID_CommonPrograms : FOLDERID_Programs, 0, nullptr, &root)) &&
                root != nullptr) {
                CollectShortcutFiles(root, &lnks);
                ::CoTaskMemFree(root);
            }
        }

        for (const auto& lnk : lnks) {
            // COM 均在采集线程（已 init_apartment MTA）上调用。
            // 注意：IShellLinkW 与 IPersistFile 是同一个 COM 对象的两个接口，
            // 拿到 file 并 Load 之后 link 仍然有效，不需要再反向 QueryInterface。
            winrt::com_ptr<IShellLinkW> link;
            if (FAILED(::CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(link.put())))) {
                continue;
            }
            winrt::com_ptr<IPersistFile> file;
            if (FAILED(link->QueryInterface(IID_PPV_ARGS(file.put()))) ||
                FAILED(file->Load(lnk.c_str(), STGM_READ))) {
                continue;
            }
            bool aumid_match = false;
            winrt::com_ptr<IPropertyStore> store;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(store.put())))) {
                PROPVARIANT var{};
                if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &var))) {
                    if (var.vt == VT_LPWSTR && var.pwszVal != nullptr) {
                        aumid_match = AumidMatches(var.pwszVal, aumid_w);
                    }
                    ::PropVariantClear(&var);
                }
            }
            if (!aumid_match) continue;

            // 先对 .lnk 本身取 Shell 图标。UWP 快捷方式的 GetPath 返回的是
            // explorer.exe（直接拿 exe 的图标会变成资源管理器），而 Shell 对
            // .lnk 会解析出应用自己的 logo —— B 站、计算器这类全靠这条。
            if (SaveExeIconPng(lnk, png_path)) {
                TrimCache(dir, kCacheKeep);
                return Narrow(png_path);
            }

            wchar_t target[MAX_PATH] = {0};
            if (FAILED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)) ||
                target[0] == L'\0') {
                continue;
            }
            if (SaveExeIconPng(target, png_path)) {
                TrimCache(dir, kCacheKeep);
                return Narrow(png_path);
            }
        }
    } catch (...) {
    }
    return std::string();
}

// ---------------------------------------------------------------- 前台应用

struct ForegroundApp {
    std::string exe;    // "Code.exe"
    std::string label;  // "Code"
    std::string path;   // 完整路径，一览点击拉起时用（exe 只是文件名，开不了）
};

// 复制不会改变焦点，所以「现在的前台」几乎总是刚按下 Ctrl+C 的那个应用。
// 例外：资源管理器右键复制、全局热键工具代劳 —— 会归属到 explorer.exe，
// 这类情况在文档里写明，不当成 bug。
inline ForegroundApp ForegroundAppOf() {
    ForegroundApp out;
    HWND fg = ::GetForegroundWindow();
    if (fg == nullptr) return out;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);
    if (pid == 0) return out;

    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) return out;
    wchar_t path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    if (::QueryFullProcessImageNameW(proc, 0, path, &size)) {
        std::wstring full(path, size);
        const std::size_t slash = full.find_last_of(L"\\/");
        std::wstring base = slash == std::wstring::npos ? full : full.substr(slash + 1);
        out.exe = Narrow(std::wstring(base));
        out.path = Narrow(full);
        if (base.size() > 4 && _wcsicmp(base.c_str() + base.size() - 4, L".exe") == 0) {
            base.resize(base.size() - 4);
        }
        out.label = Narrow(base);
    }
    ::CloseHandle(proc);
    return out;
}

// ---------------------------------------------------------------- 采集器

class LiveSources {
    enum class Command { kSeek, kTogglePlayPause };
    struct PendingCommand {
        Command cmd;
        std::int64_t value = 0;
        // 命令发给哪个应用。多播放器并行时必须指定，否则「拖 QQ 音乐的进度」
        // 会发给 SMTC 的「当前会话」（可能已经切到别人），表现为「拖了没反应」。
        std::string app_id;
    };

public:
    LiveSources() = default;
    ~LiveSources() { Stop(); }

    LiveSources(const LiveSources&) = delete;
    LiveSources& operator=(const LiveSources&) = delete;

    void Start() {
        if (running_.load()) return;
        running_.store(true);
        worker_ = std::thread([this] { Loop(); });
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
    }

    // 采集线程的诊断日志钩子（跨线程调用，注入方必须自己保证线程安全）。
    // 必须在 Start() 之前设置。定位「通知到底有没有采到」这类问题，
    // 日志是唯一可信的证据 —— 截图只看得到结果，看不到原因。
    void SetLogger(std::function<void(const std::string&)> fn) {
        log_fn_ = std::move(fn);
        agents_.SetLogger(log_fn_);
    }

    // Agent 采集的开关与上限由用户配置决定，热加载时会被反复调用。
    // 关掉后连目标目录都不扫（不是「扫了但不显示」）—— 用户关它往往就是
    // 不想让这个程序读那些会话文件。
    void ConfigureAgents(bool enabled, int max_islands, int idle_sec, bool suspended) {
        agents_.SetEnabled(enabled);
        agents_.SetMaxIslands(max_islands);
        agents_.SetIdleHideMs(static_cast<std::int64_t>(idle_sec) * 1000);
        agents_.SetSuppressed(suspended);
    }

    // 浏览器采集的开关。关掉后不打开任何浏览器的数据库 —— 见 rules.browsers 的注释。
    // 会议：只依赖 WASAPI，和浏览器一样由配置开关控制。
    // 编辑器：零 IO（只读前台窗口），所以允许和浏览器同频。
    void ConfigureEditors(bool enabled, bool suspended) {
        editors_.SetEnabled(enabled);
        editors_.SetSuppressed(suspended);
    }

    void ConfigureMeetings(bool enabled, bool suspended) {
        meetings_.SetEnabled(enabled);
        meetings_.SetSuppressed(suspended);
    }

    void ConfigureBrowsers(bool enabled, bool suspended) {
        browsers_.SetEnabled(enabled);
        browsers_.SetSuppressed(suspended);
    }

    // 仅调试：假装某个浏览器在前台。视觉回归需要「一个稳定的浏览器岛」，
    // 而真去开浏览器、翻到某个页面，截图就没法重复。
    void SetForceBrowserBrand(const std::string& brand) { browsers_.SetForceBrand(brand); }
    void SetBrowserLogger(std::function<void(const std::string&)> fn) { browsers_.SetLogger(std::move(fn)); }
    void SetMeetingLogger(std::function<void(const std::string&)> fn) { meetings_.SetLogger(std::move(fn)); }
    void SetEditorLogger(std::function<void(const std::string&)> fn) { editors_.SetLogger(std::move(fn)); }
    void SetForceEditor(const std::string& exe, const std::string& title) {
        editors_.SetForce(exe, title);
    }
    void SetFakeMeeting(const std::string& label) { meetings_.SetFake(label); }

    // 仅调试：让 Agent 监视器去读一份固定的假 home，验证「有清单的展开态」。
    void SetAgentHome(const std::wstring& home) { agents_.SetHomeOverride(home); }

    Snapshot Get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    // 由窗口线程在收到 WM_DEVICECHANGE 时调用。seq 是「变化检测」的唯一依据。
    void PushDeviceEvent(const std::string& label) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.device.seq += 1;
        snapshot_.device.label = label;
    }

    // ---- 媒体控制：窗口线程投递，采集线程消费 ----
    // WinRT 异步调用必须在已 init apartment 的线程上；主线程没有 apartment，
    // 直接调会失败。顺带把连点合并成一拍一个动作。
    void SeekToMs(std::int64_t ms, const std::string& app_id = std::string()) {
        if (ms < 0) ms = 0;
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        commands_.push_back({Command::kSeek, ms, app_id});
    }
    void TogglePlayPause(const std::string& app_id = std::string()) {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        commands_.push_back({Command::kTogglePlayPause, 0, app_id});
    }

    // ---- 「用户叉掉了这条」的抑制 ----
    // 只抑制「同一个应用的同一条内容」。绝不能只是「不再显示当前会话」：
    // 用户叉掉 A 之后往往会去开 B，而 B 刚启动时多半是暂停的、抢不到
    // SMTC 的「当前会话」——于是采集层继续只看到被抑制的 A，界面上就是
    // 「叉掉之后再打开软件不再生成新岛，直到把前一个进程结束掉」。
    // 所以抑制条件必须传给采集层：当前会话被抑制时，改挑另一个会话。
    //
    // 用列表而不是单个槽：并行任务有两条时，用户会连着叉两个，
    // 单槽会把前一条的抑制挤掉 —— 表现为「叉掉第二个，第一个又跳回来」。
    void SuppressMedia(const std::string& app_id, const std::string& title,
                       std::int64_t ttl_ms = 90 * 1000) {
        bool was_playing = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            was_playing = snapshot_.media.playing;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        // 同一条重复叉：刷新时间即可，不要堆重复项
        for (auto& s : sup_) {
            if (s.app == app_id && s.title == title) {
                s.until_tick = ::GetTickCount64() + static_cast<std::uint64_t>(ttl_ms);
                // 叉掉时正在暂停的，之后用户主动按播放应当重新出现（那是新的意图）；
                // 叉掉时正在播放的，就一直别再弹（用户就是要它闭嘴）。
                s.was_paused = !was_playing;
                s.saw_pause = false;
                return;
            }
        }
        if (sup_.size() >= 8) sup_.erase(sup_.begin());
        sup_.push_back({app_id, title, ::GetTickCount64() + static_cast<std::uint64_t>(ttl_ms),
                        !was_playing, false});
        alt_valid_ = false;  // 抑制集合变了，换会话的缓存要重算
    }

    // 清空所有「被叉掉」的抑制。第二次启动 exe 时主循环会调它，
    // 给用户一个「反悔 / 找回被隐藏的提示」的入口。
    void ClearSuppressedMedia() { ClearMediaSuppression(); }

    static constexpr int kTickMs = 100;         // 主循环节拍
    static constexpr int kMediaEveryTicks = 4;  // 400ms 采一次媒体
    // Agent 会话文件：800ms 一次。比媒体慢一点没关系（agent 的一步要几秒），
    // 但比 2s 快 —— 用户盯着看的时候，800ms 内的更新看起来才是「实时」的。
    static constexpr int kAgentEveryTicks = 8;
    // 浏览器：前台窗口标题是「便宜且要及时」的信息（用户切标签页要马上变），
    // 3 个 tick（300ms）足够跟手；数据库那半截自己在内部按 3s 节流。
    static constexpr int kBrowserEveryTicks = 3;
    // 会议每秒一次就够：麦克风会话的开关远没有标签页切换那么频繁，
    // 而每次枚举要过一遍 WASAPI，太勤只是白烧 CPU。
    static constexpr int kMeetingEveryTicks = 10;
    // 编辑器：每 3 个 tick（300ms）读一次前台窗口标题 —— 用户切文件要马上跟着变。
    static constexpr int kEditorEveryTicks = 3;
    // 通知采样的成本实测（notif_diag_probe --bench，通知中心 28 条 toast）：
    // 单次 GetNotificationsAsync 平均 130ms（跨进程枚举全部 toast）。
    // 常态 500ms 轮询会把空闲 CPU 从 ~2% 拉到 ~11% 单核，不可接受；
    // 常态 2s 轮询成本 ~2%，但「消息一到、聊天窗一点开就被撤掉」的 toast
    // 可能整条错过。折中：常态 2s，**捕获到新通知后的 10 秒内切到 500ms**
    // —— 聊天消息成串到达，第一条靠 2s 抓，后续串内消息快速抓。
    // （NotificationChanged 事件对未打包进程一律 0x80070490，只能轮询。）
    static constexpr int kNotifEveryTicks = 20;  // 常态 2000ms 采一次通知
    static constexpr int kNotifFastTicks = 5;    // 快速窗口内 500ms
    static constexpr std::int64_t kNotifFastWindowMs = 10000;  // 窗口时长
    static constexpr int kAudioRetryTicks = 50; // 5s 重试一次音频设备

private:
    void Loop() {
        // 只走 C++/WinRT 的 apartment 初始化：它内部已处理 CoInitializeEx，
        // 再单独调一次会 RPC_E_CHANGED_MODE。
        bool co_ready = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            co_ready = true;
        } catch (...) {
        }

        InitAudio();
        InitNotifications();
        InitFileBaseline();
        InitClipboardBaseline();

        int tick = 0;
        while (running_.load()) {
            const auto t0 = std::chrono::steady_clock::now();

            if (tick % kMediaEveryTicks == 0) PollMedia();
            // Agent：只读文件，成本取决于「这一拍新增了多少字节」，
            // 空闲会话几乎为零（只 stat 一次），所以可以固定 800ms。
            if (tick % kAgentEveryTicks == 0) PollAgents();
            if (tick % kBrowserEveryTicks == 0) PollBrowsers();
            if (tick % kMeetingEveryTicks == 0) PollMeetings();
            if (tick % kEditorEveryTicks == 0) PollEditors();
            DrainCommands();
            // 通知：常态 2s，捕获到新通知后的快速窗口内 500ms（见常量注释）。
            // 事件订阅在未打包进程被系统拒绝（0x80070490），订阅成功时
            // 置脏标志让下一拍立即采，与轮询互补。
            {
                const bool fast = WallNowFileTimeMs() < notif_fast_until_ms_;
                const int every = fast ? kNotifFastTicks : kNotifEveryTicks;
                if (notif_dirty_.exchange(false) || tick % every == 0) {
                    PollNotifications();
                }
            }
            if (!volume_ && tick % kAudioRetryTicks == 0) InitAudio();
            PollMisc();
            PollClipboard();
            PollFile();

            ++tick;
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count();
            const long long remain = kTickMs - spent;
            ::Sleep(remain > 0 ? static_cast<DWORD>(remain) : 1);
        }

        if (co_ready) {
            try {
                winrt::uninit_apartment();
            } catch (...) {
            }
        }
    }

    // Agent 会话：本拍读到的活跃列表直接替换快照。没有「变化检测」的语义：
    // 每条会话的状态每分钟都在变，逐字段比对新旧反而更容易出错。
    // 时间基准必须是 Unix epoch（会话文件里的 timestamp 就是它）——
    // WallNowFileTimeMs 走的是 1601 纪元，两者相差 1.16e14 ms，
    // 直接相减会让「最近 45 分钟内动过的文件」全部被判定成过旧。
    void PollAgents() {
        const std::int64_t wall = win32::NowUnixMs();
        agents_.Poll(wall);
        std::vector<AgentSession> list = agents_.Active();
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.agents = std::move(list);
        snapshot_.wall_now_ms = wall;
    }

    // 浏览器：窗口标题每一拍都可能变（用户在哪读一句话的时间都可能切走），
    // 所以节拍比 agent 快得多；真正的 IO（History/Favicons/downloads）
    // 在 BrowserWatcher 内部按 3s / 6s 自己节流。
    void PollBrowsers() {
        browsers_.Poll(win32::NowUnixMs());
        std::vector<BrowserPage> list = browsers_.Active();
        if (log_fn_ && !browser_reported_) {
            browser_reported_ = true;
            Log("browser: first poll -> " + std::to_string(list.size()) + " page(s); " +
                browsers_.Diag());
        }
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.browsers = std::move(list);
    }

    void PollEditors() {
        editors_.Poll(win32::NowUnixMs());
        std::vector<EditorState> list = editors_.Active();
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.editors = std::move(list);
    }

    void PollMeetings() {
        meetings_.Poll(win32::NowUnixMs());
        const auto list = meetings_.Active();
        std::vector<MeetingSnapshot> out;
        out.reserve(list.size());
        for (const auto& m : list) {
            MeetingSnapshot snap;
            snap.brand = m.brand;
            snap.label = m.label;
            snap.exe = m.exe;
            snap.elapsed_ms = m.elapsed_ms;
            out.push_back(std::move(snap));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.meetings = std::move(out);
    }

    // 调试：采集层这一拍看到了什么。
    std::string BrowserDiag() const { return browsers_.Diag(); }

    void DrainCommands() {        std::vector<PendingCommand> pending;
        {
            std::lock_guard<std::mutex> lock(cmd_mutex_);
            pending.swap(commands_);
        }
        if (pending.empty()) return;
        for (const auto& c : pending) {
            // 按 app_id 找会话：多播放器并行时「当前会话」可能已经不是用户
            // 正在操作的那一个，命令发错对象就表现为「拖了没反应」。
            auto target = media_session_;
            if (!c.app_id.empty() && manager_) {
                try {
                    for (const auto& sess : manager_.GetSessions()) {
                        if (!sess) continue;
                        if (Narrow(sess.SourceAppUserModelId()) == c.app_id) {
                            target = sess;
                            break;
                        }
                    }
                } catch (...) {
                }
            }
            if (!target) continue;
            try {
                if (c.cmd == Command::kSeek) {
                    // 投影签名是 TryChangePlaybackPositionAsync(int64_t)，单位 100ns，
                    // 不是 TimeSpan；传 TimeSpan 会 C2664。毫秒 × 10000 即可。
                    // 返回值必须看：部分播放器（腾讯视频等）不支持 SMTC 定位，
                    // 会直接返回 false —— 不记的话界面上看起来就是「拖了没反应」。
                    const bool accepted =
                        target.TryChangePlaybackPositionAsync(c.value * 10000).get();
                    if (!accepted) {
                        Log("seek: player rejected position change (unsupported) app=" +
                            (c.app_id.empty() ? std::string("(current)") : c.app_id));
                    }
                } else {
                    target.TryTogglePlayPauseAsync().get();
                }
            } catch (...) {
                // 播放器不暴露该控制（或会话刚切走）：静默忽略，不影响采集。
            }
        }
    }

    void SetError(const char* what, const winrt::hresult_error& e) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s failed: 0x%08X", what, static_cast<unsigned>(e.code()));
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.last_error = buf;
    }

    // ---- 媒体：SMTC ----
    // 返回命中的抑制项下标（-1 = 没被抑制）。锁内只做比较，不产生副作用。
    int SuppressedIndexOf(const MediaState& m, bool* was_paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t now = ::GetTickCount64();
        for (std::size_t i = 0; i < sup_.size(); ++i) {
            if (sup_[i].until_tick != 0 && now > sup_[i].until_tick) continue;
            if (sup_[i].app == m.app_id && sup_[i].title == m.title) {
                if (was_paused != nullptr) *was_paused = sup_[i].was_paused;
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // 抑制只对「还在跑的会话」有效。应用关掉后它的抑制必须立刻作废 ——
    // 否则「叉掉岛 → 关掉播放器 → 重新打开」会发现岛再也不出现
    // （会话标题一模一样，抑制一直命中），只能靠杀进程或等超时。
    void PruneSuppressionsByLiveApps(
        const winrt::Windows::Foundation::Collections::IVectorView<
            winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession>& sessions) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sup_.empty()) return;
        }
        std::vector<std::string> live_apps;
        for (const auto& s : sessions) {
            if (!s) continue;
            try {
                live_apps.push_back(Narrow(s.SourceAppUserModelId()));
            } catch (...) {
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sup_.begin(); it != sup_.end();) {
            bool alive = false;
            for (const auto& a : live_apps) {
                if (a == it->app) {
                    alive = true;
                    break;
                }
            }
            if (!alive) {
                it = sup_.erase(it);
                alt_valid_ = false;
            } else {
                ++it;
            }
        }
    }

    void DropSuppression(int index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= 0 && index < static_cast<int>(sup_.size())) {
            sup_.erase(sup_.begin() + index);
        }
        alt_valid_ = false;
    }

    // 抑制期间盯一下播放状态：只要中间出现过暂停，之后再播放就当作
    // 「用户重新开始听」，解除抑制。回到 true = 应当解除。
    bool SuppressionShouldLift(int index, bool playing) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || index >= static_cast<int>(sup_.size())) return false;
        if (!playing) {
            sup_[index].saw_pause = true;
            return false;
        }
        return sup_[index].saw_pause;
    }

    void ClearMediaSuppression() {
        std::lock_guard<std::mutex> lock(mutex_);
        sup_.clear();
        alt_valid_ = false;
    }

    // 当前会话被用户叉掉时，从其余会话里挑一个能显示的。
    // 属性读取（异步、会阻塞）按「会话变化 / 事件置脏 / 每 ~1.6s」节流；
    // 播放状态与进度是同步属性，每拍都读，成本可忽略。
    bool SwitchToOtherSession(
        const winrt::Windows::Foundation::Collections::IVectorView<
            winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession>& sessions,
        const std::string& skip_app, MediaState* out) {
        if (out == nullptr) return false;
        for (const auto& s : sessions) {
            if (!s) continue;
            std::string app;
            try {
                app = Narrow(s.SourceAppUserModelId());
            } catch (...) {
                continue;
            }
            if (app.empty() || app == skip_app) continue;

            const bool cache_ok = alt_valid_ && alt_app_ == app && !media_props_dirty_.load() &&
                                  (media_poll_count_ % 4) != 0;
            MediaState alt;
            if (cache_ok) {
                alt = alt_cache_;
            } else {
                try {
                    auto props = s.TryGetMediaPropertiesAsync().get();
                    if (!props) continue;
                    alt.present = true;
                    alt.app_id = app;
                    alt.app_label = diw::detail::FriendlyAppName(app);
                    alt.title = Narrow(props.Title());
                    if (alt.title.empty()) continue;  // 空标题的会话没东西可展示
                    alt.artist = Narrow(props.Artist());
                    alt.album = Narrow(props.AlbumTitle());
                    alt.cover_path = SaveCoverToCache(
                        props.Thumbnail(), app + "|" + alt.title + "|" + alt.album);
                    media_props_dirty_.store(false);
                } catch (...) {
                    continue;
                }
                alt_cache_ = alt;
                alt_app_ = app;
                alt_valid_ = true;
            }

            // 这条也被用户叉过：换下一个候选。
            // 少了这一步，两个应用都被叉时会互相顶出来（「叉掉 A 冒出 B，
            // 叉掉 B 又冒回 A」）。
            if (SuppressedIndexOf(alt, nullptr) >= 0) continue;

            try {
                if (auto info = s.GetPlaybackInfo()) {
                    alt.playing =
                        info.PlaybackStatus() ==
                        winrt::Windows::Media::Control::
                            GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                }
                if (auto timeline = s.GetTimelineProperties()) {
                    alt.position_ms = timeline.Position().count() / 10000;
                    alt.end_ms = timeline.EndTime().count() / 10000;
                }
            } catch (...) {
            }

            if (alt.app_id != app_icon_aumid_) {
                app_icon_aumid_ = alt.app_id;
                app_icon_path_ = AppIconForAumid(alt.app_id);
                Log("media: app_id=\"" + alt.app_id + "\" icon=" +
                    (app_icon_path_.empty() ? "none" : "ok"));
            }
            alt.icon_path = app_icon_path_;

            // 事件挂钩与命令路由都跟着切：否则 seek / 播放暂停会发给被抑制的那个。
            if (s != media_session_) {
                props_revoker_ = s.MediaPropertiesChanged(
                    winrt::auto_revoke,
                    [this](const winrt::Windows::Foundation::IInspectable&,
                           const winrt::Windows::Media::Control::MediaPropertiesChangedEventArgs&) {
                        media_props_dirty_.store(true);
                    });
                media_session_ = s;
            }
            props_session_id_ = alt.app_id;
            props_loaded_ = true;
            *out = std::move(alt);
            return true;
        }
        alt_valid_ = false;
        return false;
    }

    // 每个会话的 props 缓存。TryGetMediaPropertiesAsync 是异步阻塞调用，
    // 不能每拍对每个会话都读 —— 多播放器并行时会变成一串串行等待。
    // 节流：正在播的 1.6s 一次，其余 4.8s 一次；有属性变更事件时全部重读。
    struct MediaProps {
        std::string title, artist, album, cover;
        int last_read = -1000;
    };

    void PollMedia() {
        try {
            if (!manager_) {
                manager_ = winrt::Windows::Media::Control::
                               GlobalSystemMediaTransportControlsSessionManager::RequestAsync()
                                   .get();
                if (!manager_) return;
            }
            ++media_poll_count_;

            auto sessions = manager_.GetSessions();
            // 抑制项只对「还在跑的会话」有效（关掉播放器再打开要能重新出现）
            PruneSuppressionsByLiveApps(sessions);
            const bool props_dirty = media_props_dirty_.exchange(false);

            std::vector<MediaState> list;
            for (const auto& sess : sessions) {
                if (!sess) continue;
                MediaState m;
                try {
                    m.present = true;
                    m.app_id = Narrow(sess.SourceAppUserModelId());
                } catch (...) {
                    continue;
                }
                if (m.app_id.empty()) continue;
                m.app_label = diw::detail::FriendlyAppName(m.app_id);
                try {
                    if (auto info = sess.GetPlaybackInfo()) {
                        m.playing =
                            info.PlaybackStatus() ==
                            winrt::Windows::Media::Control::
                                GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                    }
                    if (auto timeline = sess.GetTimelineProperties()) {
                        m.position_ms = timeline.Position().count() / 10000;  // 100ns → ms
                        m.end_ms = timeline.EndTime().count() / 10000;
                    }
                } catch (...) {
                }

                MediaProps& pc = media_props_[m.app_id];
                const int period = m.playing ? 4 : 12;  // 1.6s / 4.8s
                if (props_dirty || media_poll_count_ - pc.last_read >= period) {
                    try {
                        if (auto props = sess.TryGetMediaPropertiesAsync().get()) {
                            pc.title = Narrow(props.Title());
                            pc.artist = Narrow(props.Artist());
                            pc.album = Narrow(props.AlbumTitle());
                            pc.cover = SaveCoverToCache(
                                props.Thumbnail(), m.app_id + "|" + pc.title + "|" + pc.album);
                        }
                    } catch (...) {
                    }
                    pc.last_read = media_poll_count_;
                }
                m.title = pc.title;
                m.artist = pc.artist;
                m.album = pc.album;
                m.cover_path = pc.cover;

                // 应用图标：扫开始菜单 / AppsFolder 很贵，app 不变就不重复解析
                if (m.app_id != app_icon_aumid_) {
                    app_icon_aumid_ = m.app_id;
                    app_icon_path_ = AppIconForAumid(m.app_id);
                    Log("media: app_id=\"" + m.app_id + "\" icon=" +
                        (app_icon_path_.empty() ? "none" : "ok"));
                }
                m.icon_path = app_icon_path_;

                // 用户叉掉过这一条：只打标记（数据保持完整，见 MediaState::suppressed）
                bool sup_was_paused = false;
                const int sup_hit = SuppressedIndexOf(m, &sup_was_paused);
                if (sup_hit >= 0) {
                    if (sup_was_paused && m.playing) {
                        DropSuppression(sup_hit);
                        Log("media: suppression lifted (playback resumed)");
                    } else if (SuppressionShouldLift(sup_hit, m.playing)) {
                        DropSuppression(sup_hit);
                        Log("media: suppression lifted (pause -> play)");
                    } else {
                        m.suppressed = true;
                    }
                }
                list.push_back(std::move(m));
            }

            // 正在播的排前面；再按 app_id 排，保证顺序稳定（否则岛会互相换位）
            std::sort(list.begin(), list.end(), [](const MediaState& a, const MediaState& b) {
                if (a.playing != b.playing) return a.playing;
                return a.app_id < b.app_id;
            });

            // 去重：同一个播放器常常注册了多个 SMTC 会话（主窗口 / 广告窗口 /
            // WebView 壳各一个），内容完全一样 —— 不同 app_id、同标题同歌手。
            // 不去重就会在屏幕上出现两个一模一样的岛（用户实测截图就是这样）。
            // 键用「标题 + 歌手」：不同应用放同一首歌本来也分不出区别，合并更合理。
            {
                std::set<std::string> seen;
                std::vector<MediaState> uniq;
                uniq.reserve(list.size());
                int merged = 0;
                for (auto& m : list) {
                    if (!m.title.empty()) {
                        const std::string key = m.title + "|" + m.artist;
                        if (!seen.insert(key).second) {
                            ++merged;
                            continue;
                        }
                    }
                    uniq.push_back(std::move(m));
                }
                if (merged > 0) {
                    Log("media: merged " + std::to_string(merged) +
                        " duplicate session(s) (same title)");
                }
                list.swap(uniq);
            }

            // 事件挂到「当前会话」上：切歌最敏感的那一条靠它即时刷新
            auto current = manager_.GetCurrentSession();
            if (current && current != media_session_) {
                props_revoker_ = current.MediaPropertiesChanged(
                    winrt::auto_revoke,
                    [this](const winrt::Windows::Foundation::IInspectable&,
                           const winrt::Windows::Media::Control::MediaPropertiesChangedEventArgs&) {
                        media_props_dirty_.store(true);
                    });
                media_session_ = current;
            }

            std::string prev_title, prev_cover;
            std::size_t prev_count = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                prev_title = snapshot_.media.title;
                prev_cover = snapshot_.media.cover_path;
                prev_count = snapshot_.media_all.size();
                snapshot_.media_all = list;
                snapshot_.media = list.empty() ? MediaState{} : list.front();
            }
            // 会话数变化 / 主媒体属性变化时各记一行：「为什么只有一个岛」全靠它定位
            if (list.size() != prev_count) {
                Log("media: sessions=" + std::to_string(list.size()));
            }
            if (!list.empty() && (list.front().title != prev_title ||
                                  list.front().cover_path != prev_cover)) {
                Log("media: props \"" + list.front().title + "\" cover=" +
                    (list.front().cover_path.empty() ? "none" : "ok"));
            }
        } catch (const winrt::hresult_error& e) {
            SetError("PollMedia", e);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.last_error = "PollMedia unknown failure";
        }
    }

    // ---- 通知：UserNotificationListener ----
    void InitNotifications() {
        try {
            listener_ =
                winrt::Windows::UI::Notifications::Management::UserNotificationListener::Current();
            if (!listener_) {
                Log("notif: listener unavailable");
                return;
            }
            const auto status = listener_.GetAccessStatus();
            const bool allowed =
                status == winrt::Windows::UI::Notifications::Management::
                              UserNotificationListenerAccessStatus::Allowed;
            if (allowed) {
                // 事件驱动：通知的增加/移除立刻置脏，主循环下一拍就去采。
                // 只轮询时，QQ/微信「消息一到、聊天窗一点开就把 toast
                // 从通知中心撤掉」的行为会整个错过（撤得比 2s 轮询快）。
                try {
                    notif_revoker_ = listener_.NotificationChanged(
                        winrt::auto_revoke,
                        [this](const winrt::Windows::UI::Notifications::Management::
                                   UserNotificationListener&,
                               const winrt::Windows::UI::Notifications::
                                   UserNotificationChangedEventArgs&) {
                            notif_dirty_.store(true);
                        });
                    Log("notif: listener allowed, event-driven");
                } catch (const winrt::hresult_error& e) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "notif: event subscribe failed 0x%08X",
                                  static_cast<unsigned>(e.code()));
                    Log(std::string(buf) + " (poll-only)");
                } catch (...) {
                    Log("notif: event subscribe failed unknown (poll-only)");
                }
            } else {
                Log("notif: listener access denied (check notification settings)");
            }
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.notif.ok = allowed;
        } catch (...) {
            Log("notif: init failed");
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.notif.ok = false;
        }
    }

    void PollNotifications() {
        if (!listener_) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!snapshot_.notif.ok) return;
        }
        try {
            auto items = listener_
                             .GetNotificationsAsync(
                                 winrt::Windows::UI::Notifications::NotificationKinds::Toast)
                             .get();
            if (!items) return;

            // 首次采样只建立基线：否则程序一启动就会把系统里已有的旧通知
            // 全当成「刚收到的新消息」刷出来。
            std::unordered_set<std::uint32_t> now_ids;
            // UserNotification 属于 Windows::UI::Notifications，不在 Management 子命名空间
            //（Management 里只有 Listener 与 AccessStatus）。
            winrt::Windows::UI::Notifications::UserNotification newest{nullptr};
            std::uint32_t newest_id = 0;
            const std::uint32_t count = items.Size();
            for (std::uint32_t i = 0; i < count; ++i) {
                auto n = items.GetAt(i);
                const std::uint32_t id = n.Id();
                now_ids.insert(id);
                if (seen_ids_.find(id) == seen_ids_.end() && id >= newest_id) {
                    newest_id = id;
                    newest = n;
                }
            }

            // 基线判定必须用「是不是第一次采样」这个显式标志，
            // 不能用 seen_ids_.empty()：用户点过「清除所有通知」之后集合会变空，
            // 于是下一条真实通知被当成历史基线吞掉（永远不弹）。
            const bool first_sample = !notif_primed_;
            notif_primed_ = true;
            seen_ids_ = std::move(now_ids);
            if (first_sample || !newest) return;

            std::string app, app_id, title, body, icon;
            try {
                app = Narrow(newest.AppInfo().DisplayInfo().DisplayName());
            } catch (...) {
            }
            try {
                app_id = Narrow(newest.AppInfo().AppUserModelId());
            } catch (...) {
            }
            try {
                auto binding = newest.Notification().Visual().GetBinding(
                    winrt::Windows::UI::Notifications::KnownNotificationBindings::ToastGeneric());
                if (binding) {
                    auto texts = binding.GetTextElements();
                    if (texts.Size() > 0) title = Narrow(texts.GetAt(0).Text());
                    if (texts.Size() > 1) body = Narrow(texts.GetAt(1).Text());
                }
            } catch (...) {
            }
            // 来源应用图标，两级回落：
            //   1) WinRT AppInfo().DisplayInfo().GetLogo —— 实测对本机所有应用
            //      一律返回 null（见文件头注释），但保留调用以防个别环境可用；
            //   2) Win32：AUMID → 开始菜单 .lnk → 目标 exe 图标 → PNG 缓存。
            // 都拿不到就留空，渲染层回落默认图形。
            if (!app_id.empty()) {
                try {
                    icon = SaveCoverToCache(
                        newest.AppInfo().DisplayInfo().GetLogo(
                            winrt::Windows::Foundation::Size{96.0f, 96.0f}),
                        "appicon|" + app_id);
                } catch (...) {
                }
                if (icon.empty()) icon = AppIconForAumid(app_id);
            }
            // 采到即记一行：这是「QQ/微信消息到底进没进通知中心」的唯一证据。
            Log("notif: id=" + std::to_string(newest_id) + " app=" +
                (app.empty() ? app_id : app) + " title=\"" +
                (title.size() > 40 ? title.substr(0, 40) + "…" : title) + "\" icon=" +
                (icon.empty() ? "none" : "ok"));

            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.notif.seq += 1;
            snapshot_.notif.app = app;
            snapshot_.notif.app_id = app_id;
            snapshot_.notif.title = title;
            snapshot_.notif.body = body;
            snapshot_.notif.icon_path = icon;
            // 进入快速采样窗口：聊天消息通常成串到达，后续几条要在
            // 它们被应用撤掉之前抓到（窗口时长见 kNotifFastWindowMs）。
            notif_fast_until_ms_ = WallNowFileTimeMs() + kNotifFastWindowMs;
        } catch (const winrt::hresult_error& e) {
            SetError("PollNotifications", e);
        } catch (...) {
        }
    }

    // ---- 剪贴板：文本 / 图片 / 截图 / 文件 ----
    void InitClipboardBaseline() {
        // 不读内容，只对齐序列号：启动前剪贴板里已有的东西不该被当成「刚复制」。
        clip_seq_seen_ = ::GetClipboardSequenceNumber();
    }

    void PollClipboard() {
        const DWORD seq = ::GetClipboardSequenceNumber();
        if (seq == 0 || seq == clip_seq_seen_) return;
        // 别的程序正占用剪贴板时不推进基线，下一拍重试，避免丢事件。
        if (!::OpenClipboard(nullptr)) return;

        ClipState clip;
        clip.seq = seq;
        // 来源应用必须在读取前取：Ctrl+C 不改变焦点，此时前台一般仍是复制方。
        const ForegroundApp fg = ForegroundAppOf();
        clip.app_id = fg.exe;
        clip.app_label = fg.label;
        clip.app_path = fg.path;

        if (HANDLE hdrop = ::GetClipboardData(CF_HDROP)) {
            const HDROP drop = static_cast<HDROP>(hdrop);
            const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
            wchar_t first[MAX_PATH] = {0};
            if (count > 0) {
                ::DragQueryFileW(drop, 0, first, MAX_PATH);
            }
            clip.kind = "files";
            clip.item_count = static_cast<int>(count);
            clip.text = Narrow(std::wstring(first));
        } else if (::IsClipboardFormatAvailable(CF_DIB) ||
                   ::IsClipboardFormatAvailable(CF_DIBV5)) {
            if (HANDLE hdib = ::GetClipboardData(CF_DIB)) {
                if (auto* bi = static_cast<BITMAPINFOHEADER*>(::GlobalLock(hdib))) {
                    clip.width = static_cast<int>(bi->biWidth);
                    clip.height = static_cast<int>(bi->biHeight < 0 ? -bi->biHeight : bi->biHeight);
                    ::GlobalUnlock(hdib);
                }
            }
            // 截图工具走的是同一条剪贴板通道，只能靠来源进程区分。
            const std::string app = AsciiLower(clip.app_id);
            const bool snipping = app.find("snipping") != std::string::npos ||
                                  app.find("screenclip") != std::string::npos ||
                                  app.find("screenshot") != std::string::npos ||
                                  app.find("snipaste") != std::string::npos ||
                                  app.find("screen") != std::string::npos;
            clip.kind = snipping ? "screenshot" : "image";
            if (clip.width > 0 && clip.height > 0) {
                clip.text = std::to_string(clip.width) + "×" + std::to_string(clip.height);
            }
        } else if (::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
                    // 只留前 400 字符：胶囊放不下长文本，也没必要把剪贴板内容长期留在内存。
                    std::wstring text(p, ::wcsnlen(p, 400));
                    clip.text = Narrow(text);
                    ::GlobalUnlock(h);
                }
            }
            clip.kind = "text";
        }
        ::CloseClipboard();

        if (clip.kind.empty()) {
            clip_seq_seen_ = seq;
            return;
        }
        clip_seq_seen_ = seq;
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.clipboard = clip;
    }

    // ---- 电源 / 音量 / 全屏 ----
    void PollMisc() {
        SYSTEM_POWER_STATUS ps{};
        if (::GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255) {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.power.known = true;
            snapshot_.power.has_battery = true;
            snapshot_.power.ac_online = ps.ACLineStatus == 1;
            snapshot_.power.pct = static_cast<int>(ps.BatteryLifePercent);
        } else if (::GetSystemPowerStatus(&ps)) {
            // 台式机：没有电池，但交流状态仍然可读。
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.power.known = true;
            snapshot_.power.has_battery = false;
            snapshot_.power.ac_online = ps.ACLineStatus == 1;
        }

        if (volume_) {
            float scalar = 0.0f;
            BOOL mute = FALSE;
            if (SUCCEEDED(volume_->GetMasterVolumeLevelScalar(&scalar)) &&
                SUCCEEDED(volume_->GetMute(&mute))) {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.volume_pct = static_cast<int>(scalar * 100.0f + 0.5f);
                snapshot_.muted = mute != FALSE;
                snapshot_.volume_known = true;
            }
        }

        // 全屏检测：这三种状态都表示「现在不该打扰用户」。
        QUERY_USER_NOTIFICATION_STATE state{};
        if (SUCCEEDED(::SHQueryUserNotificationState(&state))) {
            const bool fullscreen = state == QUNS_BUSY ||
                                    state == QUNS_RUNNING_D3D_FULL_SCREEN ||
                                    state == QUNS_PRESENTATION_MODE;
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.fullscreen = fullscreen;
        }
    }

    // ---- 文件落盘：下载目录最新文件 ----
    std::wstring NewestFileInDownloads(std::int64_t* stamp_ms) const {
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = downloads_dir_ + L"\\*";
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return {};

        std::wstring newest;
        FILETIME newest_time{};
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            if (newest.empty() || ::CompareFileTime(&fd.ftLastWriteTime, &newest_time) > 0) {
                newest = fd.cFileName;
                newest_time = fd.ftLastWriteTime;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);

        if (newest.empty()) return {};
        if (stamp_ms != nullptr) *stamp_ms = FileTimeToMs(newest_time);
        return newest;
    }

    void InitFileBaseline() {
        PWSTR path = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path)) && path) {
            downloads_dir_ = path;
            ::CoTaskMemFree(path);
        }
        if (downloads_dir_.empty()) return;
        // 启动前已经存在的文件不算「刚下载完」。
        std::int64_t stamp = 0;
        const std::wstring newest = NewestFileInDownloads(&stamp);
        if (!newest.empty()) last_file_stamp_ = stamp;
    }

    void PollFile() {
        if (downloads_dir_.empty()) return;
        std::int64_t stamp = 0;
        const std::wstring newest = NewestFileInDownloads(&stamp);
        if (newest.empty() || stamp == last_file_stamp_) return;

        last_file_stamp_ = stamp;
        const std::string name = Narrow(newest);
        const std::int64_t age = WallNowFileTimeMs() - stamp;
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.file.name = name;
        snapshot_.file.ext = ExtensionOf(name);
        snapshot_.file.age_ms = age;
    }

    void InitAudio() {
        try {
            winrt::com_ptr<IMMDeviceEnumerator> enumerator;
            if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(enumerator.put())))) {
                return;
            }
            winrt::com_ptr<IMMDevice> device;
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) return;
            winrt::com_ptr<IAudioEndpointVolume> vol;
            if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                        vol.put_void()))) {
                return;
            }
            volume_ = vol;
        } catch (...) {
        }
    }

    mutable std::mutex mutex_;
    Snapshot snapshot_;
    std::mutex cmd_mutex_;
    std::vector<PendingCommand> commands_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager manager_{nullptr};
    winrt::Windows::UI::Notifications::Management::UserNotificationListener listener_{nullptr};

    // 编码 Agent 的会话文件监视器。只碰文件系统，不需要 COM/WinRT，
    // 所以放在采集线程里跑没有额外的 apartment 约束。
    win32::AgentWatcher agents_;
    win32::BrowserWatcher browsers_;
    win32::MeetingWatcher meetings_;
    win32::EditorWatcher editors_;

    std::string props_session_id_;
    bool props_loaded_ = false;
    std::map<std::string, MediaProps> media_props_;  // app_id → 上次读到的属性
    std::unordered_set<std::uint32_t> seen_ids_;
    bool notif_primed_ = false;  // 首次采样只建基线，之后一律按「新 id」判定

    // ---- 事件驱动状态（修「切歌不刷新」与「通知撤太快采不到」）----
    // 会话对象与事件连接必须保活：C++/WinRT 的事件以对象为源，
    // 源被释放连接就断。revoker 在析构/重新赋值时自动撤销订阅。
    // 媒体应用图标的解析缓存：app_id 不变就不重复扫开始菜单。
    std::string app_icon_aumid_;
    std::string app_icon_path_;
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession media_session_{nullptr};
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession::
        MediaPropertiesChanged_revoker props_revoker_;
    std::atomic<bool> media_props_dirty_{true};
    int media_poll_count_ = 0;

    // 被用户叉掉的那几条媒体（app + 标题）。空 = 没有抑制。
    struct MediaSuppression {
        std::string app;
        std::string title;
        std::uint64_t until_tick = 0;
        bool was_paused = false;  // 叉掉的那一刻是暂停
        bool saw_pause = false;   // 抑制期间观察到过「暂停」
    };
    std::vector<MediaSuppression> sup_;

    // 抑制期间挑出来的「另一个会话」缓存，避免每 400ms 都做一次异步属性读取。
    MediaState alt_cache_;
    std::string alt_app_;
    bool alt_valid_ = false;
    std::string last_switch_app_;  // 只为日志去重：同一目标不重复刷
    winrt::Windows::UI::Notifications::Management::UserNotificationListener::
        NotificationChanged_revoker notif_revoker_;
    std::atomic<bool> notif_dirty_{false};

    std::function<void(const std::string&)> log_fn_;  // 可空；Start 前注入
    bool browser_reported_ = false;
    void Log(const std::string& line) {
        if (log_fn_) log_fn_(line);
    }

    // 通知快速采样窗口的截止时刻（wall clock ms）；0 = 不在窗口内
    std::int64_t notif_fast_until_ms_ = 0;

    winrt::com_ptr<IAudioEndpointVolume> volume_;

    std::wstring downloads_dir_;
    std::int64_t last_file_stamp_ = 0;
    DWORD clip_seq_seen_ = 0;
};

}  // namespace live
}  // namespace diw

#endif  // DIW_SRC_WIN32_LIVE_SOURCES_HPP
