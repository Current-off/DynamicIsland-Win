"""DesignTokens：把 Apple HIG 的设计约束编码为可测试的确定性常量/函数。

来源：WWDC23 10194「设计动态实时活动」：
- 圆角 44pt，内圆角 = 外圆角 − margin（同心边距）
- 单次内容动画 ≤ 2 秒
- Reduce Motion 时降级为 fade
- 扩展视图高度 84–160pt
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class DesignTokens:
    corner_radius: float = 44.0
    concentric_margin: float = 20.0
    animation_max_ms: int = 2000
    expanded_min_height: float = 84.0
    expanded_max_height: float = 160.0
    spring_stiffness: float = 320.0
    spring_damping: float = 26.0
    reduce_motion: bool = False

    def inner_corner_radius(self) -> float:
        """同心边距规则：内圆角 = 外圆角 − margin，且不得为负。"""
        return max(0.0, self.corner_radius - self.concentric_margin)

    def is_valid_expanded_height(self, height: float) -> bool:
        return self.expanded_min_height <= height <= self.expanded_max_height

    def animation_duration_ms(self, requested_ms: int) -> int:
        """纯函数：请求时长裁剪到设计上限；Reduce Motion 时返回 0（改用 fade）。"""
        if self.reduce_motion:
            return 0
        if requested_ms < 0:
            raise ValueError("animation duration must be non-negative")
        return min(requested_ms, self.animation_max_ms)

    def transition_kind(self, requested_ms: int) -> str:
        return "fade" if self.animation_duration_ms(requested_ms) == 0 else "spring"


DEFAULT_TOKENS = DesignTokens()
