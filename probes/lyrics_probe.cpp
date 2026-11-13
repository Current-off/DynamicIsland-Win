// 在线歌词链路实测：用产品代码里的 WinHTTP 路径真发一次 LRCLIB 请求，
// 解析 LRC 时间轴并按 35s 进度选行。PowerShell 能访问不代表 WinHTTP 能，
// 所以这条必须单独跑。
//
// 用法：lyrics_probe.exe [artist] [title]
#include <cstdio>
#include <string>

#include "../../src/win32/lyrics.hpp"

using diw::win32::ExtractFirstNonEmptyJsonString;
using diw::win32::HttpGetUtf8;
using diw::win32::ParseLrc;
using diw::win32::SelectLyricLines;
using diw::win32::UrlEncodeUtf8;
using diw::win32::Utf8ToWide;

int main(int argc, char** argv) {
    const std::string artist = argc > 1 ? argv[1] : "周杰伦";
    const std::string title = argc > 2 ? argv[2] : "晴天";
    const std::string query = "/api/search?track_name=" + UrlEncodeUtf8(title) +
                              "&artist_name=" + UrlEncodeUtf8(artist);
    std::string body, error;
    const bool ok = HttpGetUtf8(L"lrclib.net", Utf8ToWide(query), &body, &error);
    std::printf("http ok=%d bytes=%zu error=[%s]\n", ok ? 1 : 0, body.size(), error.c_str());
    if (!ok) return 1;

    const std::string lrc = ExtractFirstNonEmptyJsonString(body, "syncedLyrics");
    const auto lines = ParseLrc(lrc);
    std::printf("lrc bytes=%zu lines=%zu\n", lrc.size(), lines.size());
    if (lines.empty()) return 2;
    for (std::size_t i = 0; i < 3 && i < lines.size(); ++i) {
        std::printf("  [%lld] %s\n", static_cast<long long>(lines[i].ms), lines[i].text.c_str());
    }
    const auto t = SelectLyricLines(lines, 35000);
    std::printf("at 35s: prev=[%s] cur=[%s] next=[%s]\n", t.prev.c_str(), t.cur.c_str(),
                t.next.c_str());
    return 0;
}
