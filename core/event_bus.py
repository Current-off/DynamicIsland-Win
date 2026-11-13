"""EventBus：Provider 故障隔离。

关键验收点（EARS Optional）：单个 Provider 抛异常不得影响其他订阅者与渲染层。
异常被收集到 failures 中供本地诊断，不向外抛出。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable

Subscriber = Callable[[str, dict[str, Any]], None]


@dataclass(frozen=True)
class BusError:
    subscriber: str
    error_type: str
    message: str


@dataclass
class EventBus:
    _subscribers: dict[str, Subscriber] = field(default_factory=dict)

    def subscribe(self, name: str, handler: Subscriber) -> None:
        if name in self._subscribers:
            raise ValueError(f"duplicate subscriber: {name!r}")
        self._subscribers[name] = handler

    def unsubscribe(self, name: str) -> bool:
        return self._subscribers.pop(name, None) is not None

    def subscriber_names(self) -> tuple[str, ...]:
        return tuple(sorted(self._subscribers))

    def publish(self, topic: str, payload: dict[str, Any]) -> tuple[BusError, ...]:
        """按订阅名排序确定性分发；逐个 try/except 隔离。"""
        failures: list[BusError] = []
        for name in self.subscriber_names():
            try:
                self._subscribers[name](topic, payload)
            except Exception as exc:  # noqa: BLE001 - 隔离是契约要求
                failures.append(
                    BusError(name, type(exc).__name__, str(exc))
                )
        return tuple(failures)
