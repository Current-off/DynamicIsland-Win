// 探针：检查当前 SMTC 会话到底有没有暴露专辑封面。
//
// 为什么单独写：产品里「封面读不到」和「封面解码失败」都会静默回退到色块图标，
// 从外部看不出是哪一步断的。这个探针把链路拆成四段逐段打印：
//   会话 → 媒体属性 → Thumbnail 句柄 → 打开的流大小。
//
// 用法：先起一个带封面的会话（probes/media_inject_probe.cpp），再跑本探针。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstdio>

using namespace winrt;

int wmain() {
    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (const hresult_error& e) {
        std::printf("init_apartment FAILED: 0x%08X\n", static_cast<unsigned>(e.code()));
        return 1;
    }

    try {
        using namespace winrt::Windows::Media::Control;
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        if (!manager) {
            std::printf("manager NULL\n");
            return 1;
        }

        auto session = manager.GetCurrentSession();
        if (!session) {
            std::printf("[1] no active session\n");
            return 2;
        }
        std::printf("[1] session app=%ls\n", session.SourceAppUserModelId().c_str());

        auto props = session.TryGetMediaPropertiesAsync().get();
        if (!props) {
            std::printf("[2] media properties NULL\n");
            return 3;
        }
        std::printf("[2] title=%ls artist=%ls album=%ls\n", props.Title().c_str(), props.Artist().c_str(),
                    props.AlbumTitle().c_str());

        auto thumb = props.Thumbnail();
        if (!thumb) {
            std::printf("[3] Thumbnail reference is NULL  <-- 会话没带封面\n");
            return 4;
        }
        std::printf("[3] Thumbnail reference OK\n");

        auto stream = thumb.OpenReadAsync().get();
        if (!stream) {
            std::printf("[4] OpenReadAsync returned NULL\n");
            return 5;
        }
        std::printf("[4] stream size=%llu bytes  content_type=%ls\n",
                    static_cast<unsigned long long>(stream.Size()), stream.ContentType().c_str());
        std::printf("RESULT: cover chain reachable\n");
        return 0;
    } catch (const hresult_error& e) {
        std::printf("FAILED: 0x%08X (%ls)\n", static_cast<unsigned>(e.code()), e.message().c_str());
        return 6;
    }
}
