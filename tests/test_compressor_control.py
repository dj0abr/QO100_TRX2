"""TX1 compressor query/set sequencing and isolation on the actual GUI UDP socket."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'gui'))
from test_gui import Receiver
from protocol import HEADER, compressor_control


def wait(predicate):
    deadline=time.monotonic()+3
    while not predicate() and time.monotonic()<deadline:
        time.sleep(.01)
    assert predicate(), 'Missing compressor acknowledgment'


def main():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    with tempfile.TemporaryDirectory() as temp:
        conf=Path(temp)/'backend.conf'
        conf.write_text(Path(sys.argv[2]).read_text().replace('gui_port = 40900',f'gui_port = {port}')
                        .replace('tx1_compressor_percent = 100','tx1_compressor_percent = 137.5'))
        backend=subprocess.Popen([sys.argv[1],'--config',str(conf),'--source','synthetic'],
                                 stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        receiver=Receiver('127.0.0.1',port)
        def setting(amount):
            request=receiver.set_compressor(amount)
            wait(lambda: receiver.compressor_acked==request)
            assert not receiver.compressor_error
            return receiver.compressor_applied
        try:
            wait(lambda: receiver.compressor_applied is not None and receiver.channels[2].identity is not None)
            assert receiver.compressor_applied==1375, 'GUI overwrote configured startup value'
            identities=[receiver.channels[s].identity for s in (1,2)]
            tx_identity=receiver.tx_identity
            assert setting(701)==701
            request=receiver.compressor_command[0];session=receiver.compressor_session
            receiver.sock.send(compressor_control(request,session,0))
            receiver.sock.send(compressor_control(request-1,session,0))
            receiver.sock.send(compressor_control(request+1,session^1,0))
            assert setting(None)==701, 'Duplicate, stale or wrong-session setting applied'
            invalid=list(HEADER.unpack(compressor_control(receiver.compressor_command[0]+1,session,0)))
            invalid[4]=2
            receiver.sock.send(HEADER.pack(*invalid))
            assert setting(None)==701, 'RX2 compressor command accepted'
            # Invalid but well-formed settings receive an ACK retaining the prior amount.
            with receiver.compressor_lock:
                request=receiver.compressor_command[0]+1
                invalid=list(HEADER.unpack(compressor_control(request,session,0)));invalid[10]=1401
                receiver.sock.send(HEADER.pack(*invalid))
                receiver.compressor_command=(request,None);receiver.compressor_acked=None
            wait(lambda: receiver.compressor_acked==request)
            assert receiver.compressor_error and receiver.compressor_applied==701
            assert setting(0)==0
            assert setting(1400)==1400
            for value in range(600,1401,37): receiver.set_compressor(value)
            assert setting(1000)==1000
            assert identities==[receiver.channels[s].identity for s in (1,2)], 'Compression reset RX'
            assert receiver.tx_identity==tx_identity and not receiver.tx_confirmed, 'Compression changed PTT/epoch'
            assert receiver.filter_applied==(3900,2050) and receiver.invalid_audio==0
            assert receiver.received>0 and receiver.channels[2].received>0
            print('Compressor config/query, fractional amount, bypass, rapid/stale/invalid/session controls; RX/filter/PTT isolation verified')
        finally:
            receiver.close();backend.terminate()
            print(backend.communicate(timeout=5)[0])
        assert backend.returncode==0


if __name__=='__main__': main()
