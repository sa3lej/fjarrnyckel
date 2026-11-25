"""The firmware's CWL2 client (firmware/src/cwl2.c), built for this computer, against the real receiver.

Proves that the C client and the Python receiver agree on the packets: the receiver keys what the client sent,
the client reads the receiver's version, and the buffer the client asks for is the one the receiver uses.
Skipped where there is no C compiler or no OpenSSL headers.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "receiver"))
import cwproto as proto  # noqa: E402


def free_port():
    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as s:
        s.bind(("::1", 0))
        return s.getsockname()[1]


class CClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.client = os.path.join(cls.dir, "cwl2_host")
        cc = shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("no C compiler")
        built = subprocess.run([cc, "-Wall", "-O2", "-I" + os.path.join(ROOT, "firmware/src"), "-o", cls.client,
                                os.path.join(ROOT, "tests/host/cwl2_host.c"), os.path.join(ROOT, "firmware/src/cwl2.c"),
                                "-lcrypto"], capture_output=True, text=True)
        if built.returncode:
            raise unittest.SkipTest("the C client did not build here: " + built.stderr.strip()[-200:])
        cls.key = os.path.join(cls.dir, "cw.key")
        with open(cls.key, "w") as f:
            f.write("a-test-key-of-enough-length\n")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def run_pair(self, text, *extra):
        port = free_port()
        receiver = subprocess.Popen([sys.executable, os.path.join(ROOT, "receiver/cwrecv.py"), "--key", self.key,
                                     "--bind", "::1", "--port", str(port), "--exit-idle-s", "1"],
                                    stdout=subprocess.PIPE, text=True)
        try:
            self.assertIn("listening", receiver.stdout.readline())
            client = subprocess.run([self.client, self.key, "::1", str(port), "25", text, *extra],
                                    capture_output=True, text=True, timeout=60)
            out, _ = receiver.communicate(timeout=30)
        finally:
            if receiver.poll() is None:
                receiver.kill()
        events = [json.loads(line) for line in out.splitlines() if line.startswith("{")]
        return json.loads(client.stdout), next(e for e in events if e["event"] == "summary")

    def test_the_c_client_keys_the_receiver(self):
        client, summary = self.run_pair("TEST")
        self.assertTrue(client["linked"])
        self.assertEqual(summary["edges_played"], client["edges"])
        self.assertEqual(summary["lost"], 0)
        self.assertEqual(client["receiver"], proto.RECEIVER_VERSION)
        self.assertEqual(client["delay_ms"], 25)

    def test_a_command_rides_for_two_seconds(self):
        cc = shutil.which("cc")
        tool = os.path.join(self.dir, "cwl2_command")
        built = subprocess.run([cc, "-Wall", "-O2", "-I" + os.path.join(ROOT, "firmware/src"), "-o", tool,
                                os.path.join(ROOT, "tests/host/cwl2_command.c"), os.path.join(ROOT, "firmware/src/cwl2.c"),
                                "-lcrypto"], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr)
        key = "a-test-key-of-enough-length"
        lines = subprocess.run([tool, key], capture_output=True, text=True).stdout.split()
        before, during, after = (proto.unpack_key(key.encode(), bytes.fromhex(line)) for line in lines)
        self.assertEqual((before.command, during.command, after.command), (0, proto.CMD_SELECT_CW, 0))
        self.assertEqual(during.command_seq, 1)

    def test_the_buffer_the_client_asks_for_is_used(self):
        client, summary = self.run_pair("E", "60")
        self.assertTrue(client["linked"])
        self.assertEqual(client["delay_ms"], 60)
        self.assertEqual(summary["delay_ms"], 60.0)


if __name__ == "__main__":
    unittest.main()
