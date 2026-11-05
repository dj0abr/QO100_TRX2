#!/usr/bin/env python3
"""Opt-in real Pluto LO/attenuation and persisted headless-start test; never enables PTT."""
import json
from pathlib import Path
import socket,subprocess,sys,tempfile,time

PROJECT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PROJECT/'gui'))
from test_gui import Receiver


def wait(predicate,timeout=10):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if predicate():return
        time.sleep(.05)
    assert predicate(),'Live settings timeout'


def main():
    capture=PROJECT/'build/captures';capture.mkdir(exist_ok=True)
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='qo100-settings-live-') as temp, (capture/'settings-live.log').open('w') as log:
        conf=Path(temp)/'backend.conf'
        conf.write_text((PROJECT/'config/backend.conf').read_text().replace('gui_port = 40900',f'gui_port = {port}'))
        def start():return subprocess.Popen([str(PROJECT/'build/bin/qo100_backend'),'--config',str(conf)],stdout=log,stderr=log)
        backend=start();receiver=Receiver('127.0.0.1',port)
        baseline=None
        def setting(patch):
            wait(lambda:not receiver.settings.pending)
            request=receiver.settings.set(patch)
            wait(lambda:request in receiver.settings.results)
            assert receiver.settings.results[request][0]==0,receiver.settings.results[request]
        def attr(channel,name):
            return float(subprocess.check_output(['iio_attr','-u',uri,'-o','-c','ad9361-phy',channel,name],text=True,timeout=5).split()[0])
        try:
            wait(lambda:receiver.settings.values is not None and receiver.settings.values['hardware_online']=='1')
            current=receiver.settings.values
            uri='ip:'+current['pluto_ip']
            baseline={k:current[k] for k in ('lnb_reference_hz','rx_correction_hz','tx_correction_hz','tx_attenuation_db')}
            setting({'lnb_reference_hz':24999999,'rx_correction_hz':123,'tx_correction_hz':-456,'tx_attenuation_db':12.25})
            expected_rx=int(receiver.settings.values['pluto_rx_hz']);expected_tx=int(receiver.settings.values['pluto_tx_hz'])
            assert abs(int(attr('altvoltage0','frequency'))-expected_rx)<=5
            assert abs(int(attr('altvoltage1','frequency'))-expected_tx)<=5
            assert abs(attr('voltage0','hardwaregain')+12.25)<.001
            actual_rx=int(attr('altvoltage0','frequency'));actual_tx=int(attr('altvoltage1','frequency'))
            assert not receiver.tx_confirmed and receiver.tx_packets==0
            receiver.close();backend.terminate();backend.wait(timeout=5)
            backend=start() # No GUI or subscription: stored hardware settings must already apply.
            time.sleep(3)
            assert backend.poll() is None
            assert abs(int(attr('altvoltage0','frequency'))-expected_rx)<=5
            assert abs(int(attr('altvoltage1','frequency'))-expected_tx)<=5
            assert abs(attr('voltage0','hardwaregain')+12.25)<.001
            receiver=Receiver('127.0.0.1',port)
            wait(lambda:receiver.settings.values is not None and receiver.channels[2].received>50)
            assert receiver.settings.values['lnb_reference_hz']=='24999999'
            setting(baseline)
            assert abs(int(attr('altvoltage0','frequency'))-int(receiver.settings.values['pluto_rx_hz']))<=5
            assert abs(attr('voltage0','hardwaregain')+float(baseline['tx_attenuation_db']))<.001
            result={'rx_lo_hz':expected_rx,'tx_lo_hz':expected_tx,'tx_attenuation_db':12.25,'actual_rx_lo_hz':actual_rx,'actual_tx_lo_hz':actual_tx,
                    'headless_restart_restored_settings':True,'gui_loaded_saved_settings':True,
                    'baseline_restored':True,'tx_audio_packets':receiver.tx_packets,
                    'invalid_audio':receiver.invalid_audio,'tested_at_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())}
            (capture/'settings-live.json').write_text(json.dumps(result,indent=2)+'\n')
            print(json.dumps(result,indent=2))
        finally:
            if baseline is not None and receiver.settings.ready and not receiver.settings.pending:
                try:setting(baseline)
                except Exception:pass
            receiver.close();backend.terminate();backend.wait(timeout=6)
        assert backend.returncode==0


if __name__=='__main__': main()
