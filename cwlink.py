#!/usr/bin/env python3
"""cwlink - remote CW keying over UDP.

One file, two roles:

  cwlink.py recv   runs next to the radio and keys it (serial DTR line)
  cwlink.py send   test sender: keys a text at a given speed

The sender timestamps every key edge and the receiver replays the edges with
the original spacing after a fixed playout delay, so network jitter smaller
than that delay does not distort the keying. Each packet repeats the most
recent edges, so a lost packet does not lose an edge. Packets double as a
heartbeat: if they stop while the key is down the receiver releases the key.

Packets are authenticated with HMAC-SHA256 over a shared key.
"""
import argparse
import collections
import hashlib
import hmac
import json
import os
import select
import signal
import socket
import statistics
import struct
import sys
import time

MAGIC = b"CWL1"
ACK_MAGIC = b"CWA1"
HEADER = struct.Struct("!4sIIQQB")   # magic, session, seq, mono_us, wall_us, n_events
EVENT = struct.Struct("!IQB")        # event seq, mono_us, state
ACK = struct.Struct("!4sIIQ")        # magic, session, seq, receiver wall_us
TAG_LEN = 16
REPEAT_EVENTS = 8                    # edges repeated in every packet


def now_us():
    return time.monotonic_ns() // 1000


def wall_us():
    return time.time_ns() // 1000


def tag(key, data):
    return hmac.new(key, data, hashlib.sha256).digest()[:TAG_LEN]


def load_key(path):
    with open(path, "rb") as handle:
        key = handle.read().strip()
    if len(key) < 16:
        raise SystemExit(f"key file {path} is too short")
    return key


def pack_packet(key, session, seq, events):
    body = HEADER.pack(MAGIC, session, seq, now_us(), wall_us(), len(events))
    body += b"".join(EVENT.pack(*event) for event in events)
    return body + tag(key, body)


def unpack_packet(key, data):
    if len(data) < HEADER.size + TAG_LEN:
        return None
    body, received = data[:-TAG_LEN], data[-TAG_LEN:]
    if not hmac.compare_digest(tag(key, body), received):
        return None
    magic, session, seq, mono, wall, count = HEADER.unpack_from(body)
    if magic != MAGIC or len(body) != HEADER.size + count * EVENT.size:
        return None
    events = [EVENT.unpack_from(body, HEADER.size + i * EVENT.size) for i in range(count)]
    return session, seq, mono, wall, events


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def spread(values, scale=1000.0):
    """Summary of a list of microsecond values, reported in milliseconds."""
    if not values:
        return None
    return {
        "n": len(values),
        "min": round(min(values) / scale, 3),
        "p50": round(percentile(values, 0.50) / scale, 3),
        "p99": round(percentile(values, 0.99) / scale, 3),
        "max": round(max(values) / scale, 3),
    }


# --------------------------------------------------------------------------
# Key outputs
# --------------------------------------------------------------------------

class NullKey:
    """Records nothing and keys nothing; for network-only tests."""
    name = "null"

    def set(self, down):
        pass

    def close(self):
        pass


class SerialLineKey:
    """Keys a modem-control line (DTR or RTS) on a serial port.

    The port is opened with both lines held low. Linux still raises them for
    an instant inside open() unless the port speed is zero, so the caller
    should make sure the radio ignores the line until this has opened it.
    """

    def __init__(self, port, line):
        import serial
        self.line = line
        self.name = f"{line}:{port}"
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = 115200
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.serial.dtr = False
        self.serial.rts = False

    def set(self, down):
        if self.line == "dtr":
            self.serial.dtr = down
        else:
            self.serial.rts = down

    def close(self):
        try:
            self.set(False)
        finally:
            self.serial.close()


# --------------------------------------------------------------------------
# Receiver
# --------------------------------------------------------------------------

