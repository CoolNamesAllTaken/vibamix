"""Minimal Bluetooth-Mesh proxy *client* (TX only).

Builds encrypted mesh network PDUs for the badge fleet's vendor model and frames
them as GATT Proxy PDUs to write to a proxy node's Mesh Proxy Data-In characteristic.
Implements just the transmit path of the Mesh Profile crypto (k2/k4 derivation,
application + network encryption, lower-transport segmentation, proxy SAR).

We never receive/decrypt — the vendor model commands are fire-and-forget.
"""

from __future__ import annotations

from dataclasses import dataclass

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.cmac import CMAC

# --- AES primitives ----------------------------------------------------------


def aes_cmac(key: bytes, msg: bytes) -> bytes:
    c = CMAC(algorithms.AES(key))
    c.update(msg)
    return c.finalize()


def aes_ecb(key: bytes, block: bytes) -> bytes:
    enc = Cipher(algorithms.AES(key), modes.ECB()).encryptor()  # noqa: S305 (ECB used per Mesh spec for obfuscation)
    return enc.update(block) + enc.finalize()


def aes_ccm(key: bytes, nonce: bytes, plaintext: bytes, mic_len: int) -> bytes:
    """Returns ciphertext || MIC (mic_len bytes)."""
    return AESCCM(key, tag_length=mic_len).encrypt(nonce, plaintext, None)


# --- Mesh key derivation (Mesh Profile §3.8.2) -------------------------------


def s1(m: bytes) -> bytes:
    return aes_cmac(b"\x00" * 16, m)


def k2(n: bytes, p: bytes) -> tuple[int, bytes, bytes]:
    """Returns (NID, EncryptionKey, PrivacyKey)."""
    salt = s1(b"smk2")
    t = aes_cmac(salt, n)
    t1 = aes_cmac(t, p + b"\x01")
    t2 = aes_cmac(t, t1 + p + b"\x02")
    t3 = aes_cmac(t, t2 + p + b"\x03")
    return t1[15] & 0x7F, t2, t3


def k4(n: bytes) -> int:
    """Returns AID (6 bits)."""
    salt = s1(b"smk4")
    t = aes_cmac(salt, n)
    return aes_cmac(t, b"id6" + b"\x01")[15] & 0x3F


def _xor(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))


@dataclass
class MeshCrypto:
    net_key: bytes
    app_key: bytes
    iv_index: int = 0

    def __post_init__(self) -> None:
        self.nid, self.enc_key, self.priv_key = k2(self.net_key, b"\x00")
        self.aid = k4(self.app_key)
        self.ivi = self.iv_index & 1

    # -- transport (application) layer --
    def _app_encrypt(self, access: bytes, src: int, dst: int, seq_auth: int) -> bytes:
        nonce = (
            bytes([0x01, 0x00])
            + seq_auth.to_bytes(3, "big")
            + src.to_bytes(2, "big")
            + dst.to_bytes(2, "big")
            + self.iv_index.to_bytes(4, "big")
        )
        return aes_ccm(self.app_key, nonce, access, 4)  # upper transport PDU + 4B TransMIC

    def _lower_transport_pdus(self, upper: bytes, seq0: int) -> list[bytes]:
        """One unsegmented PDU, or N segmented PDUs (12-byte segments)."""
        akf = 1
        if len(upper) <= 15:
            return [bytes([(akf << 6) | self.aid]) + upper]

        seqzero = seq0 & 0x1FFF
        segs = [upper[i : i + 12] for i in range(0, len(upper), 12)]
        seg_n = len(segs) - 1
        out = []
        for seg_o, seg in enumerate(segs):
            hdr0 = 0x80 | (akf << 6) | self.aid          # SEG=1
            val = (0 << 23) | (seqzero << 10) | (seg_o << 5) | seg_n  # SZMIC=0
            out.append(bytes([hdr0]) + val.to_bytes(3, "big") + seg)
        return out

    # -- network layer --
    def _network_pdu(self, lower: bytes, src: int, dst: int, seq: int, ttl: int) -> bytes:
        ctl = 0
        ctl_ttl = (ctl << 7) | (ttl & 0x7F)
        nonce = (
            bytes([0x00, ctl_ttl])
            + seq.to_bytes(3, "big")
            + src.to_bytes(2, "big")
            + b"\x00\x00"
            + self.iv_index.to_bytes(4, "big")
        )
        enc = aes_ccm(self.enc_key, nonce, dst.to_bytes(2, "big") + lower, 4)
        # Obfuscate CTL/TTL || SEQ || SRC.
        pecb_in = b"\x00\x00\x00\x00\x00" + self.iv_index.to_bytes(4, "big") + enc[0:7]
        pecb = aes_ecb(self.priv_key, pecb_in)[0:6]
        header = bytes([ctl_ttl]) + seq.to_bytes(3, "big") + src.to_bytes(2, "big")
        obf = _xor(header, pecb)
        return bytes([(self.ivi << 7) | self.nid]) + obf + enc

    def network_pdus(
        self, src: int, dst: int, seq0: int, access: bytes, ttl: int = 7
    ) -> tuple[list[bytes], int]:
        """Build network PDU(s) for one access message. Returns (pdus, seqs_used).

        Each segment consumes one sequence number, all starting at seq0.
        """
        upper = self._app_encrypt(access, src, dst, seq0)
        lowers = self._lower_transport_pdus(upper, seq0)
        pdus = [
            self._network_pdu(lower, src, dst, seq0 + i, ttl)
            for i, lower in enumerate(lowers)
        ]
        return pdus, len(lowers)


