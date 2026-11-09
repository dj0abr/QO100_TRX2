"""Measured pilot, bounded telemetry and stale/reset behavior without radio or audio devices."""
from pathlib import Path
import json
import sys
import time
import unittest
from unittest.mock import patch
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'decoder'))
from mm_diagnostics import Diagnostics, MAX_POINTS


class DiagnosticsTest(unittest.TestCase):
    def prepared(self,pilot=500.2):
        d=Diagnostics();d.reset((99,1,10489992820))
        for block in range(100):
            t=(np.arange(240)+block*240)/12000
            d.add_audio(.006*np.cos(2*np.pi*pilot*t)+.03*np.cos(2*np.pi*1900*t))
        d.add_status({'event':'status','locked':True,'center_hz':1400+pilot,'pll_hz':0,'points':[[.5,.8]]*96})
        return d

    def test_measured_500_hz_pilot_and_bounded_spectrum(self):
        d=self.prepared();s=d.snapshot()
        self.assertAlmostEqual(s['pilot_hz'],500.2,delta=.1)
        self.assertTrue(s['receiver']['locked'])
        self.assertEqual(len(s['spectrum']['dbfs']),768)
        self.assertEqual(s['frequency_hz'],10489992820)
        json.dumps(s,allow_nan=False)

    def test_old_100_hz_and_low_pilot_still_measured(self):
        for pilot in (25.3,100.2,850.1):
            self.assertAlmostEqual(self.prepared(pilot).snapshot()['pilot_hz'],pilot,delta=.15)

    def test_stale_diagnostics_cannot_claim_lock_or_show_old_points(self):
        d=self.prepared();now=time.monotonic()
        with patch('mm_diagnostics.time.monotonic',return_value=now+3):s=d.snapshot()
        self.assertFalse(s['audio_fresh']);self.assertFalse(s['receiver']['locked'])
        self.assertIsNone(s['pilot_hz']);self.assertIsNone(s['spectrum']);self.assertEqual(s['points'],[])

    def test_unrelated_tone_is_not_a_verified_pilot(self):
        d=self.prepared(850)
        d.add_status({'event':'status','locked':True,'center_hz':1900,'pll_hz':0})
        self.assertIsNone(d.snapshot()['pilot_hz'])

    def test_reset_silence_and_point_bounds(self):
        d=self.prepared()
        for _ in range(20):d.add_status({'event':'status','locked':True,'points':[[1,0]]*96})
        self.assertEqual(len(d.snapshot()['points']),MAX_POINTS)
        d.reset((99,2,10489993220));s=d.snapshot()
        self.assertFalse(s['audio_fresh']);self.assertIsNone(s['pilot_hz']);self.assertEqual(s['points'],[])
        for _ in range(100):d.add_audio(np.full(240,np.nan))
        d.add_status({'event':'status','locked':False})
        s=d.snapshot();self.assertIsNone(s['pilot_hz']);json.dumps(s,allow_nan=False)


if __name__=='__main__':unittest.main()
