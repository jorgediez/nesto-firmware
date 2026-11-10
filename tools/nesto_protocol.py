"""Reference implementation of the Nesto Panel protocol's cryptography.

This is the executable form of PROTOCOL.md sections 3, 6 and 7. It is what the
test vectors in tests/vectors/ are generated from, so the panel's C++ code and
Home Assistant's Python code are both checked against the same values.

Needs the `cryptography` package, which Home Assistant already depends on.
"""

from __future__ import annotations

import hashlib
import hmac
import struct

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey,
    X25519PublicKey,
)
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

MAX_FRAME = 512 * 1024
TAG_SIZE = 16

ROLE_HA = "ha"
ROLE_PANEL = "panel"


class ProtocolError(Exception):
    """A frame or handshake value is invalid."""


def hkdf_sha512(ikm: bytes, salt: bytes, info: bytes, length: int = 32) -> bytes:
    return HKDF(hashes.SHA512(), length, salt, info).derive(ikm)


def hmac_sha512(key: bytes, message: bytes) -> bytes:
    return hmac.new(key, message, hashlib.sha512).digest()


# Pairing, section 6.2 -------------------------------------------------------


def public_key(private: bytes) -> bytes:
    return X25519PrivateKey.from_private_bytes(private).public_key().public_bytes_raw()


def shared_secret(private: bytes, peer_public: bytes) -> bytes:
    """X25519. Rejects an all-zero result, which a malicious peer can force."""
    secret = X25519PrivateKey.from_private_bytes(private).exchange(
        X25519PublicKey.from_public_bytes(peer_public)
    )
    if secret == bytes(32):
        raise ProtocolError("X25519 produced an all-zero shared secret")
    return secret


def pairing_code(ha_pub: bytes, panel_pub: bytes, shared: bytes) -> str:
    """The six digits the panel shows and the user types into Home Assistant."""
    digest = hashlib.sha512(b"nesto-sas-v1" + ha_pub + panel_pub + shared).digest()
    return f"{int.from_bytes(digest[:8], 'big') % 1_000_000:06d}"


def pairing_key(shared: bytes, ha_pub: bytes, panel_pub: bytes) -> bytes:
    return hkdf_sha512(shared, ha_pub + panel_pub, b"nesto-pair-key-v1")


def confirm_mac(key: bytes, role: str) -> bytes:
    return hmac_sha512(key, f"nesto-confirm-{role}-v1".encode())


# Authentication and session keys, sections 7.1 and 7.2 ---------------------


def auth_mac(key: bytes, role: str, panel_nonce: bytes, ha_nonce: bytes, panel_id: str) -> bytes:
    """HA's MAC covers the panel's nonce first; the panel's MAC, HA's nonce first."""
    first, second = (panel_nonce, ha_nonce) if role == ROLE_HA else (ha_nonce, panel_nonce)
    label = f"nesto-auth-{role}-v1".encode()
    return hmac_sha512(key, label + first + second + panel_id.encode("ascii"))


def session_keys(key: bytes, panel_nonce: bytes, ha_nonce: bytes) -> tuple[bytes, bytes]:
    """Returns (HA-to-panel key, panel-to-HA key)."""
    salt = panel_nonce + ha_nonce
    return (
        hkdf_sha512(key, salt, b"nesto-session-h2p-v1"),
        hkdf_sha512(key, salt, b"nesto-session-p2h-v1"),
    )


# Frames, sections 3 and 7.3 ------------------------------------------------


def frame(payload: bytes) -> bytes:
    """A plaintext frame: 4-byte big-endian length, then the payload."""
    if not 0 < len(payload) <= MAX_FRAME:
        raise ProtocolError(f"payload of {len(payload)} bytes")
    return struct.pack(">I", len(payload)) + payload


def frame_nonce(counter: int) -> bytes:
    return bytes(4) + struct.pack("<Q", counter)


def seal(key: bytes, counter: int, plaintext: bytes) -> bytes:
    """An encrypted frame: length, then ciphertext and tag. The length is the AAD."""
    length = struct.pack(">I", len(plaintext) + TAG_SIZE)
    if len(plaintext) + TAG_SIZE > MAX_FRAME:
        raise ProtocolError(f"payload of {len(plaintext)} bytes")
    return length + ChaCha20Poly1305(key).encrypt(frame_nonce(counter), plaintext, length)


def open_frame(key: bytes, counter: int, data: bytes) -> bytes:
    """Decrypts a whole encrypted frame (length included)."""
    (length,) = struct.unpack(">I", data[:4])
    if length != len(data) - 4 or length < TAG_SIZE:
        raise ProtocolError("bad frame length")
    return ChaCha20Poly1305(key).decrypt(frame_nonce(counter), data[4:], data[:4])
