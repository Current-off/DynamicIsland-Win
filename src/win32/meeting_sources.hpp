#pragma once
// 会议检测：判断「你现在是不是在开会」。
//
// 为什么不用窗口标题猜：Zoom / 腾讯会议的主窗口在会议进行中和只是开着客户端时
// 长得几乎一样，标题也常常就是「Zoom Workplace」。按标题猜必然一半是误报。
//
// 真正的判据是**麦克风占用**：开会时这个进程一定在采音（除了全程静音的人，
// 那种情况本来也没什么可提醒的）。所以这里的做法是枚举默认麦克风的音频会话，
// 看哪个进程正在 Active 采音 —— 只知道「谁在用麦克风」，不听任何内容。
//
// 只依赖 WASAPI，不需要各家 SDK、不联网。

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/base.h>

#include "../core/browser_page.hpp"  // 复用 Lower / EndsWith 这类小工具

namespace bi {
namespace win32 {

struct MeetingBrand {
    const char* id;
    const char* label;
    const char* exes;  // '|' 分隔，小写
};

inline const MeetingBrand* MeetingBrandTable(int* count) {
    static const MeetingBrand kBrands[] = {
        {"wemeet", "腾讯会议", "wemeetapp.exe|wemeet.exe"},
        {"voov", "VooV Meeting", "voovmeeting.exe"},
        {"zoom", "Zoom", "zoom.exe"},
        {"teams", "Microsoft Teams", "ms-teams.exe|teams.exe"},
        {"dingtalk", "钉钉", "dingtalk.exe"},
        {"feishu", "飞书", "feishu.exe|lark.exe"},
        {"welink", "WeLink", "welink.exe"},
        {"discord", "Discord", "discord.exe"},
        // Discord 不算会议软件，但它确实会长期占麦克风。
        // 放进来的理由：用户在语音房里待着时，"正在通话 40 分钟"是同样的信息。
    };
    if (count != nullptr) *count = static_cast<int>(sizeof(kBrands) / sizeof(kBrands[0]));
    return kBrands;
}

struct MeetingState {
    std::string brand;
    std::string label;
    std::string exe;
    std::int64_t started_ms = 0;    // 第一次检测到「在用麦」的时刻
    std::int64_t last_active_ms = 0;
    std::int64_t elapsed_ms = 0;

    bool Present() const { return started_ms > 0 && !brand.empty(); }
};

class MeetingWatcher {
public:
    void SetEnabled(bool on) { enabled_ = on; }
    void SetSuppressed(bool on) { suppressed_ = on; }
    void SetLogger(std::function<void(const std::string&)> fn) { log_fn_ = std::move(fn); }
    void SetIdleHideMs(std::int64_t ms) { idle_hide_ms_ = std::max<std::int64_t>(10000, ms); }
    // 调试：伪造一个正在进行的会议。真去开个会才能截图是不现实的，
    // 而「会议中」这条岛的排版必须能被重复验证。
    void SetFake(const std::string& label) { fake_label_ = label; }

    void Poll(std::int64_t now_ms) {
        if (!enabled_ || suppressed_) {
            std::lock_guard<std::mutex> lock(mutex_);
            active_.clear();
            return;
        }

        if (!fake_label_.empty()) {
            // 假数据走和真数据完全相同的后半段（排序 / 计时 / 清理），
            // 这样截出来的图就是真实链路的产物，只是来源被替换了。
            MeetingState fake;
            fake.brand = "fake";
            fake.label = fake_label_;
            fake.exe = "fake.exe";
            if (fake_started_ == 0) fake_started_ = now_ms - 754000;  // 12:34
            fake.started_ms = fake_started_;
            fake.last_active_ms = now_ms;
            fake.elapsed_ms = now_ms - fake.started_ms;
            std::lock_guard<std::mutex> lock(mutex_);
            active_.assign(1, fake);
            return;
        }
        std::vector<std::pair<std::string, std::string>> live;  // (brand, exe)
        if (CollectCaptureUsers(&live)) {
            for (const auto& item : live) {
                MeetingState* st = Find(item.first);
                if (st == nullptr) {
                    MeetingState fresh;
                    const MeetingBrand* def = BrandById(item.first);
                    fresh.brand = item.first;
                    fresh.label = def != nullptr ? def->label : item.first;
                    fresh.exe = item.second;
                    fresh.started_ms = now_ms;
                    fresh.last_active_ms = now_ms;
                    by_brand_.push_back(fresh);
                    Log("meeting: start " + fresh.label + " (" + item.second + ")");
                } else {
                    st->last_active_ms = now_ms;
                }
            }
        }

        // 25 秒的宽限：会议软件切共享屏幕 / 短暂断流时麦克风会话会掉一下，
        // 立刻收岛会让岛在会议中反复闪烁。
        const std::int64_t grace = std::max<std::int64_t>(15000, idle_hide_ms_);
        std::vector<MeetingState> out;
        for (auto it = by_brand_.begin(); it != by_brand_.end();) {
            if (now_ms - it->last_active_ms > grace) {
                Log("meeting: end " + it->label);
                it = by_brand_.erase(it);
                continue;
            }
            it->elapsed_ms = now_ms - it->started_ms;
            out.push_back(*it);
            ++it;
        }
        // 时间长的排前面：同时开两个（腾讯会议 + Discord）时，正在进行的会议更值得占位。
        std::sort(out.begin(), out.end(), [](const MeetingState& a, const MeetingState& b) {
            return a.started_ms < b.started_ms;
        });

        std::lock_guard<std::mutex> lock(mutex_);
        active_ = std::move(out);
    }

