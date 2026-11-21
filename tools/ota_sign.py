"""Signs Nesto's program for firmware updates from Home Assistant.

    python3 tools/ota_sign.py keygen PUBLIC_FILE
    python3 tools/ota_sign.py sign PROGRAM VERSION [--model MODEL] [--key FILE] [--out FILE]
    python3 tools/ota_sign.py verify PROGRAM VERSION SIGNATURE_FILE PUBLIC_FILE [--model MODEL]

The panel installs a program only when one of the keys compiled into it
signed it (PROTOCOL.md section 19). The signature, Ed25519, covers the text
ota_signed_text() builds below, which src/protocol/Ota.cpp builds the same way.

keygen writes the public key, in hex, to PUBLIC_FILE and prints the private
key, in hex, to the terminal, once: keep it somewhere safe, such as a password
manager. sign reads the private key from --key FILE, or from the NESTO_OTA_KEY
environment variable (as CI does), and writes the signature, in base64, to
PROGRAM.sig or --out.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
import sys
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

MODEL = "nest-gen2"


def ota_signed_text(model: str, version: str, size: int, sha512: bytes) -> bytes:
    """What a release's signature covers."""
    return (
        f"nesto-ota-v1\nmodel={model}\nversion={version}\nsize={size}\nsha512={sha512.hex()}\n"
    ).encode()


def signed_text_for(program: Path, version: str, model: str) -> bytes:
    data = program.read_bytes()
    return ota_signed_text(model, version, len(data), hashlib.sha512(data).digest())


def public_hex(key: Ed25519PrivateKey) -> str:
    return key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex()


def load_private(path: str | None) -> Ed25519PrivateKey:
    text = Path(path).read_text() if path else os.environ.get("NESTO_OTA_KEY", "")
    if not text.strip():
        sys.exit("no private key: give --key FILE, or set NESTO_OTA_KEY")
    return Ed25519PrivateKey.from_private_bytes(bytes.fromhex(text.strip()))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    keygen = commands.add_parser("keygen", help="make a key pair")
    keygen.add_argument("public", type=Path)
    sign = commands.add_parser("sign", help="sign a program")
    sign.add_argument("program", type=Path)
    sign.add_argument("version")
    sign.add_argument("--model", default=MODEL)
    sign.add_argument("--key", help="the private key's file (else NESTO_OTA_KEY)")
    sign.add_argument("--out", type=Path)
    verify = commands.add_parser("verify", help="check a signature")
    verify.add_argument("program", type=Path)
    verify.add_argument("version")
    verify.add_argument("signature", type=Path)
    verify.add_argument("public", type=Path)
    verify.add_argument("--model", default=MODEL)
    args = parser.parse_args()

    if args.command == "keygen":
        key = Ed25519PrivateKey.generate()
        args.public.parent.mkdir(parents=True, exist_ok=True)
        args.public.write_text(public_hex(key) + "\n")
        private = key.private_bytes_raw().hex()
        print(f"Public key written to {args.public}.")
        print("Private key, shown once; keep it safe and never commit it:")
        print(private)
    elif args.command == "sign":
        key = load_private(args.key)
        signature = key.sign(signed_text_for(args.program, args.version, args.model))
        out = args.out or args.program.with_name(args.program.name + ".sig")
        out.write_text(base64.b64encode(signature).decode() + "\n")
        print(f"{out}: Nesto {args.version} for {args.model}, key {public_hex(key)[:16]}…")
    else:
        public = Ed25519PublicKey.from_public_bytes(bytes.fromhex(args.public.read_text().strip()))
        signature = base64.b64decode(args.signature.read_text().strip())
        try:
            public.verify(signature, signed_text_for(args.program, args.version, args.model))
        except InvalidSignature:
            sys.exit("the signature doesn't match")
        print("the signature matches")


if __name__ == "__main__":
    main()
