#!/usr/bin/env python3
"""Test both loop modes over UDP with an RF-free, restartable Pluto double."""
import json
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'gui'))
from test_gui import Receiver
from protocol import HEADER,loop_control,tx_audio

DRIVER=r'''#!/usr/bin/python3
from collections import deque
import json,signal,socket,struct,time
from pathlib import Path
import numpy as np
settings={}
for line in Path('pluto_config.txt').read_text().splitlines():
    p=line.split()
    if len(p)==2: settings[p[0]]=p[1]
rx_lo=round(float(settings['RX_FREQ'])*1e6);tx_lo=round(float(settings['TX_FREQ'])*1e6)
sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024)
sock.bind(('127.0.0.1',40808));sock.setblocking(False)
running=True
def stop(*args):
    global running
    running=False
signal.signal(signal.SIGTERM,stop)
fifo=deque(maxlen=8);parts={};sequence=0;next_tick=time.monotonic()
history=Path(__file__).with_suffix('.history.jsonl').open('a',buffering=1)
normal=np.exp(2j*np.pi*151000*np.arange(12000)/600000)*.04
try:
    while running:
        for _ in range(64):
            try: packet=sock.recv(65536)
            except BlockingIOError: break
            if len(packet)<16 or packet[:4]!=b'PTXI': continue
            block,offset,total=struct.unpack_from('!III',packet,4)
            if not offset: parts={block:bytearray()}
            if block not in parts or offset!=len(parts[block]): continue
            parts[block].extend(packet[16:])
            if len(parts[block])==total: fifo.append(bytes(parts.pop(block)))
        if time.monotonic()>=next_tick:
            raw=fifo.popleft() if fifo else bytes(96000)
            iq=np.frombuffer(raw,dtype='<i2').reshape(-1,2)
            peak=float(np.max(np.linalg.norm(iq.astype(float),axis=1))/32768)
            history.write(json.dumps([time.monotonic(),rx_lo,peak])+'\n')
            if rx_lo==tx_lo: received=iq[::2].copy().tobytes()
            else:
                pcm=np.column_stack((normal.real,normal.imag))
                received=(pcm*32767).astype('<i2').tobytes()
            sock.sendto(received[:32768],('127.0.0.1',40809));sock.sendto(received[32768:],('127.0.0.1',40809))
            sequence+=1;sock.sendto(struct.pack('!4sIII',b'PTXS',sequence,len(fifo),8),('127.0.0.1',40810))
            next_tick+=.02
        time.sleep(.001)
finally:
    history.close();sock.close()
'''


def wait(predicate,timeout=6):
    deadline=time.monotonic()+timeout
    while not predicate() and time.monotonic()<deadline: time.sleep(.01)
    assert predicate(),'Timed out waiting for loop state'


