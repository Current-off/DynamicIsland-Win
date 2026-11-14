// balance_island 平衡岛 / 范式规则常量（跨语言契约，C++ 侧）
//
// 这些数字是 docs/PARADIGM.md 的可执行形式。Python 侧对应 core/balance.py，
// 资源侧对应 contracts/balance-paradigm.json，三者由 tools/check_contracts.py
// 逐一比对。**规则必须是可断言的数字，不能只写在文档里。**
#ifndef BI_CONTRACTS_BALANCE_RULES_HPP
#define BI_CONTRACTS_BALANCE_RULES_HPP

namespace bi {
namespace balance {

// ---- 注意力轴 ----
constexpr int kBalanceCollapsedMainItems = 1;      // 收起态只有 1 条主信息
constexpr int kBalanceMaxDetachedDots = 4;         // 并行任务最多 4 枚小圆
constexpr int kBalanceInfoDepthMax = 3;            // 展开态层级：身份/内容/操作
constexpr int kBalanceTransitionMaxMs = 400;       // 单次状态动效总上限
constexpr int kBalanceEnterMaxMs = 250;            // 进入（WinUI3 ControlNormal=250ms）
constexpr int kBalanceExitMaxMs = 167;             // 退场（WinUI3 ControlFast=167ms），入慢退快

// ---- 存在感轴 ----
constexpr double kBalanceQuietAlphaMin = 0.12;     // 安静态呼吸点透明度下限
constexpr double kBalanceQuietAlphaMax = 0.30;     // 上限
constexpr int kBalanceQuietBreathMs = 4000;        // 呼吸周期

// ---- 布局 ----
constexpr double kBalanceCornerRadiusPx = 28.0;    // 圆角封顶（与 Apple 44pt 无关）
constexpr double kBalanceShortHeight = 80.0;       // 短档：通知/剪贴板/日历
constexpr double kBalanceMediaHeight = 124.0;      // 媒体档
constexpr double kBalanceTallHeight = 196.0;       // 歌词 / Agent 档
constexpr double kBalanceTopSafeInset = 4.0;       // 工作区顶部最小内缩

// ---- 可读性与控制权 ----
constexpr double kBalanceContrastMin = 4.5;        // 正文对比度（WCAG AA）
constexpr double kBalanceYieldOpacity = 0.35;      // 让路时整体不透明度系数

}  // namespace balance
}  // namespace bi

#endif  // BI_CONTRACTS_BALANCE_RULES_HPP
