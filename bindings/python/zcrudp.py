"""Python ctypes binding for libzcrudp (zero external dependencies)."""
from __future__ import annotations

import ctypes
import os
from pathlib import Path
from typing import List, Optional, Tuple


RUDP_OK = 0
RUDP_ERR_INVALID_ARG = -1
RUDP_ERR_DISCONNECTED = -2
RUDP_ERR_BUFFER_FULL = -3
RUDP_ERR_OUT_OF_WINDOW = -4
RUDP_ERR_AUTH = -5

RUDP_CHANNEL_FLAG_UNRELIABLE = 0
RUDP_CHANNEL_FLAG_RELIABLE = 1 << 0
RUDP_CHANNEL_FLAG_ORDERED = 1 << 1
RUDP_CHANNEL_FLAG_ENCRYPTED = 1 << 2
RUDP_CHANNEL_FLAG_FEC_K2 = 1 << 3

RUDP_RECORD_FLAG_UNRELIABLE = 0
RUDP_RECORD_FLAG_RELIABLE = 1 << 0
RUDP_RECORD_FLAG_ACK = 1 << 1
RUDP_RECORD_FLAG_FEC_PARITY = 1 << 2

RUDP_WIRE_DATAGRAM_HEADER_SIZE = 4
RUDP_WIRE_RECORD_SIZE = 8
RUDP_MAX_DATAGRAM_SIZE = 1404
RUDP_CRYPTO_KEY_BYTES = 32
RUDP_CRYPTO_OVERHEAD_BYTES = 24
RUDP_SESSION_ABI_BYTES = 5364
RUDP_CRYPTO_CTX_ABI_BYTES = 160  # Conservative aligned buffer covering rudp_crypto_ctx_s


class TfvPacket(ctypes.Union):
    _fields_ = [
        ("raw", ctypes.c_uint32),
        ("_bytes", ctypes.c_uint8 * 4),
    ]

    @classmethod
    def from_fields(cls, ptype: int, flags: int, value: int) -> "TfvPacket":
        pkt = cls()
        raw = ((ptype & 0xFF)) | ((flags & 0xFF) << 8) | ((value & 0xFFFF) << 16)
        # Keep struct bitfield equivalence by packing through library when available;
        # raw field stores host uint32 value.
        pkt.raw = raw
        return pkt


class DatagramHeader(ctypes.Structure):
    _fields_ = [
        ("ack", ctypes.c_uint16),
        ("ack_channel", ctypes.c_uint8),
        ("count", ctypes.c_uint8),
    ]


