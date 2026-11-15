#!/usr/bin/env python3
"""Live end-to-end test: cwlink receiver keys the IC-7300 over USB DTR and the
radio transmits at its lowest power, while the power meter and transmit state
are polled over CI-V to time the radio against the key line.

One process owns the serial port for both CI-V and the key line, because
opening the port a second time would raise DTR and key the transmitter.
Every changed setting is read first and restored at the end.

Usage: radio_live.py SERIAL_PORT KEY_FILE FREQ_HZ[,FREQ_HZ...]
"""
import argparse
import json
import sys
import threading
import time

import cwlink
from civ import CONTROLLER, RADIO, command, open_port


def ask(link, text):
    prefix = bytes.fromhex(text)
    for reply in command(link, prefix):
        if reply.startswith(prefix):
            return reply[len(prefix):].hex()
    raise RuntimeError(f"no answer to {text}")


def tell(link, text):
    if not any(reply == b"\xfb" for reply in command(link, bytes.fromhex(text))):
        raise RuntimeError(f"radio refused {text}")


def frequency_bytes(hertz):
    digits = f"{hertz:010d}"
    return "".join(digits[i:i + 2] for i in range(8, -1, -2))


def quick(link, payload, lock):
    """One CI-V query with tight timing: (sent ns, answered ns, reply data or None)."""
    frame = bytes([0xFE, 0xFE, RADIO, CONTROLLER]) + payload + b"\xFD"
    with lock:
        link.reset_input_buffer()
        sent = time.monotonic_ns()
        link.write(frame)
        data = b""
        while time.monotonic_ns() - sent < 80_000_000:
            waiting = link.in_waiting
            if not waiting:
                time.sleep(0.0003)
                continue
            data += link.read(waiting)
            while b"\xFD" in data:
                chunk, data = data.split(b"\xFD", 1)
                start = chunk.rfind(b"\xFE\xFE")
                if start >= 0 and len(chunk) >= start + 4 and chunk[start + 2] == CONTROLLER \
                        and chunk[start + 3] == RADIO and chunk[start + 4:].startswith(payload):
                    return sent, time.monotonic_ns(), chunk[start + 4 + len(payload):]
    return sent, time.monotonic_ns(), None


class SharedKey:
    name = "dtr (shared port)"

    def __init__(self, link):
        self.link = link
        self.down = False
        self.changed = time.monotonic_ns()

    def set(self, down):
        self.link.dtr = down
        if down != self.down:
            self.down = down
            self.changed = time.monotonic_ns()

    def close(self):
        self.set(False)


