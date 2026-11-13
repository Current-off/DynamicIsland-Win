"""状态机：合法转移、非法转移显式报错、历史可追溯。"""
import unittest

from core.state_machine import (
    CapsuleEvent as E,
    CapsuleState as S,
    CapsuleStateMachine,
    InvalidTransition,
)


class TestCapsuleStateMachine(unittest.TestCase):
    def test_initial_state_is_idle(self):
        self.assertEqual(CapsuleStateMachine().state, S.IDLE)

    def test_happy_path_idle_to_expanded_and_back(self):
        sm = CapsuleStateMachine()
        self.assertEqual(sm.send(E.ACTIVITY_STARTED), S.COMPACT)
        self.assertEqual(sm.send(E.EXPAND), S.EXPANDED)
        self.assertEqual(sm.send(E.COLLAPSE), S.COMPACT)
        self.assertEqual(sm.send(E.DISMISS), S.DISMISSED)

    def test_second_activity_path(self):
        sm = CapsuleStateMachine()
        sm.send(E.ACTIVITY_STARTED)
        self.assertEqual(sm.send(E.SECOND_ACTIVITY), S.MINIMAL)
        self.assertEqual(sm.send(E.EXPAND), S.EXPANDED)

    def test_illegal_transition_raises_and_does_not_change_state(self):
        sm = CapsuleStateMachine()
        with self.assertRaises(InvalidTransition):
            sm.send(E.EXPAND)
        self.assertEqual(sm.state, S.IDLE)
        self.assertEqual(sm.history, ())

    def test_can_is_pure_query(self):
        sm = CapsuleStateMachine()
        self.assertFalse(sm.can(E.EXPAND))
        self.assertTrue(sm.can(E.ACTIVITY_STARTED))

    def test_dismissed_can_reset(self):
        sm = CapsuleStateMachine()
        sm.send(E.ACTIVITY_STARTED)
        sm.send(E.DISMISS)
        self.assertEqual(sm.send(E.RESET), S.IDLE)

    def test_history_records_all_transitions(self):
        sm = CapsuleStateMachine()
        sm.send(E.ACTIVITY_STARTED)
        sm.send(E.EXPAND)
        self.assertEqual(sm.history, (
            (S.IDLE, E.ACTIVITY_STARTED, S.COMPACT),
            (S.COMPACT, E.EXPAND, S.EXPANDED),
        ))


if __name__ == "__main__":
    unittest.main()
