import sys
from pathlib import Path
import unittest
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gui"))
from protocol import Assembler, HEADER, subscription


def packets(frame=1, stamp=100, session=7):
    values = np.arange(1024, dtype=np.int16) - 500
    result = []
    for first in range(0, len(values), 568):
        count = min(568, len(values) - first)
        header = HEADER.pack(b"QTR2", 1, 2, 64, 0, frame, stamp, 30000,
                             10489750000, 600000, 1024, first, count, 1, 1024, session)
        result.append(header + values[first:first+count].astype(">i2").tobytes())
    return result


class ProtocolTest(unittest.TestCase):
    def test_subscribe(self):
        self.assertEqual(len(subscription()), 64)
        self.assertEqual(subscription()[:8], b"QTR2\x01\x01\x00\x40")

    def test_out_of_order_duplicate(self):
        a = Assembler()
        p = packets()
        self.assertIsNone(a.push(p[1]))
        self.assertIsNone(a.push(p[1]))
        frame = a.push(p[0])
        np.testing.assert_allclose(frame.db, (np.arange(1024) - 500) / 100, atol=1e-6)
        self.assertEqual(frame.center_hz, 10489750000)
        self.assertIsNone(a.push(p[0]))
        self.assertIsNone(a.push(p[1]))

    def test_loss_and_new_frame(self):
        a = Assembler()
        self.assertIsNone(a.push(packets()[0]))
        for p in packets(frame=2, stamp=101):
            result = a.push(p)
        self.assertEqual(result.frame_id, 2)
        self.assertEqual(a.incomplete_frames, 1)
        self.assertLessEqual(len(a.pending), 4)

    def test_malformed(self):
        a = Assembler()
        for p in (b"", b"garbage", packets()[0][:-1], subscription(), b"X" * 1201):
            self.assertIsNone(a.push(p))
        self.assertEqual(a.invalid_packets, 5)

    def test_metadata_mismatch_and_restart(self):
        a = Assembler()
        a.push(packets()[0])
        bad = bytearray(packets()[1])
        bad[43] ^= 1
        self.assertIsNone(a.push(bad))
        self.assertEqual(a.invalid_packets, 1)
        self.assertIsNotNone(a.push(packets()[1]))
        for p in packets(frame=0, stamp=200, session=8):
            frame = a.push(p)
        self.assertEqual(frame.session_id, 8)
        self.assertEqual(frame.frame_id, 0)


if __name__ == "__main__":
    unittest.main()
