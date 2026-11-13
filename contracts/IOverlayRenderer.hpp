// DynamicIsland-Win / IOverlayRenderer.hpp
// 渲染层抽象：P0 Windhawk tool mod 与 P2 独立 WinUI3/Win32 进程共用此接口。
//
// 状态：接口契约（阶段 S3），已通过 MSVC C++17 编译验证。
// 目标标准：C++17。禁止在本接口引入任何平台头文件，便于核心域保持可测。
#ifndef DIW_CONTRACTS_I_OVERLAY_RENDERER_HPP
#define DIW_CONTRACTS_I_OVERLAY_RENDERER_HPP

#include <string>
#include <vector>

#include "Types.hpp"

namespace diw {

class IOverlayRenderer {
public:
    virtual ~IOverlayRenderer() = default;

    // 初始化分层窗口、DPI 与显示器绑定。返回 false 时核心域应降级为无渲染模式，
    // 不得把透明材质捕获失败当作功能不可用的理由（ZenDesktop 1.6.8 教训）。
    virtual bool Initialize(const RendererConfig& config) = 0;

    // 应用一帧调度结果。实现方须自行做「内容未变化不重绘」判断。
    virtual void Render(const Frame& frame) = 0;

    // 全屏/游戏/反作弊场景：隐藏胶囊并暂停非必要绘制，但不销毁活动状态。
    virtual void SetSuspended(bool suspended) = 0;

    // 透明效果不可用、远程桌面会话等场景的降级外观。
    virtual void SetBackdropMode(BackdropMode mode) = 0;

    virtual void Shutdown() = 0;
};

}  // namespace diw

#endif  // DIW_CONTRACTS_I_OVERLAY_RENDERER_HPP
