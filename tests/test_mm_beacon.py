"""Real RX5 DSP/decoder/HTTP/WS chain on private synthetic transports; no hardware."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import time
import urllib.request
import numpy as np
from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


def main():
    bridge_bin, backend_bin, worker, fixture, rx_chain, probe, defaults, assets = map(lambda p:Path(p).resolve(),sys.argv[1:])
    sys.path.insert(0,str(worker.parents[1]/'external/mmdecoder'))
    sys.path.insert(0,str(worker.parents[1]/'external/mmdecoder/tests'))
    from data_collector import Collector
    from test_data_collector import dx, file_frames
    raw = subprocess.check_output([str(fixture),'400','clean','35'])  # 500-Hz pilot, 1900-Hz data center
    pcm = subprocess.run([str(rx_chain)],input=raw,capture_output=True,check=True).stdout
    assert len(pcm)>12000*8*2
    with tempfile.TemporaryDirectory(prefix='qo100-mm-test-') as temp:
        root=Path(temp);data=root/'data'
        seed=Collector(data)
        seed.ingest(dx())
        for frame in file_frames(text='~B:1:Test~\n<h3>Test-Bulletin</h3>\n<b>Plain text</b>'):
            seed.ingest(frame)
        seed.close()  # Never open a second Collector while the worker is running.
        backend_port, web_port, mm_port, mm_http=[port() for _ in range(4)]
        config=root/'backend.conf';config.write_text(private_config(defaults,backend_port))
        logs=[(root/name).open('w') for name in ('backend.log','bridge.log','worker.log')]
        radio=subprocess.Popen([str(backend_bin),'--config',str(config),'--source','synthetic'],stdout=logs[0],stderr=logs[0])
        proxy=Proxy(backend_port)
        position=0
        def inject(packet):
            nonlocal position
            h=HEADER.unpack_from(packet)
            if h[2]!=5 or h[4]!=8: return False
            if position+480>len(pcm): position=0
            payload=np.frombuffer(pcm[position:position+480],dtype='<i2').astype('>i2').tobytes()
            position+=480
            proxy.socket.sendto(packet[:64]+payload,proxy.peer)
            return True
        proxy.drop,proxy.drop_once=inject,False
        def start_worker():
            return subprocess.Popen([sys.executable,str(worker),
                '--probe',str(probe),'--port',str(mm_port),'--http-port',str(mm_http),'--data-dir',str(data)],stdout=logs[2],stderr=logs[2])
        decoder=start_worker()
        bridge=subprocess.Popen([str(bridge_bin),'--listen','127.0.0.1','--port',str(web_port),
            '--backend-port',str(proxy.port),'--web-root',str(assets),
            '--rx5-decoder-audio-port',str(mm_port),'--mm-http-port',str(mm_http)],stdout=logs[1],stderr=logs[1])
        clients=[]
        def status():
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{mm_http}/v1/status.json',timeout=.5) as response:
                    return json.load(response)
            except OSError: return {}
        try:
            wait(lambda:status().get('last_sequence',0)>=2)
            def available():
                try: return urllib.request.urlopen(f'http://127.0.0.1:{web_port}/health',timeout=.3).status==200
                except OSError: return False
            wait(available)
            owner=Client(web_port);clients.append(owner);owner.role('operator')
            initial=owner.state(lambda s:s.get('ready') and '5' in s['rx'])
            assert initial['rx']['5']['frequency_hz']==10489993000
            def text(client,channel,fragment):
                value=client.wait(lambda s:isinstance(s,dict) and s.get('type')=='decoder_text'
                    and s.get('stream')==5 and s.get('channel')==channel and fragment in s.get('text',''),timeout=15)
                assert value['replace'] is True
                return value['text']
            text(owner,'dx-cluster','VK9TEST')
            assert '<h3>' not in text(owner,'bulletin','Test-Bulletin')
            text(owner,'cw-skimmer','TEST CW-SKIMMER RX VALIDATION')
            wait(lambda:status().get('receiver',{}).get('locked'),timeout=12)
            owner.send({'type':'mm_diagnostics','enabled':True})
            diagnostics=owner.wait(lambda s:isinstance(s,dict) and s.get('type')=='mm_diagnostics' and s.get('audio_fresh')
                and s.get('receiver',{}).get('locked') and s.get('pilot_hz') is not None,timeout=5)
            assert abs(diagnostics['pilot_hz']-500)<1,diagnostics['pilot_hz']
            assert len(diagnostics['spectrum']['dbfs'])==768 and 0<len(diagnostics['points'])<=480
            assert diagnostics['frequency_hz']==10489993000 and diagnostics['pilot_search_hz']==[20,900]
            viewer=Client(web_port);clients.append(viewer);viewer.role('viewer')
            for channel,fragment in [('dx-cluster','VK9TEST'),('bulletin','Test-Bulletin'),('cw-skimmer','TEST CW-SKIMMER')]:
                text(viewer,channel,fragment)  # cached snapshots reach a newly joined browser
            viewer.send({'type':'mm_diagnostics','enabled':True})
            viewer.wait(lambda s:isinstance(s,dict) and s.get('type')=='mm_diagnostics',timeout=3)
            owner.send({'type':'mm_diagnostics','enabled':False})
            time.sleep(.4);owner.messages.clear();viewer.messages.clear()
            viewer.wait(lambda s:isinstance(s,dict) and s.get('type')=='mm_diagnostics',timeout=3)
            assert not any(isinstance(s,dict) and s.get('type')=='mm_diagnostics' for s in owner.messages)
            # A decoder restart retains the archive and leaves RX1/RX2/session untouched.
            sequence=status()['last_sequence']
            decoder.terminate();assert decoder.wait(timeout=8)==0
            for client in clients:
                client.send({'type':'state'});state=client.state(lambda s:s.get('ready'))
                assert state['session']==initial['session'] and not state['tx']['ptt']
                assert not any(isinstance(p,bytes) and p[5]==5 and HEADER.unpack_from(p)[4]==8 for p in client.messages)
            viewer.send({'type':'mm_diagnostics','enabled':False})
            decoder=start_worker()
            wait(lambda:status().get('last_sequence',0)>=sequence)
            assert (data/'receiver.sqlite3').is_file()
            print('RX5: real IQ/USB with 500-Hz pilot -> AFC/frame decode -> persistent texts and subscribed diagnostics, spectrum/constellation, viewer isolation, worker restart and no browser PCM passed')
        finally:
            for client in clients: client.close()
            for process in (decoder,bridge,radio):
                if process.poll() is None: process.terminate();process.wait(timeout=10)
            proxy.close()
            for log in logs:log.close()
            if sys.exc_info()[0]:
                for name in ('backend.log','bridge.log','worker.log'):print((root/name).read_text())


if __name__=='__main__':main()
