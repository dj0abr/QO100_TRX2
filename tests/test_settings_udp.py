"""Settings transactions and backend-owned GUI defaults survive both GUI and backend restart."""
from pathlib import Path
import socket,subprocess,sys,tempfile,time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'gui'))
from test_gui import Receiver
from protocol import settings_control,settings_state,subscription


def wait(predicate,seconds=4):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if predicate(): return
        time.sleep(.01)
    assert predicate(),'Settings timeout'


def main():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    with tempfile.TemporaryDirectory() as temp:
        conf=Path(temp)/'backend.conf'
        conf.write_text(Path(sys.argv[2]).read_text().replace('gui_port = 40900',f'gui_port = {port}'))
        logs=[]
        def start():
            log=open(Path(temp)/f'backend-{len(logs)}.log','w');logs.append(log)
            return subprocess.Popen([sys.argv[1],'--config',str(conf),'--source','synthetic'],stdout=log,stderr=log)
        backend=start();receiver=Receiver('127.0.0.1',port)
        def setting(patch):
            wait(lambda:not receiver.settings.pending)
            request=receiver.settings.set(patch)
            wait(lambda:request in receiver.settings.results)
            return receiver.settings.results[request]
        try:
            wait(lambda:receiver.applied_hz is not None and receiver.settings.values is not None)
            assert receiver.settings.values['lnb_reference_hz']=='25000000'
            assert receiver.settings.values['pluto_rx_hz']=='739750000'
            assert setting({'rx_correction_hz':123,'tx_correction_hz':-456,'lnb_reference_hz':24999999,
                            'tx_attenuation_db':12.25,'gui_audio_gain':5.5,'gui_display_floor':-115,'gui_wheel_step_hz':25})[0]==0
            values=receiver.settings.values
            assert values['pluto_rx_hz']=='739750513' and values['pluto_tx_hz']=='1290249544'
            file=conf.with_suffix('.settings');assert file.exists()
            before=file.read_bytes()
            assert setting({'tx_attenuation_db':41,'gui_audio_gain':7})[0]==1
            assert file.read_bytes()==before,'Invalid transaction partially saved'
            # Session and request sequencing: a stale set must not override the accepted revision.
            sid=receiver.settings.session;rev=receiver.settings.revision
            receiver.sock.send(settings_control(0,sid,rev,{'gui_audio_gain':17}))
            receiver.sock.send(settings_control(receiver.settings.request+1,sid^1,rev,{'gui_audio_gain':17}))
            assert setting({'gui_nr_strength':77})[0]==0
            assert receiver.settings.values['gui_audio_gain']=='5.5'
            receiver.set_frequency(10489897327);receiver.set_frequency(10489610000,2);receiver.set_tx(frequency=2400397537)
            request=receiver.set_nr(70);wait(lambda:receiver.nr_acked==request)
            wait(lambda:receiver.applied_hz==10489897327 and receiver.tx_applied_hz==2400397537)
            time.sleep(.6)
            receiver.close();receiver=Receiver('127.0.0.1',port)
            wait(lambda:receiver.applied_hz==10489897327 and receiver.channels[2].applied_hz==10489610000)
            assert receiver.tx_command[1]==2400397537 and not receiver.tx_command[2]
            wait(lambda:receiver.nr_applied==70)
            assert receiver.settings.values['gui_audio_gain']=='5.5'
            backend.terminate();backend.wait(timeout=5)
            # The GUI remains open. Restart must discover the new session before sending stale settings.
            backend=start()
            old_session=receiver.settings.session
            wait(lambda:receiver.settings.session!=old_session,6)
            wait(lambda:receiver.nr_applied==70 and receiver.applied_hz==10489897327)
            assert receiver.settings.values['gui_wheel_step_hz']=='25'
            assert receiver.settings.values['tx_attenuation_db']=='12.25'
            assert receiver.settings.values['pluto_rx_hz']=='739750513'
            assert not receiver.tx_confirmed and receiver.loop_mode==0
            print('UDP settings: atomic patch, durable ACK, invalid/stale/session rejection, DSP persistence, GUI/backend restart and PTT-off verified')
        finally:
            receiver.close();backend.terminate();backend.wait(timeout=5)
            for log in logs:
                log.close();print(Path(log.name).read_text())
        assert backend.returncode==0


if __name__=='__main__': main()
