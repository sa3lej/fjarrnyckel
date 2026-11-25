"""Tests of the keying link against a simulated clock and a simulated network."""
import heapq
import pathlib
import random
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "receiver"))

import cwproto as proto
from cwrecv import Receiver
from cwsend import Client, keying_plan

KEY = b"0123456789abcdef0123456789abcdef"
CLOCK_OFFSET = 7_000_000_000          # the client's clock runs this far ahead of the server's


class Out:
    def __init__(self):
        self.down = False
        self.changes = 0

    def set(self, down):
        if down != self.down:
            self.changes += 1
        self.down = down


class Rig:
    def __init__(self, cw=True):
        self.cw = cw

    def cw_mode(self):
        return self.cw

    def mode_and_freq(self):
        return (0x03 if self.cw else 0x01, 7_018_000)

    def select_cw(self):
        self.asked = getattr(self, "asked", 0) + 1
        self.cw = True


class World:
    """Client, network and receiver on one event queue. Times are server microseconds."""

    def __init__(self, *, latency=10_000, jitter=0, loss=0.0, drift_ppm=0.0, seed=1, rig=None, wpm=25,
                 delay_ms=0, **receiver_options):
        self.random = random.Random(seed)
        self.latency, self.jitter, self.loss, self.drift = latency, jitter, loss, drift_ppm / 1e6
        self.out = Out()
        self.receiver = Receiver(KEY, self.out, rig=rig, random_bytes=lambda n: self.random.randbytes(n),
                                 **receiver_options)
        self.client = Client(KEY, wpm=wpm, delay_ms=delay_ms, session=0x1234)
        self.now = 1_000_000
        self.events = []                 # (time, order, kind, payload)
        self.order = 0
        self.blackout = (0, 0)           # server time span in which the uplink drops everything
        self.hold = {}                   # uplink packet number -> extra delay
        self.uplink_count = 0
        self.captured = []
        self.client_sending = True
        self.drop_first_edge_packet = False

    def client_time(self, server_time):
        return CLOCK_OFFSET + int(server_time * (1 + self.drift))

    def push(self, when, kind, payload=None):
        self.order += 1
        heapq.heappush(self.events, (when, self.order, kind, payload))

    def uplink(self, data):
        self.uplink_count += 1
        self.captured.append(data)
        if self.blackout[0] <= self.now < self.blackout[1] or self.random.random() < self.loss:
            return
        delay = self.latency + int(self.random.random() * self.jitter) + self.hold.get(self.uplink_count, 0)
        self.push(self.now + delay, "to_server", data)

    def run(self, until):
        while True:
            deadline = self.receiver.poll(self.now)
            next_event = self.events[0][0] if self.events else None
            candidates = [t for t in (deadline, next_event) if t is not None]
            if not candidates or min(candidates) > until:
                break
            self.now = max(self.now, min(candidates))
            self.receiver.poll(self.now)
            while self.events and self.events[0][0] <= self.now:
                _, _, kind, payload = heapq.heappop(self.events)
                if kind == "to_server":
                    for reply, _ in self.receiver.on_packet(payload, ("client", 1), self.now):
                        self.push(self.now + self.latency, "to_client", reply)
                elif kind == "to_client":
                    self.client.on_status(payload, self.client_time(self.now))
                elif kind == "edge":
                    self.client.edge(self.client_time(self.now), payload)
                    if self.client_sending:
                        data = self.client.packet(self.client_time(self.now))
                        if not self.drop_first_edge_packet:
                            self.uplink(data)
                elif kind == "tick":
                    if self.client_sending and self.client.due(self.client_time(self.now)):
                        self.uplink(self.client.packet(self.client_time(self.now)))
                    self.push(self.now + 1_000, "tick")
        self.now = until
        self.receiver.poll(self.now)

    def link_up(self):
        self.push(self.now, "tick")
        for _ in range(50):
            self.run(self.now + 100_000)
            if self.client.linked():
                return
        raise AssertionError("link did not come up")

    def key(self, text, wpm=25, start=None):
        plan, span = keying_plan(text, wpm)
        start = self.now + 100_000 if start is None else start
        for offset, down in plan:
            self.push(start + offset, "edge", down)
        return start, start + span, [(start + offset, down) for offset, down in plan]

    def mark_errors(self):
        played = self.receiver.played
        errors = []
        for earlier, later in zip(played, played[1:]):
            if earlier[2] and not later[2]:
                errors.append(abs((later[1] - earlier[1]) - (later[0] - earlier[0])))
        return errors