class Record(ctypes.Structure):
    _fields_ = [
        ("channel_id", ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
        ("seq_num", ctypes.c_uint16),
        ("payload", TfvPacket),
    ]


class Stats(ctypes.Structure):
    _fields_ = [
        ("rx_datagrams", ctypes.c_uint32),
        ("rx_malformed", ctypes.c_uint32),
        ("rx_ack_rejected", ctypes.c_uint32),
        ("rx_out_of_window", ctypes.c_uint32),
        ("rx_unreliable_jump_rejected", ctypes.c_uint32),
        ("rx_unreliable_resyncs", ctypes.c_uint32),
        ("rx_sack_blocks", ctypes.c_uint32),
        ("rx_fec_recoveries", ctypes.c_uint32),
    ]


def _default_lib_path() -> Optional[Path]:
    env = os.environ.get("ZCRUDP_LIB")
    if env:
        return Path(env)
    root = Path(__file__).resolve().parents[2]
    candidates = [
        root / "build" / "libzcrudp.so",
        root / "build" / "libzcrudp.dylib",
        root / "build" / "zcrudp.dll",
        root / "build" / "libzcrudp.dll",
    ]
    for path in candidates:
        if path.exists():
            return path
    return None


def load_library(lib_path: Optional[str | Path] = None) -> ctypes.CDLL:
    resolved = Path(lib_path) if lib_path is not None else _default_lib_path()
    if resolved is None or not resolved.exists():
        raise FileNotFoundError(
            "Unable to locate libzcrudp shared library; set ZCRUDP_LIB or build build/libzcrudp.so"
        )
    lib = ctypes.CDLL(str(resolved))

    c_vp = ctypes.c_void_p
    c_u8p = ctypes.POINTER(ctypes.c_uint8)
    c_cu8p = ctypes.POINTER(ctypes.c_uint8)

    lib.rudp_session_init.argtypes = [c_vp]
    lib.rudp_session_init.restype = ctypes.c_int

    lib.rudp_session_config_channel.argtypes = [c_vp, ctypes.c_uint8, ctypes.c_uint8]
    lib.rudp_session_config_channel.restype = ctypes.c_int

    lib.rudp_session_config_recovery.argtypes = [
        c_vp,
        ctypes.c_uint8,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_uint32,
    ]
    lib.rudp_session_config_recovery.restype = ctypes.c_int

    lib.rudp_session_send_reliable.argtypes = [
        c_vp,
        ctypes.c_uint8,
        TfvPacket,
        ctypes.c_uint32,
    ]
    lib.rudp_session_send_reliable.restype = ctypes.c_int

    lib.rudp_session_queue_unreliable.argtypes = [c_vp, ctypes.c_uint8, TfvPacket]
    lib.rudp_session_queue_unreliable.restype = ctypes.c_int

    lib.rudp_session_build_datagram.argtypes = [
        c_vp,
        ctypes.c_uint8,
        c_u8p,
        ctypes.c_size_t,
        ctypes.c_uint32,
        ctypes.c_uint32,
    ]
    lib.rudp_session_build_datagram.restype = ctypes.c_int

    lib.rudp_session_has_pending_work.argtypes = [
        c_vp,
        ctypes.c_uint32,
        ctypes.c_uint32,
    ]
    lib.rudp_session_has_pending_work.restype = ctypes.c_bool

    lib.rudp_session_next_wakeup_ms.argtypes = [
        c_vp,
        ctypes.c_uint32,
        ctypes.c_uint32,
    ]
    lib.rudp_session_next_wakeup_ms.restype = ctypes.c_uint32

    lib.rudp_session_process_datagram_at.argtypes = [
        c_vp,
        c_cu8p,
        ctypes.c_size_t,
        ctypes.POINTER(Record),
        ctypes.c_size_t,
        ctypes.c_uint32,
    ]
    lib.rudp_session_process_datagram_at.restype = ctypes.c_int

    lib.rudp_session_poll.argtypes = [c_vp, ctypes.POINTER(Record), ctypes.c_size_t]
    lib.rudp_session_poll.restype = ctypes.c_int

    lib.rudp_pack_payload.argtypes = [ctypes.POINTER(TfvPacket), c_u8p, ctypes.c_size_t]
    lib.rudp_pack_payload.restype = ctypes.c_int

    lib.rudp_unpack_payload.argtypes = [c_cu8p, ctypes.c_size_t, ctypes.POINTER(TfvPacket)]
    lib.rudp_unpack_payload.restype = ctypes.c_int

    lib.rudp_crypto_init.argtypes = [c_vp, c_cu8p, ctypes.c_uint64, ctypes.c_uint8]
    lib.rudp_crypto_init.restype = ctypes.c_int

    lib.rudp_crypto_seal.argtypes = [
        c_vp,
        c_cu8p,
        ctypes.c_size_t,
        c_cu8p,
        ctypes.c_size_t,
        c_u8p,
        ctypes.c_size_t,
    ]
    lib.rudp_crypto_seal.restype = ctypes.c_int

    lib.rudp_crypto_open.argtypes = [
        c_vp,
        c_cu8p,
        ctypes.c_size_t,
        c_cu8p,
        ctypes.c_size_t,
        c_u8p,
        ctypes.c_size_t,
    ]
    lib.rudp_crypto_open.restype = ctypes.c_int

    lib.rudp_crypto_wipe.argtypes = [c_vp]
    lib.rudp_crypto_wipe.restype = None

    return lib


class ZcrudpError(RuntimeError):
    def __init__(self, code: int, action: str) -> None:
        super().__init__(f"{action} failed with zcrudp code {code}")
        self.code = code


def _check(code: int, action: str) -> int:
    if code < 0:
        raise ZcrudpError(code, action)
    return code


def pack_payload(lib: ctypes.CDLL, ptype: int, flags: int, value: int) -> bytes:
    """Packs canonical TFV bytes `[type:1][flags:1][value_be:2]` into an aligned TfvPacket."""
    wire = (ctypes.c_uint8 * 4)(
        ptype & 0xFF, flags & 0xFF, (value >> 8) & 0xFF, value & 0xFF
    )
    return bytes(wire)


def make_packet(lib: ctypes.CDLL, ptype: int, flags: int, value: int) -> TfvPacket:
    wire = (ctypes.c_uint8 * 4)(
        ptype & 0xFF, flags & 0xFF, (value >> 8) & 0xFF, value & 0xFF
    )
    pkt = TfvPacket()
    _check(lib.rudp_unpack_payload(wire, 4, ctypes.byref(pkt)), "rudp_unpack_payload")
    return pkt


def decode_packet(lib: ctypes.CDLL, pkt: TfvPacket) -> Tuple[int, int, int]:
    wire = (ctypes.c_uint8 * 4)()
    _check(lib.rudp_pack_payload(ctypes.byref(pkt), wire, 4), "rudp_pack_payload")
    return wire[0], wire[1], (wire[2] << 8) | wire[3]


class Session:
    def __init__(self, lib: Optional[ctypes.CDLL] = None) -> None:
        self.lib = lib or load_library()
        # Allocate generous 8-byte aligned backing storage (>= 5364 bytes)
        self._buf = (ctypes.c_uint64 * 1024)()
        _check(self.lib.rudp_session_init(ctypes.byref(self._buf)), "rudp_session_init")

    def config_channel(self, channel_id: int, flags: int) -> None:
        _check(
            self.lib.rudp_session_config_channel(
                ctypes.byref(self._buf), channel_id, flags
            ),
            "rudp_session_config_channel",
        )

    def config_recovery(
        self, channel_id: int, initial_ms: int, min_ms: int, max_ms: int
    ) -> None:
        _check(
            self.lib.rudp_session_config_recovery(
                ctypes.byref(self._buf), channel_id, initial_ms, min_ms, max_ms
            ),
            "rudp_session_config_recovery",
        )

    def send_reliable(self, channel_id: int, pkt: TfvPacket, now_ms: int = 0) -> None:
        _check(
            self.lib.rudp_session_send_reliable(
                ctypes.byref(self._buf), channel_id, pkt, now_ms
            ),
            "rudp_session_send_reliable",
        )

    def queue_unreliable(self, channel_id: int, pkt: TfvPacket) -> None:
        _check(
            self.lib.rudp_session_queue_unreliable(
                ctypes.byref(self._buf), channel_id, pkt
            ),
            "rudp_session_queue_unreliable",
        )

    def has_pending_work(self, now_ms: int, fallback_timeout_ms: int = 100) -> bool:
        return bool(
            self.lib.rudp_session_has_pending_work(
                ctypes.byref(self._buf), now_ms, fallback_timeout_ms
            )
        )

    def next_wakeup_ms(self, now_ms: int, fallback_timeout_ms: int = 100) -> int:
        return int(
            self.lib.rudp_session_next_wakeup_ms(
                ctypes.byref(self._buf), now_ms, fallback_timeout_ms
            )
        )

    def build_datagram(
        self,
        primary_ack_channel: int = 0,
        now_ms: int = 0,
        fallback_timeout_ms: int = 100,
        max_len: int = RUDP_MAX_DATAGRAM_SIZE,
    ) -> bytes:
        out = (ctypes.c_uint8 * max_len)()
        n = _check(
            self.lib.rudp_session_build_datagram(
                ctypes.byref(self._buf),
                primary_ack_channel,
                out,
                max_len,
                now_ms,
                fallback_timeout_ms,
            ),
            "rudp_session_build_datagram",
        )
        return bytes(out[:n])

    def process_datagram_at(
        self, data: bytes, now_ms: int = 0, capacity: int = 175
    ) -> List[Tuple[int, int, int, TfvPacket]]:
        if not data:
            raise ZcrudpError(RUDP_ERR_INVALID_ARG, "rudp_session_process_datagram_at")
        in_buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        records = (Record * capacity)()
        n = _check(
            self.lib.rudp_session_process_datagram_at(
                ctypes.byref(self._buf), in_buf, len(data), records, capacity, now_ms
            ),
            "rudp_session_process_datagram_at",
        )
        return [
            (records[i].channel_id, records[i].flags, records[i].seq_num, records[i].payload)
            for i in range(n)
        ]

    def poll(self, capacity: int = 175) -> List[Tuple[int, int, int, TfvPacket]]:
        records = (Record * capacity)()
        n = _check(
            self.lib.rudp_session_poll(ctypes.byref(self._buf), records, capacity),
            "rudp_session_poll",
        )
        return [
            (records[i].channel_id, records[i].flags, records[i].seq_num, records[i].payload)
            for i in range(n)
        ]


class Crypto:
    def __init__(
        self,
        shared_key: bytes,
        session_id: int,
        local_is_initiator: bool,
        lib: Optional[ctypes.CDLL] = None,
    ) -> None:
        if len(shared_key) != RUDP_CRYPTO_KEY_BYTES:
            raise ValueError("shared_key must be exactly 32 bytes")
        self.lib = lib or load_library()
        self._buf = (ctypes.c_uint64 * 32)()  # 256 bytes, >= rudp_crypto_ctx_s
        key_arr = (ctypes.c_uint8 * RUDP_CRYPTO_KEY_BYTES).from_buffer_copy(shared_key)
        _check(
            self.lib.rudp_crypto_init(
                ctypes.byref(self._buf),
                key_arr,
                session_id,
                1 if local_is_initiator else 0,
            ),
            "rudp_crypto_init",
        )

    def seal(self, plaintext: bytes, aad: bytes = b"") -> bytes:
        aad_arr = (
            (ctypes.c_uint8 * len(aad)).from_buffer_copy(aad) if aad else None
        )
        pt_arr = (
            (ctypes.c_uint8 * len(plaintext)).from_buffer_copy(plaintext)
            if plaintext
            else (ctypes.c_uint8 * 1)()
        )
        out_len = len(plaintext) + RUDP_CRYPTO_OVERHEAD_BYTES
        out_arr = (ctypes.c_uint8 * out_len)()
        n = _check(
            self.lib.rudp_crypto_seal(
                ctypes.byref(self._buf),
                aad_arr,
                len(aad),
                pt_arr,
                len(plaintext),
                out_arr,
                out_len,
            ),
            "rudp_crypto_seal",
        )
        return bytes(out_arr[:n])

    def open(self, wire: bytes, aad: bytes = b"") -> bytes:
        aad_arr = (
            (ctypes.c_uint8 * len(aad)).from_buffer_copy(aad) if aad else None
        )
        if len(wire) < RUDP_CRYPTO_OVERHEAD_BYTES:
            raise ZcrudpError(RUDP_ERR_AUTH, "rudp_crypto_open")
        wire_arr = (ctypes.c_uint8 * len(wire)).from_buffer_copy(wire)
        out_cap = max(1, len(wire) - RUDP_CRYPTO_OVERHEAD_BYTES)
        out_arr = (ctypes.c_uint8 * out_cap)()
        n = _check(
            self.lib.rudp_crypto_open(
                ctypes.byref(self._buf),
                aad_arr,
                len(aad),
                wire_arr,
                len(wire),
                out_arr,
                out_cap,
            ),
            "rudp_crypto_open",
        )
        return bytes(out_arr[:n])

    def wipe(self) -> None:
        if hasattr(self, "_buf") and hasattr(self, "lib"):
            self.lib.rudp_crypto_wipe(ctypes.byref(self._buf))


def _self_test() -> None:
    lib = load_library()
    a = Session(lib)
    b = Session(lib)
    key = bytes(range(32))
    ca = Crypto(key, 0x1122334455667788, True, lib)
    cb = Crypto(key, 0x1122334455667788, False, lib)

    assert not a.has_pending_work(0, 100)
    a.send_reliable(0, make_packet(lib, 7, 1, 1001), 10)
    a.queue_unreliable(0, make_packet(lib, 8, 0, 2002))
    assert a.has_pending_work(10, 100)

    dgram = a.build_datagram(0, 10, 100)
    sealed = ca.seal(dgram, b"hdr")
    opened = cb.open(sealed, b"hdr")
    assert opened == dgram

    delivered = b.process_datagram_at(opened, 15)
    assert len(delivered) == 2
    assert decode_packet(lib, delivered[0][3]) == (7, 1, 1001)
    assert decode_packet(lib, delivered[1][3]) == (8, 0, 2002)

    # Tampered packet must fail AEAD authentication
    tampered = bytearray(sealed)
    tampered[-1] ^= 0x55
    try:
        cb.open(bytes(tampered), b"hdr")
        raise AssertionError("Tampered ciphertext must raise ZcrudpError")
    except ZcrudpError as exc:
        assert exc.code == RUDP_ERR_AUTH

    ack = b.build_datagram(0, 15, 100)
    assert len(ack) == RUDP_WIRE_DATAGRAM_HEADER_SIZE
    assert a.process_datagram_at(ack, 20) == []
    assert not a.has_pending_work(20, 100)
    ca.wipe()
    cb.wipe()
    print("PASS: Python ctypes binding reliable/unreliable/AEAD roundtrip")


if __name__ == "__main__":
    _self_test()
