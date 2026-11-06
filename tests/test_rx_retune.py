"""RX tuning must preserve PCM, the audio clock, and the GUI jitter buffer."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'gui'))
from test_gui import Receiver
from audio import JitterBuffer


def wait(predicate, timeout=4):
    end = time.monotonic()+timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.005)
    assert predicate(), 'RX tuning timeout'


def main():
    binary, defaults = map(Path, sys.argv[1:])
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='qo100-rx-retune-') as temp:
        root = Path(temp)
        config = root/'backend.conf'
        config.write_text(defaults.read_text().replace('gui_port = 40900', f'gui_port = {port}'))
        with (root/'backend.log').open('w') as log:
            backend = subprocess.Popen([str(binary), '--config', str(config), '--source', 'synthetic'], stdout=log, stderr=log)
            received = {1: [], 2: []}
            buffers = {1: JitterBuffer(), 2: JitterBuffer()}
            def save(packet):
                b = buffers[packet.stream_id]
                b.add(packet)
                while b.level() > 960:
                    b.take()
                received[packet.stream_id].append((time.monotonic(), packet, b.level()))
            receiver = Receiver('127.0.0.1', port, rx_hz=10489748500, rx2_hz=10489600000, audio_sink=save)
            receiver.channels[2].sink = save
            try:
                wait(lambda: all(len(received[s]) >= 12 for s in (1, 2)) and receiver.tx_identity is not None)
                baseline = {s: receiver.channels[s].identity for s in (1, 2)}
                initial_tx = receiver.tx_identity
                start = {s: len(received[s]) for s in (1, 2)}
                receiver.set_frequency(10489748510)
                wait(lambda: receiver.applied_hz == 10489748510)
                assert receiver.channels[1].identity == baseline[1], 'Retuning changed the RX1 epoch and flushed the audio device'
                for i in range(120):
                    receiver.set_frequency(10489748500+(i % 5)*10)
                    receiver.set_frequency(10489600000+(i % 7)*10, 2)
                    time.sleep(.01)
                wait(lambda: receiver.applied_hz == 10489748540 and receiver.channels[2].applied_hz == 10489600000)
                assert all(receiver.channels[s].identity == baseline[s] for s in (1, 2)), 'Continuous tuning changed RX identity'

                # The settings interface must use the same continuous tuning path.
                wait(lambda: not receiver.settings.pending)
                ticket = receiver.settings.set({'initial_rx_hz': 10489748600, 'initial_rx2_hz': 10489600100})
                wait(lambda: ticket in receiver.settings.results)
                assert receiver.settings.results[ticket][0] == 0, receiver.settings.results[ticket]
                wait(lambda: receiver.applied_hz == 10489748600 and receiver.channels[2].applied_hz == 10489600100)
                assert all(receiver.channels[s].identity == baseline[s] for s in (1, 2)), 'Settings tuning reset RX'

                def pitch(frequency):
                    receiver.set_frequency(frequency)
                    wait(lambda: receiver.applied_hz == frequency)
                    count = len(received[1])
                    wait(lambda: len(received[1]) >= count+24)
                    # Exclude the short ordinary filter transition, not an audio hole.
                    x = np.concatenate([r[1].samples for r in received[1][count+7:count+24]])
                    peak = np.argmax(abs(np.fft.rfft(x*np.hanning(len(x)))))
                    return peak*12000/len(x)
                before, after = pitch(10489748500), pitch(10489748600)
                assert abs(before-1000) < 5 and abs(after-900) < 5, (before, after)
                for stream in (1, 2):
                    records = received[stream][start[stream]:]
                    assert len(records) > 100, (stream, len(records))
                    assert all((p.session_id, p.epoch) == baseline[stream] for _, p, _ in records), 'RX epoch changed'
                    assert all(b[1].sample_index == a[1].sample_index+240 for a, b in zip(records, records[1:])), 'Audio sample clock has a gap'
                    assert min(level for _, _, level in records) == 960, 'Retuning emptied the GUI jitter buffer'
                    assert min(float(np.sqrt(np.mean(p.samples**2))) for _, p, _ in records) > .02, 'Retuning inserted a silent PCM block'
                    gap = max(b[0]-a[0] for a, b in zip(records, records[1:]))
                    assert gap < .18, f'Audio UDP stalled for {gap:.3f} seconds'
                    print(f'RX{stream}: {len(records)} continuous PCM blocks, unchanged epoch, jitter buffer preserved; max UDP interval {gap*1000:.1f} ms')
                assert receiver.tx_identity == initial_tx and not receiver.tx_confirmed
                assert receiver.tx_packets == 0 and receiver.invalid_audio == 0
                print(f'120 rapid tuning steps per RX + settings tuning: SSB tone {before:.1f} -> {after:.1f} Hz, no audio reset, TX unchanged')
            finally:
                receiver.close()
                backend.terminate()
                backend.wait(timeout=5)
        assert backend.returncode == 0, (root/'backend.log').read_text()


if __name__ == '__main__':
    main()