    std::vector<MeetingState> Active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }

    void Invalidate() {
        std::lock_guard<std::mutex> lock(mutex_);
        by_brand_.clear();
        active_.clear();
    }

    std::string Diag() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return diag_;
    }

private:
    static const MeetingBrand* BrandById(const std::string& id) {
        int count = 0;
        const MeetingBrand* table = MeetingBrandTable(&count);
        for (int i = 0; i < count; ++i) {
            if (id == table[i].id) return &table[i];
        }
        return nullptr;
    }

    static bool ExeMatches(const std::string& exe, const std::string& list, std::string* brand_id) {
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t bar = list.find('|', start);
            const std::string one =
                list.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (!one.empty() && one == exe) {
                (void)brand_id;
                return true;
            }
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
        return false;
    }

    MeetingState* Find(const std::string& brand) {
        for (auto& m : by_brand_) {
            if (m.brand == brand) return &m;
        }
        return nullptr;
    }

    void Log(const std::string& line) {
        if (log_fn_) log_fn_(line);
    }

    // 枚举默认麦克风上所有正在采音的进程，映射成品牌。
    // 返回 false 表示这次枚举失败（设备被独占 / COM 未就绪），这一拍跳过。
    bool CollectCaptureUsers(std::vector<std::pair<std::string, std::string>>* out) {
        try {
            winrt::com_ptr<IMMDeviceEnumerator> enumerator;
            if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(enumerator.put())))) {
                diag_ = "no-enumerator";
                return false;
            }
            winrt::com_ptr<IMMDevice> mic;
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, mic.put()))) {
                diag_ = "no-capture-device";
                return false;
            }
            winrt::com_ptr<IAudioSessionManager2> manager;
            if (FAILED(mic->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                     manager.put_void()))) {
                diag_ = "no-session-manager";
                return false;
            }
            winrt::com_ptr<IAudioSessionEnumerator> sessions;
            if (FAILED(manager->GetSessionEnumerator(sessions.put()))) {
                diag_ = "no-enumerator2";
                return false;
            }
            int count = 0;
            sessions->GetCount(&count);
            const DWORD self = ::GetCurrentProcessId();
            for (int i = 0; i < count; ++i) {
                winrt::com_ptr<IAudioSessionControl> ctl;
                if (FAILED(sessions->GetSession(i, ctl.put()))) continue;
                winrt::com_ptr<IAudioSessionControl2> ctl2;
                if (FAILED(ctl->QueryInterface(IID_PPV_ARGS(ctl2.put())))) continue;
                DWORD pid = 0;
                if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || pid == self) continue;
                AudioSessionState state = AudioSessionStateInactive;
                if (FAILED(ctl2->GetState(&state))) continue;
                // 只有 Active 才算「正在采音」。Inactive 表示会话还在但没在录，
                // 比如会议软件开着但已离会 —— 那正是我们不想报的情况。
                if (state != AudioSessionStateActive) continue;
                const std::string exe = Lower(ProcessImageName(pid));
                if (exe.empty()) continue;
                int brands = 0;
                const MeetingBrand* table = MeetingBrandTable(&brands);
                for (int b = 0; b < brands; ++b) {
                    if (ExeMatches(exe, table[b].exes, nullptr)) {
                        out->push_back({table[b].id, exe});
                        break;
                    }
                }
            }
            diag_ = "sessions=" + std::to_string(count) + " matched=" + std::to_string(out->size());
            return true;
        } catch (...) {
            diag_ = "exception";
            return false;
        }
    }

    static std::string ProcessImageName(DWORD pid) {
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h == nullptr) return std::string();
        wchar_t buf[1024] = {0};
        DWORD size = 1024;
        std::wstring out;
        if (::QueryFullProcessImageNameW(h, 0, buf, &size) != 0) out.assign(buf, size);
        ::CloseHandle(h);
        const std::size_t slash = out.find_last_of(L"\\/");
        if (slash != std::wstring::npos) out = out.substr(slash + 1);
        if (out.empty()) return std::string();
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                               static_cast<int>(out.size()), nullptr, 0, nullptr,
                                               nullptr);
        std::string narrow(static_cast<std::size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, out.c_str(), static_cast<int>(out.size()), &narrow[0], need,
                              nullptr, nullptr);
        return narrow;
    }

    bool enabled_ = true;
    bool suppressed_ = false;
    std::int64_t idle_hide_ms_ = 25000;
    std::function<void(const std::string&)> log_fn_;

    std::vector<MeetingState> by_brand_;  // 只由采集线程访问
    std::vector<MeetingState> active_;
    mutable std::mutex mutex_;
    std::string diag_;
    std::string fake_label_;
    std::int64_t fake_started_ = 0;
};

}  // namespace win32
}  // namespace bi
