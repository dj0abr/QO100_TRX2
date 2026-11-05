#!/usr/bin/env python3
"""Opt-in Pluto LO-switch test. Starts its own backend; never enables PTT.

Checks hardware RX/TX LO values, RX1 following and restoration. An actual
over-the-air/cabled self-listening test is left to the station operator.
"""
import json
from pathlib import Path
import subprocess
import sys
import time

PROJECT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PROJECT/'gui'))
from test_gui import Receiver


def wait(predicate,timeout=15):
    deadline=time.monotonic()+timeout
    while not predicate() and time.monotonic()<deadline: time.sleep(.03)
    assert predicate(),'Hardware loop state timeout'


def main():
    settings={}
    config=PROJECT/'config/backend.conf'
    for line in config.read_text().splitlines():
        line=line.split('#',1)[0].strip()
        if line:
            k,v=line.split('=',1);settings[k.strip()]=v.strip()
    captures=PROJECT/'build/captures';captures.mkdir(parents=True,exist_ok=True)
    log=(captures/'loop-hardware-backend.log').open('w')
    backend=subprocess.Popen([str(PROJECT/'build/bin/qo100_backend'),'--config',str(config)],stdout=log,stderr=log)
    receiver=Receiver('127.0.0.1',int(settings['gui_port']),rx_hz=int(settings['initial_rx_hz']),
                      rx2_hz=int(settings['initial_rx2_hz']),tx_hz=int(settings['initial_tx_hz']))
    def frequency(channel):
        return int(subprocess.check_output(['iio_attr','-u',settings['pluto_uri'],'-o','-c','ad9361-phy',channel,'frequency'],text=True,timeout=5).strip())
    def mode(value):
        request=receiver.set_loop(value)
        wait(lambda:receiver.loop_acked==request and receiver.loop_mode==value)
        assert receiver.loop_error=='' and not receiver.tx_confirmed
        return frequency('altvoltage0')
    try:
        wait(lambda:receiver.loop_mode==0 and receiver.received>5)
        normal=frequency('altvoltage0');tx=frequency('altvoltage1')
        assert normal==int(settings['pluto_rx_hz']) and tx==int(settings['pluto_tx_hz'])
        assert mode(1)==normal
        rf=mode(2)
        assert rf==tx and receiver.loop_rx_lo==tx
        expected=int(settings['rx_center_hz'])+int(settings['initial_tx_hz'])-int(settings['tx_center_hz'])
        wait(lambda:receiver.applied_hz==expected)
        receiver.set_tx(frequency=int(settings['initial_tx_hz'])+100)
        wait(lambda:receiver.applied_hz==expected+100)
        restored=mode(0)
        assert restored==normal
        wait(lambda:receiver.applied_hz==int(settings['initial_rx_hz']))
        assert receiver.tx_packets==0 and not receiver.tx_confirmed
        report=dict(normal_rx_lo=normal,tx_lo=tx,rf_loop_rx_lo=rf,restored_rx_lo=restored,
                    rf_rx1_followed_tx=True,tx_audio_packets=receiver.tx_packets,
                    invalid_audio=receiver.invalid_audio,tested_at_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
        (captures/'loop-hardware.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2))
    finally:
        receiver.close();backend.terminate()
        try: backend.wait(timeout=7)
        except subprocess.TimeoutExpired: backend.kill();backend.wait()
        log.close()
    assert backend.returncode==0,'See loop-hardware-backend.log'


if __name__=='__main__': main()
