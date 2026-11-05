#!/usr/bin/env python3
"""Full GUI UDP -> backend -> local driver TX test, with an IIO-free driver double."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'gui'))
from test_gui import Receiver
from protocol import tx_audio, tx_control, HEADER, TX_STATE

DRIVER = r'''#!/usr/bin/python3
import json, signal, socket, struct, time
from collections import deque
from pathlib import Path
import numpy as np
sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024)
sock.bind(('127.0.0.1',40808));sock.setblocking(False)
queue=deque(maxlen=8);parts={};records=[];seq=0;running=True
def stop(*args):
    global running
    running=False
signal.signal(signal.SIGTERM,stop)
next_tick=time.monotonic()
reference={f:np.exp(-2j*np.pi*f*np.arange(24000)/1200000) for f in (151000,149000,150000)}
try:
    while running:
        for _ in range(64):
            try: p=sock.recv(65536)
            except BlockingIOError: break
            if len(p)<16 or p[:4]!=b'PTXI': continue
            block,off,total=struct.unpack_from('!III',p,4)
            if off==0: parts={block:bytearray()}
            if block not in parts or off!=len(parts[block]): continue
            parts[block].extend(p[16:])
            if len(parts[block])==total:
                queue.append(bytes(parts.pop(block)))
        if time.monotonic()>=next_tick:
            raw=queue.popleft() if queue else bytes(96000)
            a=np.frombuffer(raw,dtype='<i2').astype(np.float32).reshape(-1,2)/32768
            iq=a[:,0]+1j*a[:,1]
            records.append([time.monotonic(),float(np.max(np.abs(iq)))]+[float(abs(np.mean(iq*r))) for r in reference.values()])
            rx=bytes(48000)
            sock.sendto(rx[:32768],('127.0.0.1',40809));sock.sendto(rx[32768:],('127.0.0.1',40809))
            seq+=1;sock.sendto(struct.pack('!4sIII',b'PTXS',seq,len(queue),8),('127.0.0.1',40810))
            next_tick+=.02
        time.sleep(.001)
finally:
    Path(__file__).with_suffix('.json').write_text(json.dumps(records))
'''


def wait(predicate, timeout=4):
    deadline=time.monotonic()+timeout
    while not predicate() and time.monotonic()<deadline:
        time.sleep(.01)
    assert predicate(), 'Timed out waiting for state'


def main():
    binary, config=map(Path,sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix='qo100-tx-test-') as directory:
        root=Path(directory);bin_dir=root/'bin';bin_dir.mkdir()
        shutil.copy2(binary,bin_dir/'qo100_backend')
        driver=bin_dir/'pluto';driver.write_text(DRIVER);driver.chmod(0o755)
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as port_socket:
            port_socket.bind(('127.0.0.1',0));port=port_socket.getsockname()[1]
        conf=root/'backend.conf'
        conf.write_text(config.read_text().replace('gui_port = 40900',f'gui_port = {port}'))
        log=(root/'backend.log').open('w')
        backend=subprocess.Popen([str(bin_dir/'qo100_backend'),'--config',str(conf)],stdout=log,stderr=log)
        receiver=Receiver('127.0.0.1',port,rx_hz=10489900000)
        try:
            wait(lambda: receiver.tx_identity is not None)
            assert not receiver.tx_confirmed
            receiver.set_tx(frequency=2401000000,enabled=True)
            wait(lambda: bool(receiver.tx_error))
            assert not receiver.tx_confirmed
            receiver.set_tx(frequency=2400400000,enabled=True)
            wait(lambda: receiver.tx_confirmed)
            session,epoch=receiver.tx_identity
            on=time.monotonic()
            index=0
            for number in range(120):
                if number == 50:  # A lost UDP audio interval must not stall TX.
                    index += 240
                t=np.arange(240)+index
                receiver.send_microphone(.4*np.cos(2*np.pi*1000*t/12000))
                index+=240
                time.sleep(.02)
            no_audio=time.monotonic()
            time.sleep(.6)  # PTT remains asserted, but vanished audio must be silence.
            # Resume after the timeout; prefill again, rather than replay old sound.
            for _ in range(30):
                receiver.send_microphone(.4*np.cos(2*np.pi*1000*(np.arange(240)+index)/12000))
                index+=240;time.sleep(.02)
            receiver.set_tx(enabled=False)
            wait(lambda: not receiver.tx_confirmed)
            off=time.monotonic()
            # Stale control and PCM may never re-key a released PTT.
            receiver.sock.send(tx_control(receiver.tx_command[0]-1,2400400000,True,session))
            receiver.sock.send(tx_audio(epoch,index,2400400000,session,np.ones(240)*.4))
            time.sleep(.6)
            assert not receiver.tx_confirmed
            assert receiver.received>30 and receiver.channels[2].received>30
            # Renew PTT once, then stop the GUI control thread. The backend
            # must revoke the lease, including the epoch, after 1.5 seconds.
            receiver.set_tx(enabled=True)
            wait(lambda: receiver.tx_confirmed)
            lease_epoch=receiver.tx_identity[1]
            receiver.stop.set();receiver.thread.join(timeout=1)
            deadline=time.monotonic()+2.5
            lease_cleared=False
            while time.monotonic()<deadline:
                try: packet=receiver.sock.recv(65536)
                except socket.timeout: continue
                if len(packet)==64 and packet[5]==TX_STATE:
                    fields=HEADER.unpack(packet)
                    if not fields[11] and fields[7]!=lease_epoch:
                        lease_cleared=True;break
            assert lease_cleared, 'Lost control lease retained PTT'
        finally:
            receiver.close();backend.terminate();backend.wait(timeout=5);log.close()
        assert backend.returncode==0,(root/'backend.log').read_text()
        records=np.array(json.loads(driver.with_suffix('.json').read_text()))
        active=records[(records[:,0]>on+.7)&(records[:,0]<no_audio-.1)]
        assert len(active)>30
        assert np.median(active[:,2])>.19, (receiver.tx_packets,receiver.tx_error,(root/'backend.log').read_text(),active[:3].tolist())
        assert np.median(active[:,3])<.0002
        assert np.median(active[:,4])<.0002
        absent=records[(records[:,0]>no_audio+.4)&(records[:,0]<no_audio+.58)]
        released=records[records[:,0]>off+.3]
        assert len(absent)>3 and np.max(absent[:,1])==0,absent
        assert len(released)>3 and np.max(released[:,1])==0,released
        assert 'TX underruns=0' in (root/'backend.log').read_text()
        print('TX end-to-end: USB +151 kHz, LSB/carrier rejected; missing audio/PTT-off exact zero; stale packets rejected; RX1/RX2 live.')

if __name__=='__main__': main()