def main():
    port, key_file = sys.argv[1], sys.argv[2]
    candidates = [int(value) for value in sys.argv[3].split(",")]
    link = open_port(port)
    link.timeout = 0.05
    lock = threading.Lock()
    report = {}
    names = {"freq": "03", "mode": "04", "data": "1a06", "break_in": "1647", "power": "140a",
             "usb_send": "1a050078", "usb_keying_cw": "1a050079", "tx": "1c00"}
    before = {name: ask(link, code) for name, code in names.items()}
    report["before"] = before
    if before["tx"] != "00":
        raise SystemExit("radio is transmitting; not touching it")
    if before["usb_send"] != "00":
        raise SystemExit("USB SEND is enabled on the radio; stopping")

    out = SharedKey(link)
    stop = {"flag": False}
    samples = []           # (sent ns, answered ns, kind, value)
    notes = []
    polling = {"run": False}
    summary, edges = None, []
    problem = None

    def poll():
        tx_since = None
        turn = 0
        while polling["run"]:
            turn += 1
            kind, payload = ("po", b"\x15\x11") if turn % 4 else (("tx", b"\x1c\x00") if turn % 8 else ("swr", b"\x15\x12"))
            sent, answered, data = quick(link, payload, lock)
            if data is None:
                continue
            value = int(data.hex()) if kind != "tx" else data[0]
            samples.append((sent, answered, kind, value))
            if kind == "tx":
                if value and not out.down and answered - out.changed > 3_000_000_000:
                    tx_since = tx_since or answered
                    if answered - tx_since > 2_000_000_000:
                        notes.append("transmitter stayed on with the key up; keying disabled")
                        out.set(False)
                        stop["flag"] = True
                else:
                    tx_since = None

    poller = threading.Thread(target=poll, daemon=True)
    try:
        tell(link, "060301")                                   # CW, filter 1
        quiet = []
        for hertz in candidates:
            tell(link, "05" + frequency_bytes(hertz))
            time.sleep(0.4)
            levels = []
            for _ in range(12):
                levels.append(int(ask(link, "1502")))
                time.sleep(0.1)
            quiet.append((max(levels), hertz))
        report["s_meter_max_by_frequency"] = {str(hertz): level for level, hertz in quiet}
        level, chosen = min(quiet)
        report["frequency_hz"] = chosen
        if level > 60:                                         # 120 is S9; 60 is about S4-S5
            raise RuntimeError("no candidate frequency is quiet enough")
        tell(link, "05" + frequency_bytes(chosen))
        tell(link, "140a0000")                                 # lowest RF power
        tell(link, "164701")                                   # semi break-in
        if ask(link, "140a") != "0000" or ask(link, "04")[:2] != "03" or ask(link, "1c00") != "00":
            raise RuntimeError("radio did not take the test settings")
        tell(link, "1a05007901")                               # USB Keying (CW) = DTR
        if ask(link, "1c00") != "00":
            raise RuntimeError("radio went to transmit when USB keying was enabled")
        polling["run"] = True
        poller.start()

        args = argparse.Namespace(key=key_file, bind="::", listen=7373, output="dtr", port=port,
                                  buffer_ms=25.0, deadman_ms=150.0, window_s=30.0, exit_idle_s=3.0, edges=None)

        def give_up():
            time.sleep(90)
            stop["flag"] = True

        threading.Thread(target=give_up, daemon=True).start()
        summary, edges = cwlink.receive(args, out=out, stop=stop)
    except Exception as error:
        problem = str(error)
    finally:
        out.set(False)
        polling["run"] = False
        if poller.is_alive():
            poller.join(timeout=1)
        time.sleep(1.2)                                        # let break-in drop back to receive
        problems = []
        with lock:
            for text in ("1a05007900", "05" + before["freq"], "06" + before["mode"], "1a06" + before["data"],
                         "140a" + before["power"], "1647" + before["break_in"]):
                try:
                    tell(link, text)
                except Exception as error:
                    problems.append(f"{text}: {error}")
            time.sleep(0.3)
            after = {name: ask(link, code) for name, code in names.items()}
        link.close()
        report["after"] = after
        report["restored"] = after == before
        report["restore_problems"] = problems
        report["problem"] = problem
        report["notes"] = notes

    report["receiver"] = summary
    po = [(sent, answered, value) for sent, answered, kind, value in samples if kind == "po"]
    tx = [value for _, _, kind, value in samples if kind == "tx"]
    swr = [value for sent, answered, kind, value in samples if kind == "swr"]
    report["polls"] = {"po": len(po), "tx": len(tx), "tx_seen": int(any(tx)), "po_max": max([v for _, _, v in po], default=0),
                       "poll_ms_median": round(sorted((a - s) / 1e6 for s, a, _ in po)[len(po) // 2], 1) if po else None}
    if po and edges and report["polls"]["po_max"] > 0:
        top = sorted(value for _, _, value in po)[-max(1, len(po) // 10):]
        threshold = (sum(top) / len(top)) / 2
        report["po_threshold"] = round(threshold, 1)
        key_on = [value for sent, answered, value in po
                  if any(d <= sent // 1000 and answered // 1000 <= u for d, u in
                         zip([e[3] for e in edges if e[1]], [e[3] for e in edges if not e[1]]))]
        report["swr_meter_while_keyed_max"] = max(swr, default=None)
        tables = {}
        for state, label in ((1, "after_key_down"), (0, "after_key_up")):
            bins = {}
            for sent, answered, value in po:
                moment = (sent + answered) // 2000            # midpoint, microseconds
                last = None
                for edge in edges:
                    if edge[3] <= moment:
                        last = edge
                    else:
                        break
                if last is None or last[1] != state:
                    continue
                age = (moment - last[3]) / 1000.0
                if age >= 60:
                    continue
                slot = int(age // 5) * 5
                hit, count = bins.get(slot, (0, 0))
                bins[slot] = (hit + (1 if value > threshold else 0), count + 1)
            tables[label] = {f"{slot}-{slot + 5} ms": f"{hit}/{count} with power" for slot, (hit, count) in sorted(bins.items())}
        report["power_meter_vs_key"] = tables
    print("REPORT " + json.dumps(report))


if __name__ == "__main__":
    main()
