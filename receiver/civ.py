#!/usr/bin/env python3
"""Minimal Icom CI-V helper for the IC-7300 over its USB serial port.

Opens the port with DTR and RTS held low and never raises them.
Usage: civ.py PORT read|raw HEX...   (see main)
"""
import sys
import time

import serial

RADIO = 0x94
CONTROLLER = 0xE0


def open_port(port, baud=115200):
    link = serial.Serial()
    link.port = port
    link.baudrate = baud
    link.timeout = 0.05
    link.dtr = False
    link.rts = False
    link.open()
    link.dtr = False
    link.rts = False
    return link


def command(link, payload, wait=0.4):
    """Send one command; return the radio's reply payloads (echo removed)."""
    frame = bytes([0xFE, 0xFE, RADIO, CONTROLLER]) + bytes(payload) + b"\xFD"
    link.reset_input_buffer()
    link.write(frame)
    deadline = time.monotonic() + wait
    data = b""
    replies = []
    while time.monotonic() < deadline:
        data += link.read(64)
        while b"\xFD" in data:
            chunk, data = data.split(b"\xFD", 1)
            start = chunk.rfind(b"\xFE\xFE")
            if start < 0 or len(chunk) < start + 4:
                continue
            to, sender, body = chunk[start + 2], chunk[start + 3], chunk[start + 4:]
            if to == CONTROLLER and sender == RADIO:
                replies.append(body)
        if replies:
            break
    return replies


def main():
    port = sys.argv[1]
    link = open_port(port)
    try:
        for text in sys.argv[2:]:
            payload = bytes.fromhex(text)
            replies = command(link, payload)
            print(text, "->", " | ".join(reply.hex() for reply in replies) or "no reply")
    finally:
        link.close()


if __name__ == "__main__":
    main()


def query(link, payload, wait_ns=80_000_000):
    """One CI-V read with tight timing: the reply data after the echoed command, or None."""
    frame = bytes([0xFE, 0xFE, RADIO, CONTROLLER]) + bytes(payload) + b"\xFD"
    link.reset_input_buffer()
    sent = time.monotonic_ns()
    link.write(frame)
    data = b""
    while time.monotonic_ns() - sent < wait_ns:
        waiting = link.in_waiting
        if not waiting:
            time.sleep(0.0003)
            continue
        data += link.read(waiting)
        while b"\xFD" in data:
            chunk, data = data.split(b"\xFD", 1)
            start = chunk.rfind(b"\xFE\xFE")
            if start >= 0 and len(chunk) >= start + 4 and chunk[start + 2] == CONTROLLER \
                    and chunk[start + 3] == RADIO and chunk[start + 4:].startswith(bytes(payload)):
                return chunk[start + 4 + len(payload):]
    return None
