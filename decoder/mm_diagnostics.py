# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded RX5 diagnostics; FFT work happens only when a viewer requests a snapshot."""
from collections import deque
import math
import threading
import time
import numpy as np

RATE = 12000
NFFT = 16384
MAX_POINTS = 480


class Diagnostics:
    def __init__(self):
        self.guard = threading.Lock()
        self.reset()

    def reset(self, identity=None):
        with self.guard:
            self.identity = identity
            self.audio = deque(maxlen=100)  # two seconds of 20-ms packets
            self.points = deque(maxlen=MAX_POINTS)
            self.status = {}
            self.audio_at = self.status_at = 0

    def add_audio(self, samples):
        with self.guard:
            self.audio.append(np.nan_to_num(samples, nan=0., posinf=0., neginf=0.).copy())
            self.audio_at = time.monotonic()

    def add_status(self, event):
        if event.get('event') != 'status':
            return
        with self.guard:
            self.status = {k:v for k,v in event.items() if k not in ('points','event')}
            for point in event.get('points', [])[-96:]:
                if len(point) == 2 and all(isinstance(v,(int,float)) and math.isfinite(v) for v in point):
                    self.points.append(point)
            self.status_at = time.monotonic()

    def snapshot(self):
        now = time.monotonic()
        with self.guard:
            audio = np.concatenate(list(self.audio)) if self.audio else np.array([])
            receiver, points = dict(self.status), list(self.points)
            identity, audio_at, status_at = self.identity, self.audio_at, self.status_at
        audio_age = now-audio_at if audio_at else None
        status_age = now-status_at if status_at else None
        fresh = audio_age is not None and audio_age < 2 and status_age is not None and status_age < 2
        receiver['locked'] = bool(fresh and receiver.get('locked'))
        result = {'audio_fresh': fresh, 'audio_age_s': audio_age, 'status_age_s': status_age,
                  'frequency_hz': identity[2] if identity else None, 'receiver': receiver,
                  'passband_hz': [0,4000], 'pilot_search_hz': [20,900],
                  'pilot_hz': None, 'pilot_prominence_db': None,
                  'points': points if fresh else [], 'spectrum': None}
        if not fresh or len(audio) < NFFT:
            return result
        window = np.hanning(NFFT)
        starts = np.unique(np.linspace(0,len(audio)-NFFT,3).astype(int))
        power = np.mean([np.abs(np.fft.rfft(audio[s:s+NFFT]*window))**2 for s in starts],axis=0)
        freq = np.fft.rfftfreq(NFFT,1/RATE)
        db = 10*np.log10(np.maximum(power*(2/window.sum())**2,1e-14))
        # Local maxima and local prominence distinguish a narrow pilot from modem data.
        candidates = []
        for i in np.flatnonzero((freq>=20)&(freq<=900)):
            if power[i]<=power[i-1] or power[i]<=power[i+1]: continue
            neighbors = np.r_[power[max(0,i-34):max(0,i-5)],power[i+6:i+35]]
            prominence = 10*np.log10(max(float(power[i]),1e-30)/max(float(np.median(neighbors)),1e-30))
            if prominence<12 or db[i]<-110: continue
            a,b,c = np.log(np.maximum(power[i-1:i+2],1e-30))
            delta = .5*(a-c)/(a-2*b+c) if a-2*b+c else 0
            hz = (i+np.clip(delta,-.5,.5))*RATE/NFFT
            candidates.append((float(hz),float(prominence)))
        if candidates:
            # During verified reception prefer the pilot consistent with the tracked modem.
            expected = receiver.get('center_hz',1500)+receiver.get('pll_hz',0)-1400
            nearby = [c for c in candidates if abs(c[0]-expected)<8] if receiver['locked'] else []
            choices = nearby if receiver['locked'] else candidates
            if choices:
                chosen = max(choices,key=lambda c:c[1])
                result['pilot_hz'],result['pilot_prominence_db'] = chosen
        # One maximum per displayed bin preserves narrow tones without a large WS payload.
        bins = 768
        indices = np.minimum(np.searchsorted(freq,np.linspace(0,4000,bins+1)),len(db)-1)
        values = [round(float(np.max(db[a:max(a+1,b)])),1) for a,b in zip(indices,indices[1:])]
        result['spectrum'] = {'start_hz':0,'end_hz':4000,'dbfs':values}
        return result
