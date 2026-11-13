// 探针：制造一个「真实的系统媒体会话」，用于端到端验证灵动岛的媒体链路。
//
// 关键点：这不是伪造数据。MediaPlayer 注册的是和网易云 / QQ 音乐 / 浏览器
// 完全相同的 GlobalSystemMediaTransportControlsSessionManager 会话，
// 灵动岛那边读到的路径与实际听歌时一字不差。
//
// 用静音 WAV（全零采样），所以不会出声打扰。
// 标题故意写成「SMTC 链路验证」，避免和用户真实歌曲混淆。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

using namespace winrt;

namespace {

// 写一个 16bit 单声道 44.1kHz 的静音 WAV。
void WriteSilentWav(const std::wstring& path, int seconds) {
    constexpr int kRate = 44100;
    constexpr int kChannels = 1;
    constexpr int kBits = 16;
    const std::uint32_t data_bytes =
        static_cast<std::uint32_t>(kRate) * kChannels * (kBits / 8) * seconds;

    std::ofstream f(path, std::ios::binary);
    auto u32 = [&f](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&f](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    f.write("RIFF", 4);
    u32(36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    u32(16);
    u16(1);  // PCM
    u16(kChannels);
    u32(kRate);
    u32(kRate * kChannels * (kBits / 8));  // byte rate
    u16(kChannels * (kBits / 8));          // block align
    u16(kBits);
    f.write("data", 4);
    u32(data_bytes);

    // 分块写零，避免一次分配 5MB 缓冲。
    const std::size_t chunk = 65536;
    std::string zeros(chunk, '\0');
    std::uint32_t left = data_bytes;
    while (left > 0) {
        const std::size_t n = left < chunk ? left : chunk;
        f.write(zeros.data(), static_cast<std::streamsize>(n));
        left -= static_cast<std::uint32_t>(n);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    int hold_seconds = 30;
    // 切歌实验：同一个会话（app_id 不变）中途换曲目，用来验证
    // 「MediaPropertiesChanged / 定期重读」是否真的把新歌名与新封面刷出来。
    // 修之前 props 只在 app_id 变化时才重读，切歌后胶囊会一直停在上一首。
    std::wstring title = L"SMTC 链路验证";
    std::wstring title2;      // 非空则做切歌实验
    int switch_at = 0;        // 秒
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--hold=", 0) == 0) {
            hold_seconds = _wtoi(arg.c_str() + 7);
        } else if (arg.rfind(L"--title=", 0) == 0) {
            title = arg.substr(8);
        } else if (arg.rfind(L"--title2=", 0) == 0) {
            title2 = arg.substr(9);
        } else if (arg.rfind(L"--switch-at=", 0) == 0) {
            switch_at = _wtoi(arg.c_str() + 12);
        }
    }

    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (const hresult_error& e) {
        std::printf("init_apartment FAILED: 0x%08X\n", static_cast<unsigned>(e.code()));
        return 1;
    }

    wchar_t temp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, temp);
    const std::wstring wav_path = std::wstring(temp) + L"diw_silent_probe.wav";
    WriteSilentWav(wav_path, 120);
    std::printf("[inject] silent wav -> %ls\n", wav_path.c_str());

    try {
        auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(wav_path).get();
        if (!file) {
            std::printf("[inject] GetFileFromPathAsync returned null\n");
            return 2;
        }

        winrt::Windows::Media::Playback::MediaPlayer player;

        // 元数据必须挂在 MediaPlaybackItem 上，再用 ApplyDisplayProperties 提交。
        // 第一版用 player.SystemMediaTransportControls().DisplayUpdater() 设置，
        // 结果被 WAV 的空内嵌元数据覆盖，SMTC 里 title/artist 全是空 —— 探针实测发现。
        // MediaPlaybackItem 没有 CreateFromStorageFile 静态方法，用 MediaSource 构造。
        // 做成 lambda 是为了切歌实验能复用同一套属性设置（含封面）。
        // StorageFile API 在桌面进程里对 C://Windows 这类受保护目录会直接返回
        // 0x800700A1（路径无效，实测踩到）。先复制到 temp 再引用。
        const wchar_t* kCover = L"C:////Windows////Web////Wallpaper////Windows////img0.jpg";
        auto build_item = [&](const std::wstring& t) {
            winrt::Windows::Media::Playback::MediaPlaybackItem item(
                winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(file));
            auto props = item.GetDisplayProperties();
            props.Type(winrt::Windows::Media::MediaPlaybackType::Music);
            props.MusicProperties().Title(t.c_str());
            props.MusicProperties().Artist(L"DynamicIsland-Win");
            props.MusicProperties().AlbumTitle(L"真实会话通道");
            // 给会话挂一张真实封面，用来验证「SMTC Thumbnail → 解码 → 落盘 → 渲染」整条链路。
            if (::GetFileAttributesW(kCover) != INVALID_FILE_ATTRIBUTES) {
                try {
                    wchar_t tmp_dir[MAX_PATH]{};
                    ::GetTempPathW(MAX_PATH, tmp_dir);
                    const std::wstring cover_copy = std::wstring(tmp_dir) + L"diw_cover_probe.jpg";
                    ::CopyFileW(kCover, cover_copy.c_str(), FALSE);
                    auto cover_file =
                        winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(cover_copy).get();
                    props.Thumbnail(winrt::Windows::Storage::Streams::
                                        RandomAccessStreamReference::CreateFromFile(cover_file));
                } catch (const hresult_error& e) {
                    std::printf("[inject] thumbnail FAILED: 0x%08X\n",
                                static_cast<unsigned>(e.code()));
                }
            }
            item.ApplyDisplayProperties(props);
            return item;
        };

        auto item = build_item(title);
        std::printf("[inject] thumbnail attached for \"%ls\"\n", title.c_str());

        player.Source(item);
        player.AudioCategory(winrt::Windows::Media::Playback::MediaPlayerAudioCategory::Media);

        player.Play();
        std::printf("[inject] playing (silent). holding %d s...\n", hold_seconds);
        for (int i = 0; i < hold_seconds; ++i) {
            ::Sleep(1000);
            if (i % 5 == 4) std::printf("[inject] +%ds\n", i + 1);
            if (!title2.empty() && i + 1 == switch_at) {
                // 同一个 MediaPlayer（同一个 app_id）换曲目：模拟播放器切歌。
                auto item2 = build_item(title2);
                player.Source(item2);
                player.Play();
                std::printf("[inject] switch -> \"%ls\"\n", title2.c_str());
            }
        }

        player.Pause();
        player.Close();
        std::printf("[inject] done\n");
        ::DeleteFileW(wav_path.c_str());
    } catch (const hresult_error& e) {
        std::printf("[inject] FAILED: 0x%08X (%ls)\n", static_cast<unsigned>(e.code()),
                    e.message().c_str());
        return 3;
    }
    return 0;
}
