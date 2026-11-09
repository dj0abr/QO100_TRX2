"""End-to-end checks with independently generated HS-Modem audio frames."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
PROBE = os.environ.get("QO100_MM_PROBE", str(ROOT/"build/rx_probe"))
FIXTURE = os.environ.get("QO100_MM_FIXTURE", str(ROOT/"build/generate_fixture"))
sys.path.insert(0, str(ROOT))
from rx_monitor import spectrum


def fixture(offset=0, corrupt=False, mode="clean", frames=45):
    args = [FIXTURE, str(offset),
            "corrupt" if corrupt else mode, str(frames)]
    return subprocess.check_output(args)


def decode(audio, center=1500, automatic=False):
    args = [PROBE, "--center-hz", str(center)]
    if not automatic:
        args.append("--manual")
    result = subprocess.run(args,
                            input=audio, capture_output=True, check=True)
    return [json.loads(line) for line in result.stdout.splitlines()]


class DecoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clean = fixture()

    def test_clean_audio_recovers_known_payload_and_locks(self):
        events = decode(self.clean)
        frames = [x for x in events if x["event"] == "frame"]
        self.assertGreaterEqual(len(frames), 35)
        for event in frames:
            self.assertEqual(event["frame_type"], 8)
            self.assertEqual(event["frame_info"], 3)
            payload = bytes.fromhex(event["payload_hex"])
            self.assertEqual(len(payload), 219)
            self.assertEqual(payload[0], 2)
            self.assertEqual(payload[1:].rstrip(b"\0"), b"TEST CW-SKIMMER RX VALIDATION")
        self.assertTrue(events[-1]["locked"])

    def test_shifted_audio_at_matching_receive_center(self):
        events = decode(fixture(25), 1525)
        self.assertTrue(events[-1]["locked"])
        self.assertGreaterEqual(events[-1]["valid_frames"], 35)

    def test_valid_rs_but_wrong_crc_never_locks(self):
        events = decode(fixture(corrupt=True))
        self.assertFalse(any(x.get("locked") for x in events))
        self.assertEqual(events[-1]["valid_frames"], 0)
        self.assertGreater(events[-1]["crc_failures"], 30)

    def test_noise_and_silence_never_lock(self):
        rng = np.random.default_rng(100)
        for audio in (np.zeros(48000*4, dtype="<f4"),
                      rng.normal(0, 0.01, 48000*4).astype("<f4")):
            events = decode(audio.tobytes())
            self.assertEqual(events[-1]["valid_frames"], 0)
            self.assertFalse(events[-1]["locked"])

    def test_lock_expires_after_signal_loss(self):
        audio = self.clean + np.zeros(48000*3, dtype="<f4").tobytes()
        events = decode(audio)
        self.assertTrue(any(x.get("locked") for x in events))
        self.assertFalse(events[-1]["locked"])
        self.assertGreater(events[-1]["frame_age_s"], 2)

    def test_recovery_after_digital_silence(self):
        audio = np.zeros(48000*4, dtype="<f4").tobytes() + self.clean
        events = decode(audio)
        self.assertTrue(events[-1]["locked"])
        self.assertGreaterEqual(events[-1]["valid_frames"], 35)

    def test_incomplete_float_sample_is_rejected(self):
        result = subprocess.run([PROBE], input=b"\x01",
                                capture_output=True)
        self.assertEqual(result.returncode, 2)

    def test_pilot_frequency_measurement(self):
        rng = np.random.default_rng(3)
        t = np.arange(96000)/48000
        audio = 0.01*np.cos(2*np.pi*125.3*t) + rng.normal(0,0.001,len(t))
        _, _, pilot = spectrum(audio)
        self.assertIsNotNone(pilot)
        self.assertAlmostEqual(pilot[0], 125.3, delta=0.15)
        _, _, silent_pilot = spectrum(np.zeros(96000))
        self.assertIsNone(silent_pilot)


class AutomaticTuningTests(unittest.TestCase):
    def test_acquires_positive_and_negative_offsets(self):
        for offset in (-75, -60, 0, 25, 80, 180, 240, 300, 400, 600, 750):
            with self.subTest(offset=offset):
                events = decode(fixture(offset, frames=65), automatic=True)
                status = events[-1]
                self.assertTrue(status["locked"])
                self.assertGreater(status["valid_frames"], 40)
                self.assertAlmostEqual(status["center_hz"]+status["pll_hz"],
                                       1500+offset, delta=1)

    def test_tracks_slow_drift_without_losing_lock(self):
        events = decode(fixture(25, mode="drift", frames=100), automatic=True)
        statuses = [x for x in events if x["event"] == "status"]
        self.assertTrue(all(x["locked"] for x in statuses if x["audio_seconds"] >= 5))
        status = statuses[-1]
        self.assertAlmostEqual(status["center_hz"]+status["pll_hz"],
                               1525+status["audio_seconds"], delta=1)

    def test_tracks_500_hz_pilot_without_old_center_limit(self):
        events = decode(fixture(400, mode="drift", frames=100), automatic=True)
        statuses = [e for e in events if e['event'] == 'status']
        self.assertTrue(all(e['locked'] for e in statuses if e['audio_seconds'] >= 5))
        self.assertAlmostEqual(events[-1]['center_hz']+events[-1]['pll_hz'],
                               1900+events[-1]['audio_seconds'],delta=1)

    def test_reacquires_after_frequency_jump(self):
        events = decode(fixture(20, mode="jump", frames=100), automatic=True)
        statuses = [x for x in events if x["event"] == "status"]
        self.assertTrue(events[-1]["locked"])
        self.assertGreater(events[-1]["valid_frames"], 60)
        self.assertTrue(all(x["locked"] for x in statuses if x["audio_seconds"] >= 17))
        self.assertAlmostEqual(events[-1]["center_hz"]+events[-1]["pll_hz"],1590,delta=1)

    def test_reacquires_after_silence_and_changed_frequency(self):
        events = decode(fixture(20, mode="dropout", frames=100), automatic=True)
        statuses = [x for x in events if x["event"] == "status"]
        self.assertTrue(any(not x["locked"] for x in statuses if 10 <= x["audio_seconds"] <= 11))
        self.assertTrue(events[-1]["locked"])
        self.assertAlmostEqual(events[-1]["center_hz"]+events[-1]["pll_hz"],1560,delta=1)

    def test_rejects_stronger_unrelated_tone_then_finds_data(self):
        events = decode(fixture(80, mode="interferer", frames=100), automatic=True)
        self.assertTrue(events[-1]["locked"])
        self.assertGreater(events[-1]["valid_frames"], 20)
        self.assertAlmostEqual(events[-1]["center_hz"]+events[-1]["pll_hz"],1580,delta=1)

    def test_tone_noise_silence_and_corrupt_frames_never_claim_lock(self):
        rng = np.random.default_rng(1400)
        t = np.arange(48000*12)/48000
        cases = [(0.04*np.cos(2*np.pi*210*t)).astype("<f4").tobytes(),
                 rng.normal(0,0.01,len(t)).astype("<f4").tobytes(),
                 np.zeros(len(t),dtype="<f4").tobytes(),
                 fixture(80, corrupt=True, frames=50)]
        for audio in cases:
            events = decode(audio, automatic=True)
            self.assertEqual(events[-1]["valid_frames"], 0)
            self.assertFalse(any(x.get("locked") for x in events))

    def test_existing_pll_can_receive_when_pilot_is_absent(self):
        events = decode(fixture(25, mode="nopilot", frames=60), automatic=True)
        self.assertTrue(events[-1]["locked"])
        self.assertGreater(events[-1]["valid_frames"], 40)


if __name__ == "__main__":
    unittest.main()
