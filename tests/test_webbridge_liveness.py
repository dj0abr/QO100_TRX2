"""Native WebSocket liveness without JavaScript heartbeats or any Pluto/backend."""
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.request
from test_webbridge import port, wait
from ws_client import Client


def main():
    adapter, assets = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-ws-liveness-') as temp:
        log_path=Path(temp)/'webbridge.log'
        http_port=port()
        clients=[]
        with log_path.open('w') as log:
            process=subprocess.Popen([str(adapter),'--listen','127.0.0.1','--port',str(http_port),
                '--backend-port',str(port()),'--web-root',str(assets),'--heartbeat-seconds','2'],stdout=log,stderr=log)
            try:
                def available():
                    try:
                        return urllib.request.urlopen(f'http://127.0.0.1:{http_port}/health',timeout=.2).status==200
                    except OSError:
                        return False
                wait(available)
                quiet=Client(http_port,heartbeat=False)
                clients.append(quiet)
                role=quiet.role('operator')
                assert role['idle_seconds']==0,role
                # A background/receive-only tab sends no JSON and no activity.
                # Native pongs alone must keep it connected beyond the old 6 s.
                time.sleep(7.2)
                assert not quiet.closed,log_path.read_text()
                assert quiet.pings>=5,quiet.pings
                quiet.send({'type':'state'})
                quiet.state(lambda s:s.get('type')=='state')
                dead=Client(http_port,heartbeat=False,pong=False)
                clients.append(dead)
                dead.role('viewer')
                wait(lambda:dead.closed,4)
                assert dead.pings>0
                assert not quiet.closed
                quiet.role('operator')
                wait(lambda:'disconnected: heartbeat_timeout' in log_path.read_text())
                print('Receive-only connection survives without JS heartbeats/activity; '
                      'native ping/pong detects a dead peer independently; close reason logged.')
            finally:
                for client in clients:
                    client.close()
                process.terminate()
                process.wait(timeout=5)
                if sys.exc_info()[0]:
                    print(log_path.read_text())
            assert process.returncode==0


if __name__=='__main__':
    main()