def receive(args):
    key = load_key(args.key)
    out = NullKey() if args.output == "null" else SerialLineKey(args.port, args.output)
    try:
        os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(10))
        realtime = True
    except (PermissionError, OSError, AttributeError):
        realtime = False

    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    sock.bind((args.bind, args.listen))
    sock.setblocking(False)

    buffer_us = int(args.buffer_ms * 1000)
    deadman_us = int(args.deadman_ms * 1000)
    window_us = int(args.window_s * 1_000_000)

    session = None
    last_seq = -1
    seen_packets = 0
    highest_seq = -1
    rejected = 0
    delays = collections.deque()      # (receive time, receive - send) for the offset window
    base = None                       # smallest receive - send in the window
    pending = {}                      # event seq -> (sender time, state)
    played_seq = -1
    last_packet = None
    key_down = False
    deadman_trips = 0

    late = []                         # how late an edge was when it first arrived
    actuation = []                    # actual - scheduled key time
    interval_error = []               # replayed spacing - original spacing
    jitter = []                       # packet delay above the window minimum
    one_way = []                      # wall clock receive - send (needs NTP on both ends)
    previous = None                   # (sender time, actual time) of previous edge
    edges = []
    stop = {"flag": False}

    def request_stop(*_):
        stop["flag"] = True

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    print(json.dumps({"event": "listening", "port": args.listen, "output": out.name,
                      "buffer_ms": args.buffer_ms, "deadman_ms": args.deadman_ms,
                      "realtime": realtime}), flush=True)

    def set_key(down):
        nonlocal key_down
        out.set(down)
        key_down = down

    try:
        while not stop["flag"]:
            now = now_us()
            # Dead-man: packets stopped while the key is down.
            if key_down and last_packet is not None and now - last_packet > deadman_us:
                set_key(False)
                pending.clear()
                previous = None
                deadman_trips += 1
                print(json.dumps({"event": "deadman_release", "silent_ms": (now - last_packet) / 1000}), flush=True)

            # Play every edge that is due, in order.
            due_at = None
            if pending and base is not None:
                seq = min(pending)
                sent, state = pending[seq]
                due_at = sent + base + buffer_us
                if now >= due_at - 150:
                    while now_us() < due_at:
                        pass
                    actual = now_us()
                    set_key(bool(state))
                    del pending[seq]
                    played_seq = seq
                    actuation.append(actual - due_at)
                    if previous is not None:
                        interval_error.append((actual - previous[1]) - (sent - previous[0]))
                    previous = (sent, actual)
                    if args.edges:
                        edges.append([seq, state, sent, actual])
                    continue

            if args.exit_idle_s and session is not None and now - last_packet > args.exit_idle_s * 1_000_000:
                break

            waits = [0.5]
            if due_at is not None:
                waits.append(max(0.0, (due_at - now - 150) / 1_000_000))
            if key_down and last_packet is not None:
                waits.append(max(0.0, (last_packet + deadman_us - now) / 1_000_000 + 0.0005))
            readable, _, _ = select.select([sock], [], [], min(waits))
            if not readable:
                continue

            while True:
                try:
                    data, sender = sock.recvfrom(2048)
                except BlockingIOError:
                    break
                received = now_us()
                received_wall = wall_us()
                packet = unpack_packet(key, data)
                if packet is None:
                    rejected += 1
                    continue
                packet_session, seq, sent_mono, sent_wall, events = packet
                if session is None or packet_session > session:
                    # A newer sender start replaces the old one; older ones are replays.
                    session, last_seq, highest_seq = packet_session, -1, -1
                    delays.clear()
                    base = None
                    pending.clear()
                    played_seq = -1
                    previous = None
                    if key_down:
                        set_key(False)
                    print(json.dumps({"event": "session", "id": session, "from": sender[0]}), flush=True)
                elif packet_session < session:
                    rejected += 1
                    continue
                if seq <= last_seq:
                    rejected += 1
                    continue
                last_seq = seq
                highest_seq = max(highest_seq, seq)
                seen_packets += 1
                last_packet = received
                one_way.append(received_wall - sent_wall)

                delay = received - sent_mono
                delays.append((received, delay))
                while delays and received - delays[0][0] > window_us:
                    delays.popleft()
                base = min(entry[1] for entry in delays)
                jitter.append(delay - base)

                for event_seq, event_time, state in events:
                    if event_seq > played_seq and event_seq not in pending:
                        pending[event_seq] = (event_time, state)
                        lateness = received - (event_time + base + buffer_us)
                        if lateness > 0:
                            late.append(lateness)

                sock.sendto(ACK.pack(ACK_MAGIC, session, seq, received_wall)
                            + tag(key, ACK.pack(ACK_MAGIC, session, seq, received_wall)), sender)
    finally:
        try:
            out.close()
        except Exception as error:  # report but still print the summary
            print(json.dumps({"event": "close_error", "error": str(error)}), flush=True)

    summary = {
        "event": "summary",
        "output": out.name,
        "buffer_ms": args.buffer_ms,
        "packets_received": seen_packets,
        "packets_lost": max(0, highest_seq + 1 - seen_packets),
        "packets_rejected": rejected,
        "edges_played": len(actuation),
        "edges_never_played": len(pending),
        "edges_late": len(late),
        "late_by_ms": spread(late),
        "deadman_releases": deadman_trips,
        "packet_jitter_ms": spread(jitter),
        "one_way_wallclock_ms": spread(one_way),
        "actuation_error_ms": spread(actuation),
        "element_length_error_ms": spread([abs(value) for value in interval_error]),
    }
    print(json.dumps(summary), flush=True)
    if args.edges:
        with open(args.edges, "w") as handle:
            json.dump(edges, handle)


