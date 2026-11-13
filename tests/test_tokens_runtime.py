"""DesignTokens 纯函数 + Runtime 降级链（对应 EARS Unwanted / State-driven）。"""
import unittest

from core.activity import Activity, ActivityState
from core.runtime import ActivityRuntime
from core.state_machine import CapsuleState as S
from core.tokens import DEFAULT_TOKENS, DesignTokens
from providers.builtin import MediaProvider, SystemStatusProvider
from providers.base import Provider


class TestDesignTokens(unittest.TestCase):
    def test_concentric_inner_radius(self):
        self.assertEqual(DEFAULT_TOKENS.inner_corner_radius(), 24.0)

    def test_negative_inner_radius_clamped(self):
        t = DesignTokens(corner_radius=10.0, concentric_margin=20.0)
        self.assertEqual(t.inner_corner_radius(), 0.0)

    def test_animation_duration_is_clamped_to_2s(self):
        self.assertEqual(DEFAULT_TOKENS.animation_duration_ms(320), 320)
        self.assertEqual(DEFAULT_TOKENS.animation_duration_ms(99999), 2000)

    def test_reduce_motion_degrades_to_fade(self):
        t = DesignTokens(reduce_motion=True)
        self.assertEqual(t.animation_duration_ms(320), 0)
        self.assertEqual(t.transition_kind(320), "fade")

    def test_negative_duration_rejected(self):
        with self.assertRaises(ValueError):
            DEFAULT_TOKENS.animation_duration_ms(-1)

    def test_expanded_height_bounds(self):
        self.assertTrue(DEFAULT_TOKENS.is_valid_expanded_height(84.0))
        self.assertTrue(DEFAULT_TOKENS.is_valid_expanded_height(160.0))
        self.assertFalse(DEFAULT_TOKENS.is_valid_expanded_height(83.9))
        self.assertFalse(DEFAULT_TOKENS.is_valid_expanded_height(161.0))


class TestProvider(unittest.TestCase):
    def test_sensitive_provider_must_default_disabled(self):
        class Clipboard(Provider):
            @property
            def name(self):
                return "clipboard"

            def poll(self, now_ms):
                return ()

        with self.assertRaises(ValueError):
            Clipboard(enabled=True, sensitive=True)
        self.assertFalse(Clipboard(enabled=False, sensitive=True).enabled)

    def test_media_provider_emits_only_on_change(self):
        p = MediaProvider(media_state={"title": "Song", "playing": True})
        first = p.poll(1000)
        self.assertEqual(len(first), 1)
        self.assertEqual(p.poll(1100), ())
        p.media_state = {"title": "Other", "playing": True}
        self.assertEqual(len(p.poll(1200)), 1)

    def test_system_provider_is_builtin_kind(self):
        p = SystemStatusProvider(system_state={"battery": 42})
        self.assertEqual(p.poll(0)[0].activity.kind, "system")


class TestRuntimeDegradation(unittest.TestCase):
    def setUp(self):
        self.rt = ActivityRuntime()

    def test_bridge_offline_keeps_builtin_activities(self):
        self.rt.start_activity(Activity(id="m", kind="media", title="media", relevance_score=80))
        self.rt.start_activity(Activity(id="c", kind="bridge.calendar", title="cal", relevance_score=90))
        self.assertEqual(len(self.rt.tick(0).visible), 2)
        self.rt.set_bridge_online(False)
        result = self.rt.tick(1)
        self.assertEqual([v.activity.id for v in result.visible], ["m"])

    def test_suspension_hides_capsule_without_losing_state(self):
        self.rt.start_activity(Activity(id="m", kind="media", title="media"))
        self.rt.set_suspended(True)
        self.assertTrue(self.rt.tick(0).is_idle)
        self.assertEqual(self.rt.store.get("m").state, ActivityState.ACTIVE)
        self.rt.set_suspended(False)
        self.assertEqual(len(self.rt.tick(1).visible), 1)

    def test_expiry_marks_stale_and_publishes_event(self):
        events = []
        self.rt.bus.subscribe("observer", lambda t, p: events.append((t, p)))
        self.rt.start_activity(Activity(id="a", kind="media", title="x", stale_at_ms=5))
        result = self.rt.tick(5)
        self.assertEqual(result.expired_ids, ("a",))
        self.assertEqual(self.rt.store.get("a").state, ActivityState.STALE)
        self.assertEqual(events[0][0], "activity.expired")
        self.assertEqual(events[0][1]["ids"], ["a"])

    def test_state_machine_follows_activity_lifecycle(self):
        self.rt.start_activity(Activity(id="a", kind="media", title="x"))
        self.rt.tick(0)
        self.assertEqual(self.rt.state_machine.state, S.COMPACT)
        self.rt.start_activity(Activity(id="b", kind="system", title="y"))
        self.rt.tick(1)
        self.assertEqual(self.rt.state_machine.state, S.MINIMAL)
        self.rt.end_activity("a")
        self.rt.end_activity("b")
        self.rt.tick(2)
        self.assertEqual(self.rt.state_machine.state, S.IDLE)


if __name__ == "__main__":
    unittest.main()