class LinkTests(unittest.TestCase):
    def test_keying_is_reproduced_exactly_through_jitter(self):
        world = World(jitter=15_000)
        world.link_up()
        _, end, plan = world.key("PARIS PARIS PARIS")
        world.run(end + 500_000)
        self.assertEqual(len(world.receiver.played), len(plan))
        self.assertEqual(max(world.mark_errors()), 0)
        self.assertEqual(world.receiver.late, 0)
        self.assertEqual(world.receiver.aborts, {})
        self.assertFalse(world.out.down)

    def test_losing_the_first_packet_of_every_edge_costs_nothing(self):
        world = World(jitter=10_000)
        world.link_up()
        world.drop_first_edge_packet = True
        _, end, plan = world.key("CQ TEST DE SA3LEJ")
        world.run(end + 500_000)
        self.assertEqual(len(world.receiver.played), len(plan))
        self.assertEqual(world.receiver.late, 0)
        self.assertEqual(max(world.mark_errors()), 0)

    def test_heavy_random_loss_never_loses_an_edge(self):
        for seed in range(1, 11):
            world = World(jitter=10_000, loss=0.3, seed=seed)
            world.link_up()
            _, end, plan = world.key("CQ TEST DE SA3LEJ")
            world.run(end + 500_000)
            self.assertEqual(len(world.receiver.played), len(plan))
            self.assertEqual(world.receiver.lost, 0)
            self.assertEqual(world.receiver.aborts, {})
            self.assertLess(max(world.mark_errors()), 60_000)   # a late key-up can only lengthen a mark
            self.assertFalse(world.out.down)

    def test_clock_drift_is_absorbed_in_the_gaps(self):
        world = World(jitter=5_000, drift_ppm=100)
        world.link_up()
        _, end, plan = world.key("PARIS " * 12)
        world.run(end + 500_000)
        self.assertEqual(len(world.receiver.played), len(plan))
        self.assertEqual(max(world.mark_errors()), 0)
        self.assertEqual(world.receiver.aborts, {})

    def test_a_late_key_down_lengthens_the_gap_not_the_mark(self):
        world = World()
        world.link_up()
        before = world.uplink_count
        _, end, plan = world.key("EEEEEE")
        world.hold = {n: 80_000 for n in range(before + 1, before + 400) }   # everything arrives 80 ms late
        world.run(end + 800_000)
        self.assertEqual(len(world.receiver.played), len(plan))
        self.assertGreaterEqual(world.receiver.late, 1)
        self.assertEqual(max(world.mark_errors()), 0)
        self.assertGreater(world.receiver.extra_us, 0)

    def test_forged_and_altered_packets_are_ignored(self):
        world = World()
        world.link_up()
        good = world.client.packet(world.client_time(world.now))
        altered = bytearray(good)
        altered[9] ^= 0x01
        wrong_key = proto.pack_key(b"another key, 32 bytes long......", proto.KeyPacket(
            1, 25, 0x1234, 10_000, world.client_time(world.now), world.client.nonce, 0, 0,
            [(world.client_time(world.now), True)]))
        for data in (bytes(altered), wrong_key, good[:-1], b"", b"CWL2" + bytes(60)):
            self.assertEqual(world.receiver.on_packet(data, ("attacker", 9), world.now), [])
        world.run(world.now + 400_000)
        self.assertEqual(world.receiver.played, [])
        self.assertFalse(world.out.down)

    def test_a_recorded_session_cannot_be_replayed(self):
        world = World()
        world.link_up()
        _, end, plan = world.key("TEST")
        world.run(end + 300_000)
        played = len(world.receiver.played)
        self.assertEqual(played, len(plan))
        recording = list(world.captured)
        # Same server, same session: every packet is a duplicate.
        for data in recording:
            world.receiver.on_packet(data, ("attacker", 9), world.now)
        world.run(world.now + 500_000)
        self.assertEqual(len(world.receiver.played), played)
        # Later, after the nonces have rotated, against a server that runs another session.
        world.client_sending = False
        world.run(world.now + 12_000_000)
        world.receiver.session = 0x9999
        for data in recording:
            for reply, _ in world.receiver.on_packet(data, ("attacker", 9), world.now):
                self.assertTrue(proto.unpack_status(KEY, reply).flags & proto.ST_NONCE_BAD)
            world.now += 1_000
        world.run(world.now + 500_000)
        self.assertEqual(len(world.receiver.played), played)
        self.assertEqual(world.receiver.session, 0x9999)
        self.assertFalse(world.out.down)

    def test_a_replaced_session_cannot_return_while_its_nonce_is_still_valid(self):
        world = World()
        world.link_up()
        _, end, plan = world.key("TEST")
        world.run(end + 300_000)
        played = len(world.receiver.played)
        recording = list(world.captured)
        world.client_sending = False
        # The operator restarts the box: a new session takes over within the nonce lifetime.
        newcomer = Client(KEY, wpm=25, session=0x4321)
        newcomer.nonce = world.receiver.nonce
        world.receiver.on_packet(newcomer.packet(world.client_time(world.now)), ("client", 2), world.now)
        self.assertEqual(world.receiver.session, 0x4321)
        # An attacker replays the fresh recording of the old session at once.
        for data in recording:
            world.receiver.on_packet(data, ("attacker", 9), world.now)
            world.now += 1_000
        world.run(world.now + 800_000)
        self.assertEqual(world.receiver.session, 0x4321)
        self.assertEqual(len(world.receiver.played), played)
        self.assertFalse(world.out.down)

    def test_a_session_needs_a_fresh_nonce(self):
        world = World()
        stranger = Client(KEY, wpm=25, session=0x77)
        stranger.edge(world.client_time(world.now), True)
        replies = world.receiver.on_packet(stranger.packet(world.client_time(world.now)), ("x", 1), world.now)
        status = proto.unpack_status(KEY, replies[0][0])
        self.assertTrue(status.flags & proto.ST_NONCE_BAD)
        self.assertIsNone(world.receiver.session)
        world.run(world.now + 300_000)
        self.assertFalse(world.out.down)

    def test_the_key_is_released_when_packets_stop(self):
        world = World()
        world.link_up()
        start = world.now + 100_000
        world.push(start, "edge", True)
        world.run(start + 80_000)
        self.assertTrue(world.out.down)
        world.client_sending = False
        stopped = world.now
        world.run(stopped + 1_000_000)
        self.assertFalse(world.out.down)
        self.assertEqual(world.receiver.aborts["link timeout"], 1)
        released = world.receiver.last_packet + world.receiver.link_timeout_us
        self.assertLessEqual(released - stopped, 320_000)

    def test_a_key_held_too_long_is_released_and_stays_silent_until_a_word_gap(self):
        world = World(max_key_down_ms=5000)
        world.link_up()
        start = world.now + 100_000
        world.push(start, "edge", True)
        world.push(start + 7_000_000, "edge", False)
        world.run(start + 6_000_000)
        self.assertFalse(world.out.down)
        self.assertEqual(world.receiver.aborts["key down too long"], 1)
        changes = world.out.changes
        world.run(start + 7_200_000)
        self.assertEqual(world.out.changes, changes)       # the late key-up changes nothing
        _, end, plan = world.key("E", start=start + 8_000_000)
        world.run(end + 500_000)
        self.assertEqual(world.out.changes, changes + 2)   # keying works again after the gap

    def test_after_a_network_hole_keying_resumes_only_at_a_word_gap(self):
        world = World()
        world.link_up()
        start, end, plan = world.key("PARIS PARIS PARIS PARIS", wpm=30)
        world.blackout = (start + 900_000, start + 1_500_000)
        world.run(end + 600_000)
        self.assertGreaterEqual(sum(world.receiver.aborts.values()), 1)
        self.assertFalse(world.out.down)
        self.assertEqual(max(world.mark_errors()), 0)
        played = world.receiver.played
        hole_end = world.client_time(world.blackout[1])
        after = [entry for entry in played if entry[0] > hole_end]
        self.assertTrue(after, "nothing was keyed after the hole")
        self.assertTrue(after[0][2], "keying resumed with a key-up")
        client_edges = [world.client_time(when) for when, _ in plan]
        first = min(client_edges, key=lambda when: abs(when - after[0][0]))
        previous = max(when for when in client_edges if when < first)
        self.assertGreater(first - previous, 5 * proto.dit_us(30) - 2_000)

    def test_nothing_is_keyed_unless_the_rig_is_in_cw(self):
        rig = Rig(cw=False)
        world = World(rig=rig)
        world.push(world.now, "tick")
        world.run(world.now + 500_000)
        self.assertFalse(world.client.status.flags & proto.ST_CW_MODE)
        _, end, plan = world.key("TEST")
        world.run(end + 300_000)
        self.assertEqual(world.receiver.played, [])
        rig.cw = True
        world.run(world.now + 400_000)
        _, end, plan = world.key("TEST")
        world.run(end + 300_000)
        self.assertEqual(len(world.receiver.played), len(plan))
        # Leaving CW in the middle of a mark releases the key at once.
        start = world.now + 100_000
        world.push(start, "edge", True)
        world.run(start + 80_000)
        self.assertTrue(world.out.down)
        rig.cw = False
        world.run(world.now + 20_000)
        self.assertFalse(world.out.down)

    def test_the_receiver_tells_its_version(self):
        world = World()
        world.link_up()
        self.assertEqual(world.client.status.version, proto.RECEIVER_VERSION)
        self.assertGreater(proto.RECEIVER_VERSION, 0)

    def test_a_reported_stretch_of_keying_is_let_go(self):
        world = World()
        world.link_up()
        _, end, plan = world.key("TEST")
        world.run(end + 300_000)
        report = world.receiver.take_summary()
        self.assertEqual(report["edges_played"], len(plan))
        self.assertEqual(world.receiver.played, [])
        self.assertEqual(world.receiver.take_summary()["edges_played"], 0)
        _, end, plan = world.key("E")
        world.run(end + 300_000)
        self.assertEqual(world.receiver.take_summary()["edges_played"], len(plan))

    def test_the_box_can_put_the_radio_in_cw_once_per_asking(self):
        rig = Rig(cw=False)
        world = World(rig=rig)
        world.push(world.now, "tick")
        world.run(world.now + 500_000)
        self.assertFalse(world.client.status.flags & proto.ST_CW_MODE)
        world.client.give_command(proto.CMD_SELECT_CW, world.client_time(world.now))
        world.run(world.now + 3_000_000)          # the command rode in some twenty packets
        self.assertEqual(rig.asked, 1)
        self.assertTrue(world.client.status.flags & proto.ST_CW_MODE)
        _, end, plan = world.key("TEST")
        world.run(end + 300_000)
        self.assertEqual(len(world.receiver.played), len(plan))
        # The operator leaves CW by the radio: the old command does not come back, a new one does.
        rig.cw = False
        world.run(world.now + 3_000_000)
        self.assertEqual(rig.asked, 1)
        world.client.give_command(proto.CMD_SELECT_CW, world.client_time(world.now))
        world.run(world.now + 1_000_000)
        self.assertEqual(rig.asked, 2)

    def test_a_recorded_command_cannot_be_replayed(self):
        rig = Rig(cw=False)
        world = World(rig=rig)
        world.push(world.now, "tick")
        world.run(world.now + 500_000)
        world.client.give_command(proto.CMD_SELECT_CW, world.client_time(world.now))
        world.run(world.now + 3_000_000)
        recorded = [d for d in world.captured if proto.unpack_key(KEY, d).command]
        self.assertTrue(recorded)
        rig.cw = False
        world.client_sending = False
        world.run(world.now + 30_000_000)         # every nonce the recording carries is long out of date
        for data in recorded:
            world.receiver.on_packet(data, ("thief", 2), world.now)
        self.assertEqual(rig.asked, 1)
        self.assertFalse(rig.cw)

    def test_the_receiver_can_be_told_to_leave_the_mode_alone(self):
        rig = Rig(cw=False)
        world = World(rig=rig, cw_from_box=False)
        world.push(world.now, "tick")
        world.run(world.now + 500_000)
        world.client.give_command(proto.CMD_SELECT_CW, world.client_time(world.now))
        world.run(world.now + 3_000_000)
        self.assertFalse(rig.cw)


if __name__ == "__main__":
    unittest.main()
