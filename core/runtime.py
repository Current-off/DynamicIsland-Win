"""ActivityRuntime：核心域门面，组合 Store / Scheduler / StateMachine / EventBus。

设计要点：
- 纯逻辑、无 Windows API 依赖，因此可在任意平台确定性测试。
- 桥接离线（bridge_online=False）时，内置 Provider 的活动仍正常调度——
  对应 EARS Unwanted 验收条目，也是 dynamic-island-v2 架构中最值得保留的决策。
- 全屏/游戏场景由 set_suspended 处理，暂停采样但不丢失活动状态。
"""
from __future__ import annotations

from dataclasses import dataclass, field

from .activity import Activity, ActivityState
from .event_bus import EventBus
from .scheduler import ScheduleResult, Scheduler
from .state_machine import (
    CapsuleEvent,
    CapsuleState,
    CapsuleStateMachine,
    InvalidTransition,
)
from .store import ActivityStore
from .tokens import DEFAULT_TOKENS, DesignTokens


@dataclass
class ActivityRuntime:
    max_visible: int = 2
    tokens: DesignTokens = DEFAULT_TOKENS
    store: ActivityStore = field(default_factory=ActivityStore)
    scheduler: Scheduler = field(init=False)
    state_machine: CapsuleStateMachine = field(default_factory=CapsuleStateMachine)
    bus: EventBus = field(default_factory=EventBus)
    bridge_online: bool = True
    suspended: bool = False
    _last_result: ScheduleResult | None = field(default=None, init=False)

    def __post_init__(self) -> None:
        self.scheduler = Scheduler(max_visible=self.max_visible)

    @property
    def last_result(self) -> ScheduleResult | None:
        return self._last_result

    def start_activity(self, activity: Activity) -> None:
        self.store.upsert(activity)
        self._drive_machine()

    def end_activity(self, activity_id: str) -> None:
        self.store.mark(activity_id, ActivityState.ENDED)
        self._drive_machine()

    def set_bridge_online(self, online: bool) -> None:
        """桥接掉线只影响桥接类活动，内置 Provider 活动不受影响。"""
        self.bridge_online = online

    def set_suspended(self, suspended: bool) -> None:
        self.suspended = suspended

    def tick(self, now_ms: int, expand_target: str | None = None) -> ScheduleResult:
        expired = self.store.purge_expired(now_ms)
        if expired:
            self.bus.publish("activity.expired", {"ids": list(expired)})

        if self.suspended:
            # 全屏/反作弊场景：隐藏胶囊并暂停非必要绘制，但保留活动状态。
            result = ScheduleResult(visible=(), suppressed_count=0, expired_ids=expired)
            self._last_result = result
            return result

        candidates = tuple(self.store.all())
        if not self.bridge_online:
            candidates = tuple(
                a for a in candidates if not a.kind.startswith("bridge.")
            )

        result = self.scheduler.select(candidates, now_ms, expand_target)
        self._last_result = result
        self._drive_machine()
        return result

    def _drive_machine(self) -> None:
        """把调度结果映射为状态机事件；非法转移直接忽略（不改变契约）。"""
        result = self._last_result
        if result is None:
            return
        if result.is_idle:
            self._try(CapsuleEvent.ALL_ENDED)
            return
        if self.state_machine.state in (CapsuleState.IDLE, CapsuleState.DISMISSED):
            self._try(CapsuleEvent.ACTIVITY_STARTED)
        if len(result.visible) >= 2:
            self._try(CapsuleEvent.SECOND_ACTIVITY)

    def _try(self, event: CapsuleEvent) -> None:
        try:
            if self.state_machine.can(event):
                self.state_machine.send(event)
        except InvalidTransition:  # 防御性：can() 已保证不会发生
            pass
