import sys
from pathlib import Path
import unittest
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gui"))
from protocol import Assembler, HEADER, subscription, filter_control, filter_state, valid_filter, filter_edges_control
from protocol import compressor_control, compressor_state
from protocol import loop_control,loop_state
from protocol import nr_control,nr_state
from protocol import settings_control,settings_state


def packets(frame=1, stamp=100, session=7, n=1024, stream=0, rate=600000):
    values = np.arange(n, dtype=np.int16) - 500
    result = []
    for first in range(0, len(values), 568):
        count = min(568, len(values) - first)
        header = HEADER.pack(b"QTR2", 1, 2, 64, stream, frame, stamp, 30000,
                             10489750000, rate, n, first, count, 1, n, session)
        result.append(header + values[first:first+count].astype(">i2").tobytes())
    return result


class ProtocolTest(unittest.TestCase):
    def test_shared_spectrum_sizes(self):
        for stream,n,rate in [(0,2000,600000),(1,2400,60000),(2,2400,60000)]:
            assembler=Assembler(stream)
            frame=None
            for packet in reversed(packets(n=n,stream=stream,rate=rate)):
                frame=assembler.push(packet) or frame
            self.assertIsNotNone(frame)
            self.assertEqual((frame.fft_size,frame.sample_rate),(n,rate))
            self.assertEqual(assembler.invalid_packets,0)
            self.assertAlmostEqual(frame.frequencies_hz[1]-frame.frequencies_hz[0],rate/n)

    def test_settings_wire(self):
        query=HEADER.unpack(settings_control(8))
        self.assertEqual((query[2],query[4],query[5],query[7],query[13],query[15]),(17,0,8,0,0,0))
        command=settings_control(9,99,7,{'rx_correction_hz':-123})
        f=HEADER.unpack_from(command);n=len(command)-64
        self.assertEqual((f[7],f[10],f[12],f[13],f[14],f[15]),(7,n,n,1,n,99))
        payload=b'settings_schema=1\nrx_correction_hz=-123\n'
        reply=HEADER.pack(b'QTR2',1,18,64,0,9,0,8,0,0,len(payload),0,len(payload),0,len(payload),99)+payload
        self.assertEqual(settings_state(reply),(9,99,8,0,0,{'settings_schema':'1','rx_correction_hz':'-123'}))
        self.assertIsNone(settings_state(reply+b'x'))
        for bad in ({}, {'x':'a'*1200}):
            with self.assertRaises(ValueError):settings_control(9,99,7,bad)

    def test_nr_wire(self):
        query=HEADER.unpack(nr_control(7,99))
        self.assertEqual((query[2],query[4],query[5],query[9],query[10],query[13],query[15]),(15,1,7,0,0,0,99))
        for percent in (0,1,60,100):
            setting=HEADER.unpack(nr_control(8,99,percent))
            self.assertEqual((setting[9],setting[10],setting[13]),(12000,percent,1))
        reply=HEADER.pack(b'QTR2',1,16,64,1,8,0,3,0,12000,60,0,0,0,0,99)
        self.assertEqual(nr_state(reply),(8,99,60,0))
        self.assertIsNone(nr_state(reply+b'x'))
        for index,value in ((4,2),(9,48000),(10,101),(13,2)):
            bad=list(HEADER.unpack(reply));bad[index]=value
            self.assertIsNone(nr_state(HEADER.pack(*bad)))
        for bad in (-1,101,1.2,float('nan')):
            with self.assertRaises(ValueError): nr_control(1,99,bad)

    def test_loop_wire(self):
        query=HEADER.unpack(loop_control(7,99))
        self.assertEqual((query[2],query[4],query[5],query[10],query[13],query[15]),(13,0,7,0,0,99))
        for mode in (0,1,2):
            setting=HEADER.unpack(loop_control(8,99,mode))
            self.assertEqual((setting[10],setting[13]),(mode,1))
        reply=HEADER.pack(b'QTR2',1,14,64,0,8,0,3,1290250000,600000,2,2,0,0,0,99)
        self.assertEqual(loop_state(reply),(8,99,2,2,1290250000,3,0))
        self.assertIsNone(loop_state(reply+b'x'))
        for bad in (-1,3,1.2):
            with self.assertRaises(ValueError): loop_control(1,99,bad)

    def test_compressor_wire(self):
        query=HEADER.unpack(compressor_control(7,99))
        self.assertEqual((query[2],query[4],query[5],query[9],query[10],query[13],query[15]),(11,3,7,0,0,0,99))
        for amount in (0,600,1000,1400):
            setting=HEADER.unpack(compressor_control(8,99,amount))
            self.assertEqual((setting[9],setting[10],setting[13]),(12000,amount,1))
        reply=HEADER.pack(b'QTR2',1,12,64,3,8,0,3,0,12000,500,4250,0,0,1000,99)
        self.assertEqual(compressor_state(reply),(8,99,500,0,4.25,1.0))
        self.assertIsNone(compressor_state(reply+b'x'))
        invalid=list(HEADER.unpack(reply));invalid[4]=2
        self.assertIsNone(compressor_state(HEADER.pack(*invalid)))
        for bad in (-1,1401,1.2,float('nan')):
            with self.assertRaises(ValueError): compressor_control(1,99,bad)
        extended=list(HEADER.unpack(reply));extended[10]=1400
        self.assertEqual(compressor_state(HEADER.pack(*extended))[2],1400)
        extended[10]=1401
        self.assertIsNone(compressor_state(HEADER.pack(*extended)))

    def test_rx_filter_wire(self):
        query = HEADER.unpack(filter_control(7, 99))
        self.assertEqual((query[2], query[4], query[5], query[13], query[15]), (9, 1, 7, 0, 99))
        setting = HEADER.unpack(filter_control(8, 99, 500, 700))
        self.assertEqual((setting[8], setting[9], setting[10], setting[13]), (700, 12000, 500, 1))
        reply = HEADER.pack(b'QTR2', 1, 10, 64, 1, 8, 0, 3, 700, 12000, 500, 0, 0, 0, 0, 99)
        self.assertEqual(filter_state(reply), (8, 99, 500, 700, 0))
        self.assertIsNone(filter_state(reply+b'x'))
        bad = bytearray(reply);bad[11] = 2
        self.assertIsNone(filter_state(bad))
        for bandwidth, center in ((499, 700), (3900, 700), (500, 349), (500, 3751)):
            self.assertFalse(valid_filter(bandwidth, center))
            with self.assertRaises(ValueError): filter_control(1, 99, bandwidth, center)
        self.assertTrue(valid_filter(501, 700))

    def test_filter_edges_wire(self):
        self.assertEqual(HEADER.unpack(filter_edges_control(7, 99))[13], 3)
        setting = HEADER.unpack(filter_edges_control(8, 99, 301, 2700))
        self.assertEqual((setting[8], setting[9], setting[10], setting[13]), (301000, 12000, 2700000, 2))
        reply = HEADER.pack(b'QTR2', 1, 10, 64, 1, 8, 0, 3, 1501, 12000, 2399, 301000, 0, 0, 2700000, 99)
        self.assertEqual(filter_state(reply), (8, 99, 2399, 1500.5, 0))
        bad = list(HEADER.unpack(reply)); bad[14] = 4001000
        self.assertIsNone(filter_state(HEADER.pack(*bad)))
        for low, high in [(99, 2700), (301, 4001), (800, 1200), (2000, 1000), (float('nan'), 2700)]:
            with self.assertRaises(ValueError): filter_edges_control(1, 99, low, high)

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
