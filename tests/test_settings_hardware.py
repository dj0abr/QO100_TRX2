"""RF-free hardware-setting restart, rollback, USB selection and offline recovery."""
import json
from pathlib import Path
import shutil,socket,subprocess,sys,tempfile,time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'gui'))
from test_gui import Receiver
from test_loops_integration import DRIVER,wait


def main():
    binary,config=map(Path,sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-settings-driver-') as temp:
        root=Path(temp);(root/'bin').mkdir()
        shutil.copy2(binary,root/'bin/qo100_backend')
        driver=root/'bin/pluto'
        code=DRIVER.replace("sock=socket.socket", "if settings['PLUTO_ID']=='ip:192.0.2.1': raise SystemExit(1)\nsock=socket.socket",1)
        code=code.replace('[time.monotonic(),rx_lo,peak]',"[time.monotonic(),rx_lo,peak,tx_lo,settings['TX_GAIN'],settings['PLUTO_ID']]")
        driver.write_text(code);driver.chmod(0o755)
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
        conf=root/'backend.conf';conf.write_text(config.read_text().replace('gui_port = 40900',f'gui_port = {port}'))
        log=(root/'backend.log').open('w')
        backend=subprocess.Popen([str(root/'bin/qo100_backend'),'--config',str(conf)],stdout=log,stderr=log)
        receiver=Receiver('127.0.0.1',port)
        def setting(patch):
            wait(lambda:receiver.settings.values is not None and not receiver.settings.pending)
            request=receiver.settings.set(patch)
            wait(lambda:request in receiver.settings.results,10)
            return receiver.settings.results[request]
        def history(): return [json.loads(line) for line in driver.with_suffix('.history.jsonl').read_text().splitlines()]
        try:
            wait(lambda:receiver.settings.values is not None and receiver.settings.values['hardware_online']=='1')
            assert setting({'rx_correction_hz':123,'tx_correction_hz':-456,'lnb_reference_hz':24999999,'tx_attenuation_db':12.25})[0]==0
            assert receiver.settings.values['pluto_rx_hz']=='739750513'
            assert receiver.settings.values['pluto_tx_hz']=='1290249544'
            rows=history();assert any(r[1]==739750513 and r[3]==1290249544 and r[4]=='-2.250000' for r in rows)
            assert all(r[2]==0 for r in rows),'Settings transaction transmitted IQ'
            saved=conf.with_suffix('.settings').read_bytes()
            result=setting({'pluto_ip':'192.0.2.1'})
            assert result[0]==5,result
            assert receiver.settings.values['pluto_ip']=='192.168.10.25'
            assert conf.with_suffix('.settings').read_bytes()==saved,'Failed connection saved candidate'
            assert history()[-1][1]==739750513 and receiver.settings.values['hardware_online']=='1'
            assert setting({'pluto_connection':'usb'})[0]==0
            assert history()[-1][5]=='usb:'
            assert setting({'pluto_connection':'network'})[0]==0
            assert history()[-1][5]=='ip:192.168.10.25'
            receiver.close();backend.terminate();backend.wait(timeout=5)
            # Start with an unreachable device: settings must remain accessible.
            conf.with_suffix('.settings').unlink()
            conf.write_text(conf.read_text().replace('ip:192.168.10.25','ip:192.0.2.1'))
            backend=subprocess.Popen([str(root/'bin/qo100_backend'),'--config',str(conf)],stdout=log,stderr=log)
            receiver=Receiver('127.0.0.1',port)
            wait(lambda:receiver.settings.values is not None and receiver.settings.values['hardware_online']=='0')
            assert backend.poll() is None
            assert setting({'pluto_ip':'192.168.10.25'})[0]==0
            assert receiver.settings.values['hardware_online']=='1'
            assert not receiver.tx_confirmed
            print('Driver settings: LO/attenuation readback acknowledgment, rollback, USB/network selection, zero TX IQ and offline recovery passed')
        finally:
            receiver.close();backend.terminate();backend.wait(timeout=6);log.close()
            print((root/'backend.log').read_text())
        assert backend.returncode==0


if __name__=='__main__': main()
