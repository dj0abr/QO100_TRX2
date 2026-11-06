"""Private synthetic backend -> RX2 UDP decoder -> plain text -> operator/viewer WS."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


def main():
    adapter, backend, dummy, defaults, assets = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-decoder-') as temp:
        root = Path(temp)
        backend_port, http_port, audio_port, text_port = [port() for _ in range(4)]
        config = root/'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        logs = [(root/name).open('w') for name in ['backend.log', 'adapter.log', 'dummy.log', 'dummy-text.log']]
        radio = subprocess.Popen([str(backend), '--config', str(config), '--source', 'synthetic'], stdout=logs[0], stderr=logs[0])
        proxy = Proxy(backend_port)
        bridge = subprocess.Popen([str(adapter), '--listen', '127.0.0.1', '--port', str(http_port),
                                   '--backend-port', str(proxy.port), '--web-root', str(assets),
                                   '--decoder-audio-port', str(audio_port), '--decoder-text-port', str(text_port)],
                                  stdout=logs[1], stderr=logs[1])
        decoder = None
        clients = []
        tap = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        tap.settimeout(.2)
        try:
            def ready():
                try:
                    return urllib.request.urlopen(f'http://127.0.0.1:{http_port}/health', timeout=.2).status == 200
                except OSError:
                    return False
            wait(ready)
            # The decoder may start later than the adapter. It must recover from local UDP ICMP errors.
            time.sleep(.3)
            tap.bind(('127.0.0.1', audio_port))

            def audio(frequency=None):
                deadline = time.monotonic()+3
                while time.monotonic()<deadline:
                    try:
                        packet, source = tap.recvfrom(65536)
                    except socket.timeout:
                        continue
                    f = HEADER.unpack_from(packet)
                    assert len(packet)==544 and f[:5]==(b'QTR2', 1, 5, 64, 2),f
                    assert (f[9], f[12], f[13], f[14])==(12000, 240, 2, 240),f
                    assert source==('127.0.0.1', text_port),source
                    assert any(packet[64:]),'Only silence arrived instead of synthetic RX2 signal'
                    if frequency is None or f[8]==frequency:
                        return packet
                raise AssertionError('No RX2 PCM at decoder port')

            previous = HEADER.unpack_from(audio())
            for _ in range(8):
                current = HEADER.unpack_from(audio())
                assert (current[5], current[15])==(previous[5], previous[15])
                assert current[7]==previous[7]+240,'Sample timeline changed while routing'
                previous = current
            # Routing runs even without an attached browser.
            owner, viewer = Client(http_port), Client(http_port)
            clients += [owner, viewer]
            owner.role('operator');viewer.role('viewer', True)
            owner.state();viewer.state()
            for client in clients:
                client.wait(lambda p:isinstance(p,bytes) and p[5]==5 and HEADER.unpack_from(p)[4]==1)
                client.wait(lambda p:isinstance(p,bytes) and p[5]==2)

            def output(text):
                tap.sendto(text.encode('utf-8'), ('127.0.0.1', text_port))
                for client in clients:
                    reply = client.wait(lambda s:isinstance(s,dict) and s.get('type')=='decoder_text' and s.get('text')==text)
                    assert reply['stream']==2,reply

            output('abc <b>plain text</b> äöü\n')
            output('{"type":"tx","frequency_hz":2400400000,"ptt":true}')
            assert not any(p[5]==6 and HEADER.unpack_from(p)[13] for p in proxy.log),'Decoder text became a TX command'
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stranger:
                stranger.sendto(b'WRONG_SOURCE', ('127.0.0.1', text_port))
            tap.sendto(b'\xff\xfe', ('127.0.0.1', text_port))  # invalid UTF-8
            tap.sendto(b'OVERSIZE'*600, ('127.0.0.1', text_port))
            output('valid text after malformed datagrams')
            assert not any(isinstance(s,dict) and (s.get('text')=='WRONG_SOURCE' or s.get('text','').startswith('OVERSIZE'))
                           for client in clients for s in client.messages)
            # Browser tuning still controls RX2; the decoder receives the new carrier metadata.
            owner.send({'type':'tune','id':91,'stream':2,'frequency_hz':10489610000})
            owner.wait(lambda s:isinstance(s,dict) and s.get('type')=='ack' and s.get('id')==91 and s.get('status')==0)
            audio(10489610000)
            for client in clients:
                assert not any(isinstance(p,bytes) and p[5]==5 and HEADER.unpack_from(p)[4]==2 for p in client.messages), 'RX2 PCM leaked to browser'
            # Replace the test decoder with the real executable, on the same independent interface.
            tap.close()
            time.sleep(.2)
            for client in clients:
                client.messages.clear()
            decoder = subprocess.Popen([str(dummy), '--port', str(audio_port), '--interval-ms', '60'], stdout=logs[3], stderr=logs[2])
            def letters(client):
                return ''.join(s['text'] for s in list(client.messages) if isinstance(s,dict) and s.get('type')=='decoder_text')
            wait(lambda:letters(owner).startswith('abcdef\n') and letters(viewer).startswith('abcdef\n'), timeout=4)
            assert decoder.poll() is None,'Dummy stopped unexpectedly'
            assert not any(isinstance(p,bytes) and p[5]==5 and HEADER.unpack_from(p)[4]==2 for client in clients for p in client.messages)
            radio.terminate();radio.wait(timeout=5)
            time.sleep(.2)  # Drain the last in-flight packet/character.
            for client in clients:
                client.messages.clear()
            time.sleep(.35)
            assert not letters(owner) and not letters(viewer),'Dummy produced text without receiving samples'
            print('RX2: exact 12-kHz PCM framing/timeline, decoder late start/restart, headless routing, UTF-8 text to operator/viewer, '
                  'source/size/UTF-8 checks, text command isolation, tuning, no browser RX2 PCM and real abcdef dummy passed')
        finally:
            tap.close()
            for client in clients:
                client.close()
            for process in [decoder, bridge, radio]:
                if process and process.poll() is None:
                    process.terminate();process.wait(timeout=6)
            proxy.close()
            for log in logs:
                log.close()
            if sys.exc_info()[0]:
                print((root/'adapter.log').read_text())
                print((root/'dummy.log').read_text())
        assert bridge.returncode==0 and radio.returncode==0
        assert decoder.returncode==0


if __name__=='__main__':
    main()
