"""Activity 模型：过期边界、不可变性、非法输入。"""
import unittest

from core.activity import Activity, ActivityState


class TestActivity(unittest.TestCase):
    def test_expiry_boundary_is_inclusive(self):
        a = Activity(id="a", kind="media", title="x", created_at_ms=0, stale_at_ms=100)
        self.assertFalse(a.is_expired_at(99))
        self.assertTrue(a.is_expired_at(100))
        self.assertTrue(a.is_expired_at(101))

    def test_no_stale_date_never_expires(self):
        a = Activity(id="a", kind="media", title="x", created_at_ms=0)
        self.assertFalse(a.is_expired_at(10**12))

    def test_terminal_states_are_expired_regardless_of_time(self):
        for state in (ActivityState.ENDED, ActivityState.DISMISSED, ActivityState.STALE):
            a = Activity(id="a", kind="media", title="x", state=state)
            self.assertTrue(a.is_expired_at(0))

    def test_with_content_does_not_mutate_original(self):
        a = Activity(id="a", kind="media", title="x", content={"v": 1})
        b = a.with_content(v=2, extra=True)
        self.assertEqual(a.content, {"v": 1})
        self.assertEqual(b.content, {"v": 2, "extra": True})

    def test_with_state_returns_new_instance(self):
        a = Activity(id="a", kind="media", title="x")
        b = a.with_state(ActivityState.ENDED)
        self.assertIsNot(a, b)
        self.assertEqual(a.state, ActivityState.ACTIVE)

    def test_invalid_ids_rejected(self):
        with self.assertRaises(ValueError):
            Activity(id="", kind="media", title="x")
        with self.assertRaises(ValueError):
            Activity(id="a", kind="", title="x")

    def test_stale_before_creation_rejected(self):
        with self.assertRaises(ValueError):
            Activity(id="a", kind="media", title="x", created_at_ms=100, stale_at_ms=50)

    def test_to_dict_field_names_match_ipc_contract(self):
        d = Activity(id="a", kind="media", title="x", relevance_score=3.5).to_dict()
        for key in ("id", "kind", "title", "relevanceScore", "createdAtMs",
                    "staleAtMs", "content", "state"):
            self.assertIn(key, d)
        self.assertEqual(d["state"], "active")


if __name__ == "__main__":
    unittest.main()
