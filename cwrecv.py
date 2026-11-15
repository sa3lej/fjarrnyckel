#!/usr/bin/env python3
"""Receiver for the remote CW keying link (CWL2). Runs next to the radio.

`Receiver` holds all the rules and takes the time as an argument, so it can be
tested against a simulated clock. `main` wraps it in a UDP socket and a key
output.

Rules, modelled on OK1HRA's Paddle2IP:

- An edge sent at client time t is played at t + offset. The offset is the
  smallest (arrival - t_send) seen in a sliding window plus the playout delay,
  so every edge of a character gets the same offset and element lengths are
  reproduced exactly.
- The offset is only corrected at the start of a mark, by at most 1.5 ms, so
  clock drift is absorbed in the gaps.
- A key-down that arrives too late is played as soon as possible and the rest
  of the over is shifted by the same amount: the gap gets longer, the mark
  keeps its length. A late key-up is played at once.
- A session only starts with a nonce the server issued in the last seconds, so
  a recorded packet cannot start one. A session that has been replaced cannot
  come back while its nonce could still be valid. Within a session the packet
  counter must rise.
- The key is released, queued edges are dropped and the receiver goes mute
  when: packets stop while the key is down, one key-down lasts too long, edges
  are lost beyond the redundancy, a stale packet arrives, or the rig is not in
  CW. Mute ends only when the client is idle or its newest edge is a key-up
  older than five dits, so keying never resumes inside a character.
"""
import argparse
import collections
import json
import os
import select
import signal
import socket
import sys
import time

import cwproto as proto

SLEW_US = 1_500            # largest offset correction per mark start
LATE_MARGIN_US = 1_500     # a key-down closer than this to "now" is late
EXTRA_CAP_US = 500_000     # cap of the adaptive extra delay
STALE_US = 400_000         # a packet this far behind the best latency is stale
NONCE_LIFE_US = 5_000_000  # a nonce is current for this long, then previous
STATUS_EVERY_US = 100_000
MODES_CW = (0x03, 0x07)    # CI-V mode bytes for CW and CW-R


