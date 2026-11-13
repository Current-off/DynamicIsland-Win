"""Activity 领域模型。

语义对齐 Apple ActivityKit：静态属性（id / kind / title）与动态 ContentState
（content / relevance_score / state）分离。所有状态变更返回新实例，便于确定性测试。
"""
from __future__ import annotations

from dataclasses import dataclass, field, replace
from enum import Enum
from typing import Any, Mapping


class ActivityState(str, Enum):
    ACTIVE = "active"
    STALE = "stale"
    ENDED = "ended"
    DISMISSED = "dismissed"


class Presentation(str, Enum):
    """灵动岛三态呈现（WWDC23 10194）。"""
    COMPACT = "compact"
    MINIMAL = "minimal"
    EXPANDED = "expanded"


@dataclass(frozen=True)
class Activity:
    id: str
    kind: str
    title: str
    relevance_score: float = 0.0
    created_at_ms: int = 0
    stale_at_ms: int | None = None
    content: Mapping[str, Any] = field(default_factory=dict)
    state: ActivityState = ActivityState.ACTIVE

    def __post_init__(self) -> None:
        if not self.id:
            raise ValueError("activity.id must be non-empty")
        if not self.kind:
            raise ValueError("activity.kind must be non-empty")
        if self.stale_at_ms is not None and self.stale_at_ms < self.created_at_ms:
            raise ValueError("stale_at_ms must not precede created_at_ms")

    def is_expired_at(self, now_ms: int) -> bool:
        """纯函数：仅按时间判断是否过期，不修改实例。

        stale_at_ms 为 None 表示永不过期；边界 now_ms == stale_at_ms 判为过期。
        """
        if self.state in (ActivityState.ENDED, ActivityState.DISMISSED):
            return True
        if self.state == ActivityState.STALE:
            return True
        return self.stale_at_ms is not None and now_ms >= self.stale_at_ms

    def is_visible(self, now_ms: int) -> bool:
        """纯函数：是否参与调度。"""
        return self.state == ActivityState.ACTIVE and not self.is_expired_at(now_ms)

    def with_content(self, **changes: Any) -> "Activity":
        """返回合并 content 后的新实例；原实例不被修改。"""
        merged = dict(self.content)
        merged.update(changes)
        return replace(self, content=merged)

    def with_state(self, state: ActivityState) -> "Activity":
        return replace(self, state=state)

    def to_dict(self) -> dict[str, Any]:
        """IPC 序列化用的稳定结构。"""
        return {
            "id": self.id,
            "kind": self.kind,
            "title": self.title,
            "relevanceScore": self.relevance_score,
            "createdAtMs": self.created_at_ms,
            "staleAtMs": self.stale_at_ms,
            "content": dict(self.content),
            "state": self.state.value,
        }
