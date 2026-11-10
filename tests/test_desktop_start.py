"""Real private synthetic stack and fake desktop openers; no RF or browser window."""
from pathlib import Path
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import urllib.request
from test_webbridge import port, private_config, wait

OPENER = '''#!/usr/bin/python3
import json,os,sys,urllib.request
from pathlib import Path
url=sys.argv[-1]
client=urllib.request.build_opener(urllib.request.ProxyHandler({}))
with client.open(url+'health',timeout=2) as reply: health=json.load(reply)
ready='Dieses Terminal offen lassen.' in Path(os.environ['QO100_TEST_START_LOG']).read_text()
with Path(os.environ['QO100_TEST_BROWSER_FILE']).open('a') as output:
    output.write(json.dumps({'args':sys.argv[1:],'health':health,'ready':ready})+'\\n')
sys.exit(int(os.environ.get('QO100_TEST_OPEN_FAIL','0')))
'''


def run_case(launcher, supervisor, binaries, defaults, assets, mode):
    with tempfile.TemporaryDirectory(prefix='qo100 desktop ') as temporary:
        root=Path(temporary);(root/'config').mkdir();(root/'build/bin').mkdir(parents=True)
        shutil.copy2(launcher,root/'start.sh');shutil.copy2(supervisor,root/'start_all.sh')
        for name in ('qo100_backend','qo100_webbridge','pluto'):
            (root/'build/bin'/name).symlink_to(binaries/name)
        backend_port,http=port(),port()
        text=private_config(defaults,backend_port).replace('external_decoders_enabled = 1','external_decoders_enabled = 0')
        assert 'external_decoders_enabled = 0' in text
        (root/'config/backend.conf').write_text(text)
        def bridge_config(path, endpoint):
            path.write_text(f'listen = 0.0.0.0\nport = {endpoint}\nbackend_port = {backend_port}\nweb_root = {assets}\n')
        bridge_config(root/'config/webbridge.conf',http)
        arguments=['--source','synthetic','--no-digital-audio']
        if mode=='gio':
            alternate=root/'alternate bridge.conf';http=port();bridge_config(alternate,http)
            arguments+=['--webbridge-config',str(alternate)]
        # Explicit PATH ensures fallback/missing-opener cases never launch a real browser.
        commands=root/'commands';commands.mkdir()
        for name in ('bash','python3','dirname','date','mkdir','grep','tail','sleep'):
            (commands/name).symlink_to(shutil.which(name))
        if mode!='missing':
            opener=commands/('gio' if mode=='gio' else 'xdg-open');opener.write_text(OPENER);opener.chmod(0o755)
        browser_log=root/'browser.jsonl';stack_log=root/'stack.log'
        env={**os.environ,'PATH':str(commands),'QO100_TEST_BROWSER_FILE':str(browser_log),
            'QO100_TEST_START_LOG':str(stack_log),'QO100_TEST_OPEN_FAIL':'1' if mode=='opener-fails' else '0'}
        reserved=None
        if mode=='startup-fails':
            reserved=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);reserved.bind(('127.0.0.1',backend_port))
        processes=set()
        with stack_log.open('w') as output:
            stack=subprocess.Popen([str(root/'start.sh'),*arguments],cwd='/tmp',env=env,stdout=output,stderr=output)
            try:
                if mode=='startup-fails':
                    assert stack.wait(timeout=8)!=0
                    assert 'Start abgebrochen:' in stack_log.read_text()
                    assert not browser_log.exists(),'Failed stack startup opened a browser'
                    return
                wait(lambda:'Dieses Terminal offen lassen.' in stack_log.read_text() or stack.poll() is not None,timeout=15)
                assert stack.poll() is None,stack_log.read_text()
                if mode=='missing':
                    wait(lambda:'Kein Browserstarter gefunden' in stack_log.read_text())
                    assert not browser_log.exists()
                else:
                    wait(browser_log.exists)
                    records=[json.loads(line) for line in browser_log.read_text().splitlines()]
                    assert len(records)==1,records
                    expected=['open'] if mode=='gio' else []
                    assert records[0]['args']==expected+[f'http://127.0.0.1:{http}/'],records
                    assert records[0]['health']=={'service':'qo100_webbridge'} and records[0]['ready'],records
                    if mode=='opener-fails':
                        wait(lambda:'Browser konnte nicht geoeffnet werden' in stack_log.read_text())
                assert stack.poll() is None,'Browser opener terminated the supervisor'
                client=urllib.request.build_opener(urllib.request.ProxyHandler({}))
                with client.open(f'http://127.0.0.1:{http}/health',timeout=2) as reply:
                    assert json.load(reply)['service']=='qo100_webbridge'
                logs=list((root/'build/run').glob('stack-*/backend.log'))
                assert len(logs)==1 and 'Source=synthetic;' in logs[0].read_text(),'Stack arguments were not forwarded'
                children=Path(f'/proc/{stack.pid}/task/{stack.pid}/children').read_text().split()
                for pid in children:
                    try: command=Path(f'/proc/{pid}/cmdline').read_bytes()
                    except FileNotFoundError: continue
                    if b'qo100_backend' in command or b'qo100_webbridge' in command: processes.add(int(pid))
                assert len(processes)==2,processes
                stop=signal.SIGINT if mode=='xdg' else signal.SIGTERM
                stack.send_signal(stop)
                assert stack.wait(timeout=12)==128+stop,stack_log.read_text()
                wait(lambda:not any(Path(f'/proc/{pid}').exists() for pid in processes),timeout=3)
            finally:
                if stack.poll() is None: stack.terminate();stack.wait(timeout=12)
                if reserved: reserved.close()
                if sys.exc_info()[0]: print(stack_log.read_text())


def main():
    inputs=list(map(Path,sys.argv[1:]))
    for mode in ('xdg','gio','opener-fails','missing','startup-fails'):
        run_case(*inputs,mode)
    print('Desktop start: ready-before-open, default browser, configured port, forwarded arguments, gio fallback, opener/startup failures and SIGINT/SIGTERM cleanup passed; no desktop browser or hardware accessed')


if __name__=='__main__':
    main()
