"""Real backend RX2 zoom -> byte-identical WS fragments, independent RX tuning/audio."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import numpy as np
from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'gui'))
from protocol import Assembler


def main():
    adapter,backend,defaults,assets=map(Path,sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-rx2-zoom-') as temp:
        root=Path(temp);backend_port,http_port,audio_port,text_port=[port() for _ in range(4)]
        config=root/'backend.conf';config.write_text(private_config(defaults,backend_port))
        logs=[(root/name).open('w') for name in ['backend.log','adapter.log']]
        radio=subprocess.Popen([str(backend),'--config',str(config),'--source','synthetic'],stdout=logs[0],stderr=logs[0])
        proxy=Proxy(backend_port);udp_rx2=set()
        def observe(packet):
            if packet[5]==2 and HEADER.unpack_from(packet)[4]==2: udp_rx2.add(packet)
            return False
        proxy.drop=observe
        tap=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);tap.bind(('127.0.0.1',audio_port));tap.setblocking(False)
        bridge=subprocess.Popen([str(adapter),'--listen','127.0.0.1','--port',str(http_port),'--backend-port',str(proxy.port),
            '--web-root',str(assets),'--decoder-audio-port',str(audio_port),'--decoder-text-port',str(text_port)],stdout=logs[1],stderr=logs[1])
        clients=[]
        try:
            def available():
                try: return urllib.request.urlopen(f'http://127.0.0.1:{http_port}/health',timeout=.2).status==200
                except OSError: return False
            wait(available)
            owner=Client(http_port);clients.append(owner);owner.role('operator');initial=owner.state()
            viewer=Client(http_port);clients.append(viewer);viewer.role('viewer',True);viewer.state()
            positions={c:0 for c in clients}
            assemblers={c:{s:Assembler(s) for s in (0,1,2)} for c in clients}
            frames={c:{s:[] for s in (0,1,2)} for c in clients}
            received_rx2={c:set() for c in clients};audio={1:[],2:[]}
            def pump():
                while True:
                    try: p,_=tap.recvfrom(65536)
                    except BlockingIOError: break
                    f=HEADER.unpack_from(p)
                    assert len(p)==544 and (f[2],f[4],f[9])==(5,2,12000),f
                    audio[2].append(f)
                for c in clients:
                    messages=list(c.messages)
                    for p in messages[positions[c]:]:
                        if not isinstance(p,bytes): continue
                        f=HEADER.unpack_from(p)
                        if f[2]==2:
                            assert f[4] in (0,1,2),'Invalid FFT stream reached WS'
                            if f[4]==2:
                                assert p in udp_rx2,'RX2 FFT changed between backend UDP and WS'
                                received_rx2[c].add(p)
                            frame=assemblers[c][f[4]].push(p)
                            if frame is not None: frames[c][f[4]].append(frame)
                        elif f[2]==5:
                            assert f[4]!=2,'RX2 PCM leaked to browser'
                            if c is owner and f[4]==1: audio[1].append(f)
                    positions[c]=len(messages)
            def until(predicate):
                def check(): pump();return predicate()
                wait(check,timeout=4)
            def latest(stream,center):
                return next((f for f in reversed(frames[owner][stream]) if f.center_hz==center),None)
            request=0
            def tune(stream,hz):
                nonlocal request
                request+=1;owner.send({'type':'activity'})
                owner.send({'type':'tune','id':request,'stream':stream,'frequency_hz':hz})
                ack=owner.wait(lambda s:isinstance(s,dict) and s.get('type')=='ack' and s.get('id')==request)
                assert ack['status']==0,ack
                owner.state(lambda s:s.get('rx',{}).get(str(stream),{}).get('frequency_hz')==hz)
            rx1,rx2=10489748500,10489600000
            tune(1,rx1);tune(2,rx2)
            until(lambda:all(len(frames[c][s])>=4 for c in clients for s in (0,1,2)))
            for c in clients:
                frame=frames[c][2][-1]
                assert frame.stream_id==2 and frame.center_hz==rx2 and frame.sample_rate==75000 and len(frame.db)==8192
            def peak(frame): return (np.argmax(frame.db)-len(frame.db)//2)*frame.sample_rate/len(frame.db)
            assert abs(peak(latest(2,rx2)))<10,'RX2 FFT is not centered on RX2 carrier'
            first1,first2=latest(1,rx1),latest(2,rx2)
            # RX2 moves its own FFT center; its bin frequencies follow the known test signal.
            tune(2,rx2+1200)
            until(lambda:latest(2,rx2+1200) is not None and latest(2,rx2+1200).sample_index>first2.sample_index+24576)
            moved=latest(2,rx2+1200)
            assert abs(peak(moved)+1200)<10,(peak(moved),moved.center_hz)
            assert latest(1,rx1).sample_index>first1.sample_index,'RX2 tuning reset/stopped RX1 zoom'
            assert moved.frame_id>first2.frame_id,'RX2 retune reset FFT frame sequence'
            # The reverse direction remains independent, too.
            tune(1,rx1+200)
            until(lambda:latest(1,rx1+200) is not None and latest(2,rx2+1200).sample_index>moved.sample_index+16384)
            assert abs(peak(latest(2,rx2+1200))+1200)<10,'RX1 tuning changed RX2 spectrum'
            until(lambda:len(audio[1])>60 and len(audio[2])>60)
            for stream,packets in audio.items():
                assert all((p[15],p[5])==(initial['session'],initial['rx'][str(stream)]['epoch']) for p in packets),'Audio epoch changed'
                assert all(b[7]==a[7]+240 for a,b in zip(packets,packets[1:])),'Audio sample timeline interrupted'
            for c in clients:
                assert all(a.invalid_packets==0 for a in assemblers[c].values())
                assert all(b.frame_id>a.frame_id and b.sample_index>a.sample_index for s in (1,2) for a,b in zip(frames[c][s],frames[c][s][1:])), 'Zoom timeline reset'
            assert received_rx2[owner]&received_rx2[viewer],'Operator/viewer did not share RX2 spectrum fragments'
            assert not owner.state()['tx']['ptt']
            print('RX2 zoom: 8192 bins/75 kHz, carrier and retuned tone position, full frames at operator/viewer, '
                  'byte-identical UDP/WS, independent tuning/frame clocks, continuous dual PCM and decoder-only RX2 audio passed')
        finally:
            for c in clients: c.close()
            for process in [bridge,radio]:
                if process.poll() is None: process.terminate();process.wait(timeout=6)
            proxy.close();tap.close()
            for log in logs: log.close()
            if sys.exc_info()[0]: print((root/'backend.log').read_text());print((root/'adapter.log').read_text())
        assert bridge.returncode==0 and radio.returncode==0


if __name__=='__main__': main()
