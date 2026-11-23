#!/usr/bin/env python3
"""Signs a Fjärrnyckel firmware file, or checks a signed one.

A signed file is the firmware image followed by 72 bytes: the text FNSIG001 and
an ECDSA P-256 signature (r and s, 32 bytes each) over the SHA-256 of the image.
The box holds the public key and only installs files that carry a valid
signature, unless its owner has put it in service mode.

  sign_firmware.py sign firmware.bin signed.bin     key from $FN_SIGNING_KEY (PEM text) or --key FILE
  sign_firmware.py check signed.bin --pub firmware/src/signing_key.h
"""
import argparse, os, re, sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature

MAGIC = b"FNSIG001"

def main():
    p = argparse.ArgumentParser()
    p.add_argument("action", choices=["sign", "check"])
    p.add_argument("file")
    p.add_argument("out", nargs="?")
    p.add_argument("--key")
    p.add_argument("--pub")
    a = p.parse_args()
    data = open(a.file, "rb").read()
    if a.action == "sign":
        pem = open(a.key, "rb").read() if a.key else os.environ["FN_SIGNING_KEY"].encode()
        key = serialization.load_pem_private_key(pem, None)
        r, s = decode_dss_signature(key.sign(data, ec.ECDSA(hashes.SHA256())))
        open(a.out, "wb").write(data + MAGIC + r.to_bytes(32, "big") + s.to_bytes(32, "big"))
        print(f"signed {a.out}: {len(data)} bytes of firmware + 72")
    else:
        point = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", open(a.pub).read()))
        pub = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), point)
        image, trailer = data[:-72], data[-72:]
        if trailer[:8] != MAGIC: sys.exit("no signature in this file")
        sig = encode_dss_signature(int.from_bytes(trailer[8:40], "big"), int.from_bytes(trailer[40:], "big"))
        pub.verify(sig, image, ec.ECDSA(hashes.SHA256()))
        print("the signature is valid")

if __name__ == "__main__":
    main()