def main():
    binary,config=map(Path,sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix='qo100-loops-') as temp:
        root=Path(temp);(root/'bin').mkdir()
        shutil.copy2(binary,root/'bin/qo100_backend')
        driver=root/'bin/pluto';driver.write_text(DRIVER);driver.chmod(0o755)
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
        conf=root/'backend.conf'
        conf.write_text(config.read_text().replace('gui_port = 40900',f'gui_port = {port}')
                        .replace('subscription_seconds = 8','subscription_seconds = 2'))
        log=(root/'backend.log').open('w')
        backend=subprocess.Popen([str(root/'bin/qo100_backend'),'--config',str(conf)],stdout=log,stderr=log)
        delivered=[]
        receiver=Receiver('127.0.0.1',port,audio_sink=delivered.append)
        def mode(value):
            request=receiver.set_loop(value)
            wait(lambda: receiver.loop_acked==request and receiver.loop_mode==value)
            assert not receiver.loop_error,receiver.loop_error
            wait(lambda: not receiver.tx_confirmed)
        try:
            wait(lambda: receiver.loop_mode==0 and receiver.channels[2].identity is not None and receiver.tx_identity is not None)
            identities=[receiver.channels[s].identity for s in (1,2)]
            mode(1);audio_start=time.monotonic();delivered.clear()
            receiver.set_tx(enabled=True);wait(lambda:receiver.tx_confirmed)
            session,epoch=receiver.tx_identity
            # Full signed PCM range must return byte-for-byte, including -32768.
            values=np.linspace(-32768,32767,240).astype('>i2')
            payload=HEADER.pack(b'QTR2',1,8,64,3,epoch,0,0,2400400000,12000,0,0,240,2,240,session)+values.tobytes()
            receiver.sock.send(payload)
            echo=receiver.echo_frames.get(timeout=3)
            assert echo.stream_id==4 and echo.sample_index==0 and echo.epoch==epoch
            np.testing.assert_array_equal((echo.samples*32768).astype('>i2'),values)
            receiver.sock.send(tx_audio(epoch,480,2400400000,session,np.ones(240)*.1))
            receiver.sock.send(tx_audio(epoch,240,2400400000,session,np.ones(240)*.2))
            reordered=[receiver.echo_frames.get(timeout=2) for _ in range(2)]
            assert [p.sample_index for p in reordered]==[480,240],'Valid reordered audio was discarded'
            assert all(p.stream_id==4 for p in delivered),'Normal RX mixed into audio echo'
            assert identities==[receiver.channels[s].identity for s in (1,2)],'Audio loop reset normal receivers'
            receiver.sock.send(payload) # Duplicate and stale epochs must not echo.
            receiver.sock.send(tx_audio(epoch-1,240,2400400000,session,np.ones(240)*.2))
            receiver.sock.send(tx_audio(epoch,240,2400400000,session^1,np.ones(240)*.2))
            time.sleep(.15)
            assert receiver.echo_frames.empty()
            audio_end=time.monotonic()
            receiver.set_frequency(10489760000);wait(lambda:receiver.applied_hz==10489760000)
            mode(2)
            assert receiver.loop_rx_lo==1290250000
            wait(lambda:receiver.applied_hz==10489900000)
            receiver.set_tx(frequency=2400360000)
            wait(lambda:receiver.tx_applied_hz==2400360000 and receiver.applied_hz==10489860000)
            delivered.clear();receiver.set_tx(enabled=True);wait(lambda:receiver.tx_confirmed)
            for number in range(75):
                receiver.send_microphone(.02*np.cos(2*np.pi*1000*(np.arange(240)+number*240)/12000))
                time.sleep(.02)
            decoded=np.concatenate([p.samples for p in delivered[-25:] if p.stream_id==1])
            tone=2*abs(np.mean(decoded*np.exp(-2j*np.pi*1000*np.arange(len(decoded))/12000)))
            assert .03<tone<.05,('HF USB loop amplitude with shared IQ headroom',tone)
            mode(0)
            wait(lambda:receiver.applied_hz==10489760000)
            assert receiver.loop_rx_lo==739750000
            generation=receiver.loop_generation
            request=receiver.loop_command[0]
            receiver.sock.send(loop_control(request,session,2))
            receiver.sock.send(loop_control(request-1,session,2))
            receiver.sock.send(loop_control(request+1,session^1,2))
            time.sleep(.15)
            assert receiver.loop_mode==0 and receiver.loop_generation==generation
            # A dropped client must restore the ordinary RX LO after subscription expiry.
            mode(2)
            receiver.stop.set();receiver.thread.join(timeout=1)
            time.sleep(2.8)
            history=[json.loads(line) for line in driver.with_suffix('.history.jsonl').read_text().splitlines()]
            assert history[-1][1]==739750000,'Expired subscription left RX on TX band'
            audio_records=[row for row in history if audio_start+.05<row[0]<audio_end]
            assert audio_records and max(row[2] for row in audio_records)==0,'Audio echo transmitted RF IQ'
            assert any(row[1]==1290250000 and row[2]>.025 for row in history),'HF test did not traverse TX IQ'
            print('Bit-exact 12-kHz echo, RF mute, source isolation, stale rejection; actual TX-IQ -> RX USB loop, '
                  'TX following, LO restore and lost-client restore verified')
        finally:
            receiver.close();backend.terminate()
            try: backend.wait(timeout=7)
            except subprocess.TimeoutExpired: backend.kill();backend.wait()
            log.close()
            print((root/'backend.log').read_text())
        assert backend.returncode==0


if __name__=='__main__': main()
