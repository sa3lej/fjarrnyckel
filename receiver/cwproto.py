"""CWL2 - packet format of the remote CW keying link.

The box at the operator's end (the client) runs the keyer and sends every key
edge with the time it happened on the client's own microsecond clock. The
receiver at the radio (the server) replays each edge a fixed time later, so
marks and spaces go out with the lengths the keyer made, whatever the network
jitter is. The rules are modelled on OK1HRA's Paddle2IP; the format is ours.

All fields are big-endian. Every packet ends with a 16-byte tag: HMAC-SHA256
over all preceding bytes with the shared key, truncated to 16 bytes.

KEY packet, client to server (40-byte header, nev x 8-byte edges, tag):

  magic     4   "CWL2"
  type      1   1
  flags     1   bit 0 ACTIVE: an edge in the last 2 s
  wpm       1   keyer speed (0 = unknown, e.g. straight key)
  nev       1   edges in this packet, at most 8; 0 = idle
  session   4   random per client start, never 0
  seq       4   packet counter, strictly increasing within a session
  t_send    8   client clock, microseconds, when the packet was built
  nonce     8   last nonce received in a STATUS (0 = none yet)
  ev_idx0   4   index of the first edge in this packet
  delay_ms  2   requested playout delay, 0 = server default
  reserved  2   (box firmware 0.13.0 put a command here; see KEYCMD_SELECT_CW)
  edges     8n  (t_us << 1) | down   - time of the edge, new key state

  Client rules: a packet at once on every edge, sent again 3 ms and 8 ms later
  so one lost packet costs no time; a heartbeat every 20 ms while ACTIVE and
  every 100 ms when idle; every packet repeats the edges of the
  last 500 ms (at most 8) and always the newest edge while the key is down.

STATUS packet, server to client (48 bytes, tag):

  magic     4   "CWL2"
  type      1   2
  flags     1   see ST_* below
  mode      1   rig mode byte (CI-V), 0 = unknown
  version   1   receiver version (RECEIVER_VERSION); 0 = a receiver from before it had one
  session   4   session the server is running
  seq       4   status counter
  echo_t    8   t_send of the last accepted KEY packet
  echo_age  4   microseconds since that packet arrived
  nonce     8   current nonce
  freq_hz   4   rig frequency, 0 = unknown
  delay_ms  2   playout delay in use
  late      2   edges that arrived too late
  lost      2   edges never received
  rx_pkts   2   accepted KEY packets

  The client measures the round trip as now - echo_t - echo_age.

COMMAND packet, client to server (36 bytes, tag): something for the radio that is not keying

  magic     4   "CWL2"
  type      1   3
  command   1   CMD_SET_MODE, CMD_SET_FREQ or CMD_TUNE
  cmd_seq   2   raised by the client for every new command
  session   4   the client's session; the server must be running it
  t_send    8   client clock, microseconds
  nonce     8   last nonce received in a STATUS
  arg       4   the mode as its CI-V byte, or the frequency in Hz
  reserved  4

INFO packet, server to client (28 bytes, tag): what the radio is and can do, every two seconds

  magic     4   "CWL2"
  type      1   4
  caps      1   CAP_CONTROL: mode and frequency can be set from the box. CAP_TUNER: it has an antenna tuner to start
  bands     2   one bit per band the radio covers, in the order of BANDS
  session   4   session the server is running
  model     16  the radio's model name, e.g. "IC-7300", padded with zero bytes; empty when not known

  Keying never waits for a command and never shares a packet or a counter with one: KEY packets are as they
  always were, and the server carries a command out away from its keying loop. The client sends a command at
  once and again every 100 ms for a second, so a lost packet does not lose it; the server carries out a
  cmd_seq once, and only one that is newer than the last, so a recorded command cannot be played again.
"""
import collections
import hashlib
import hmac
import struct

MAGIC = b"CWL2"
TYPE_KEY = 1
TYPE_STATUS = 2
TAG_LEN = 16
MAX_EDGES = 8

# Raised with every receiver change the box should be able to tell apart. The box shows it, and says so when the
# receiver is older than the one its firmware was made for (CWL2_RECEIVER_LATEST in firmware/src/cwl2.h).
RECEIVER_VERSION = 4     # 2: CW on request (in the KEY packet, box 0.13.0). 3: COMMAND packets. 4: INFO packets

TYPE_COMMAND = 3
CMD_SET_MODE = 1         # arg: the mode as its CI-V byte, one of MODES_SETTABLE
CMD_SET_FREQ = 2         # arg: the frequency in Hz
CMD_TUNE = 3             # start the antenna tuner; the radio transmits while it tunes
MODES_SETTABLE = {0x00: "LSB", 0x01: "USB", 0x03: "CW"}
KEYCMD_SELECT_CW = 1     # box firmware 0.13.0 only: CW asked for in the KEY packet's spare bytes

FLAG_ACTIVE = 0x01

ST_RIG_VALID = 0x01     # mode and frequency are valid
ST_CW_MODE = 0x02       # keying is allowed
ST_KEY_DOWN = 0x08      # the key line is closed now
ST_NONCE_BAD = 0x20     # the last KEY packet carried a stale nonce
ST_MUTED = 0x40         # the server is waiting for a word gap

KEY_HEAD = struct.Struct("!4sBBBBIIQQIHBB")
EDGE = struct.Struct("!Q")
STATUS_BODY = struct.Struct("!4sBBBBIIQIQIHHHH")
COMMAND_BODY = struct.Struct("!4sBBHIQQII")

KeyPacket = collections.namedtuple("KeyPacket", "flags wpm session seq t_send nonce ev_idx0 delay_ms edges command command_seq",
                                   defaults=(0, 0))
