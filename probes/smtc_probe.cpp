// 探针：验证能否从系统真实的媒体会话（SMTC）读到「此刻正在播放什么」。
//
// 这不是产品代码，是可行性验证。跑通即证明：平衡岛可以吃真实数据，不需要剧本。
//
// 用到的 API：
//   GlobalSystemMediaTransportControlsSessionManager —— 未打包的桌面应用也能用，
//   不需要 MSIX 身份。这是它能落地的关键前提（对比 UserNotificationListener 需要身份）。
//
// 编译：cl /std:c++17 /EHsc /W4 /DNOMINMAX probes\smtc_probe.cpp /link windowsapp.lib

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstdio>
#include <ctime>
#include <string>

using namespace winrt;
using namespace Windows::Media::Control;

namespace {

std::string Narrow(const wchar_t* w, int len) {
    if (w == nullptr || len <= 0) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), need, nullptr, nullptr);
    return out;
}

std::string Narrow(const std::wstring& w) {
    return Narrow(w.c_str(), static_cast<int>(w.size()));
}

// winrt::hstring 不能隐式转 std::wstring，必须显式重载，
// 否则每个 SourceAppUserModelId() / Title() 调用点都会 C2664。
std::string Narrow(const winrt::hstring& h) {
    return Narrow(h.c_str(), static_cast<int>(h.size()));
}

// TimeSpan 是 100ns 单位。
std::string MmSs(int64_t hundred_ns) {
    const int64_t total_ms = hundred_ns / 10000;
    const int64_t total_s = total_ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", total_s / 60, total_s % 60);
    return buf;
}

const char* StatusName(GlobalSystemMediaTransportControlsSessionPlaybackStatus s) {
    switch (s) {
        case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed: return "Closed";
        case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened: return "Opened";
        case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing: return "Changing";
        case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped: return "Stopped";
        case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing: return "Playing";
        case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused: return "Paused";
        default: return "Unknown";
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const bool watch = (argc > 1 && std::wstring(argv[1]) == L"--watch");
    const int rounds = watch ? 40 : 1;

    try {
        // MTA：不需要消息泵，适合无窗口的后台采样。
        init_apartment(apartment_type::multi_threaded);
    } catch (const hresult_error& e) {
        std::printf("init_apartment FAILED: 0x%08X\n", static_cast<unsigned>(e.code()));
        return 1;
    }

    auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    if (!manager) {
        std::printf("RequestAsync returned null manager\n");
        return 2;
    }
    std::printf("[probe] session manager acquired\n");

    // 列出所有会话：这一步能看出系统里到底有多少个媒体源（浏览器、播放器等各占一个）。
    auto sessions = manager.GetSessions();
    std::printf("[probe] sessions=%u\n", static_cast<unsigned>(sessions.Size()));
    for (uint32_t i = 0; i < sessions.Size(); ++i) {
        const auto s = sessions.GetAt(i);
        std::printf("  [%u] app_id=%s\n", i, Narrow(s.SourceAppUserModelId()).c_str());
    }

    std::string last_line;
    for (int r = 0; r < rounds; ++r) {
        auto session = manager.GetCurrentSession();
        if (!session) {
            std::printf("[%02d] (no current session — 系统里没有活跃媒体)\n", r);
        } else {
            auto props = session.TryGetMediaPropertiesAsync().get();
            auto info = session.GetPlaybackInfo();
            auto timeline = session.GetTimelineProperties();

            const std::string title = props ? Narrow(props.Title()) : "";
            const std::string artist = props ? Narrow(props.Artist()) : "";
            const std::string album = props ? Narrow(props.AlbumTitle()) : "";
            const std::string app = Narrow(session.SourceAppUserModelId());
            const char* status = info ? StatusName(info.PlaybackStatus()) : "?";

            // 封面：能拿到 IRandomAccessStreamReference 就说明可解码为位图。
            std::size_t art_bytes = 0;
            bool has_art = false;
            if (props) {
                auto thumb_ref = props.Thumbnail();
                if (thumb_ref) {
                    has_art = true;
                    try {
                        auto stream = thumb_ref.OpenReadAsync().get();
                        if (stream) art_bytes = static_cast<std::size_t>(stream.Size());
                    } catch (...) {
                    }
                }
            }

            char line[1024];
            std::snprintf(line, sizeof(line),
                          "[%02d] %s | %s — %s | album=%s | %s / %s | art=%s(%zu B) | app=%s",
                          r, status, artist.c_str(), title.c_str(), album.c_str(),
                          MmSs(timeline.Position().count()).c_str(),
                          MmSs(timeline.EndTime().count()).c_str(),
                          has_art ? "yes" : "no", art_bytes, app.c_str());

            // 只在内容变化时打印，避免刷屏；但每 10 轮强制打一次以显示时间在走。
            if (line != last_line || r % 10 == 0) {
                std::printf("%s\n", line);
                last_line = line;
            }
        }
        if (watch) ::Sleep(500);
    }

    std::printf("[probe] done\n");
    return 0;
}
