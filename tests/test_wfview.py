"""WfviewKey: reads the rig over rigctld and never keys without wfview's port."""
import os
import socket
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "receiver"))
import cwrecv  # noqa: E402


class FakeRigctld:
    def __init__(self, mode="CW", freq="7023000"):
        self.mode, self.freq = mode, freq
        self.server = socket.create_server(("127.0.0.1", 0))
        self.port = self.server.getsockname()[1]
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                conn, _ = self.server.accept()
            except OSError:
                return
            with conn:
                buffer = b""
                while True:
                    data = conn.recv(64)
                    if not data:
                        break
                    buffer += data
                    while b"\n" in buffer:
                        line, buffer = buffer.split(b"\n", 1)
                        if line == b"m":
                            conn.sendall(f"{self.mode}\n500\n".encode())
                        elif line == b"f":
                            conn.sendall(f"{self.freq}\n".encode())

    def close(self):
        self.server.close()


class WfviewKeyTests(unittest.TestCase):
    def test_reads_rig_but_refuses_to_key_without_a_holder(self):
        rig = FakeRigctld("CW", "7023000")
        key = cwrecv.WfviewKey("/dev/null-not-a-radio", ("127.0.0.1", rig.port), process="no-such-process")
        try:
            deadline = time.monotonic() + 3
            while key.freq == 0 and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertEqual(key.mode_and_freq(), (0x03, 7023000))
            self.assertIsNone(key.fd)
            self.assertFalse(key.cw_mode(), "no radio descriptor: must not report ready to key")
            key.set(True)  # must be a harmless no-op
        finally:
            key.close()
            rig.close()

    def test_maps_rigctld_modes_to_civ_bytes(self):
        self.assertEqual(cwrecv.WfviewKey.RIG_MODES["CWR"], 0x07)
        self.assertEqual(cwrecv.WfviewKey.RIG_MODES["PKTUSB"], 0x01)


if __name__ == "__main__":
    unittest.main()


class UsbKeyingFollowsModeTests(unittest.TestCase):
    """Selecting CW in wfview turns the radio's USB keying on; leaving CW or
    stopping turns it off. Frames are checked byte for byte."""

    def make(self):
        key = cwrecv.WfviewKey.__new__(cwrecv.WfviewKey)
        key.radio, key.controller = 0x94, 0xE0
        key.usb_keying = None
        key.mode, key.seen = 0, time.monotonic()
        key.dtr = []
        key._dtr = lambda fd, on: key.dtr.append(on)
        read, write = os.pipe()
        key.fd, key.pid, self.read = write, None, read
        return key

    def frames(self):
        os.set_blocking(self.read, False)
        try:
            return os.read(self.read, 4096)
        except BlockingIOError:
            return b""

    def test_cw_enables_dtr_keying_after_forcing_dtr_low(self):
        key = self.make()
        key.mode = 0x01
        key._follow_mode()
        self.assertEqual(self.frames(), b"", "not in CW: menu left untouched")
        key.mode = 0x03
        key._follow_mode()
        self.assertEqual(self.frames(), bytes.fromhex("FEFE94E01A05007901FD"))
        self.assertEqual(key.dtr, [False])
        self.assertTrue(key.cw_mode())
        key._follow_mode()
        self.assertEqual(self.frames(), b"", "no repeat while still in CW")
        key.mode = 0x01
        key._follow_mode()
        self.assertEqual(self.frames(), bytes.fromhex("FEFE94E01A05007900FD"))
        self.assertFalse(key.cw_mode())

    def test_release_turns_usb_keying_off(self):
        key = self.make()
        key.mode = 0x07
        key._follow_mode()
        self.frames()
        key._release()
        self.assertEqual(self.frames(), bytes.fromhex("FEFE94E01A05007900FD"))
        self.assertIsNone(key.fd)
        os.close(self.read)
