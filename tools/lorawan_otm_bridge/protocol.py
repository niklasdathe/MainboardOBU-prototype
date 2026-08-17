"""BicycleOBU LoRaWAN raw-frame fragment protocol."""

from __future__ import annotations

from dataclasses import dataclass
import time
from typing import Dict, Optional, Tuple

MAGIC = b"BO"
VERSION = 1
HEADER_BYTES = 12


class FragmentError(ValueError):
    """Raised when a LoRaWAN fragment is malformed."""


@dataclass(frozen=True)
class Fragment:
    frame_sequence: int
    fragment_index: int
    fragment_count: int
    total_length: int
    frame_crc16: int
    data: bytes


@dataclass
class _PendingFrame:
    created_at: float
    fragment_count: int
    total_length: int
    frame_crc16: int
    parts: Dict[int, bytes]


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def parse_fragment(payload: bytes) -> Fragment:
    if len(payload) < HEADER_BYTES:
        raise FragmentError("fragment shorter than header")
    if payload[:2] != MAGIC:
        raise FragmentError("bad fragment magic")
    if payload[2] != VERSION:
        raise FragmentError(f"unsupported fragment version {payload[2]}")
    if payload[3] != 0:
        raise FragmentError("unsupported fragment flags")

    frame_sequence = int.from_bytes(payload[4:6], "big")
    fragment_index = payload[6]
    fragment_count = payload[7]
    total_length = int.from_bytes(payload[8:10], "big")
    frame_crc16 = int.from_bytes(payload[10:12], "big")

    if fragment_count == 0:
        raise FragmentError("fragment_count is zero")
    if fragment_index >= fragment_count:
        raise FragmentError("fragment index outside frame")
    if total_length == 0:
        raise FragmentError("total length is zero")
    if not payload[HEADER_BYTES:]:
        raise FragmentError("empty fragment data")

    return Fragment(
        frame_sequence=frame_sequence,
        fragment_index=fragment_index,
        fragment_count=fragment_count,
        total_length=total_length,
        frame_crc16=frame_crc16,
        data=bytes(payload[HEADER_BYTES:]),
    )


class Reassembler:
    def __init__(self, timeout_seconds: float = 600.0, max_frame_bytes: int = 512) -> None:
        self.timeout_seconds = timeout_seconds
        self.max_frame_bytes = max_frame_bytes
        self._pending: Dict[Tuple[str, int], _PendingFrame] = {}

    def expire(self, now: Optional[float] = None) -> int:
        now = time.monotonic() if now is None else now
        expired = [
            key for key, pending in self._pending.items()
            if now - pending.created_at > self.timeout_seconds
        ]
        for key in expired:
            del self._pending[key]
        return len(expired)

    def add(self, device_id: str, payload: bytes, now: Optional[float] = None) -> Optional[bytes]:
        now = time.monotonic() if now is None else now
        self.expire(now)
        fragment = parse_fragment(payload)

        if fragment.total_length > self.max_frame_bytes:
            raise FragmentError(
                f"frame length {fragment.total_length} exceeds maximum {self.max_frame_bytes}"
            )

        key = (device_id, fragment.frame_sequence)
        pending = self._pending.get(key)
        metadata = (
            fragment.fragment_count,
            fragment.total_length,
            fragment.frame_crc16,
        )
        if pending is None or (
            pending.fragment_count,
            pending.total_length,
            pending.frame_crc16,
        ) != metadata:
            pending = _PendingFrame(
                created_at=now,
                fragment_count=fragment.fragment_count,
                total_length=fragment.total_length,
                frame_crc16=fragment.frame_crc16,
                parts={},
            )
            self._pending[key] = pending

        existing = pending.parts.get(fragment.fragment_index)
        if existing is not None and existing != fragment.data:
            del self._pending[key]
            raise FragmentError("conflicting duplicate fragment")

        pending.parts[fragment.fragment_index] = fragment.data
        if len(pending.parts) != pending.fragment_count:
            return None

        frame = b"".join(pending.parts[index] for index in range(pending.fragment_count))
        del self._pending[key]

        if len(frame) != pending.total_length:
            raise FragmentError(
                f"reassembled length {len(frame)} does not match {pending.total_length}"
            )
        if crc16_ccitt(frame) != pending.frame_crc16:
            raise FragmentError("whole-frame CRC16 mismatch")
        return frame
