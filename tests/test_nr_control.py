"""RX1 NR settings, backend defaults, sequencing, and RX2/TX/FFT isolation."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'gui'))
from test_gui import Receiver
from protocol import HEADER, nr_control


def wait(predicate):
    deadline=time.monotonic()+3
    while not predicate() and time.monotonic()<deadline: time.sleep(.01)
    assert predicate(), 'Missing NR acknowledgment'


def main():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    with tempfile.TemporaryDirectory() as temp:
        conf=Path(temp)/'backend.conf'
        conf.write_text(Path(sys.argv[2]).read_text().replace('gui_port = 40900',f'gui_port = {port}')
                        .replace('rx1_nr_percent = 0','rx1_nr_percent = 43'))
        backend=subprocess.Popen([sys.argv[1],'--config',str(conf),'--source','synthetic'],
                                 stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        receiver=Receiver('127.0.0.1',port)
        def setting(percent):
            request=receiver.set_nr(percent)
            wait(lambda: receiver.nr_acked==request)
            assert not receiver.nr_error
            return receiver.nr_applied
        try:
            wait(lambda: receiver.nr_applied is not None and receiver.channels[2].received>0 and receiver.tx_identity is not None)
            assert receiver.nr_applied==43,'GUI overwrote configured NR startup value'
            identities=[receiver.channels[s].identity for s in (1,2)]
            tx_identity=receiver.tx_identity
            assert setting(71)==71
            request=receiver.nr_command[0];session=receiver.nr_session
            receiver.sock.send(nr_control(request,session,0))
            receiver.sock.send(nr_control(request-1,session,0))
            receiver.sock.send(nr_control(request+1,session^1,0))
            assert setting(None)==71,'Duplicate, stale or wrong-session setting applied'
            invalid=list(HEADER.unpack(nr_control(receiver.nr_command[0]+1,session,0)));invalid[4]=2
            receiver.sock.send(HEADER.pack(*invalid))
            assert setting(None)==71,'RX2 NR command accepted'
            with receiver.nr_lock:
                request=receiver.nr_command[0]+1
                invalid=list(HEADER.unpack(nr_control(request,session,0)));invalid[10]=101
                receiver.sock.send(HEADER.pack(*invalid))
                receiver.nr_command=(request,None);receiver.nr_acked=None
            wait(lambda: receiver.nr_acked==request)
            assert receiver.nr_error and receiver.nr_applied==71
            assert setting(0)==0
            assert setting(100)==100
            for value in range(1,100,7): receiver.set_nr(value)
            assert setting(60)==60
            assert identities==[receiver.channels[s].identity for s in (1,2)],'NR reset RX sample clock'
            assert receiver.tx_identity==tx_identity and not receiver.tx_confirmed,'NR changed TX/PTT'
            assert receiver.filter_applied==(3900,2050) and receiver.invalid_audio==0
            assert receiver.received>0 and receiver.channels[2].received>0
            assert setting(0)==0
            # A retune must reset the noise estimate without changing its configured strength.
            assert setting(80)==80
            epoch=receiver.channels[1].identity
            receiver.set_frequency(10489901000)
            wait(lambda: receiver.channels[1].identity!=epoch)
            assert setting(None)==80
            print('NR config/query/bypass/strength/stale/invalid/session controls; RX2/filter/TX/FFT isolation and retune verified')
        finally:
            receiver.close();backend.terminate()
            print(backend.communicate(timeout=5)[0])
        assert backend.returncode==0


if __name__=='__main__': main()
