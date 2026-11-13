"""Store 与 Scheduler：幂等、终止态保护、确定性排序、过期与抑制计数。"""
import unittest

from core.activity import Activity, ActivityState, Presentation
from core.scheduler import Scheduler
from core.store import ActivityStore


def act(aid, score=0.0, created=0, stale=None, state=ActivityState.ACTIVE):
    return Activity(id=aid, kind="media", title=aid, relevance_score=score,
                    created_at_ms=created, stale_at_ms=stale, state=state)


class TestActivityStore(unittest.TestCase):
    def test_upsert_is_idempotent(self):
        s = ActivityStore()
        self.assertTrue(s.upsert(act("a")))
        self.assertFalse(s.upsert(act("a", score=9)))
        self.assertEqual(len(s), 1)
        self.assertEqual(s.get("a").relevance_score, 9)

    def test_terminated_activity_cannot_be_revived(self):
        s = ActivityStore()
        s.upsert(act("a"))
        s.mark("a", ActivityState.ENDED)
        with self.assertRaises(ValueError):
            s.upsert(act("a"))
        s.remove("a")
        self.assertTrue(s.upsert(act("a")))

    def test_purge_expired_marks_once_and_returns_sorted_ids(self):
        s = ActivityStore()
        for aid in ("c", "a", "b"):
            s.upsert(act(aid, stale=10))
        self.assertEqual(s.purge_expired(10), ("a", "b", "c"))
        self.assertEqual(s.purge_expired(99), ())
        self.assertEqual(s.get("a").state, ActivityState.STALE)

    def test_all_returns_id_sorted_snapshot(self):
        s = ActivityStore()
        for aid in ("z", "m", "a"):
            s.upsert(act(aid))
        self.assertEqual(tuple(a.id for a in s.all()), ("a", "m", "z"))

    def test_unknown_id_raises_keyerror(self):
        s = ActivityStore()
        with self.assertRaises(KeyError):
            s.update_content("missing", v=1)


class TestScheduler(unittest.TestCase):
    def test_empty_is_idle(self):
        r = Scheduler().select((), 0)
        self.assertTrue(r.is_idle)
        self.assertEqual(r.expired_ids, ())

    def test_single_activity_uses_compact(self):
        r = Scheduler().select((act("a"),), 0)
        self.assertEqual(len(r.visible), 1)
        self.assertEqual(r.visible[0].presentation, Presentation.COMPACT)
        self.assertEqual(r.visible[0].region, "compactLeading")

    def test_two_activities_primary_compact_secondary_minimal(self):
        r = Scheduler().select((act("low", 1), act("high", 9)), 0)
        self.assertEqual([v.activity.id for v in r.visible], ["high", "low"])
        self.assertEqual(r.visible[0].presentation, Presentation.COMPACT)
        self.assertEqual(r.visible[1].presentation, Presentation.MINIMAL)

    def test_tie_break_by_created_then_id_is_deterministic(self):
        items = (act("b", 5, 10), act("a", 5, 10), act("c", 5, 1))
        r = Scheduler().select(items, 0)
        self.assertEqual([v.activity.id for v in r.visible], ["c", "a"])
        # 输入顺序反转，结果必须完全一致
        r2 = Scheduler().select(tuple(reversed(items)), 0)
        self.assertEqual([v.activity.id for v in r2.visible],
                         [v.activity.id for v in r.visible])

    def test_max_visible_and_suppressed_count(self):
        items = tuple(act(c) for c in "abcd")
        r = Scheduler(max_visible=2).select(items, 0)
        self.assertEqual(len(r.visible), 2)
        self.assertEqual(r.suppressed_count, 2)

    def test_expired_are_excluded_and_reported(self):
        items = (act("old", 100, stale=5), act("fresh", 1))
        r = Scheduler().select(items, 10)
        self.assertEqual(r.expired_ids, ("old",))
        self.assertEqual([v.activity.id for v in r.visible], ["fresh"])

    def test_expand_target_uses_expanded_presentation(self):
        r = Scheduler().select((act("a", 9), act("b", 1)), 0, expand_target="b")
        self.assertEqual(r.visible[1].presentation, Presentation.EXPANDED)
        self.assertEqual(r.visible[1].region, "expanded")

    def test_invalid_max_visible_rejected(self):
        with self.assertRaises(ValueError):
            Scheduler(max_visible=0)


if __name__ == "__main__":
    unittest.main()
