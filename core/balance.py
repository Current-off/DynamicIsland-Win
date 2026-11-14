"""平衡岛范式规则（Python 侧）。

对应 C++ 的 contracts/BalanceRules.hpp 与资源 contracts/balance-paradigm.json，
由 tools/check_contracts.py 做三方一致性校验。

注意：core/tokens.py 里的 44pt / 84–160pt / ≤2000ms 是调研参照值
（WWDC23 10194，Apple 灵动岛），**不是平衡岛采用的规则**。本模块才是。
"""
from __future__ import annotations

from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class BalanceRules:
    # 注意力轴
    collapsed_main_items: int = 1
    max_detached_dots: int = 4
    info_depth_max: int = 3
    transition_max_ms: int = 400
    enter_max_ms: int = 250      # WinUI3 ControlNormalAnimationDuration
    exit_max_ms: int = 167       # WinUI3 ControlFastAnimationDuration（入慢退快）
    # 存在感轴
    quiet_alpha_min: float = 0.12
    quiet_alpha_max: float = 0.30
    quiet_breath_ms: int = 4000
    # 布局
    corner_radius_px: float = 28.0
    short_height: float = 80.0
    media_height: float = 124.0
    tall_height: float = 196.0
    top_safe_inset: float = 4.0
    # 可读性与控制权
    contrast_min: float = 4.5
    yield_opacity: float = 0.35


BALANCE = BalanceRules()


def clip_transition_ms(requested_ms: int, reduce_motion: bool = False) -> int:
    """动效只用于状态迁移；Reduce Motion 时降级为 fade（0 = 无弹簧）。"""
    if requested_ms < 0:
        raise ValueError("transition duration must be non-negative")
    if reduce_motion:
        return 0
    return min(requested_ms, BALANCE.transition_max_ms)


def quiet_alpha(phase: float) -> float:
    """安静态呼吸曲线：phase ∈ [0,1)，输出限制在 [min,max]。

    用余弦而非线性三角波，避免转折处的突兀跳变。
    """
    if not 0.0 <= phase < 1.0:
        raise ValueError("phase must be in [0, 1)")
    mid = (BALANCE.quiet_alpha_min + BALANCE.quiet_alpha_max) / 2.0
    amp = (BALANCE.quiet_alpha_max - BALANCE.quiet_alpha_min) / 2.0
    import math

    return mid - amp * math.cos(2.0 * math.pi * phase)


def height_tier(kind: str, has_lyrics: bool = False, has_list: bool = False) -> float:
    """按内容决定展开高度，不用统一大容器再留白。"""
    if has_lyrics or has_list:
        return BALANCE.tall_height
    if kind.startswith("media"):
        return BALANCE.media_height
    return BALANCE.short_height


def corner_radius_for(height: float, scale: float = 1.0) -> float:
    """圆角封顶：min(h/2, 28×scale)，安静态自然得到正圆。"""
    return min(height / 2.0, BALANCE.corner_radius_px * scale)


def yield_opacity(normal_alpha: float) -> float:
    """让路（按住 Ctrl）时的整体不透明度。"""
    return max(0.0, min(1.0, normal_alpha)) * BALANCE.yield_opacity


def info_level(recent_user_action: bool, running_task: bool, passive_state: bool) -> int:
    """信息分级：L3 需处理 > L2 进行中 > L1 环境 > L0 安静。

    纯函数，便于测试「L3 必须能回落」这条范式规则。
    """
    if recent_user_action:
        return 3
    if running_task:
        return 2
    if passive_state:
        return 1
    return 0


def as_dict() -> dict:
    return asdict(BALANCE)
