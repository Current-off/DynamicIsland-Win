"""Scheduler：确定性活动调度。

排序规则（全部显式、可测，不依赖字典遍历顺序）：
1. 只保留 is_visible() 的活动；
2. relevance_score 降序；
3. created_at_ms 升序（同分先到先得）；
4. id 升序（最终兜底，保证完全确定）。

呈现分配对齐 Apple：单活动用 compact；两个及以上时主活动 compact、
次活动 minimal（或按 expand_target 展开为 expanded）。
"""
from __future__ import annotations

from dataclasses import dataclass

from .activity import Activity, Presentation


@dataclass(frozen=True)
class ScheduledActivity:
    activity: Activity
    presentation: Presentation
    region: str  # compactLeading | compactTrailing | minimal | expanded


@dataclass(frozen=True)
class ScheduleResult:
    visible: tuple[ScheduledActivity, ...]
    suppressed_count: int
    expired_ids: tuple[str, ...]

    @property
    def is_idle(self) -> bool:
        return not self.visible


class Scheduler:
    def __init__(self, max_visible: int = 2) -> None:
        if max_visible < 1:
            raise ValueError("max_visible must be >= 1")
        self.max_visible = max_visible

    def select(
        self,
        activities: tuple[Activity, ...] | list[Activity],
        now_ms: int,
        expand_target: str | None = None,
    ) -> ScheduleResult:
        """纯函数式调度：不修改传入对象，相同输入恒定产生相同输出。"""
        visible = [a for a in activities if a.is_visible(now_ms)]
        expired = tuple(sorted(a.id for a in activities if a.is_expired_at(now_ms)))

        ranked = sorted(
            visible,
            key=lambda a: (-a.relevance_score, a.created_at_ms, a.id),
        )
        chosen = ranked[: self.max_visible]
        suppressed = len(ranked) - len(chosen)

        scheduled: list[ScheduledActivity] = []
        for index, activity in enumerate(chosen):
            if expand_target is not None and activity.id == expand_target:
                presentation = Presentation.EXPANDED
                region = "expanded"
            elif index == 0:
                presentation = Presentation.COMPACT
                region = "compactLeading"
            else:
                presentation = Presentation.MINIMAL
                region = "minimal"
            scheduled.append(ScheduledActivity(activity, presentation, region))

        return ScheduleResult(tuple(scheduled), suppressed, expired)
