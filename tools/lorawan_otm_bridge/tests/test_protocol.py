import unittest

from protocol import FragmentError, Reassembler, crc16_ccitt, parse_fragment


def make_fragments(frame: bytes, sequence: int = 7, chunk: int = 32) -> list[bytes]:
    crc = crc16_ccitt(frame)
    count = (len(frame) + chunk - 1) // chunk
    result = []
    for index in range(count):
        part = frame[index * chunk:(index + 1) * chunk]
        header = (
            b"BO"
            + bytes([1, 0])
            + sequence.to_bytes(2, "big")
            + bytes([index, count])
            + len(frame).to_bytes(2, "big")
            + crc.to_bytes(2, "big")
        )
        result.append(header + part)
    return result


class ProtocolTests(unittest.TestCase):
    def test_parse_fragment(self):
        payload = make_fragments(b"abc")[0]
        fragment = parse_fragment(payload)
        self.assertEqual(fragment.frame_sequence, 7)
        self.assertEqual(fragment.fragment_index, 0)
        self.assertEqual(fragment.fragment_count, 1)
        self.assertEqual(fragment.total_length, 3)
        self.assertEqual(fragment.data, b"abc")

    def test_reassembles_out_of_order_and_ignores_identical_duplicate(self):
        frame = bytes(range(100))
        fragments = make_fragments(frame)
        r = Reassembler()
        self.assertIsNone(r.add("bike", fragments[2], now=1))
        self.assertIsNone(r.add("bike", fragments[0], now=2))
        self.assertIsNone(r.add("bike", fragments[0], now=3))
        self.assertIsNone(r.add("bike", fragments[3], now=4))
        self.assertEqual(r.add("bike", fragments[1], now=5), frame)

    def test_crc_failure_discards_complete_frame(self):
        frame = b"hello world" * 5
        fragments = make_fragments(frame)
        damaged = bytearray(fragments[-1])
        damaged[-1] ^= 0x01
        fragments[-1] = bytes(damaged)
        r = Reassembler()
        for payload in fragments[:-1]:
            self.assertIsNone(r.add("bike", payload, now=1))
        with self.assertRaisesRegex(FragmentError, "CRC16"):
            r.add("bike", fragments[-1], now=1)

    def test_conflicting_duplicate_rejected(self):
        fragments = make_fragments(b"x" * 40)
        r = Reassembler()
        self.assertIsNone(r.add("bike", fragments[0], now=1))
        changed = bytearray(fragments[0])
        changed[-1] ^= 0x01
        with self.assertRaisesRegex(FragmentError, "conflicting duplicate"):
            r.add("bike", bytes(changed), now=2)

    def test_expiry(self):
        fragments = make_fragments(b"x" * 40)
        r = Reassembler(timeout_seconds=5)
        self.assertIsNone(r.add("bike", fragments[0], now=1))
        self.assertEqual(r.expire(now=7), 1)

    def test_rejects_wrong_magic_and_oversize(self):
        with self.assertRaises(FragmentError):
            parse_fragment(b"NO" + b"\x01\x00" + b"\x00" * 8 + b"x")

        frame = b"x" * 40
        fragments = make_fragments(frame)
        r = Reassembler(max_frame_bytes=32)
        with self.assertRaisesRegex(FragmentError, "exceeds"):
            r.add("bike", fragments[0], now=1)


if __name__ == "__main__":
    unittest.main()