Status = collections.namedtuple(
    "Status", "flags mode session seq echo_t echo_age nonce freq_hz delay_ms late lost rx_pkts version", defaults=(0,))


def tag(key, data):
    return hmac.new(key, data, hashlib.sha256).digest()[:TAG_LEN]


def load_key(path):
    with open(path, "rb") as handle:
        key = handle.read().strip()
    if len(key) < 16:
        raise SystemExit(f"key file {path} is too short")
    return key


def pack_key(key, packet):
    """edges: list of (time_us, down)."""
    body = KEY_HEAD.pack(MAGIC, TYPE_KEY, packet.flags, packet.wpm, len(packet.edges), packet.session,
                         packet.seq & 0xFFFFFFFF, packet.t_send, packet.nonce, packet.ev_idx0 & 0xFFFFFFFF,
                         packet.delay_ms, packet.command & 0xFF, packet.command_seq & 0xFF)
    body += b"".join(EDGE.pack((when << 1) | (1 if down else 0)) for when, down in packet.edges)
    return body + tag(key, body)


def unpack_key(key, data):
    if len(data) < KEY_HEAD.size + TAG_LEN:
        return None
    body, received = data[:-TAG_LEN], data[-TAG_LEN:]
    magic, kind, flags, wpm, nev, session, seq, t_send, nonce, ev_idx0, delay_ms, command, command_seq = KEY_HEAD.unpack_from(body)
    if magic != MAGIC or kind != TYPE_KEY or nev > MAX_EDGES or len(body) != KEY_HEAD.size + nev * EDGE.size:
        return None
    if session == 0 or not hmac.compare_digest(tag(key, body), received):
        return None
    edges = []
    for index in range(nev):
        (value,) = EDGE.unpack_from(body, KEY_HEAD.size + index * EDGE.size)
        edges.append((value >> 1, bool(value & 1)))
    return KeyPacket(flags, wpm, session, seq, t_send, nonce, ev_idx0, delay_ms, edges, command, command_seq)


def pack_status(key, status):
    body = STATUS_BODY.pack(MAGIC, TYPE_STATUS, status.flags, status.mode, status.version & 0xFF, status.session,
                            status.seq & 0xFFFFFFFF, status.echo_t, status.echo_age & 0xFFFFFFFF, status.nonce,
                            status.freq_hz, status.delay_ms & 0xFFFF, status.late & 0xFFFF, status.lost & 0xFFFF,
                            status.rx_pkts & 0xFFFF)
    return body + tag(key, body)


def unpack_status(key, data):
    if len(data) != STATUS_BODY.size + TAG_LEN:
        return None
    body, received = data[:-TAG_LEN], data[-TAG_LEN:]
    if not hmac.compare_digest(tag(key, body), received):
        return None
    magic, kind, flags, mode, version, session, seq, echo_t, echo_age, nonce, freq_hz, delay_ms, late, lost, rx_pkts = \
        STATUS_BODY.unpack(body)
    if magic != MAGIC or kind != TYPE_STATUS:
        return None
    return Status(flags, mode, session, seq, echo_t, echo_age, nonce, freq_hz, delay_ms, late, lost, rx_pkts, version)


TYPE_INFO = 4
CAP_CONTROL = 0x01       # mode and frequency can be set from the box
CAP_TUNER = 0x02         # the radio has an antenna tuner that CMD_TUNE starts
BANDS = ("160", "80", "40", "30", "20", "17", "15", "12", "10", "6", "2", "70cm", "23cm")   # bit 0 first
BANDS_HF6 = 0x03FF       # 160 m to 6 m
BAND_2M, BAND_70CM, BAND_23CM = 0x0400, 0x0800, 0x1000
INFO_BODY = struct.Struct("!4sBBHI16s")
Info = collections.namedtuple("Info", "caps bands session model")


def pack_info(key, info):
    body = INFO_BODY.pack(MAGIC, TYPE_INFO, info.caps & 0xFF, info.bands & 0xFFFF, info.session,
                          info.model.encode("ascii", "replace")[:16])
    return body + tag(key, body)


def unpack_info(key, data):
    if len(data) != INFO_BODY.size + TAG_LEN:
        return None
    body, received = data[:-TAG_LEN], data[-TAG_LEN:]
    if not hmac.compare_digest(tag(key, body), received):
        return None
    magic, kind, caps, bands, session, model = INFO_BODY.unpack(body)
    if magic != MAGIC or kind != TYPE_INFO:
        return None
    return Info(caps, bands, session, model.split(b"\0")[0].decode("ascii", "replace"))


Command = collections.namedtuple("Command", "command command_seq session t_send nonce arg")


def pack_command(key, command):
    body = COMMAND_BODY.pack(MAGIC, TYPE_COMMAND, command.command & 0xFF, command.command_seq & 0xFFFF, command.session,
                             command.t_send, command.nonce, command.arg & 0xFFFFFFFF, 0)
    return body + tag(key, body)


def unpack_command(key, data):
    if len(data) != COMMAND_BODY.size + TAG_LEN:
        return None
    body, received = data[:-TAG_LEN], data[-TAG_LEN:]
    if not hmac.compare_digest(tag(key, body), received):
        return None
    magic, kind, command, command_seq, session, t_send, nonce, arg, _ = COMMAND_BODY.unpack(body)
    if magic != MAGIC or kind != TYPE_COMMAND or session == 0:
        return None
    return Command(command, command_seq, session, t_send, nonce, arg)


def signed16(value):
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def signed32(value):
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def dit_us(wpm):
    return 1_200_000 // wpm if wpm else 60_000       # unknown speed: assume 20 WPM
