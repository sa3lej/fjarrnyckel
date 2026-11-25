#!/usr/bin/env python3
"""Client side of the remote CW keying link (CWL2).

`Client` builds the packets exactly as the keyer box will and is what the
tests drive. `main` is a test sender: it keys a text at a given speed and can
drop or delay its own packets to imitate a bad network.
"""
import argparse
import collections
import heapq
import json
import os
import random
import select
import socket
import sys
import time

import cwproto as proto

REPEAT_US = 500_000        # every packet repeats the edges of this long
ACTIVE_US = 2_000_000      # ACTIVE for this long after an edge
HEARTBEAT_ACTIVE_US = 20_000
HEARTBEAT_IDLE_US = 100_000
EDGE_REPEATS_US = (3_000, 8_000)   # an edge packet is sent again this long after the edge
COMMAND_US = 1_000_000     # a command is sent again every COMMAND_EVERY_US for this long
COMMAND_EVERY_US = 100_000


class Client:
    def __init__(self, key, *, wpm=0, delay_ms=0, session=None):
        self.key = key
        self.wpm = wpm
        self.delay_ms = delay_ms
        self.session = session or (int.from_bytes(os.urandom(4), "big") or 1)
        self.seq = 0
        self.nonce = 0
        self.edges = collections.deque()       # (index, time, down)
        self.next_index = 0
        self.key_down = False
        self.last_edge = None
        self.last_sent = None
        self.repeats = collections.deque()
        self.status = None
        self.rtt = []
        self.info = None                       # the newest INFO packet: what the radio is and can do
        self.command = 0
        self.command_arg = 0
        self.command_seq = 0
        self.command_until = 0
        self.command_next = 0

    def edge(self, when, down):
        """Record a key edge that happened at client time `when`."""
        self.edges.append((self.next_index, when, down))
        self.next_index += 1
        self.key_down = down
        self.last_edge = when
        self.repeats = collections.deque(when + delay for delay in EDGE_REPEATS_US)

    def active(self, now):
        return self.last_edge is not None and now - self.last_edge < ACTIVE_US

    def due(self, now):
        """True when a heartbeat should go out."""
        if self.last_sent is None:
            return True
        if self.repeats and now >= self.repeats[0]:
            self.repeats.popleft()
            return True
        return now - self.last_sent >= (HEARTBEAT_ACTIVE_US if self.active(now) else HEARTBEAT_IDLE_US)

    def give_command(self, command, arg, now):
        """Ask the receiver for something for the radio, e.g. (proto.CMD_SET_MODE, 0x03). It goes out in its own
        packets, at once and again for a second; a newer command takes the place of one still being repeated."""
        self.command, self.command_arg = command, arg
        self.command_seq = (self.command_seq + 1) & 0xFFFF
        self.command_until = now + COMMAND_US
        self.command_next = now

    def command_due(self, now):
        return bool(self.command) and self.command_next <= now < self.command_until

    def command_packet(self, now):
        self.command_next = now + COMMAND_EVERY_US
        return proto.pack_command(self.key, proto.Command(
            self.command, self.command_seq, self.session, now, self.nonce, self.command_arg))

    def packet(self, now):
        while self.edges and now - self.edges[0][1] > REPEAT_US and not (len(self.edges) == 1 and self.key_down):
            self.edges.popleft()
        chosen = list(self.edges)[-proto.MAX_EDGES:]
        first = chosen[0][0] if chosen else self.next_index
        self.seq += 1
        self.last_sent = now
        return proto.pack_key(self.key, proto.KeyPacket(
            proto.FLAG_ACTIVE if self.active(now) else 0, self.wpm, self.session, self.seq, now, self.nonce,
            first, self.delay_ms, [(when, down) for _, when, down in chosen]))

    def on_status(self, data, now):
        status = proto.unpack_status(self.key, data)
        if status is None:
            info = proto.unpack_info(self.key, data)
            if info is not None and info.session == self.session:
                self.info = info
            return None
        self.nonce = status.nonce
        self.status = status
        if status.session == self.session and status.echo_t:
            self.rtt.append(now - status.echo_t - status.echo_age)
        return status

    def linked(self):
        """The server runs our session and will key what we send."""
        return (self.status is not None and self.status.session == self.session
                and not self.status.flags & (proto.ST_MUTED | proto.ST_NONCE_BAD))


