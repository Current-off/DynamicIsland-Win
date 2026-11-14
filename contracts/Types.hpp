// balance_island / Types.hpp
// 跨语言共享类型定义。字段名与 core/activity.py::to_dict() 及 bridge/protocol.py 对齐。
//
// 状态：接口契约（阶段 S3），已通过 MSVC C++17 编译验证。
// 使用传统 include guard 而非 #pragma once：同一头文件可能经由
// "Types.hpp" 与 "../../contracts/Types.hpp" 两条路径被包含，
// MSVC 会视为不同文件而重复定义（2026-11-12 真实编译暴露的缺陷）。
#ifndef BI_CONTRACTS_TYPES_HPP
#define BI_CONTRACTS_TYPES_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bi {

enum class Presentation { Compact, Minimal, Expanded };
enum class BackdropMode { Acrylic, LiquidGlass, OpaqueFallback };
enum class ActivityState { Active, Stale, Ended, Dismissed };

struct Activity {
    std::string id;
    std::string kind;
    std::string title;
    double relevance_score = 0.0;
    std::int64_t created_at_ms = 0;
    std::optional<std::int64_t> stale_at_ms;
    std::string content_json;  // UTF-8 JSON 对象
    ActivityState state = ActivityState::Active;
};

struct ScheduledActivity {
    Activity activity;
    Presentation presentation = Presentation::Compact;
    std::string region;  // compactLeading | compactTrailing | minimal | expanded
};

struct Frame {
    std::vector<ScheduledActivity> visible;
    int suppressed_count = 0;
    std::vector<std::string> expired_ids;
    std::int64_t now_ms = 0;
};

struct RendererConfig {
    int target_monitor_index = 0;
    std::string target_monitor_edid;  // 优先于 index，避免睡眠/扩展坞后编号漂移
    std::string position = "top-center";
    double size_scale = 1.0;
    bool auto_dpi_scale = true;
    bool click_through = true;
};

// 设计约束常量，与 core/tokens.py 保持一致（改动需同步并跑 contract 校验）。
constexpr int kProtocolVersion = 1;
// 4 = 一览模式能铺开的岛数（Layout::kOverviewMax）。以前是 2：同时放歌 +
// 看视频就把「agent 正在等你」挤掉了 —— 那恰恰是最需要被看到的一条。
constexpr int kMaxVisibleActivities = 4;
constexpr int kAnimationMaxMs = 2000;
constexpr double kCornerRadius = 44.0;
constexpr double kConcentricMargin = 20.0;
constexpr double kExpandedMinHeight = 84.0;
constexpr double kExpandedMaxHeight = 160.0;

}  // namespace bi

#endif  // BI_CONTRACTS_TYPES_HPP