# --- GATT Proxy framing (Mesh Profile §6.3) ----------------------------------

PROXY_TYPE_NETWORK = 0x00


def proxy_pdus(network_pdu: bytes, mtu: int) -> list[bytes]:
    """Frame a network PDU into one or more proxy PDUs for the Data-In write.

    SAR: 0=complete, 1=first, 2=continuation, 3=last (top 2 bits of byte 0).
    `mtu` is the ATT MTU; a write carries at most mtu-3 bytes, of which 1 is the
    proxy header — so each segment is at most mtu-4 network bytes.
    """
    max_seg = max(1, mtu - 4)
    if len(network_pdu) <= max_seg:
        return [bytes([(0 << 6) | PROXY_TYPE_NETWORK]) + network_pdu]

    out = []
    segs = [network_pdu[i : i + max_seg] for i in range(0, len(network_pdu), max_seg)]
    for i, seg in enumerate(segs):
        sar = 1 if i == 0 else (3 if i == len(segs) - 1 else 2)
        out.append(bytes([(sar << 6) | PROXY_TYPE_NETWORK]) + seg)
    return out


# --- High-level session ------------------------------------------------------


class MeshSession:
    """Crypto + a monotonic sequence number; produces ready-to-write proxy PDUs."""

    def __init__(self, crypto: MeshCrypto, src: int, seq_store) -> None:
        self.crypto = crypto
        self.src = src
        self.seq_store = seq_store

    def build(self, dst: int, access: bytes, mtu: int, ttl: int = 7) -> list[bytes]:
        seq0 = self.seq_store.peek()
        pdus, used = self.crypto.network_pdus(self.src, dst, seq0, access, ttl)
        self.seq_store.advance(used)
        writes: list[bytes] = []
        for pdu in pdus:
            writes.extend(proxy_pdus(pdu, mtu))
        return writes


# --- Self-test against Mesh Profile §8 sample data ---------------------------


def selftest() -> None:
    """Validate the derivations against the Mesh Profile sample vectors."""
    net = bytes.fromhex("7dd7364cd842ad18c17c2b820c84c3d6")
    nid, enc, priv = k2(net, b"\x00")
    assert nid == 0x68, hex(nid)
    assert enc.hex() == "0953fa93e7caac9638f58820220a398e", enc.hex()
    assert priv.hex() == "8b84eedec100067d670971dd2aa700cf", priv.hex()

    app = bytes.fromhex("63964771734fbd76e3b40519d1d94a48")
    assert k4(app) == 0x26, hex(k4(app))
    print("mesh selftest OK (k2/k4 match Mesh Profile §8 vectors)")


if __name__ == "__main__":
    selftest()
