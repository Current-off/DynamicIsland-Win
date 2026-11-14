// 编译冒烟测试：验证 contracts/*.hpp 在 C++17 下可通过 MSVC 编译。
// 不做任何运行时行为验证，仅覆盖头文件语法、类型完整性与接口可实例化性。
#include "IOverlayRenderer.hpp"
#include "IProvider.hpp"
#include "Types.hpp"

#include <cassert>
#include <cmath>
#include <memory>

namespace {

class NullRenderer : public bi::IOverlayRenderer {
public:
    bool Initialize(const bi::RendererConfig&) override { return true; }
    void Render(const bi::Frame&) override { render_count_++; }
    void SetSuspended(bool suspended) override { suspended_ = suspended; }
    void SetBackdropMode(bi::BackdropMode mode) override { mode_ = mode; }
    void Shutdown() override {}

    int render_count() const { return render_count_; }
    bool suspended() const { return suspended_; }
    bi::BackdropMode mode() const { return mode_; }

private:
    int render_count_ = 0;
    bool suspended_ = false;
    bi::BackdropMode mode_ = bi::BackdropMode::Acrylic;
};

class ClockProvider : public bi::IProvider {
public:
    std::string Name() const override { return "clock"; }
    bool IsSensitive() const override { return false; }
    void SetEnabled(bool enabled) override { enabled_ = enabled; }
    bool IsEnabled() const override { return enabled_; }
    std::vector<bi::ProviderUpdate> Poll(std::int64_t) override { return {}; }

private:
    bool enabled_ = false;  // 敏感/外接 Provider 默认关闭
};

}  // namespace

int main() {
    static_assert(bi::kProtocolVersion == 1, "protocol version must be 1");
    static_assert(bi::kMaxVisibleActivities == 4, "max visible must be 4");
    static_assert(bi::kAnimationMaxMs == 2000, "animation cap must be 2000ms");
    static_assert(bi::kExpandedMinHeight <= bi::kExpandedMaxHeight, "bad height range");

    bi::Activity a;
    a.id = "media:now-playing";
    a.kind = "media";
    a.title = "playing";
    a.relevance_score = 80.0;
    a.stale_at_ms = 5000;
    a.content_json = "{\"playing\":true}";

    bi::Frame frame;
    frame.visible.push_back(bi::ScheduledActivity{a, bi::Presentation::Compact,
                                                   "compactLeading"});
    frame.now_ms = 1000;

    NullRenderer renderer;
    assert(renderer.Initialize(bi::RendererConfig{}));
    renderer.Render(frame);
    renderer.SetSuspended(true);
    renderer.SetBackdropMode(bi::BackdropMode::OpaqueFallback);
    renderer.Shutdown();
    assert(renderer.render_count() == 1);
    assert(renderer.suspended());
    assert(renderer.mode() == bi::BackdropMode::OpaqueFallback);

    ClockProvider provider;
    assert(!provider.IsEnabled());
    assert(!provider.IsSensitive());
    provider.SetEnabled(true);
    assert(provider.IsEnabled());
    assert(provider.Poll(0).empty());

    assert(std::abs(bi::kCornerRadius - bi::kConcentricMargin - 24.0) < 1e-9);
    return 0;
}
