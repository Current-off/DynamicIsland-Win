"""胶囊状态机：显式转移表，非法转移不静默吞掉。

状态：idle → compact → minimal → expanded → dismissed
转移全部由事件驱动；Reduce Motion 只影响动画时长，不改变可达状态。
"""
from __future__ import annotations

from enum import Enum


class CapsuleState(str, Enum):
    IDLE = "idle"
    COMPACT = "compact"
    MINIMAL = "minimal"
    EXPANDED = "expanded"
    DISMISSED = "dismissed"


class CapsuleEvent(str, Enum):
    ACTIVITY_STARTED = "activity_started"
    SECOND_ACTIVITY = "second_activity"
    EXPAND = "expand"
    COLLAPSE = "collapse"
    DISMISS = "dismiss"
    ALL_ENDED = "all_ended"
    RESET = "reset"


class InvalidTransition(ValueError):
    pass


TRANSITIONS: dict[tuple[CapsuleState, CapsuleEvent], CapsuleState] = {
    (CapsuleState.IDLE, CapsuleEvent.ACTIVITY_STARTED): CapsuleState.COMPACT,
    (CapsuleState.IDLE, CapsuleEvent.DISMISS): CapsuleState.DISMISSED,
    (CapsuleState.COMPACT, CapsuleEvent.SECOND_ACTIVITY): CapsuleState.MINIMAL,
    (CapsuleState.COMPACT, CapsuleEvent.EXPAND): CapsuleState.EXPANDED,
    (CapsuleState.COMPACT, CapsuleEvent.DISMISS): CapsuleState.DISMISSED,
    (CapsuleState.COMPACT, CapsuleEvent.ALL_ENDED): CapsuleState.IDLE,
    (CapsuleState.MINIMAL, CapsuleEvent.EXPAND): CapsuleState.EXPANDED,
    (CapsuleState.MINIMAL, CapsuleEvent.COLLAPSE): CapsuleState.COMPACT,
    (CapsuleState.MINIMAL, CapsuleEvent.DISMISS): CapsuleState.DISMISSED,
    (CapsuleState.MINIMAL, CapsuleEvent.ALL_ENDED): CapsuleState.IDLE,
    (CapsuleState.EXPANDED, CapsuleEvent.COLLAPSE): CapsuleState.COMPACT,
    (CapsuleState.EXPANDED, CapsuleEvent.DISMISS): CapsuleState.DISMISSED,
    (CapsuleState.EXPANDED, CapsuleEvent.ALL_ENDED): CapsuleState.IDLE,
    (CapsuleState.DISMISSED, CapsuleEvent.RESET): CapsuleState.IDLE,
    (CapsuleState.DISMISSED, CapsuleEvent.ALL_ENDED): CapsuleState.IDLE,
}


class CapsuleStateMachine:
    def __init__(self, initial: CapsuleState = CapsuleState.IDLE) -> None:
        self._state = initial
        self._history: list[tuple[CapsuleState, CapsuleEvent, CapsuleState]] = []

    @property
    def state(self) -> CapsuleState:
        return self._state

    @property
    def history(self) -> tuple[tuple[CapsuleState, CapsuleEvent, CapsuleState], ...]:
        return tuple(self._history)

    def can(self, event: CapsuleEvent) -> bool:
        return (self._state, event) in TRANSITIONS

    def send(self, event: CapsuleEvent) -> CapsuleState:
        target = TRANSITIONS.get((self._state, event))
        if target is None:
            raise InvalidTransition(
                f"cannot {event.value!r} while in {self._state.value!r}"
            )
        self._history.append((self._state, event, target))
        self._state = target
        return target