# --------------------------------------------------------------------------
# Test sender
# --------------------------------------------------------------------------

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
                plan.append((int(at), 1))
                at += unit * (1 if symbol == "." else 3)
                plan.append((int(at), 0))
                at += unit
            at += unit * 2
        at += unit * 4
    return plan


def send(args):
    key = load_key(args.key)
    info = socket.getaddrinfo(args.host, args.port, type=socket.SOCK_DGRAM)[0]
    sock = socket.socket(info[0], socket.SOCK_DGRAM)
    sock.connect(info[4])
    sock.setblocking(False)

    session = int(time.time())
    plan = keying_plan(args.text, args.wpm) * args.repeat
    span = keying_plan(args.text, args.wpm)[-1][0] + int(7 * 1_200_000 / args.wpm)
    plan = [(offset + (index // (len(plan) // args.repeat)) * span, state)
            for index, (offset, state) in enumerate(plan)]

    heartbeat_us = int(args.heartbeat_ms * 1000)
    start = now_us() + 300_000        # heartbeats first, so the receiver has a delay estimate
    recent = collections.deque(maxlen=REPEAT_EVENTS)
    sent_at = {}
    rtt = []
    seq = 0
    event_seq = 0
    next_heartbeat = now_us()
    index = 0
    end = start + plan[-1][0] + 300_000

    def transmit():
        nonlocal seq
        sent_at[seq] = now_us()
        try:
            sock.send(pack_packet(key, session, seq, list(recent)))
        except OSError:
            pass
        seq += 1

    def collect_acks():
        while True:
            try:
                data = sock.recv(256)
            except (BlockingIOError, ConnectionRefusedError):
                return
            if len(data) != ACK.size + TAG_LEN or not hmac.compare_digest(tag(key, data[:ACK.size]), data[ACK.size:]):
                continue
            _, ack_session, ack_seq, _ = ACK.unpack(data[:ACK.size])
            if ack_session == session and ack_seq in sent_at:
                rtt.append(now_us() - sent_at.pop(ack_seq))

    while True:
        now = now_us()
        if now >= end:
            break
        next_edge = start + plan[index][0] if index < len(plan) else None
        if next_edge is not None and now >= next_edge - 150:
            while now_us() < next_edge:
                pass
            recent.append((event_seq, now_us(), plan[index][1]))
            event_seq += 1
            index += 1
            transmit()
            next_heartbeat = now_us() + heartbeat_us
            continue
        if now >= next_heartbeat:
            transmit()
            next_heartbeat = now + heartbeat_us
            continue
        target = min(value for value in (next_edge, next_heartbeat, end) if value is not None)
        select.select([sock], [], [], max(0.0, (target - now - 150) / 1_000_000))
        collect_acks()

    deadline = now_us() + 500_000
    while now_us() < deadline:
        select.select([sock], [], [], 0.05)
        collect_acks()

    print(json.dumps({
        "event": "sender_summary",
        "target": f"{info[4][0]}:{args.port}",
        "wpm": args.wpm,
        "edges": event_seq,
        "packets_sent": seq,
        "acks": len(rtt),
        "packets_unacked": seq - len(rtt),
        "round_trip_ms": spread(rtt),
        "seconds": round((end - start) / 1_000_000, 1),
    }), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    roles = parser.add_subparsers(dest="role", required=True)

    recv = roles.add_parser("recv", help="receive and key the radio")
    recv.add_argument("--key", required=True, help="file holding the shared secret")
    recv.add_argument("--bind", default="::", help="address to listen on")
    recv.add_argument("--listen", type=int, default=7373, help="UDP port")
    recv.add_argument("--output", choices=["null", "dtr", "rts"], default="null")
    recv.add_argument("--port", help="serial port for dtr/rts output")
    recv.add_argument("--buffer-ms", type=float, default=25.0, help="playout delay")
    recv.add_argument("--deadman-ms", type=float, default=150.0,
                      help="release the key after this long without packets")
    recv.add_argument("--window-s", type=float, default=30.0, help="span of the delay-minimum window")
    recv.add_argument("--exit-idle-s", type=float, default=0.0, help="test use: exit after this long idle")
    recv.add_argument("--edges", help="test use: write replayed edges to this file")
    recv.set_defaults(run=receive)

    snd = roles.add_parser("send", help="test sender")
    snd.add_argument("--key", required=True)
    snd.add_argument("--host", required=True)
    snd.add_argument("--port", type=int, default=7373)
    snd.add_argument("--wpm", type=float, default=25.0)
    snd.add_argument("--text", default="PARIS PARIS PARIS PARIS PARIS")
    snd.add_argument("--repeat", type=int, default=1)
    snd.add_argument("--heartbeat-ms", type=float, default=20.0)
    snd.set_defaults(run=send)

    args = parser.parse_args()
    if args.role == "recv" and args.output != "null" and not args.port:
        parser.error("--port is required for dtr/rts output")
    args.run(args)


if __name__ == "__main__":
    main()
