"""EventBus 故障隔离 + IPC 协议严格解码与版本协商。"""
import unittest

from bridge.protocol import (
    PROTOCOL_VERSION,
    Message,
    ProtocolError,
    decode,
    encode,
    negotiate,
)
from core.event_bus import EventBus


class TestEventBus(unittest.TestCase):
    def test_all_subscribers_receive_in_sorted_order(self):
        bus = EventBus()
        seen = []
        bus.subscribe("z", lambda t, p: seen.append("z"))
        bus.subscribe("a", lambda t, p: seen.append("a"))
        failures = bus.publish("topic", {})
        self.assertEqual(seen, ["a", "z"])
        self.assertEqual(failures, ())

    def test_failing_subscriber_is_isolated(self):
        bus = EventBus()
        seen = []

        def boom(topic, payload):
            raise RuntimeError("provider crashed")

        bus.subscribe("a_bad", boom)
        bus.subscribe("b_good", lambda t, p: seen.append("ok"))
        failures = bus.publish("topic", {})
        self.assertEqual(seen, ["ok"])
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0].subscriber, "a_bad")
        self.assertEqual(failures[0].error_type, "RuntimeError")

    def test_duplicate_subscription_rejected(self):
        bus = EventBus()
        bus.subscribe("a", lambda t, p: None)
        with self.assertRaises(ValueError):
            bus.subscribe("a", lambda t, p: None)

    def test_unsubscribe(self):
        bus = EventBus()
        bus.subscribe("a", lambda t, p: None)
        self.assertTrue(bus.unsubscribe("a"))
        self.assertFalse(bus.unsubscribe("a"))


class TestProtocol(unittest.TestCase):
    def test_roundtrip(self):
        m = Message(PROTOCOL_VERSION, 7, "activity.upsert", 1234, {"id": "a"})
        self.assertEqual(decode(encode(m)), m)

    def test_encoding_is_single_line_and_stable(self):
        line = encode(Message(PROTOCOL_VERSION, 1, "heartbeat", 5))
        self.assertNotIn("\n", line)
        self.assertEqual(line, '{"protocolVersion":1,"seq":1,"type":"heartbeat","ts":5}')

    def test_missing_fields_are_rejected(self):
        for line in (
            '{"seq":1,"type":"x","ts":1}',
            '{"protocolVersion":1,"type":"x","ts":1}',
            '{"protocolVersion":1,"seq":1,"ts":1}',
            '{"protocolVersion":1,"seq":1,"type":"x"}',
        ):
            with self.assertRaises(ProtocolError):
                decode(line)

    def test_malformed_inputs_are_rejected(self):
        for line in ("", "   ", "not json", "[1,2]", '{"a":1}\n{"b":2}', "null"):
            with self.assertRaises(ProtocolError):
                decode(line)

    def test_type_and_range_validation(self):
        base = {"seq": 1, "type": "x", "ts": 1}
        bad_values = [
            {**base, "protocolVersion": 0},
            {**base, "protocolVersion": True},
            {**base, "protocolVersion": "1"},
            {**base, "protocolVersion": 1, "seq": -1},
            {**base, "protocolVersion": 1, "ts": -5},
            {**base, "protocolVersion": 1, "type": ""},
            {**base, "protocolVersion": 1, "activity": "not-object"},
        ]
        import json
        for payload in bad_values:
            with self.assertRaises(ProtocolError):
                decode(json.dumps(payload))

    def test_unknown_fields_are_ignored_for_forward_compatibility(self):
        m = decode('{"protocolVersion":1,"seq":1,"type":"x","ts":1,"future":42}')
        self.assertEqual(m.type, "x")

    def test_negotiate_matrix(self):
        self.assertEqual(negotiate(PROTOCOL_VERSION), "compatible")
        self.assertEqual(negotiate(0), "incompatible")
        self.assertEqual(negotiate(-1), "incompatible")
        self.assertEqual(negotiate(PROTOCOL_VERSION + 1), "incompatible")
        self.assertEqual(negotiate(True), "incompatible")  # bool 不是版本号


if __name__ == "__main__":
    unittest.main()
