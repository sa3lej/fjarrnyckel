#!/usr/bin/env python3
"""Open a Fjärrnyckel backup file (.fnb) on a computer and show what is in it.

    tools/backup.py show fjarrnyckel-backup.fnb            secrets masked
    tools/backup.py show fjarrnyckel-backup.fnb --secrets  secrets shown

The passphrase is asked for, or taken from FN_BACKUP_PASS. Needs the "cryptography" package.
The format is described in firmware/src/backup.h.
"""
import argparse, getpass, hashlib, os, struct, sys
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

MAGIC, HEAD, ROUNDS = b"FNBACKUP", 40, 100000

def key(passphrase, salt):
    return hashlib.pbkdf2_hmac("sha256", passphrase.encode(), salt, ROUNDS, 32)

def seal(passphrase, plain):
    rnd = os.urandom(28)
    head = MAGIC + bytes([1, 0, 0, 0]) + rnd
    return head + AESGCM(key(passphrase, rnd[:16])).encrypt(rnd[16:], plain, head)

def open_(passphrase, data):
    if len(data) < HEAD + 16 or data[:8] != MAGIC or data[8] != 1:
        raise SystemExit("not a Fjärrnyckel backup this tool knows")
    try:
        return AESGCM(key(passphrase, data[12:28])).decrypt(data[28:40], data[HEAD:], data[:HEAD])
    except Exception:
        raise SystemExit("wrong passphrase, or the file was changed")

# The contents: firmware version (16) | has Tailscale identity (1) | 0 (1) | settings length (2, little-endian)
# | Tailscale keys (6 x 32: machine, WireGuard and disco, each private then public) | the settings blob.
# settings_t from firmware/src/fn.h, packed as the ESP32-S3 compiler lays it out.
FIELDS = [("ssid", "33s", 0), ("pass", "65s", 1), ("key_type", "B", 0), ("swap", "B", 0), ("wpm", "B", 0),
          (None, "x", 0), ("tone_hz", "H", 0), ("volume", "B", 0), ("reserved", "B", 0), ("link_host", "64s", 0),
          ("link_port", "H", 0), ("link_key", "96s", 1), ("iambic_b", "B", 0), ("ts_on", "B", 0), ("ts_key", "128s", 1),
          ("page_lock", "B", 0), ("page_salt", "8s", 1), ("page_hash", "32s", 1), ("led_off", "B", 0), ("tones_off", "B", 0),
          ("key_target", "B", 0), ("usb_raw", "B", 0), ("hp_volume", "B", 0), ("ts_region", "H", 0),
          ("ts_region_auto", "H", 0), ("radio_volume", "B", 0), ("led_level", "B", 0), ("link_delay", "B", 0), (None, "x", 0)]

def show(plain, secrets):
    version = plain[:16].split(b"\0")[0].decode()
    has_ts, _, slen = struct.unpack_from("<BBH", plain, 16)
    blob = plain[20 + 192:20 + 192 + slen]
    print(f"made by firmware {version}, Tailscale identity {'included' if has_ts else 'not included'}")
    at = 0
    for name, fmt, secret in FIELDS:
        size = struct.calcsize("<" + fmt)
        if at + size > len(blob): break
        if name:
            (v,) = struct.unpack_from("<" + fmt, blob, at)
            if isinstance(v, bytes): v = v.split(b"\0")[0].decode(errors="replace") if fmt.endswith("s") and name not in ("page_salt", "page_hash") else v.hex()
            if secret and v and not secrets: v = "(set)"
            print(f"  {name}: {v}")
        at += size

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["show"]); ap.add_argument("file"); ap.add_argument("--secrets", action="store_true")
    a = ap.parse_args()
    pw = os.environ.get("FN_BACKUP_PASS") or getpass.getpass("Passphrase: ")
    show(open_(pw, open(a.file, "rb").read()), a.secrets)
