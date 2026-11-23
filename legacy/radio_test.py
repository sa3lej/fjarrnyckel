#!/usr/bin/env python3
"""Key the IC-7300 over USB DTR without transmitting and time its sidetone.

The radio is put in CW with break-in OFF and RF power at zero, so keying
only produces the sidetone. Every setting that is changed is read first and
restored afterwards, and the transmit state is polled: if the radio ever
reports TX the test stops at once.

Usage: radio_test.py SERIAL_PORT ALSA_DEVICE [elements]
"""
import json
import pathlib
import random
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "receiver"))
import threading
import time

import numpy

from civ import command, open_port

RATE = 48000


def ask(link, text):
    replies = command(link, bytes.fromhex(text))
    prefix = bytes.fromhex(text)
    for reply in replies:
        if reply.startswith(prefix):
            return reply[len(prefix):].hex()
    raise RuntimeError(f"no answer to {text}: {[r.hex() for r in replies]}")


def tell(link, text):
    replies = command(link, bytes.fromhex(text))
    if not any(reply == b"\xfb" for reply in replies):
        raise RuntimeError(f"radio refused {text}: {[r.hex() for r in replies]}")


def transmitting(link):
    return ask(link, "1c00") != "00"


def main():
    port, device = sys.argv[1], sys.argv[2]
    elements = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    link = open_port(port)
    report = {}
    before = {
        "mode": ask(link, "04"), "data": ask(link, "1a06"), "break_in": ask(link, "1647"),
        "power": ask(link, "140a"), "usb_send": ask(link, "1a050078"),
        "usb_keying_cw": ask(link, "1a050079"), "tx": ask(link, "1c00"),
    }
    report["before"] = before
    if before["tx"] != "00":
        raise SystemExit("radio is transmitting; not touching it")
    if before["usb_send"] != "00":
        raise SystemExit("USB SEND is enabled on the radio; a serial line could key the transmitter, stopping")

    chunks = []          # (time after read, samples so far)
    audio = bytearray()
    recorder = None
    downs, ups = [], []
    aborted = None
    try:
        tell(link, "164700")                 # break-in OFF: key gives sidetone only
        tell(link, "140a0000")               # RF power 0 as a second guard
        tell(link, "060301")                 # CW, filter 1
        if ask(link, "1647") != "00" or ask(link, "04")[:2] != "03" or ask(link, "140a") != "0000":
            raise RuntimeError("radio did not take the safe settings")
        if transmitting(link):
            raise RuntimeError("radio reports TX before keying")

        recorder = subprocess.Popen(
            ["arecord", "-q", "-D", device, "-f", "S16_LE", "-c", "1", "-r", str(RATE), "-t", "raw",
             "--period-time=2000", "--buffer-time=20000"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        def read_audio():
            total = 0
            while True:
                data = recorder.stdout.read(192)
                if not data:
                    return
                total += len(data) // 2
                chunks.append((time.monotonic_ns(), total))
                audio.extend(data)

        reader = threading.Thread(target=read_audio, daemon=True)
        reader.start()
        time.sleep(0.8)

        tell(link, "1a05007901")             # USB Keying (CW) = DTR
        if transmitting(link):
            raise RuntimeError("radio reports TX after enabling USB keying")
        time.sleep(0.3)

        for index in range(elements):
            downs.append(time.monotonic_ns())
            link.dtr = True
            time.sleep(0.060)
            ups.append(time.monotonic_ns())
            link.dtr = False
            time.sleep(0.090 + random.random() * 0.030)
            if index in (0, 1) or index % 10 == 9:
                if transmitting(link):
                    raise RuntimeError(f"radio reports TX during keying (element {index})")
        time.sleep(0.4)
    except Exception as error:
        aborted = str(error)
    finally:
        link.dtr = False
        problems = []
        for text in ("1a05007900",
                     "06" + before["mode"],
                     "1a06" + before["data"],
                     "140a" + before["power"],
                     "1647" + before["break_in"]):
            try:
                tell(link, text)
            except Exception as error:
                problems.append(f"{text}: {error}")
        if recorder:
            recorder.terminate()
            try:
                recorder.wait(timeout=2)
            except subprocess.TimeoutExpired:
                recorder.kill()
        time.sleep(0.2)
        after = {
            "mode": ask(link, "04"), "data": ask(link, "1a06"), "break_in": ask(link, "1647"),
            "power": ask(link, "140a"), "usb_send": ask(link, "1a050078"),
            "usb_keying_cw": ask(link, "1a050079"), "tx": ask(link, "1c00"),
        }
        link.close()
        report["after"] = after
        report["restored"] = after == before
        report["restore_problems"] = problems
        report["aborted"] = aborted

    if aborted is None and chunks:
        samples = numpy.frombuffer(bytes(audio[: len(audio) // 2 * 2]), dtype="<i2").astype(numpy.float64)
        # Sample n was captured no later than (read time - samples read after it): use the
        # tightest such bound, so audio buffering only ever makes the delay look longer.
        origin = min(stamp - total * 1_000_000_000 // RATE for stamp, total in chunks)
        numpy.savez_compressed("last-run.npz", samples=samples.astype(numpy.int16), origin=origin,
                               downs=numpy.array(downs), ups=numpy.array(ups))
        window = RATE // 500                                   # 2 ms
        centred = samples - numpy.convolve(samples, numpy.ones(480) / 480, mode="same")
        envelope = numpy.sqrt(numpy.convolve(centred ** 2, numpy.ones(window) / window, mode="same"))
        first_down = (downs[0] - origin) * RATE // 1_000_000_000
        quiet = envelope[: max(1, first_down - RATE // 10)]
        floor = float(numpy.percentile(quiet, 95))
        peak = float(numpy.percentile(envelope[first_down:], 99))
        report["audio"] = {"noise_floor": round(floor, 1), "tone_level": round(peak, 1),
                           "seconds": round(len(samples) / RATE, 2)}
        if peak > max(4 * floor, floor + 200):
            threshold = (floor + peak) / 2
            above = envelope > threshold
            edges = numpy.flatnonzero(numpy.diff(above.astype(numpy.int8)))
            rises = [int(i) for i in edges if not above[i]]
            falls = [int(i) for i in edges if above[i]]

            def stamp(index):
                return origin + index * 1_000_000_000 // RATE

            on_delay, off_delay, lengths = [], [], []
            for down, up in zip(downs, ups):
                rise = next((stamp(i) for i in rises if stamp(i) >= down - 2_000_000), None)
                fall = next((stamp(i) for i in falls if stamp(i) >= up - 2_000_000), None)
                if rise is not None and rise - down < 140_000_000:
                    on_delay.append((rise - down) / 1e6)
                if fall is not None and fall - up < 140_000_000:
                    off_delay.append((fall - up) / 1e6)
                if rise is not None and fall is not None and fall > rise:
                    lengths.append((fall - rise) / 1e6)

            def stats(values):
                if not values:
                    return None
                ordered = sorted(values)
                return {"n": len(values), "min": round(ordered[0], 1), "median": round(ordered[len(ordered) // 2], 1),
                        "max": round(ordered[-1], 1)}

            report["tone_detected"] = True
            report["key_down_to_tone_ms"] = stats(on_delay)
            report["key_up_to_silence_ms"] = stats(off_delay)
            report["tone_length_ms_for_60ms_key"] = stats(lengths)
        else:
            report["tone_detected"] = False
    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
