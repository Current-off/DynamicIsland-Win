"""两个最小内置 Provider（可确定性测试，无系统 API 依赖）。

真实实现时把 media_state / system_state 换成 Windows 媒体会话与系统事件源；
这里的接口形状与「内容未变化不产生更新」的规则保持一致。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from core.activity import Activity
from .base import Provider, ProviderUpdate


@dataclass
class MediaProvider(Provider):
    """kind = 'media'，非桥接类，桥接离线时仍工作。"""

    media_state: dict[str, Any] = field(default_factory=dict)
    _last_signature: str | None = field(default=None, init=False, repr=False)

    @property
    def name(self) -> str:
        return "media"

    def poll(self, now_ms: int) -> tuple[ProviderUpdate, ...]:
        if not self.enabled or not self.media_state:
            return ()
        signature = f"{self.media_state.get('title')}|{self.media_state.get('playing')}"
        if signature == self._last_signature:
            return ()
        self._last_signature = signature
        activity = Activity(
            id="media:now-playing",
            kind="media",
            title=str(self.media_state.get("title", "")),
            relevance_score=80.0,
            created_at_ms=now_ms,
            content={
                "artist": self.media_state.get("artist", ""),
                "playing": bool(self.media_state.get("playing", False)),
            },
        )
        return (ProviderUpdate(activity=activity),)


@dataclass
class SystemStatusProvider(Provider):
    """kind = 'system'，电池/蓝牙等低优先级状态。"""

    system_state: dict[str, Any] = field(default_factory=dict)
    _last_signature: str | None = field(default=None, init=False)

    @property
    def name(self) -> str:
        return "system"

    def poll(self, now_ms: int) -> tuple[ProviderUpdate, ...]:
        if not self.enabled or not self.system_state:
            return ()
        signature = (
            f"{self.system_state.get('battery')}|{self.system_state.get('bluetooth')}"
        )
        if signature == self._last_signature:
            return ()
        self._last_signature = signature
        activity = Activity(
            id="system:status",
            kind="system",
            title="系统状态",
            relevance_score=10.0,
            created_at_ms=now_ms,
            content=dict(self.system_state),
        )
        return (ProviderUpdate(activity=activity),)
