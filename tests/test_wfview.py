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
    def __init__(self, mode="CW", freq="7023000", model="IC-7300", number=148):
        self.mode, self.freq, self.model, self.number = mode, freq, model, number
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
                        elif line == b"\\dump_caps":
                            conn.sendall(f"Caps dump for model: {self.number}\nModel Name:\t{self.model}\nMfg Name:\tIcom\nRPRT 0\n".encode())

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
        key.keying = (b"\x00\x79", 0x01)
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


class RadioIdentificationTests(unittest.TestCase):
    """The radio comes from wfview's dump_caps; the menu only for radios in Icom's guides."""

    def make(self, port=None):
        key = cwrecv.WfviewKey.__new__(cwrecv.WfviewKey)
        key.radio_given, key.radio, key.controller = None, 0x94, 0xE0
        key.usb_port_given, key.device = port, "/dev/ttyACM-test"
        key.model, key.keying, key.usb_keying = None, None, None
        key.mode, key.seen = 0x03, time.monotonic()
        key._dtr = lambda fd, on: None
        read, write = os.pipe()
        key.fd, key.pid, self.read = write, None, read
        return key

    def frames(self):
        os.set_blocking(self.read, False)
        try:
            return os.read(self.read, 4096)
        except BlockingIOError:
            return b""

    def caps(self, model, number):
        return f"Caps dump for model: {number}\nModel Name:\t{model}\nRPRT 0\n"

    def test_ic7300_keeps_its_menu_and_address(self):
        key = self.make()
        key._identify(self.caps("IC-7300", 148))
        key._follow_mode()
        self.assertEqual(self.frames(), bytes.fromhex("FEFE94E01A05007901FD"))

    def test_ic705_on_port_a_and_b(self):
        for port, value in (("A", "01"), ("B", "03")):
            key = self.make(port)
            key._identify(self.caps("IC-705", 164))
            key._follow_mode()
            self.assertEqual(self.frames(), bytes.fromhex(f"FEFEA4E01A050126{value}FD"))
            key.mode = 0x01
            key._follow_mode()
            self.assertEqual(self.frames(), bytes.fromhex("FEFEA4E01A05012600FD"))

    def test_menus_from_icoms_guides(self):
        for model, number, menu in (("IC-7300MK2", 182, "0097"), ("IC-9700", 162, "0121"), ("IC-7610", 152, "0096"), ("IC-905", 172, "0137")):
            key = self.make("A")
            key._identify(self.caps(model, number))
            key._follow_mode()
            self.assertEqual(self.frames(), bytes.fromhex(f"FEFE{number:02X}E01A05{menu}01FD"), model)

    def test_unknown_radio_menu_left_alone_but_keys_in_cw(self):
        key = self.make()
        key._identify(self.caps("IC-7100", 136))
        key._follow_mode()
        self.assertEqual(self.frames(), b"", "no menu writes for a radio we have no documented menu for")
        self.assertTrue(key.cw_mode(), "the operator sets the menu; CW mode is enough")
        key.mode = 0x01
        self.assertFalse(key.cw_mode())

    def test_two_port_radio_without_known_port_leaves_menu_alone(self):
        key = self.make()          # no --usb-port and no such tty in /sys
        key._identify(self.caps("IC-9700", 162))
        self.assertIsNone(key.keying)
        key._follow_mode()
        self.assertEqual(self.frames(), b"")

    def test_civ_address_override_wins(self):
        key = self.make()
        key.radio_given = key.radio = 0x70
        key._identify(self.caps("IC-7300", 148))
        key._follow_mode()
        self.assertEqual(self.frames(), bytes.fromhex("FEFE70E01A05007901FD"))
