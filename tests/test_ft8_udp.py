"""Real FT8 waveform -> RX3 UDP -> FT8 service -> adapter -> two WebSocket clients."""
from datetime import datetime, timezone
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
import wave
import urllib.request
from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


def main():
    adapter, backend, receiver, generator, defaults, assets = map(Path,sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-ft8-') as temp:
        root=Path(temp);backend_port,http_port,audio_port,text_port=[port() for _ in range(4)]
        config=root/'backend.conf';config.write_text(private_config(defaults,backend_port))
        raw=root/'signal.raw'
        subprocess.run([str(generator),str(raw),'CQ K1ABC FN42','1500','0.2','-12'],check=True,timeout=10)
        payload=raw.read_bytes();assert len(payload)==360000
        with wave.open(str(Path(__file__).resolve().parents[1]/'external/ft8/tests/data/210703_133430.wav'),'rb') as sample:
            recorded=sample.readframes(180000)
        logs=[(root/name).open('w') for name in ['backend.log','adapter.log','receiver.log']]
        radio=subprocess.Popen([str(backend),'--config',str(config),'--source','synthetic'],stdout=logs[0],stderr=logs[0])
        proxy=Proxy(backend_port)
        # Replace only the synthetic RX3 audio with a known FT8 slot; all other backend traffic stays live.
        proxy.drop=lambda p:p[5]==5 and HEADER.unpack_from(p)[4]==5
        proxy.drop_once=False
        bridge=subprocess.Popen([str(adapter),'--listen','127.0.0.1','--port',str(http_port),'--backend-port',str(proxy.port),
            '--web-root',str(assets),'--rx3-decoder-audio-port',str(audio_port),'--rx3-decoder-text-port',str(text_port)],stdout=logs[1],stderr=logs[1])
        decoder=subprocess.Popen([str(receiver),'--port',str(audio_port),'--time-offset-ms','0'],stdout=logs[2],stderr=logs[2])
        clients=[]
        try:
            wait(lambda:'waiting for a complete UTC slot' in (root/'receiver.log').read_text())
            def available():
                try:
                    return urllib.request.urlopen(f'http://127.0.0.1:{http_port}/health',timeout=.2).status==200
                except OSError:
                    return False
            wait(available)
            owner=Client(http_port);clients.append(owner);owner.role('operator');state=owner.state()
            viewer=Client(http_port);clients.append(viewer);viewer.role('viewer',True);viewer.state()
            sid,epoch,frequency=state['session'],state['rx']['3']['epoch'],state['rx']['3']['frequency_hz']
            assert frequency>2**32,'Test must cover the 10-GHz downlink frequency'
            slot_utc=1700000010

            def feed(data,seconds,index=0,dial=frequency,drop=None):
                values=struct.unpack('<180000h',data)
                for start in range(0,180000,240):
                    if start==drop: continue
                    timestamp=seconds*10**9+(start+240)*10**9//12000
                    packet=HEADER.pack(b'QTR2',1,5,64,5,epoch,timestamp,index+start,dial,12000,0,0,240,2,240,sid)
                    packet+=struct.pack('!240h',*values[start:start+240])
                    proxy.socket.sendto(packet,proxy.peer)
                    if start%3840==0: time.sleep(.004)

            def text(client):
                return ''.join(s['text'] for s in list(client.messages) if isinstance(s,dict) and s.get('type')=='decoder_text')

            feed(payload,slot_utc)
            for client in clients:
                wait(lambda:'CQ K1ABC FN42' in text(client),timeout=15)
                line=next(line for line in text(client).splitlines() if 'CQ K1ABC FN42' in line)
                assert line.startswith(datetime.fromtimestamp(slot_utc,timezone.utc).strftime('%H%M%S')),line
                assert re.search(r'1500\.\d+\s+~ CQ K1ABC FN42',line),line
            # Replayed/late packets must not decode the same slot again.
            for client in clients: client.messages.clear()
            feed(payload,slot_utc);time.sleep(.2)
            assert not any(text(client) for client in clients),'Replayed slot was decoded'
            # A missing UDP packet invalidates its partial slot.
            feed(payload,slot_utc+15,index=180000,drop=60000);time.sleep(.2)
            assert not any(text(client) for client in clients),'Incomplete slot decoded'
            # Recover automatically on the next full slot and decode a real WSJT-X recording.
            feed(recorded,slot_utc+30,index=360000)
            for client in clients:
                wait(lambda:all(message in text(client) for message in ['W1FC F5BZB -08','CQ F5RXL IN94','K1JT HA0DU KN07']),timeout=20)
                assert not any(isinstance(p,bytes) and p[5]==5 and HEADER.unpack_from(p)[4]==5 for p in client.messages),'RX3 PCM leaked to browser'
            assert 'Discarded partial slot: audio sample gap' in (root/'receiver.log').read_text()
            assert decoder.poll() is None
            print('FT8 UDP: real generated message and WSJT-X recording decoded to operator/viewer; UTC labels, '
                  '64-bit QO100 RF, duplicate rejection, packet-loss recovery and no RX3 PCM in browser passed')
        finally:
            for client in clients: client.close()
            for process in [decoder,bridge,radio]:
                if process.poll() is None: process.terminate();process.wait(timeout=8)
            proxy.close()
            for log in logs: log.close()
            if sys.exc_info()[0]:
                print((root/'receiver.log').read_text());print((root/'adapter.log').read_text())
        assert decoder.returncode==0 and bridge.returncode==0 and radio.returncode==0


if __name__=='__main__':
    main()
