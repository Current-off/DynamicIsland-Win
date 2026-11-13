"""ActivityStore：活动注册表。

规则显式化：
- 同一 id 重复 upsert 视为更新（幂等），不产生第二条活动。
- 已 ENDED / DISMISSED 的活动不接受复活；必须先 remove。
- purge_expired 只标记，不物理删除，供渲染层做退场动画后再清理。
"""
from __future__ import annotations

from dataclasses import dataclass, field

from .activity import Activity, ActivityState


@dataclass
class ActivityStore:
    _items: dict[str, Activity] = field(default_factory=dict)

    def upsert(self, activity: Activity) -> bool:
        """返回 True 表示新建，False 表示更新既有活动。"""
        current = self._items.get(activity.id)
        if current is not None and current.state in (
            ActivityState.ENDED,
            ActivityState.DISMISSED,
        ):
            raise ValueError(f"activity {activity.id!r} is terminated; remove() first")
        is_new = current is None
        self._items[activity.id] = activity
        return is_new

    def update_content(self, activity_id: str, **changes: object) -> Activity:
        current = self._require(activity_id)
        updated = current.with_content(**changes)
        self._items[activity_id] = updated
        return updated

    def set_relevance(self, activity_id: str, score: float) -> Activity:
        from dataclasses import replace

        current = self._require(activity_id)
        updated = replace(current, relevance_score=score)
        self._items[activity_id] = updated
        return updated

    def mark(self, activity_id: str, state: ActivityState) -> Activity:
        current = self._require(activity_id)
        updated = current.with_state(state)
        self._items[activity_id] = updated
        return updated

    def remove(self, activity_id: str) -> bool:
        return self._items.pop(activity_id, None) is not None

    def get(self, activity_id: str) -> Activity | None:
        return self._items.get(activity_id)

    def all(self) -> tuple[Activity, ...]:
        """返回按 id 排序的快照，保证遍历顺序确定。"""
        return tuple(self._items[k] for k in sorted(self._items))

    def purge_expired(self, now_ms: int) -> tuple[str, ...]:
        """把到期活动标记为 STALE，返回本次新标记的 id（已排序）。"""
        changed: list[str] = []
        for key, item in tuple(self._items.items()):
            if item.state == ActivityState.ACTIVE and item.is_expired_at(now_ms):
                self._items[key] = item.with_state(ActivityState.STALE)
                changed.append(key)
        return tuple(sorted(changed))

    def clear(self) -> None:
        self._items.clear()

    def __len__(self) -> int:
        return len(self._items)

    def _require(self, activity_id: str) -> Activity:
        item = self._items.get(activity_id)
        if item is None:
            raise KeyError(f"unknown activity: {activity_id!r}")
        return item
