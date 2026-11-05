"""Optional Chromium/ChromeDriver smoke test, with fake microphone and no Pluto."""
import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error
from test_webbridge import Proxy, port, wait, private_config


def main():
    adapter, backend, defaults, assets, capture = map(Path, sys.argv[1:])
    for binary in [adapter, backend]:
        assert binary.is_file() and os.access(binary, os.X_OK), f'Build first: {binary}'
    driver = shutil.which('chromedriver')
    assert driver and shutil.which('chromium'), 'Install chromium and chromium-driver for this optional test'
    session = None
    with tempfile.TemporaryDirectory(prefix='qo100-browser-') as temp:
        root = Path(temp)
        backend_port, http_port, driver_port = port(), port(), port()
        config = root/'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        logs = [(root/name).open('w') for name in ['backend.log', 'adapter.log', 'driver.log']]
        radio = subprocess.Popen([str(backend), '--config', str(config), '--source', 'synthetic'], stdout=logs[0], stderr=logs[0])
        proxy = Proxy(backend_port)
        bridge = subprocess.Popen([str(adapter), '--listen', '127.0.0.1', '--port', str(http_port),
                                   '--backend-port', str(proxy.port), '--web-root', str(assets)], stdout=logs[1], stderr=logs[1])
        driver_args = [driver, f'--port={driver_port}', '--allowed-ips=127.0.0.1']
        if os.environ.get('QO100_TEST_SKIP_DRIVER_VERSION_CHECK') == '1':
            driver_args.append('--disable-build-check')
        chrome = subprocess.Popen(driver_args, stdout=logs[2], stderr=logs[2])
        base = f'http://127.0.0.1:{driver_port}'
        def call(method, path, data=None):
            body = None if data is None else json.dumps(data).encode()
            request = urllib.request.Request(base+path, body, method=method, headers={'Content-Type': 'application/json'})
            try:
                result = json.load(urllib.request.urlopen(request, timeout=30))
            except urllib.error.HTTPError as e:
                raise AssertionError(e.read().decode()) from e
            assert not isinstance(result.get('value'), dict) or 'error' not in result['value'], result
            return result['value']
        def ready():
            try:
                return call('GET', '/status')['ready']
            except OSError:
                return False
        def execute(script, *args):
            return call('POST', f'/session/{session}/execute/sync', {'script': script, 'args': args})
        def click(id):
            element = call('POST', f'/session/{session}/element', {'using': 'css selector', 'value': '#'+id})
            identifier = element['element-6066-11e4-a52e-4f735466cecf']
            call('POST', f'/session/{session}/element/{identifier}/click', {})
        try:
            wait(ready)
            session = call('POST', '/session', {'capabilities': {'alwaysMatch': {
                'browserName': 'chrome', 'goog:loggingPrefs': {'browser': 'ALL'},
                'goog:chromeOptions': {'binary': shutil.which('chromium'), 'args': [
                    '--headless=new', '--no-sandbox', '--disable-dev-shm-usage', '--window-size=1280,1100',
                    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
                    '--autoplay-policy=no-user-gesture-required', '--mute-audio']}}}})['sessionId']
            call('POST', f'/session/{session}/url', {'url': f'http://127.0.0.1:{http_port}/'})
            wait(lambda: execute("return document.getElementById('role').textContent") == 'Bediener')
            wait(lambda: execute("return !document.getElementById('tune').disabled"))
            wait(lambda: execute("return document.getElementById('wide-frequency').textContent.length>0"))
            wait(lambda: execute("return document.getElementById('zoom-frequency').textContent.length>0"))
            click('audio')
            wait(lambda: execute("return document.getElementById('audio').textContent") == 'Audio aktiv')
            click('microphone')
            wait(lambda: execute("return document.getElementById('microphone').textContent") == 'Mikrofon aktiv')
            execute("const node=document.getElementById('loop');node.value='1';node.dispatchEvent(new Event('change'))")
            wait(lambda: any(p[5] == 13 and int.from_bytes(p[44:48], 'big') == 1 for p in proxy.log))
            time.sleep(.3)
            rect = execute("const r=document.getElementById('ptt').getBoundingClientRect();return {x:Math.round(r.x+r.width/2),y:Math.round(r.y+r.height/2)}")
            call('POST', f'/session/{session}/actions', {'actions': [{'type': 'pointer', 'id': 'mouse',
                 'parameters': {'pointerType': 'mouse'}, 'actions': [
                     {'type': 'pointerMove', 'x': rect['x'], 'y': rect['y'], 'duration': 0},
                     {'type': 'pointerDown', 'button': 0}, {'type': 'pause', 'duration': 1400},
                     {'type': 'pointerUp', 'button': 0}]}]})
            packets = [p for p in proxy.log if p[5] == 8]
            assert len(packets) > 10, f'No microphone stream; {len(packets)} TX packets, status: '+execute("return document.getElementById('status').textContent")
            assert any(any(p[64:]) for p in packets), 'Fake microphone produced no samples'
            wait(lambda: execute("return document.getElementById('tx-status').textContent") == 'Empfang')
            execute("const node=document.getElementById('loop');node.value='0';node.dispatchEvent(new Event('change'))")
            original = call('GET', f'/session/{session}/window')
            other = call('POST', f'/session/{session}/window/new', {'type': 'tab'})['handle']
            call('POST', f'/session/{session}/window', {'handle': other})
            call('POST', f'/session/{session}/url', {'url': f'http://127.0.0.1:{http_port}/'})
            wait(lambda: execute("return document.getElementById('role').textContent") == 'Zuschauer')
            assert execute("return document.getElementById('tune').disabled && document.getElementById('ptt').disabled")
            wait(lambda: execute("return document.getElementById('wide-frequency').textContent.length>0"))
            errors = call('POST', f'/session/{session}/log', {'type': 'browser'})
            assert not [e for e in errors if e['level'] == 'SEVERE' and '/favicon.ico' not in e['message']], errors
            call('POST', f'/session/{session}/window', {'handle': original})
            capture.parent.mkdir(parents=True, exist_ok=True)
            capture.write_bytes(base64.b64decode(call('GET', f'/session/{session}/screenshot')))
            print(f'Chromium: both waterfalls, AudioWorklet at 12 kHz, microphone -> TX UDP, PTT release, spectator UI passed; {len(packets)} microphone packets')
            print('Screenshot:', capture)
        finally:
            if session:
                call('DELETE', f'/session/{session}')
            for process in [chrome, bridge, radio]:
                process.terminate()
                process.wait(timeout=8)
            proxy.close()
            for log in logs:
                log.close()
            print((root/'adapter.log').read_text())
            if sys.exc_info()[0]:
                print((root/'driver.log').read_text())


if __name__ == '__main__':
    main()
