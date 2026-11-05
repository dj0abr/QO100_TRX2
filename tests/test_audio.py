import sys
from pathlib import Path
import unittest
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"gui"))
from audio import JitterBuffer
from protocol import AudioPacket, audio_packet, HEADER, tune
from test_gui import parse_frequency


def packet(index,epoch=1):
    return AudioPacket(epoch,index,10489748500,99,np.ones(240,dtype=np.float32))


class AudioTest(unittest.TestCase):
    def test_order_loss_duplicates(self):
        b=JitterBuffer()
        b.add(packet(240));b.add(packet(0));b.add(packet(0))
        self.assertEqual(b.level(),480)
        np.testing.assert_array_equal(b.take(),np.ones(240))
        b.take()
        b.add(packet(720))
        self.assertEqual(float(b.take().sum()),0)
        self.assertEqual(b.losses,1)
        self.assertEqual(float(b.take().sum()),240)

    def test_bound_and_retune(self):
        b=JitterBuffer()
        for i in range(100): b.add(packet(i*240))
        self.assertLessEqual(b.level(),6000)
        self.assertGreater(b.overruns,0)
        b.add(packet(0,epoch=2))
        self.assertEqual(b.level(),240)
        self.assertEqual(b.identity,(99,2))

    def test_wire_and_frequency(self):
        self.assertEqual(parse_frequency("10489,748500"),10489748500)
        self.assertEqual(HEADER.unpack(tune(2,10489748500))[8],10489748500)
        header=HEADER.pack(b'QTR2',1,5,64,1,3,123,240,10489748500,12000,0,0,240,2,240,99)
        values=np.arange(240,dtype='>i2')
        a=audio_packet(header+values.tobytes())
        self.assertEqual(a.epoch,3)
        self.assertEqual(a.sample_index,240)
        self.assertIsNone(audio_packet(header+values.tobytes()[:-1]))


if __name__=='__main__': unittest.main()