MORSE = {
    "A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.", "H": "....",
    "I": "..", "J": ".---", "K": "-.-", "L": ".-..", "M": "--", "N": "-.", "O": "---", "P": ".--.",
    "Q": "--.-", "R": ".-.", "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-",
    "Y": "-.--", "Z": "--..", "0": "-----", "1": ".----", "2": "..---", "3": "...--", "4": "....-",
    "5": ".....", "6": "-....", "7": "--...", "8": "---..", "9": "----.", "/": "-..-.", "?": "..--..",
}


def keying_plan(text, wpm):
    """Edges as (offset in microseconds, key down) for a text at a speed."""
    unit = 1_200_000 / wpm
    plan = []
    at = 0.0
    for word in text.upper().split():
        for letter in word:
            for symbol in MORSE.get(letter, ""):
                plan.append((int(at), True))
                at += unit * (1 if symbol == "." else 3)
                plan.append((int(at), False))
                at += unit
            at += unit * 2
        at += unit * 4
    return plan, int(at)


def now_us():
    return time.monotonic_ns() // 1000


def main():
    parser = argparse.ArgumentParser(description="Remote CW keying test sender")
    parser.add_argument("--key", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=7373)
    parser.add_argument("--wpm", type=float, default=25.0)
    parser.add_argument("--text", default="PARIS PARIS PARIS PARIS PARIS")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--delay-ms", type=int, default=0, help="playout delay to request, 0 = server default")
    parser.add_argument("--loss", type=float, default=0.0, help="fraction of own packets to drop")
    parser.add_argument("--jitter-ms", type=float, default=0.0, help="hold own packets back by up to this long")
    args = parser.parse_args()

    info = socket.getaddrinfo(args.host, args.port, type=socket.SOCK_DGRAM)[0]
    sock = socket.socket(info[0], socket.SOCK_DGRAM)
    sock.connect(info[4])
    sock.setblocking(False)
    client = Client(proto.load_key(args.key), wpm=int(round(args.wpm)), delay_ms=args.delay_ms)
    single, span = keying_plan(args.text, args.wpm)
    plan = [(offset + round_ * span, down) for round_ in range(args.repeat) for offset, down in single]
    held = []                                  # (send at, counter, data) for the jitter simulation
    counter = 0
    sent = dropped = 0

    def transmit(now):
        nonlocal counter, sent, dropped
        data = client.packet(now)
        sent += 1
        if random.random() < args.loss:
            dropped += 1
            return
        counter += 1
        heapq.heappush(held, (now + int(random.random() * args.jitter_ms * 1000), counter, data))

    def pump(now):
        while held and held[0][0] <= now:
            try:
                sock.send(heapq.heappop(held)[2])
            except OSError:
                pass
        while True:
            try:
                data = sock.recv(256)
            except (BlockingIOError, ConnectionRefusedError):
                return
            client.on_status(data, now_us())

    def wait_until(target):
        while True:
            now = now_us()
            if now >= target:
                return
            if client.due(now):
                transmit(now)
            nearest = min([target] + ([held[0][0]] if held else []) + [now + 1_000])
            select.select([sock], [], [], max(0.0, (nearest - now_us() - 150) / 1e6))
            pump(now_us())

    # Bring the link up first: the server hands out a nonce, then accepts the session.
    give_up = now_us() + 5_000_000
    while not client.linked():
        if now_us() > give_up:
            raise SystemExit("no usable answer from the server within 5 s")
        wait_until(now_us() + 20_000)

    start = now_us() + 200_000
    for offset, down in plan:
        target = start + offset
        wait_until(target - 150)
        while now_us() < target:
            pass
        client.edge(now_us(), down)
        transmit(now_us())
        pump(now_us())
    wait_until(now_us() + 1_000_000)

    def spread(values):
        if not values:
            return None
        ordered = sorted(values)
        return {"n": len(ordered), "min": round(ordered[0] / 1000, 2), "p50": round(ordered[len(ordered) // 2] / 1000, 2),
                "p99": round(ordered[min(len(ordered) - 1, int(0.99 * len(ordered)))] / 1000, 2),
                "max": round(ordered[-1] / 1000, 2)}
    status = client.status
    print(json.dumps({
        "event": "sender_summary", "edges": client.next_index, "packets_built": sent, "dropped_on_purpose": dropped,
        "round_trip_ms": spread(client.rtt),
        "server": {"rx_pkts": status.rx_pkts, "late": status.late, "lost": status.lost, "delay_ms": status.delay_ms,
                   "flags": status.flags, "mode": status.mode, "freq_hz": status.freq_hz},
    }), flush=True)


if __name__ == "__main__":
    sys.exit(main())