class Receiver:
    def __init__(self, key, out, *, delay_ms=25, link_timeout_ms=300, max_key_down_ms=5000, window_ms=2000,
                 rig=None, random_bytes=os.urandom):
        self.key = key
        self.out = out
        self.default_delay_us = int(delay_ms * 1000)
        self.delay_us = self.default_delay_us
        self.link_timeout_us = int(link_timeout_ms * 1000)
        self.max_key_down_us = int(max_key_down_ms * 1000)
        self.window_us = int(window_ms * 1000)
        self.rig = rig
        self.random_bytes = random_bytes

        self.session = None
        self.retired = {}                      # session -> when it was replaced
        self.last_seq = 0
        self.next_idx = 0
        self.muted = True
        self.wpm = 0
        self.window = collections.deque()      # (arrival, arrival - t_send)
        self.min_latency = None
        self.offset = None
        self.extra_us = 0
        self.queue = collections.deque()       # (play_at, down, edge time)
        self.planned_down = False              # key state after the last queued edge
        self.last_edge_time = None             # client time of the last queued edge
        self.key_down = False
        self.key_down_since = 0
        self.last_packet = None
        self.echo_t = 0
        self.echo_arrival = 0
        self.client = None
        self.nonce = self._new_nonce()
        self.previous_nonce = 0
        self.nonce_since = None
        self.last_status = None
        self.status_seq = 0

        self.rx_pkts = 0
        self.rejected = 0
        self.late = 0
        self.lost = 0
        self.aborts = collections.Counter()
        self.played = []                       # (edge time, actual time, down)

    # ---- helpers -----------------------------------------------------------
    def _new_nonce(self):
        value = 0
        while value == 0:
            value = int.from_bytes(self.random_bytes(8), "big")
        return value

    def _rotate_nonce(self, now):
        if self.nonce_since is None:
            self.nonce_since = now
        elif now - self.nonce_since >= NONCE_LIFE_US:
            # After a silence of two lifetimes the old nonce must not survive as "previous".
            self.previous_nonce = self.nonce if now - self.nonce_since < 2 * NONCE_LIFE_US else 0
            self.nonce = self._new_nonce()
            self.nonce_since = now

    def keying_allowed(self):
        return True if self.rig is None else self.rig.cw_mode()

    def _set_key(self, down, now):
        self.out.set(down)
        if down and not self.key_down:
            self.key_down_since = now
        self.key_down = down

    def abort(self, reason, now):
        """Release the key, forget queued edges and stay silent until a word gap."""
        if self.key_down:
            self._set_key(False, now)
        if self.key_down or self.queue or not self.muted:
            self.aborts[reason] += 1
        self.queue.clear()
        self.planned_down = False
        self.offset = None
        self.muted = True

    def _status(self, now, nonce_bad=False):
        flags = 0
        mode, freq = (0, 0)
        if self.rig is not None:
            mode, freq = self.rig.mode_and_freq()
            if mode:
                flags |= proto.ST_RIG_VALID
        if self.keying_allowed():
            flags |= proto.ST_CW_MODE
        if self.key_down:
            flags |= proto.ST_KEY_DOWN
        if nonce_bad:
            flags |= proto.ST_NONCE_BAD
        if self.muted:
            flags |= proto.ST_MUTED
        self.status_seq += 1
        self.last_status = now
        return proto.pack_status(self.key, proto.Status(
            flags, mode, self.session or 0, self.status_seq, self.echo_t, max(0, now - self.echo_arrival),
            self.nonce, freq, (self.delay_us + self.extra_us) // 1000, self.late, self.lost, self.rx_pkts))

    # ---- packets -----------------------------------------------------------
    def on_packet(self, data, sender, now):
        """Handle one datagram; return a list of (data, address) to send back."""
        packet = proto.unpack_key(self.key, data)
        if packet is None:
            self.rejected += 1
            return []
        self._rotate_nonce(now)
        nonce_ok = packet.nonce != 0 and packet.nonce in (self.nonce, self.previous_nonce)

        if packet.session != self.session:
            for old in [s for s, when in self.retired.items() if now - when > 3 * NONCE_LIFE_US]:
                del self.retired[old]
            if not nonce_ok or packet.session in self.retired:
                # Not our session and no proof of freshness, or a session that was replaced
                # while its nonce is still valid: hand out a nonce, change nothing.
                self.rejected += 1
                return [(self._status(now, nonce_bad=True), sender)]
            self.abort("new session", now)
            if self.session is not None:
                self.retired[self.session] = now
            self.session = packet.session
            self.next_idx = (packet.ev_idx0 + len(packet.edges)) & 0xFFFFFFFF   # do not replay its history
            self.muted = True
            self.extra_us = 0
            self.window.clear()
            self.min_latency = None
            self.offset = None
        elif proto.signed32(packet.seq - self.last_seq) <= 0:
            self.rejected += 1                                                   # duplicate or replay
            return []

        self.client = sender
        self.last_seq = packet.seq
        self.last_packet = now
        self.rx_pkts += 1
        self.echo_t = packet.t_send
        self.echo_arrival = now
        self.wpm = packet.wpm
        self.delay_us = min(1000, max(10, packet.delay_ms)) * 1000 if packet.delay_ms else self.default_delay_us

        sample = now - packet.t_send
        self.window.append((now, sample))
        while self.window and now - self.window[0][0] > self.window_us:
            self.window.popleft()
        self.min_latency = min(entry[1] for entry in self.window)

        replies = []
        if not nonce_ok or self.last_status is None or now - self.last_status >= STATUS_EVERY_US:
            replies.append((self._status(now, nonce_bad=not nonce_ok), sender))
        if not nonce_ok:
            return replies                      # the edges come again in the next packets

        allowed = self.keying_allowed()
        stale = sample - self.min_latency > STALE_US
        if stale:
            self.abort("stale packet", now)
        if not allowed:
            self.abort("not in CW", now)

        if self.muted and allowed and not stale:
            if not packet.edges:
                self.muted = False
            else:
                newest_time, newest_down = packet.edges[-1]
                known = proto.signed32(packet.ev_idx0 + len(packet.edges) - self.next_idx) <= 0
                if known and not newest_down and packet.t_send - newest_time > 5 * proto.dit_us(self.wpm):
                    self.muted = False

        for position, (when, down) in enumerate(packet.edges):
            gap = proto.signed32(packet.ev_idx0 + position - self.next_idx)
            if gap < 0:
                continue                        # seen before
            if gap > 0:
                self.lost += gap
                self.abort("edges lost", now)
            self.next_idx = (packet.ev_idx0 + position + 1) & 0xFFFFFFFF
            if self.muted or stale or down == self.planned_down:
                continue
            self._schedule(when, down, now)
        return replies

    def _schedule(self, when, down, now):
        target = self.min_latency + self.delay_us + self.extra_us
        idle = not self.queue and not self.key_down
        new_over = self.offset is None or (
            idle and self.last_edge_time is not None and when - self.last_edge_time > 5 * proto.dit_us(self.wpm))
        if new_over:
            self.extra_us -= self.extra_us // 4
            target = self.min_latency + self.delay_us + self.extra_us
            self.offset = target
        elif down:
            self.offset += max(-SLEW_US, min(SLEW_US, target - self.offset))
        play_at = when + self.offset
        if down and play_at < now + LATE_MARGIN_US:
            lateness = now + LATE_MARGIN_US - play_at
            self.offset += lateness             # the gap absorbs it, the mark keeps its length
            self.extra_us = min(EXTRA_CAP_US, self.extra_us + lateness)
            self.late += 1
            play_at = when + self.offset
        elif not down and play_at < now:
            self.late += 1
            self.extra_us = min(EXTRA_CAP_US, self.extra_us + (now - play_at))   # more margin from the next over
            play_at = now
        if self.queue and play_at < self.queue[-1][0]:
            play_at = self.queue[-1][0]         # keep the queue in order
        self.queue.append((play_at, down, when))
        self.planned_down = down
        self.last_edge_time = when

    # ---- time --------------------------------------------------------------
    def poll(self, now):
        """Apply everything that is due; return the time of the next deadline or None."""
        if self.key_down:
            if self.last_packet is None or now - self.last_packet > self.link_timeout_us:
                self.abort("link timeout", now)
            elif now - self.key_down_since > self.max_key_down_us:
                self.abort("key down too long", now)
        if (self.key_down or self.queue) and not self.keying_allowed():
            self.abort("not in CW", now)
        while self.queue and self.queue[0][0] <= now:
            play_at, down, when = self.queue.popleft()
            if down and (self.last_packet is None or now - self.last_packet > self.link_timeout_us):
                self.abort("link timeout", now)  # never start a mark on a dead link
                break
            self._set_key(down, now)
            self.played.append((when, now, down, play_at))
        deadlines = []
        if self.queue:
            deadlines.append(self.queue[0][0])
        if self.key_down:
            deadlines.append(self.last_packet + self.link_timeout_us + 1)
            deadlines.append(self.key_down_since + self.max_key_down_us + 1)
        return min(deadlines) if deadlines else None

    def summary(self):
        def spread(values):
            if not values:
                return None
            ordered = sorted(values)
            return {"n": len(ordered), "p50": round(ordered[len(ordered) // 2] / 1000, 3),
                    "p99": round(ordered[min(len(ordered) - 1, int(0.99 * len(ordered)))] / 1000, 3),
                    "max": round(ordered[-1] / 1000, 3)}
        marks, timing = [], []
        for index in range(1, len(self.played)):
            earlier, later = self.played[index - 1], self.played[index]
            if earlier[2] and not later[2]:      # a mark: key-down followed by key-up
                marks.append(abs((later[1] - earlier[1]) - (later[0] - earlier[0])))
        for when, actual, down, play_at in self.played:
            timing.append(actual - play_at)
        return {"packets": self.rx_pkts, "rejected": self.rejected, "edges_played": len(self.played),
                "late": self.late, "lost": self.lost, "aborts": dict(self.aborts),
                "delay_ms": (self.delay_us + self.extra_us) / 1000,
                "mark_length_error_ms": spread(marks), "actuation_error_ms": spread(timing)}


# ---- key outputs and rig ----------------------------------------------------

class NullKey:
    name = "none"

    def set(self, down):
        pass

    def close(self):
        pass


class SerialKey:
    """Keys DTR on the radio's serial port and reads mode and frequency over CI-V on
    the same port. One process owns the port: a second open would raise DTR."""

    def __init__(self, port):
        import threading

        import civ
        self.civ = civ
        self.name = f"dtr:{port}"
        self.link = civ.open_port(port)
        self.lock = threading.Lock()
        self.mode = 0
        self.freq = 0
        self.seen = 0.0
        self.running = True
        self.thread = threading.Thread(target=self._poll, daemon=True)
        self.thread.start()

    def set(self, down):
        self.link.dtr = down

    def _poll(self):
        while self.running:
            try:
                with self.lock:
                    mode = self.civ.query(self.link, b"\x04")
                    freq = self.civ.query(self.link, b"\x03")
                if mode and freq and len(freq) == 5:
                    self.mode = mode[0]
                    self.freq = int("".join(f"{byte:02x}" for byte in reversed(freq)))
                    self.seen = time.monotonic()
            except Exception:
                pass
            time.sleep(0.5)

    def cw_mode(self):
        return self.mode in MODES_CW and time.monotonic() - self.seen < 3.0

    def mode_and_freq(self):
        fresh = time.monotonic() - self.seen < 3.0
        return (self.mode, self.freq) if fresh else (0, 0)

    def close(self):
        self.running = False
        try:
            self.link.dtr = False
        finally:
            self.thread.join(timeout=1)
            self.link.close()


class WfviewKey:
    """Keys DTR on the radio port that wfview already holds open, and reads mode
    and frequency from wfview's rigctld.

    Opening the port a second time would raise DTR and key the transmitter.
    Instead the receiver duplicates wfview's own descriptor with pidfd_getfd
    and only changes the DTR modem line on it. Run it as the same user as
    wfview; that is all pidfd_getfd needs (the kernel's ptrace rule: own
    processes only, unless Yama's ptrace_scope restricts it further).
    If wfview exits or restarts, the descriptor is released and keying is
    refused until the new wfview instance holds the port again.
    """

    RIG_MODES = {"LSB": 0x00, "USB": 0x01, "AM": 0x02, "CW": 0x03, "RTTY": 0x04, "FM": 0x05,
                 "CWR": 0x07, "RTTYR": 0x08, "PKTLSB": 0x00, "PKTUSB": 0x01, "PKTFM": 0x05}

    def __init__(self, port, rigctld=("127.0.0.1", 4533), process="wfview"):
        import threading
        self.device = os.path.realpath(port)
        self.rigctld = rigctld
        self.process = process
        self.name = f"wfview-dtr:{self.device}"
        self.lock = threading.Lock()
        self.fd = None
        self.pid = None
        self.mode = 0
        self.freq = 0
        self.seen = 0.0
        self.running = True
        self.thread = threading.Thread(target=self._poll, daemon=True)
        self.thread.start()

    # -- descriptor -------------------------------------------------------
    def _holder(self):
        """(pid, fd number) of the process holding the radio port, or None."""
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open(f"/proc/{pid}/comm") as comm:
                    if comm.read().strip() != self.process:
                        continue
                for fd in os.listdir(f"/proc/{pid}/fd"):
                    if os.readlink(f"/proc/{pid}/fd/{fd}") == self.device:
                        return int(pid), int(fd)
            except OSError:
                continue
        return None

    def _acquire(self):
        import ctypes
        holder = self._holder()
        if holder is None:
            return
        pid, number = holder
        libc = ctypes.CDLL(None, use_errno=True)
        pidfd = os.pidfd_open(pid)
        try:
            fd = libc.syscall(438, pidfd, number, 0)  # pidfd_getfd
            if fd < 0:
                raise OSError(ctypes.get_errno(), os.strerror(ctypes.get_errno()))
        finally:
            os.close(pidfd)
        self._dtr(fd, False)
        self.fd, self.pid = fd, pid

    def _release(self):
        if self.fd is not None:
            try:
                self._dtr(self.fd, False)
            except OSError:
                pass
            os.close(self.fd)
        self.fd = self.pid = None

    @staticmethod
    def _dtr(fd, on):
        import fcntl, struct, termios
        fcntl.ioctl(fd, termios.TIOCMBIS if on else termios.TIOCMBIC, struct.pack("i", termios.TIOCM_DTR))

    def set(self, down):
        with self.lock:
            if self.fd is None:
                return  # no radio connection: never key
            try:
                self._dtr(self.fd, down)
            except OSError:
                self._release()

    # -- rig state ----------------------------------------------------------
    def _rigctl(self, sock, command):
        sock.sendall(command + b"\n")
        data = b""
        while not data.endswith(b"\n") or (command == b"m" and data.count(b"\n") < 2):
            chunk = sock.recv(256)
            if not chunk:
                raise OSError("rigctld closed")
            data += chunk
        return data.decode().split("\n")

    def _poll(self):
        sock = None
        while self.running:
            try:
                with self.lock:
                    if self.pid is not None and not os.path.exists(f"/proc/{self.pid}"):
                        self._release()
                    if self.fd is None:
                        self._acquire()
                if sock is None:
                    sock = socket.create_connection(self.rigctld, timeout=2)
                mode = self._rigctl(sock, b"m")[0].strip()
                freq = self._rigctl(sock, b"f")[0].strip()
                self.mode = self.RIG_MODES.get(mode, 0xFF)
                self.freq = int(float(freq))
                self.seen = time.monotonic()
            except (OSError, ValueError):
                if sock is not None:
                    sock.close()
                    sock = None
            time.sleep(0.5)
        if sock is not None:
            sock.close()

    def cw_mode(self):
        return self.fd is not None and self.mode in MODES_CW and time.monotonic() - self.seen < 3.0

    def mode_and_freq(self):
        fresh = time.monotonic() - self.seen < 3.0
        return (self.mode, self.freq) if fresh else (0, 0)

    def close(self):
        self.running = False
        self.thread.join(timeout=2)
        with self.lock:
            self._release()


def now_us():
    return time.monotonic_ns() // 1000


def main():
    parser = argparse.ArgumentParser(description="Remote CW keying receiver")
    parser.add_argument("--key", required=True, help="file holding the shared secret")
    parser.add_argument("--bind", default="::")
    parser.add_argument("--port", type=int, default=7373, help="UDP port")
    parser.add_argument("--serial", help="radio serial port; keys DTR and requires the rig to be in CW")
    parser.add_argument("--wfview", nargs="?", const="127.0.0.1:4533", metavar="HOST:PORT",
                        help="share the --serial port with a running wfview: key DTR on wfview's own descriptor, read the rig over its rigctld")
    parser.add_argument("--delay-ms", type=float, default=25.0, help="playout delay when the client asks for none")
    parser.add_argument("--link-timeout-ms", type=float, default=300.0)
    parser.add_argument("--max-key-down-ms", type=float, default=5000.0)
    parser.add_argument("--exit-idle-s", type=float, default=0.0, help="test use: exit after this long without packets")
    args = parser.parse_args()

    if args.wfview and not args.serial:
        parser.error("--wfview needs --serial to identify the radio port")
    if args.wfview:
        host, _, rport = args.wfview.rpartition(":")
        out = WfviewKey(args.serial, (host or "127.0.0.1", int(rport)))
    else:
        out = SerialKey(args.serial) if args.serial else NullKey()
    core = Receiver(proto.load_key(args.key), out, delay_ms=args.delay_ms, link_timeout_ms=args.link_timeout_ms,
                    max_key_down_ms=args.max_key_down_ms, rig=out if args.serial else None)
    try:
        os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(10))
        realtime = True
    except (PermissionError, OSError, AttributeError):
        realtime = False
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    sock.bind((args.bind, args.port))
    sock.setblocking(False)
    stop = {"flag": False}
    signal.signal(signal.SIGINT, lambda *_: stop.update(flag=True))
    signal.signal(signal.SIGTERM, lambda *_: stop.update(flag=True))
    print(json.dumps({"event": "listening", "port": args.port, "output": out.name, "realtime": realtime}), flush=True)
    reported = collections.Counter()
    try:
        while not stop["flag"]:
            now = now_us()
            deadline = core.poll(now)
            for reason, count in core.aborts.items():
                if count != reported[reason]:
                    reported[reason] = count
                    print(json.dumps({"event": "released", "reason": reason, "count": count}), flush=True)
            if deadline is not None and deadline - now <= 150:
                while now_us() < deadline:
                    pass
                continue
            if args.exit_idle_s and core.last_packet is not None and now - core.last_packet > args.exit_idle_s * 1e6:
                break
            wait = 0.05 if deadline is None else min(0.05, max(0.0, (deadline - now - 150) / 1e6))
            readable, _, _ = select.select([sock], [], [], wait)
            if not readable:
                continue
            while True:
                try:
                    data, sender = sock.recvfrom(2048)
                except BlockingIOError:
                    break
                for reply, target in core.on_packet(data, sender, now_us()):
                    try:
                        sock.sendto(reply, target)
                    except OSError:
                        pass
    finally:
        out.set(False)
        out.close()
    rig_seen = {"rig_mode": out.mode, "rig_freq_hz": out.freq} if args.serial else {}
    print(json.dumps({"event": "summary", **rig_seen, **core.summary()}), flush=True)


if __name__ == "__main__":
    sys.exit(main())
