#!/usr/bin/env python3
"""Opt-in Tk/UDP mouse test on a synthetic backend; requires a display, never transmits RF.

Usage: python3 tests/test_gui_mouse.py [path/to/qo100_backend]
"""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import tkinter as tk
from types import SimpleNamespace

PROJECT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PROJECT/'gui'))
import test_gui
from tuning import uplink_hz, tx_offset_hz


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('backend',nargs='?',type=Path,default=PROJECT/'build/bin/qo100_backend')
    args=parser.parse_args()
    original_root,original_receiver=tk.Tk,test_gui.Receiver
    captured={};errors=[]
    class Receiver(original_receiver):
        def __init__(self,*args,**kwargs):
            super().__init__(*args,**kwargs);captured['receiver']=self

    class Root(original_root):
        def mainloop(self):
            def descendants(widget):
                for child in widget.winfo_children():
                    yield child
                    yield from descendants(child)
            widgets={w.winfo_name():w for w in descendants(self)}
            wide,zoom=widgets['wide_waterfall'],widgets['zoom_waterfall']
            r=captured['receiver']
            def point(widget,hz=None):
                geometry=widget.plot_geometry
                return dict(x=round((widget.winfo_width()-geometry.width)/2+geometry.x(hz if hz is not None else geometry.center_hz)),
                            y=round((widget.winfo_height()-geometry.height)/2+geometry.waterfall_top+20))
            def click(widget,hz=None):
                widget.event_generate('<Button-1>',**point(widget,hz))
            def wheel(widget,event='<Button-4>',**kwargs):
                widget.event_generate(event,**point(widget),**kwargs)
            def expect(rx,offset):
                assert r.applied_hz==rx and r.tx_applied_hz==uplink_hz(rx,offset),(r.applied_hz,r.tx_applied_hz,rx,offset)
                assert not r.tune_error and not r.tx_error and not r.tx_confirmed
                assert tx_offset_hz(r.applied_hz,r.tx_applied_hz)==offset
                assert f'{offset:+d} Hz' in widgets['tx_offset'].cget('text')
                assert r.channels[2].identity==captured['rx2_identity'],'RX2 reset by mouse'
            def initial():
                assert hasattr(wide,'plot_geometry') and hasattr(zoom,'plot_geometry')
                assert r.applied_hz==10489900000
                captured['rx2_identity']=r.channels[2].identity
                click(wide)  # Upper waterfall center: exact RF band midpoint.
            def scroll_up():
                captured['rx1_identity']=r.channels[1].identity
                captured['wheel_tx_command']=r.tx_command
                for _ in range(3): wheel(wide)
            def check_scroll():
                expect(10489750030,-30)
                assert r.channels[1].identity==captured['rx1_identity'],'Wheel reset RX1 audio stream'
                assert r.tx_command==captured['wheel_tx_command'],'Wheel changed TX'
                wheel(zoom,'<MouseWheel>',delta=240)
                wheel(zoom,'<Button-5>')
            def lower_click():
                # Keep the click an exact number of pixels from the retuned zoom center.
                captured['lower_rx']=zoom.plot_geometry.center_hz+10000
                click(zoom,captured['lower_rx'])
            def margins():
                captured['commands']=(r.channels[1].command,r.tx_command)
                # These coordinates are on metadata/background rather than plot data.
                wide.event_generate('<Button-1>',x=10,y=10)
                wheel_args=point(zoom);wheel_args['x']=5
                zoom.event_generate('<Button-4>',**wheel_args)
            def check_margins():
                assert captured['commands']==(r.channels[1].command,r.tx_command)
                self.geometry('1300x1080')
            def resized_click():
                assert wide.winfo_width()>1100
                click(wide,10489810000)
            def stale_zoom():
                # Two clicks before Tk redraws: use the currently visible zoom's
                # center, even though the first click has changed desired tuning.
                previous=zoom.plot_geometry.center_hz
                click(wide)
                click(zoom,previous+10000)
                captured['last_rx']=previous+10000
            def filter_upper():
                captured['filter_identity']=(r.channels[1].identity,r.channels[2].identity)
                widgets['rx1_filter_high'].set(2700)
            def check_filter(low,high):
                assert r.filter_applied==(high-low,(low+high)/2),r.filter_applied
                assert not r.filter_error and r.filter_acked==r.filter_command[0]
                assert (r.channels[1].identity,r.channels[2].identity)==captured['filter_identity'],'Filter reset audio'
                assert widgets['rx1_filter_low'].get()==low
                assert widgets['rx1_filter_high'].get()==high
            def filter_lower():
                check_filter(100,2700);widgets['rx1_filter_low'].set(301)
            def filter_limit():
                check_filter(301,2700);widgets['rx1_filter_low'].set(2600)
            def filter_full():
                check_filter(2200,2700)
                next(w for w in widgets.values() if isinstance(w,tk.Button) and w.cget('text')=='Full BW').invoke()
            steps=[initial,lambda:expect(10489750000,0),scroll_up,check_scroll,
                   lambda:expect(10489750040,-40),lower_click,lambda:expect(captured['lower_rx'],-40),
                   margins,check_margins,resized_click,lambda:expect(10489810000,-40),
                   stale_zoom,lambda:expect(captured['last_rx'],-40),
                   filter_upper,filter_lower,filter_limit,filter_full,lambda:check_filter(100,4000)]
            def run(index=0):
                try:
                    steps[index]()
                    if index+1<len(steps): self.after(350,lambda:run(index+1))
                    else:
                        try:
                            from PIL import ImageGrab
                            ImageGrab.grab(bbox=(self.winfo_rootx(),self.winfo_rooty(),self.winfo_rootx()+self.winfo_width(),self.winfo_rooty()+self.winfo_height())).save(captures/'mouse-gui.png')
                        except OSError: pass
                        print('Both waterfalls, linked TX on click, RX-only wheel, RX2 isolation, margins, resized widget and stale-zoom image mapping, independent filter cutoffs, minimum width and Full BW: passed')
                        self.tk.call(self.protocol('WM_DELETE_WINDOW'))
                except Exception as exc:
                    errors.append(exc);self.tk.call(self.protocol('WM_DELETE_WINDOW'))
            self.after(1800,run)
            super().mainloop()

    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    captures=PROJECT/'build/captures';captures.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory() as temp:
        config=Path(temp)/'backend.conf'
        config.write_text((PROJECT/'config/backend.conf').read_text().replace('gui_port = 40900',f'gui_port = {port}'))
        backend=subprocess.Popen([str(args.backend.resolve()),'--config',str(config),'--source','synthetic'],
                                 stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        tk.Tk=Root;test_gui.Receiver=Receiver
        try:
            gui_args=SimpleNamespace(host='127.0.0.1',port=port,no_audio=True,rx_frequency='10489.900000',
                                     rx2_frequency='10489.600000',tx_frequency='2400.400000',wheel_step_hz=10,
                                     audio_report=captures/'mouse-gui-report.json',screenshot=None,seconds=15)
            test_gui.gui(gui_args)
        finally:
            backend.terminate();output=backend.communicate(timeout=5)[0]
            (captures/'mouse-backend.log').write_text(output)
            tk.Tk=original_root;test_gui.Receiver=original_receiver
        if errors: raise errors[0]
        report=json.loads(gui_args.audio_report.read_text())
        assert report['tx_offset_hz']==-40 and report['tx_packets']==0 and report['invalid_audio']==0,report
        assert backend.returncode==0,output


if __name__=='__main__': main()
