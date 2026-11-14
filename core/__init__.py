"""balance_island 核心域（平台无关参考实现）。

对应规划文档 7.2 目标架构中的「核心域 Activity Runtime」。
此包不含任何 Windows API 依赖，可在任意平台运行与测试。
"""
from .activity import Activity, ActivityState, Presentation
from .event_bus import BusError, EventBus
from .runtime import ActivityRuntime
from .scheduler import ScheduledActivity, ScheduleResult, Scheduler
from .state_machine import (
    CapsuleEvent,
    CapsuleState,
    CapsuleStateMachine,
    InvalidTransition,
)
from .store import ActivityStore
from .tokens import DEFAULT_TOKENS, DesignTokens

__all__ = [
    "Activity",
    "ActivityState",
    "Presentation",
    "ActivityStore",
    "Scheduler",
    "ScheduledActivity",
    "ScheduleResult",
    "CapsuleState",
    "CapsuleEvent",
    "CapsuleStateMachine",
    "InvalidTransition",
    "EventBus",
    "BusError",
    "DesignTokens",
    "DEFAULT_TOKENS",
    "ActivityRuntime",
]
