"""Real browser + private synthetic backend. Does not connect to the station."""
from pathlib import Path
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error
from test_webbridge import Proxy, port, wait, private_config
from ws_client import Client


def main():
    adapter, backend, defaults, assets, capture = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-webgui-') as temp:
        root = Path(temp)
        backend_port, http_port, driver_port = port(), port(), port()
        decoder_audio_port, decoder_text_port = port(), port()
        decoder_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        decoder_socket.bind(('127.0.0.1', decoder_audio_port))
        config = root / 'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        logs = [(root / name).open('w') for name in ['backend.log', 'adapter.log', 'driver.log']]
        def start_backend():
            return subprocess.Popen([str(backend), '--config', str(config), '--source', 'synthetic'], stdout=logs[0], stderr=logs[0])
        radio = start_backend()
        proxy = Proxy(backend_port)
        bridge = subprocess.Popen([str(adapter), '--listen', '127.0.0.1', '--port', str(http_port), '--backend-port', str(proxy.port), '--web-root', str(assets), '--decoder-audio-port', str(decoder_audio_port), '--decoder-text-port', str(decoder_text_port)], stdout=logs[1], stderr=logs[1])
        driver = '/usr/bin/chromedriver' if Path('/usr/bin/chromedriver').is_file() else shutil.which('chromedriver')
        driver_args = [driver, f'--port={driver_port}', '--allowed-ips=127.0.0.1']
        if os.environ.get('QO100_TEST_SKIP_DRIVER_VERSION_CHECK') == '1':
            driver_args.append('--disable-build-check')
        chrome = subprocess.Popen(driver_args, stdout=logs[2], stderr=logs[2])
        session = None
        observer = None
        def call(method, path, data=None):
            request = urllib.request.Request(f'http://127.0.0.1:{driver_port}'+path, json.dumps(data).encode() if data is not None else None, method=method, headers={'Content-Type':'application/json'})
            try:
                value = json.load(urllib.request.urlopen(request, timeout=30))['value']
            except urllib.error.HTTPError as e:
                raise AssertionError(e.read().decode()) from e
            assert not isinstance(value, dict) or 'error' not in value, value
            return value
        def execute(script, *args):
            return call('POST', f'/session/{session}/execute/sync', {'script':script,'args':args})
        def cdp(cmd, params):
            return call('POST', f'/session/{session}/goog/cdp/execute', {'cmd':cmd,'params':params})
        def click(selector):
            element = call('POST', f'/session/{session}/element', {'using':'css selector','value':selector})
            call('POST', f'/session/{session}/element/{element["element-6066-11e4-a52e-4f735466cecf"]}/click', {})
        def changed(selector, value):
            execute('const n=document.querySelector(arguments[0]);n.value=arguments[1];n.dispatchEvent(new Event("input",{bubbles:true}));n.dispatchEvent(new Event("change",{bubbles:true}));', selector, str(value))
        def hz(channel):
            return int(execute('return document.getElementById(arguments[0]).dataset.hz', channel))
        def ready():
            try:
                return call('GET','/status')['ready']
            except OSError:
                return False
        def snapshot(predicate=lambda s:s.get('ready')):
            observer.send({'type':'state'})
            return observer.state(predicate)
        def expect_frequency(stream, frequency):
            return snapshot(lambda s: s.get('rx',{}).get(str(stream),{}).get('frequency_hz') == frequency)
        def plot(selector, fraction, button=0, touch=False, press_ms=0):
            bounds=execute('const r=document.querySelector(arguments[0]).getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height}',selector)
            x,y=round(bounds['x']+bounds['width']*fraction),round(bounds['y']+bounds['height']*.5)
            if touch:
                cdp('Input.dispatchTouchEvent',{'type':'touchStart','touchPoints':[{'x':x,'y':y}]})
                cdp('Input.dispatchTouchEvent',{'type':'touchEnd','touchPoints':[]})
            else:
                call('POST',f'/session/{session}/actions',{'actions':[{'type':'pointer','id':'mouse','parameters':{'pointerType':'mouse'},'actions':[
                    {'type':'pointerMove','x':x,'y':y,'duration':0},{'type':'pointerDown','button':button},{'type':'pause','duration':press_ms},{'type':'pointerUp','button':button}]}]})
            return (x-bounds['x'])/bounds['width']
        def wheel(selector, shift=False, ctrl=False):
            execute('document.querySelector(arguments[0]).dispatchEvent(new WheelEvent("wheel",{deltaY:-100,shiftKey:arguments[1],ctrlKey:arguments[2],bubbles:true,cancelable:true}))',selector,shift,ctrl)
        def key(action, value=' '):
            call('POST',f'/session/{session}/actions',{'actions':[{'type':'key','id':'keyboard','actions':[{'type':action,'value':value}]}]})
        def expect_ptt(enabled):
            observer.messages.clear()
            return snapshot(lambda s:s.get('tx',{}).get('ptt') is enabled)
        def zoom_follows_rx():
            return execute('''const zoom=document.getElementById('zoom-display'),rx=zoom.dataset.receiver;
                const label=zoom.querySelector('.frequency-axis span');
                return zoom.dataset.stream===rx && !!label && Math.abs(parseFloat(label.textContent)*1e6-(Number(document.getElementById('rx'+rx).dataset.hz)-25000))<.5;''')
        def mark_history():
            wait(zoom_follows_rx)
            execute('''for (const id of ['wide','zoom']) {
                const c=document.getElementById(id),ctx=c.getContext('2d');
                ctx.fillStyle='#ff00ff';ctx.fillRect(c.width*.2,c.height*.5,c.width*.6,c.height*.4);
            }''')
            return execute('return Number(document.getElementById("zoom-display").dataset.frames)')
        def expect_history(before):
            wait(lambda:execute('return Number(document.getElementById("zoom-display").dataset.frames)')>before)
            wait(zoom_follows_rx)
            retained=execute('''return ['wide','zoom'].map(id=>{
                const c=document.getElementById(id),p=c.getContext('2d').getImageData(0,0,c.width,c.height).data;
                let count=0;for(let i=0;i<p.length;i+=4)if(p[i]===255 && p[i+1]===0 && p[i+2]===255 && p[i+3]===255)count++;
                return count;
            })''')
            assert all(count>100 for count in retained),retained
        def swipe(selector, distance=72):
            bounds=execute('const r=document.querySelector(arguments[0]).getBoundingClientRect();return {x:r.x+r.width*.35,y:r.y+r.height*.5}',selector)
            x,y=bounds['x'],bounds['y']
            cdp('Input.dispatchTouchEvent',{'type':'touchStart','touchPoints':[{'x':x,'y':y}]})
            for i in range(1,7):
                cdp('Input.dispatchTouchEvent',{'type':'touchMove','touchPoints':[{'x':x+distance*i/6,'y':y}]})
            cdp('Input.dispatchTouchEvent',{'type':'touchEnd','touchPoints':[]})
        def save_screenshot(path):
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(base64.b64decode(call('GET',f'/session/{session}/screenshot')))
        try:
            wait(ready)
            session=call('POST','/session',{'capabilities':{'alwaysMatch':{'browserName':'chrome','goog:loggingPrefs':{'browser':'ALL'},'goog:chromeOptions':{
                'binary':shutil.which('chromium'),'args':['--headless=new','--no-sandbox','--disable-dev-shm-usage','--window-size=1440,1000',
                '--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream','--autoplay-policy=no-user-gesture-required','--mute-audio',
                '--disable-background-timer-throttling','--disable-renderer-backgrounding']}}}})['sessionId']
            cdp('Page.addScriptToEvaluateOnNewDocument',{'source':'''if(!localStorage.getItem('qo100.testDbSeeded')){
                    localStorage.setItem('qo100.fullbandFloor','-113');localStorage.setItem('qo100.zoomFloor','-97');
                    localStorage.setItem('qo100.fullbandAxis','pluto-tx');
                    localStorage.setItem('qo100.testDbSeeded','true');
                }
                window.__dbLabels={};const drawText=CanvasRenderingContext2D.prototype.fillText;
                CanvasRenderingContext2D.prototype.fillText=function(text,...args){
                    if(['wide-spectrum','zoom-spectrum'].includes(this.canvas.id) && String(text).endsWith(' dBFS'))window.__dbLabels[this.canvas.id]=text;
                    return drawText.call(this,text,...args);
                };
                const Original=window.AudioWorkletNode;
                window.AudioWorkletNode=new Proxy(Original,{construct(Target,args){
                    const node=Reflect.construct(Target,args);
                    if(args[1]==='radio-audio')window.__radioNode=node;
                    return node;
                }});'''})
            url=f'http://127.0.0.1:{http_port}/'
            call('POST',f'/session/{session}/url',{'url':url})
            wait(lambda:execute('return document.getElementById("role").textContent')=='Bediener')
            wait(lambda:execute('return !document.getElementById("copy-rx-tx").disabled'))
            wait(lambda:execute('return Number(document.getElementById("fullband").dataset.frames)>1 && Number(document.getElementById("zoom-display").dataset.frames)>1'))
            decoder_probe='<b>abc</b> äöü\n'
            decoder_socket.sendto(decoder_probe.encode('utf-8'), ('127.0.0.1', decoder_text_port))
            wait(lambda:execute('return document.getElementById("decoder-text").textContent')==decoder_probe)
            assert execute('return document.querySelector("#decoder-text b")===null'),'Decoder text became HTML'
            assert execute('return document.getElementById("audio-stream")===null'),'RX2 browser audio selector remained'
            assert execute('return document.getElementById("decoder-panel").hidden'),'Decoder should initially be optional'
            original_zoom_height=execute('return document.getElementById("zoom").getBoundingClientRect().height')
            original_display_height=execute('return document.getElementById("zoom-display").getBoundingClientRect().height')
            history=mark_history();click('#decoder-toggle');expect_history(history)
            assert execute('return document.getElementById("decoder-toggle").getAttribute("aria-expanded")')=='true'
            assert execute('return Math.abs(document.getElementById("zoom").getBoundingClientRect().height-arguments[0]/2)<1',original_zoom_height)
            assert execute('return document.getElementById("zoom-display").getBoundingClientRect().height')==original_display_height
            click('#decoder-toggle')
            decoder_socket.sendto(b'hidden text\n', ('127.0.0.1', decoder_text_port))
            wait(lambda:execute('return document.getElementById("decoder-text").textContent')==decoder_probe+'hidden text\n')
            wait(lambda:execute('return document.getElementById("zoom").getBoundingClientRect().height')==original_zoom_height)
            click('#decoder-toggle')
            click('#decoder-clear')
            assert execute('return document.getElementById("decoder-text").textContent')==''
            decoder_history=''.join(f'RX2 Zeile {i:03d}\n' for i in range(200))
            decoder_socket.sendto(decoder_history.encode(), ('127.0.0.1', decoder_text_port))
            wait(lambda:execute('return document.getElementById("decoder-text").textContent')==decoder_history)
            assert execute('const n=document.getElementById("decoder-text");return n.scrollHeight>n.clientHeight && n.scrollHeight-n.clientHeight-n.scrollTop<4')
            execute('document.getElementById("decoder-text").scrollTop=0')
            decoder_socket.sendto(b'new line\n', ('127.0.0.1', decoder_text_port))
            wait(lambda:execute('return document.getElementById("decoder-text").textContent.endsWith("new line\\n")'))
            assert execute('return document.getElementById("decoder-text").scrollTop')==0,'New text moved the reading position'
            click('#decoder-clear')
            decoder_socket.sendto(b'abcdef\n', ('127.0.0.1', decoder_text_port))
            wait(lambda:execute('return document.getElementById("decoder-text").textContent')=='abcdef\n')
            click('#decoder-toggle')
            assert execute('return document.getElementById("waterfall-floor").value')=='-5'
            assert execute('return document.getElementById("waterfall-ceiling").value')=='40'
            assert execute('return document.querySelectorAll("#waterfall-content input[type=range]:not(#spectrum-average)").length')==2
            assert execute('return localStorage.getItem("qo100.fullbandFloor")')=='-113'
            # The synthetic source contains tones, no broadband noise: empty zoom bins may all clip at -160 dBFS.
            wait(lambda:execute('return document.getElementById("fullband-noise").dataset.dbfs'))
            def expect_auto_ranges():
                assert execute('''return ['fullband','zoom'].every(name=>{
                    const measured=document.getElementById(name+'-noise').dataset.dbfs,noise=Number(measured),range=document.getElementById(name+'-range').dataset;
                    if(measured==='')return range.floor==='' && range.ceiling==='';
                    return Math.abs(Number(range.floor)-noise-Number(document.getElementById('waterfall-floor').value))<.001 &&
                        Math.abs(Number(range.ceiling)-noise-Number(document.getElementById('waterfall-ceiling').value))<.001;
                });''')
            expect_auto_ranges()
            audio_off=execute('return getComputedStyle(document.getElementById("audio")).backgroundColor')
            assert audio_off=='rgb(122, 45, 53)',audio_off
            observer=Client(http_port);observer.role('viewer')
            initial=snapshot()
            rx,tx=hz('rx1'),hz('tx')
            assert execute('return !document.querySelector("#selection-label,.sidebar-bottom")')
            # The default bandplan follows RF coordinates; the switch cycles all five views.
            assert execute('return document.getElementById("fullband-axis").dataset.mode')=='bandplan'
            assert execute('return document.querySelectorAll("#fullband-axis .bandplan-segment").length')==11
            assert execute('return [...document.querySelectorAll("#fullband-axis .bandplan-beacon")].every(n=>n.textContent==="B")')
            aligned=execute('''const axis=document.getElementById('fullband-axis'),bounds=axis.getBoundingClientRect();
                return [...axis.children].every(n=>Math.abs(n.getBoundingClientRect().left-bounds.left-(Number(n.dataset.start)-10489450000)/600000*bounds.width)<1);''')
            assert aligned
            save_screenshot(capture.with_name(capture.stem+'-bandplan.png'))
            click('#fullband-axis-switch')
            axis_labels='return [...document.querySelectorAll("#fullband-axis > span")].map(n=>parseFloat(n.textContent)*1e6)'
            downlink=execute(axis_labels)
            plot('#fullband-axis > span',.5)
            assert execute('return document.getElementById("fullband-axis").dataset.mode')=='downlink'
            plot('#fullband-axis',.5)
            assert execute('return document.getElementById("fullband-axis").dataset.mode')=='downlink'
            zoom_labels=execute('return document.querySelector("#zoom-display .frequency-axis").textContent')
            offsets=[8089500000,int(initial['settings']['lnb_reference_hz'])*390,
                     8089500000+2400250000-int(initial['settings']['pluto_tx_base_hz']),None,0]
            history=mark_history()
            for mode,offset in zip(['uplink','pluto-rx','pluto-tx','bandplan','downlink'],offsets):
                # Hold over multiple FFT redraws; the button must remain the same DOM node.
                plot('#fullband-axis-switch',.5,press_ms=350)
                assert execute('return document.getElementById("fullband-axis").dataset.mode')==mode
                if offset is None:
                    assert execute('return document.querySelectorAll("#fullband-axis .bandplan-segment").length')==11
                else:
                    assert all(abs(actual-(expected-offset))<.5 for actual,expected in zip(execute(axis_labels),downlink))
                assert execute('return document.querySelector("#zoom-display .frequency-axis").textContent')==zoom_labels
                assert (hz('rx1'),hz('tx'))==(rx,tx)
            expect_history(history)
            click('#fullband-axis-switch')  # Reload below must reset this Uplink selection to Bandplan.
            assert execute('return localStorage.getItem("qo100.fullbandAxis")')=='pluto-tx'
            # Give TX a 5 kHz split, then verify left-click preserves it.
            click('#frequencytx');changed('#frequency-input',(tx+5000)/1e6)
            execute('document.getElementById("frequency-form").requestSubmit()')
            snapshot(lambda s:s.get('tx',{}).get('frequency_hz')==tx+5000)
            ratio=plot('#wide-spectrum',.42)
            target=round(int(initial['settings']['rx_center_hz'])+(ratio-.5)*600000)
            after=expect_frequency(1,target)
            snapshot(lambda s:s.get('tx',{}).get('frequency_hz')==tx+5000+target-rx)
            rx,tx=target,tx+5000+target-rx
            wait(lambda:execute('return document.getElementById("zoom-display").dataset.frames>2'))
            # Right-click only RX1; wheel and Shift use the display's step size.
            ratio=plot('#wide',.44,button=2);target=round(int(initial['settings']['rx_center_hz'])+(ratio-.5)*600000)
            after=expect_frequency(1,target);assert after['tx']['frequency_hz']==tx;rx=target
            history=mark_history();wheel('#wide');expect_frequency(1,rx+250);rx+=250;expect_history(history)
            wait(lambda:hz('rx1')==rx)
            history=mark_history();wheel('#zoom');expect_frequency(1,rx+25);rx+=25;expect_history(history)
            wheel('#wide-spectrum',True);snapshot(lambda s:s.get('tx',{}).get('frequency_hz')==tx+250);tx+=250
            wheel('#zoom-spectrum',True);snapshot(lambda s:s.get('tx',{}).get('frequency_hz')==tx+25);tx+=25
            # Ctrl preserves the split at both display resolutions and retains history.
            for selector,step in [('#wide',250),('#zoom-spectrum',25)]:
                wait(lambda:hz('rx1')==rx and hz('tx')==tx)
                history=mark_history();wheel(selector,ctrl=True)
                after=snapshot(lambda s:s.get('rx',{}).get('1',{}).get('frequency_hz')==rx+step and s.get('tx',{}).get('frequency_hz')==tx+step)
                assert after['tx']['frequency_hz']-after['rx']['1']['frequency_hz']==tx-rx
                rx+=step;tx+=step;expect_history(history)
            # RX2 clicks/wheel affect monitoring; explicit Ctrl also moves TX.
            click('#frequency2');assert execute('return document.getElementById("frequency2").getAttribute("aria-pressed")')=='true'
            ratio=plot('#wide-spectrum',.60);rx2=round(int(initial['settings']['rx_center_hz'])+(ratio-.5)*600000)
            after=expect_frequency(2,rx2);assert after['rx']['1']['frequency_hz']==rx and after['tx']['frequency_hz']==tx
            wheel('#wide');expect_frequency(2,rx2+250);rx2+=250
            wait(lambda:hz('rx2')==rx2 and hz('tx')==tx)
            wheel('#wide-spectrum',ctrl=True)
            after=snapshot(lambda s:s.get('rx',{}).get('2',{}).get('frequency_hz')==rx2+250 and s.get('tx',{}).get('frequency_hz')==tx+250)
            assert after['rx']['1']['frequency_hz']==rx
            rx2+=250;tx+=250
            wait(lambda:hz('rx2')==rx2 and hz('tx')==tx)
            wait(zoom_follows_rx)
            assert execute('return document.getElementById("zoom-title").textContent')=='RX2-ZOOM'
            # RX2 zoom uses its own center for both spectrum and waterfall clicks.
            for selector,button in [('#zoom-spectrum',0),('#zoom',2)]:
                wait(zoom_follows_rx);ratio=plot(selector,.55,button=button)
                rx2=round(rx2+(ratio-.5)*50000)
                after=expect_frequency(2,rx2)
                assert after['rx']['1']['frequency_hz']==rx and after['tx']['frequency_hz']==tx
                wait(lambda:hz('rx2')==rx2)
            history=mark_history();wheel('#zoom');rx2+=25
            after=expect_frequency(2,rx2);assert after['rx']['1']['frequency_hz']==rx and after['tx']['frequency_hz']==tx
            expect_history(history)
            wheel('#zoom-spectrum',True);tx+=25
            after=snapshot(lambda s:s.get('tx',{}).get('frequency_hz')==tx)
            assert after['rx']['1']['frequency_hz']==rx and after['rx']['2']['frequency_hz']==rx2
            wait(lambda:hz('rx2')==rx2 and hz('tx')==tx)
            history=mark_history();wheel('#zoom',ctrl=True);rx2+=25;tx+=25
            after=snapshot(lambda s:s.get('rx',{}).get('2',{}).get('frequency_hz')==rx2 and s.get('tx',{}).get('frequency_hz')==tx)
            assert after['rx']['1']['frequency_hz']==rx
            expect_history(history)
            for receiver in [1,2,1,2]:
                click(f'#frequency{receiver}');wait(zoom_follows_rx)
                assert execute('return document.getElementById("zoom-display").dataset.stream')==str(receiver)
                assert execute('return document.getElementById("zoom-title").textContent')==f'RX{receiver}-ZOOM'
            click('#decoder-toggle')
            save_screenshot(capture.with_name(capture.stem+'-rx2-zoom.png'))
            click('#decoder-toggle')
            click('#copy-rx-tx');snapshot(lambda s:s.get('tx',{}).get('frequency_hz')==rx-8089500000);tx=rx-8089500000
            wheel('#wide',True);snapshot(lambda s:s.get('tx',{}).get('frequency_hz')==tx+250);tx+=250
            click('#copy-tx-rx');expect_frequency(1,tx+8089500000)
            # Automatic ranges use independent noise estimates and two shared 1-dB offset controls.
            click('#waterfall-toggle')
            assert execute('return document.getElementById("filter-content").hidden')
            assert not execute('return document.getElementById("waterfall-content").textContent.includes("Pegelbereich für Spektrum und Wasserfall")')
            assert execute('return [...document.querySelectorAll("#display-color option")].map(n=>n.value)')==['blue','red','green']
            assert execute('return document.getElementById("display-color").value')=='blue'
            assert execute('return [...document.querySelectorAll("#waterfall-content label,#waterfall-content output,#waterfall-content p,.sidebar h2")].every(n=>getComputedStyle(n).color==="rgb(255, 255, 255)")')
            changed('#display-color','red');assert execute('return localStorage.getItem("qo100.displayColor")')=='red'
            changed('#display-color','green');assert execute('return localStorage.getItem("qo100.displayColor")')=='green'
            assert execute('return document.getElementById("spectrum-average").value')=='1'
            history=mark_history();changed('#spectrum-average',20);expect_history(history)
            assert execute('return document.getElementById("spectrum-average-value").textContent')=='20'
            assert execute('return localStorage.getItem("qo100.spectrumAverage")')=='20'
            # Controlled FFTs prove that smoothing changes only the spectrum pixels.
            smoothing=call('POST',f'/session/{session}/execute/async',{'script':'''const done=arguments[arguments.length-1];
                import('/radio-display.js').then(({RadioDisplay})=>{
                    const create=(zoom=false,width=200)=>{
                        const element=document.createElement('section');
                        element.style.cssText=`position:absolute;left:-10000px;top:0;width:${width}px`;
                        element.innerHTML=`<canvas data-spectrum style="width:${width}px;height:100px"></canvas><canvas data-waterfall style="width:${width}px;height:100px"></canvas><canvas data-overlay style="width:${width}px;height:100px"></canvas><div class="frequency-axis"></div><span class="display-empty"></span>`;
                        document.body.append(element);return new RadioDisplay(element,zoom);
                    };
                    const raw=create(),smooth=create();smooth.setAverage(2);
                    for(const level of [-6500,-9500]){
                        const frame={center:100,rate:8,n:8,values:new Int16Array(8).fill(-10000)};
                        frame.values[2]=level;
                        raw.accept(frame);smooth.accept(frame);
                    }
                    const waterfall=smooth.waterfall.toDataURL(),spectrum=smooth.spectrum.toDataURL();
                    const result={waterfallUnchanged:waterfall===raw.waterfall.toDataURL(),spectrumSmoothed:spectrum!==raw.spectrum.toDataURL()};
                    for(let i=0;i<10;i++)smooth.update({},1);
                    result.redrawStable=spectrum===smooth.spectrum.toDataURL() && smooth.average.frames.length===2;
                    const pixels=()=>smooth.waterfall.getContext('2d').getImageData(0,0,smooth.waterfall.width,smooth.waterfall.height).data;
                    const alphas=()=>Array.from(pixels()).filter((_,i)=>i%4===3).join(',');
                    const alpha=alphas(),samples=smooth.spectrumValues,frames=smooth.element.dataset.frames;
                    smooth.setColor('red');const red=pixels();
                    result.redPalette=red[0]>red[1] && red[0]>red[2] && smooth.spectrum.toDataURL()!==spectrum;
                    const redSpectrum=smooth.spectrum.toDataURL();smooth.setColor('green');const green=pixels();
                    result.greenPalette=green[1]>green[0] && green[1]>green[2] && smooth.spectrum.toDataURL()!==redSpectrum;
                    result.colorPreservesHistory=alpha===alphas() && smooth.element.dataset.frames===frames && smooth.spectrumValues===samples && smooth.average.frames.length===2;
                    smooth.setColor('blue');const blue=pixels();
                    result.bluePalette=blue[2]>blue[0] && blue[2]>blue[1];
                    const recolored=smooth.waterfall.toDataURL();
                    smooth.setAverage(1);
                    result.bypassImmediate=smooth.spectrum.toDataURL()===raw.spectrum.toDataURL() && recolored===smooth.waterfall.toDataURL();
                    const full=create(),zoom=create(true),frame={center:100,rate:600000,n:8192,values:new Int16Array(8192).fill(-6000)};
                    frame.values.fill(-1000,2000,2800);full.accept(frame);
                    zoom.update({rx:{'1':{frequency_hz:100}}},1);
                    const close={...frame,stream:1,rate:75000,values:new Int16Array(8192).fill(-8000)};
                    close.values.fill(-15000,0,1366);close.values.fill(-15000,6827);zoom.accept(close);
                    result.autoCalibration=full.noise.value===-60 && full.floor===-65 && full.ceiling===-20 && zoom.noise.value===-80 && zoom.floor===-85 && zoom.ceiling===-40;
                    const history=full.waterfall.toDataURL(),count=full.element.dataset.frames;
                    full.setOffsets(10,38);full.setAverage(20);full.update({settings:{gui_display_floor:-140}},1);
                    result.offsetsPreserveHistory=full.floor===-50 && full.ceiling===-22 && full.noise.value===-60 && full.waterfall.toDataURL()===history && full.element.dataset.frames===count;
                    full.clear();result.resetCalibration=full.noise.value===null && full.offsets.floor===10 && full.offsets.ceiling===38;
                    const zoomCtx=zoom.spectrum.getContext('2d'),zoomDrawText=zoomCtx.fillText;
                    let zoomLabels=0;
                    zoomCtx.fillText=function(text,x,y){if(/^[+][0-9]+dB$/.test(text))zoomLabels++;return zoomDrawText.call(this,text,x,y);};
                    close.values[4000]=-5000;zoom.accept(close);
                    result.noZoomLabels=zoomLabels===0;
                    // Even equal RX frequencies must never mix stream data or averages.
                    const receiverState={rx:{'1':{frequency_hz:100},'2':{frequency_hz:100}}};
                    zoom.setAverage(20);zoom.update(receiverState,2);
                    result.zoomReceiverReset=zoom.frame===null && zoom.noise.value===null && zoom.average.frames.length===0;
                    zoom.accept(close);result.zoomStreamIsolation=zoom.frame===null;
                    const second={...close,stream:2,values:new Int16Array(8192).fill(-7000)};
                    zoom.accept(second);
                    result.zoomStreamIsolation=result.zoomStreamIsolation && zoom.frame===second && zoom.noise.value===-70 && zoom.average.frames.length===1;
                    const secondHistory=zoom.waterfall.toDataURL();zoom.accept(close);
                    result.zoomStreamIsolation=result.zoomStreamIsolation && zoom.waterfall.toDataURL()===secondHistory && zoom.frame===second;
                    zoom.update({...receiverState,rx:{...receiverState.rx,'2':{frequency_hz:101}}},2);
                    const event={clientX:400,currentTarget:zoom.spectrum};
                    result.zoomStaleFrame=zoom.frequencyAt(event)===null;
                    zoom.accept(second);result.zoomStaleFrame=result.zoomStaleFrame && zoom.frame===second;
                    zoom.accept({...second,center:101});
                    result.zoomStaleFrame=result.zoomStaleFrame && zoom.geometry.center===101 && zoom.frequencyAt(event)!==null;
                    zoom.update(receiverState,1);zoom.accept(close);
                    result.zoomReceiverReset=result.zoomReceiverReset && zoom.frame===close && zoom.noise.value===-80 && zoom.average.frames.length===1;
                    const annotated=create(false,800),ctx=annotated.spectrum.getContext('2d'),drawText=ctx.fillText;
                    let drawn=[];
                    ctx.fillText=function(text,x,y){if(/^[+][0-9]+dB$/.test(text))drawn.push({text,x,y,width:this.measureText(text).width});return drawText.call(this,text,x,y);};
                    annotated.setAverage(20);
                    const measured={center:100,rate:600000,n:8192,values:new Int16Array(8192).fill(-6000)};
                    measured.values[2048]=-3500;measured.values[2212]=-3800;measured.values.fill(-2800,4000,4201);
                    measured.values[5800]=-4500;measured.values[6200]=-4200;measured.values[8150]=-3900;
                    annotated.accept(measured);
                    result.peakLabels=drawn.length===3 && drawn.some(p=>p.text==='+32dB') && drawn.some(p=>p.text==='+25dB') && drawn.some(p=>p.text==='+18dB') && drawn.every(p=>p.y===14);
                    const initialLabel=drawn.find(p=>p.text==='+25dB');
                    result.peakLabels=result.peakLabels && Math.abs(initialLabel.x+initialLabel.width/2-200)<1;
                    const preview=annotated.spectrum.toDataURL();drawn=[];
                    const weaker={...measured,values:measured.values.slice()};weaker.values[2048]=-5500;
                    annotated.accept(weaker);
                    result.peakHold=drawn.some(p=>p.text==='+25dB') && annotated.spectrumValues[2048]<-3500;
                    drawn=[];
                    annotated.accept({...weaker,center:125});
                    result.peakRetune=!drawn.some(p=>p.text==='+25dB');
                    const afterTune=annotated.waterfall.toDataURL();annotated.update({},1);
                    result.peakRedraw=annotated.waterfall.toDataURL()===afterTune;
                    annotated.clear();result.peakReset=annotated.peaks.get().length===0;
                    for(const view of [raw,smooth,full,zoom,annotated]){view.observer.disconnect();view.element.remove();}
                    done({...result,preview});
                }).catch(e=>done({error:e.message}));''','args':[]})
            preview=smoothing.pop('preview',None)
            if preview:
                capture.with_name(capture.stem+'-peaks.png').write_bytes(base64.b64decode(preview.split(',')[1]))
            assert smoothing==dict.fromkeys(['waterfallUnchanged','spectrumSmoothed','redrawStable','redPalette','greenPalette','colorPreservesHistory','bluePalette','bypassImmediate','autoCalibration','offsetsPreserveHistory','resetCalibration','noZoomLabels','zoomReceiverReset','zoomStreamIsolation','zoomStaleFrame','peakLabels','peakHold','peakRetune','peakRedraw','peakReset'],True),smoothing
            history=mark_history();changed('#waterfall-floor',-7);changed('#waterfall-ceiling',37);expect_history(history)
            assert execute('return document.getElementById("waterfall-floor-value").textContent')=='-7 dB'
            assert execute('return document.getElementById("waterfall-ceiling-value").textContent')=='+37 dB'
            assert execute('return localStorage.getItem("qo100.waterfallFloorOffset")')=='-7'
            assert execute('return localStorage.getItem("qo100.waterfallCeilingOffset")')=='37'
            expect_auto_ranges()
            assert execute('return [...document.querySelectorAll("#waterfall-content input:not(#spectrum-average)")].every(n=>n.step==="1" && Number.isInteger(n.valueAsNumber))')
            changed('#waterfall-floor',10);changed('#waterfall-ceiling',30);expect_auto_ranges()
            changed('#waterfall-floor',-7);changed('#waterfall-ceiling',37)
            save_screenshot(capture.with_name(capture.stem+'-waterfall.png'))
            assert snapshot()['settings']['gui_display_floor']==initial['settings']['gui_display_floor']
            click('#filter-toggle')
            assert execute('return document.getElementById("waterfall-content").hidden')
            # DSP controls and a revisioned settings patch.
            click('[data-bandwidth="2700"]');snapshot(lambda s:s.get('filter',{}).get('bandwidth_hz')==2700)
            changed('#filter-lower',301)
            snapshot(lambda s:s.get('filter',{}).get('low_hz')==301 and s['filter']['high_hz']==2800)
            changed('#filter-upper',2700)
            snapshot(lambda s:s.get('filter',{}).get('low_hz')==301 and s['filter']['high_hz']==2700)
            changed('#filter-lower',2600)  # Native slider clamps to preserve the upper edge and minimum width.
            snapshot(lambda s:s.get('filter',{}).get('low_hz')==2200 and s['filter']['high_hz']==2700)
            changed('#filter-upper',1000)
            snapshot(lambda s:s.get('filter',{}).get('low_hz')==2200 and s['filter']['high_hz']==2700)
            changed('#filter-lower',301)
            snapshot(lambda s:s.get('filter',{}).get('low_hz')==301 and s['filter']['high_hz']==2700)
            changed('#nr',47);snapshot(lambda s:s.get('nr',{}).get('percent')==47)
            changed('#compressor',112.5);snapshot(lambda s:s.get('compressor',{}).get('percent')==112.5)
            # Settings snapshots arrive separately from DSP acknowledgements; wait for their revision.
            settled=snapshot(lambda s:float(s.get('settings',{}).get('tx1_compressor_percent',0))==112.5 and float(s.get('settings',{}).get('rx1_nr_percent',0))==47)
            wait(lambda:execute('return document.getElementById("settings-revision").textContent')==f'Revision {settled["revision"]}')
            click('#setup')
            assert execute('return !document.querySelector("#setup-dialog [data-setting=gui_display_floor]")')
            assert execute('return document.getElementById("tx-base-help").textContent')=='Bezogen auf Bandmitte (2400,250 bzw. 10489,7540)'
            assert execute('return [...document.querySelectorAll("#setup-dialog label,#setup-dialog legend,#setup-dialog .field-help")].every(n=>getComputedStyle(n).color==="rgb(255, 255, 255)")')
            attenuation=float(initial['settings']['tx_attenuation_db'])+.25
            changed('[data-setting="tx_attenuation_db"]',attenuation)
            execute('document.getElementById("settings-form").requestSubmit()')
            wait(lambda:'gespeichert' in execute('return document.getElementById("settings-message").textContent'))
            snapshot(lambda s:float(s.get('settings',{}).get('tx_attenuation_db',0))==attenuation)
            changed('#loop',1);snapshot(lambda s:s.get('loop',{}).get('mode')==1)
            click('[data-close="setup-dialog"]')
            click('#audio');wait(lambda:execute('return !document.getElementById("ptt").disabled'))
            audio_on=execute('return getComputedStyle(document.getElementById("audio")).backgroundColor')
            assert audio_on=='rgb(33, 92, 67)',audio_on
            def ptt_hold(actions):
                r=execute('const r=document.getElementById("ptt").getBoundingClientRect();return {x:Math.round(r.x+r.width/2),y:Math.round(r.y+r.height/2)}')
                call('POST',f'/session/{session}/actions',{'actions':[{'type':'pointer','id':'mouse','parameters':{'pointerType':'mouse'},'actions':[
                    {'type':'pointerMove',**r,'duration':0},{'type':'pointerDown','button':0},*actions]}]})
            start=len(proxy.log)
            ptt_hold([{'type':'pause','duration':1100},{'type':'pointerUp','button':0}])
            snapshot(lambda s:not s.get('tx',{}).get('ptt',True))
            packets=[p for p in proxy.log[start:] if p[5]==8]
            assert len(packets)>15,len(packets)
            assert int.from_bytes(packets[0][24:32],'big')==0
            assert any(any(p[64:]) for p in packets)
            count=len([p for p in proxy.log if p[5]==8]);time.sleep(.25);assert count==len([p for p in proxy.log if p[5]==8])
            ptt_hold([]);snapshot(lambda s:s.get('tx',{}).get('ptt',False))
            execute('window.dispatchEvent(new Event("blur"))')
            observer.messages.clear();snapshot(lambda s:not s.get('tx',{}).get('ptt',True))
            call('POST',f'/session/{session}/actions',{'actions':[{'type':'pointer','id':'mouse','parameters':{'pointerType':'mouse'},'actions':[{'type':'pointerUp','button':0}]}]})
            # Global Space is opt-in and obeys the selected PTT mode.
            execute('document.activeElement.blur()')
            key('keyDown');key('keyUp');time.sleep(.2);expect_ptt(False)
            click('#setup');click('#ptt-space')
            assert execute('return document.getElementById("ptt-hold").checked')
            click('[data-close="setup-dialog"]')
            key('keyDown');expect_ptt(True);key('keyUp');expect_ptt(False)
            execute('document.getElementById("decoder-toggle").focus()')
            key('keyDown');key('keyUp');expect_ptt(False)
            assert not execute('return document.getElementById("decoder-panel").hidden')
            execute('document.getElementById("decoder-clear").focus()')
            key('keyDown');key('keyUp');expect_ptt(False)
            assert execute('return document.getElementById("decoder-text").textContent')==''
            click('#decoder-toggle')
            execute('document.getElementById("fullband-axis-switch").focus()')
            key('keyDown');key('keyUp');expect_ptt(False)
            assert execute('return document.getElementById("fullband-axis").dataset.mode')=='pluto-rx'
            for _ in range(4):click('#fullband-axis-switch')
            execute('document.activeElement.blur()')
            key('keyDown');expect_ptt(True)
            execute('document.getElementById("fullband-axis-switch").focus()')
            key('keyUp');expect_ptt(False)
            # A release after focus moved still ends a previous Space PTT hold.
            execute('document.activeElement.blur()')
            # Setup/input fields must not trigger PTT.
            click('#setup');click('[data-setting="pluto_ip"]')
            key('keyDown');key('keyUp');time.sleep(.15);expect_ptt(False)
            click('#ptt-toggle');save_screenshot(capture.with_name(capture.stem+'-setup.png'))
            click('[data-close="setup-dialog"]')
            click('#ptt');expect_ptt(True);time.sleep(.3);expect_ptt(True)
            assert 'Klick: aus' in execute('return document.getElementById("ptt-hint").textContent')
            click('#ptt');expect_ptt(False)
            key('keyDown');expect_ptt(True)
            cdp('Input.dispatchKeyEvent',{'type':'keyDown','key':' ','code':'Space','windowsVirtualKeyCode':32,'autoRepeat':True})
            key('keyUp');time.sleep(.2);expect_ptt(True)
            key('keyDown');expect_ptt(False);key('keyUp')
            click('#ptt');expect_ptt(True)
            execute('window.dispatchEvent(new Event("blur"))');expect_ptt(False)
            # Simulate a delayed timer after five minutes without waiting five minutes.
            click('#ptt');expect_ptt(True)
            execute('window.__originalNow=performance.now.bind(performance);Object.defineProperty(performance,"now",{configurable:true,value:()=>window.__originalNow()+300001})')
            expect_ptt(False)
            wait(lambda:'5 Minuten' in execute('return document.getElementById("status").textContent'))
            execute('delete performance.now;delete window.__originalNow')
            # Restore normal mode after the microphone loop test.
            click('#setup');changed('#loop',0);snapshot(lambda s:s.get('loop',{}).get('mode')==0);click('[data-close="setup-dialog"]')
            # Measure actual worklet output while retuning a synthetic audio tone.
            click('#frequency1');ratio=plot('#wide-spectrum',.2475,button=2)
            rx=round(int(initial['settings']['rx_center_hz'])+(ratio-.5)*600000);expect_frequency(1,rx)
            call('POST',f'/session/{session}/execute/async',{'script':'''const done=arguments[arguments.length-1];
                const source=`class Probe extends AudioWorkletProcessor {
                    constructor(){super();this.total=0;this.silent=0;this.longest=0;this.port.onmessage=()=>{
                        this.port.postMessage({total:this.total,longest_ms:this.longest/sampleRate*1000});
                        this.total=0;this.silent=0;this.longest=0;
                    };}
                    process(inputs){const pcm=inputs[0]?.[0];if(pcm){this.total+=pcm.length;
                        this.silent=pcm.every(v=>Math.abs(v)<1e-8)?this.silent+pcm.length:0;
                        this.longest=Math.max(this.longest,this.silent);
                    }return true;}
                }registerProcessor('continuity-probe',Probe);`;
                const url=URL.createObjectURL(new Blob([source],{type:'text/javascript'}));
                const node=window.__radioNode;
                node.context.audioWorklet.addModule(url).then(()=>{
                    URL.revokeObjectURL(url);window.__probe=new AudioWorkletNode(node.context,'continuity-probe');
                    node.connect(window.__probe);window.__probe.connect(node.context.destination);done(true);
                }).catch(e=>done({error:e.message}));''','args':[]})
            def audio_measurement():
                return call('POST',f'/session/{session}/execute/async',{'script':'''const done=arguments[arguments.length-1];
                    window.__probe.port.onmessage=e=>done(e.data);window.__probe.port.postMessage('report');''','args':[]})
            time.sleep(.6);audio_measurement()
            zoom_frames=execute('return Number(document.getElementById("zoom-display").dataset.frames)')
            for selector,step in [('#zoom',25),('#wide',250)]*6:
                wheel(selector);rx+=step;expect_frequency(1,rx);time.sleep(.06)
            continuity=audio_measurement()
            assert continuity['total']>10000 and continuity['longest_ms']==0,continuity
            assert execute('return Number(document.getElementById("zoom-display").dataset.frames)')>zoom_frames+5
            execute('window.__radioNode.disconnect(window.__probe);window.__probe.disconnect()')
            # A denied microphone must keep playback active and its error visible.
            click('#audio')
            execute('window.__originalGetUserMedia=navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);navigator.mediaDevices.getUserMedia=()=>Promise.reject(new DOMException("Permission denied","NotAllowedError"))')
            click('#audio')
            wait(lambda:execute('return !document.getElementById("audio-message").hidden'))
            assert execute('return document.getElementById("audio").textContent')=='Audio stoppen'
            assert execute('return document.getElementById("ptt").disabled')
            time.sleep(5.2)
            assert execute('return !document.getElementById("audio-message").hidden')
            execute('navigator.mediaDevices.getUserMedia=window.__originalGetUserMedia;delete window.__originalGetUserMedia')
            click('#setup')
            assert execute('return document.getElementById("microphone")===null')
            changed('#audio-input','default')
            wait(lambda:execute('return !document.getElementById("ptt").disabled && document.getElementById("audio-message").hidden'))
            # A concrete input choice must reach getUserMedia as a required device ID.
            named_input=execute('return [...document.getElementById("audio-input").options].find(n=>n.value && n.value!=="default").value')
            execute('''window.__captureGetUserMedia=navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
                navigator.mediaDevices.getUserMedia=constraints=>{window.__captureConstraints=constraints;return window.__captureGetUserMedia(constraints);};''')
            changed('#audio-input',named_input)
            wait(lambda:execute('return !document.getElementById("ptt").disabled && document.getElementById("audio-message").hidden'))
            assert execute('return window.__captureConstraints.audio.deviceId.exact')==named_input
            assert execute('return localStorage.getItem("qo100.audioInput")')==named_input
            execute('navigator.mediaDevices.getUserMedia=window.__captureGetUserMedia;delete window.__captureGetUserMedia')
            changed('#audio-output','default');changed('#audio-input','default')
            wait(lambda:execute('return !document.getElementById("ptt").disabled'))
            assert execute('return localStorage.getItem("qo100.audioOutput")')=='default'
            assert execute('return localStorage.getItem("qo100.audioInput")')=='default'
            click('[data-close="setup-dialog"]')
            click('#frequency1')
            cdp('Emulation.setDeviceMetricsOverride',{'width':1440,'height':900,'deviceScaleFactor':1,'mobile':False})
            save_screenshot(capture)
            decoder_socket.sendto('CQ CQ DE TEST\nRX2 · Decoder-Ausgabe: äöü ß\n'.encode(), ('127.0.0.1', decoder_text_port))
            wait(lambda:execute('return document.getElementById("decoder-text").textContent.includes("CQ CQ")'))
            click('#decoder-toggle')
            save_screenshot(capture.with_name(capture.stem+'-decoder.png'))
            # Real touch taps/swipes and the mobile-only RX target switch.
            cdp('Emulation.setDeviceMetricsOverride',{'width':390,'height':844,'deviceScaleFactor':1,'mobile':True})
            execute('window.scrollTo(0,0)')
            for mode in ['pluto-rx','pluto-tx','bandplan','downlink','uplink']:
                plot('#fullband-axis-switch',.5,touch=True)
                wait(lambda:execute('return document.getElementById("fullband-axis").dataset.mode')==mode)
                if mode=='bandplan':save_screenshot(capture.with_name(capture.stem+'-bandplan-mobile.png'))
            click('#frequency2');before_tx=hz('tx')
            ratio=plot('#wide-spectrum',.56,touch=True);target=round(int(initial['settings']['rx_center_hz'])+(ratio-.5)*600000)
            expect_frequency(2,target);wait(lambda:hz('rx2')==target)
            swipe('#frequency2');expect_frequency(2,target+150);assert hz('tx')==before_tx
            swipe('#frequencytx');snapshot(lambda s:s.get('tx',{}).get('frequency_hz')==before_tx+150)
            assert not execute('return document.getElementById("frequency-dialog").open')
            wait(zoom_follows_rx)
            execute('document.getElementById("zoom-spectrum").scrollIntoView({block:"center"})')
            ratio=plot('#zoom-spectrum',.56,touch=True)
            mobile_rx2=round(target+150+(ratio-.5)*50000)
            after=expect_frequency(2,mobile_rx2)
            assert after['tx']['frequency_hz']==before_tx+150
            wait(zoom_follows_rx)
            save_screenshot(capture.with_name(capture.stem+'-rx2-zoom-mobile.png'))
            click('#frequency1');click('#touch-rx');before_tx=hz('tx')
            ratio=plot('#wide-spectrum',.54,touch=True);target=round(int(initial['settings']['rx_center_hz'])+(ratio-.5)*600000)
            after=expect_frequency(1,target);assert after['tx']['frequency_hz']==before_tx
            click('#touch-together');ratio=plot('#wide-spectrum',.53,touch=True)
            new_rx=round(int(initial['settings']['rx_center_hz'])+(ratio-.5)*600000)
            expect_frequency(1,new_rx);snapshot(lambda s:s.get('tx',{}).get('frequency_hz')==before_tx+new_rx-target)
            save_screenshot(capture.with_name(capture.stem+'-mobile.png'))
            execute('document.getElementById("decoder-panel").scrollIntoView({block:"center"})')
            save_screenshot(capture.with_name(capture.stem+'-decoder-mobile.png'))
            decoder_socket.sendto(('X'*3000).encode(), ('127.0.0.1', decoder_text_port))
            wait(lambda:execute('return document.getElementById("decoder-text").textContent.endsWith("X".repeat(3000))'))
            execute('for(let i=0;i<5 && document.getElementById("fullband-axis").dataset.mode!=="bandplan";i++)document.getElementById("fullband-axis-switch").click()')
            for width,height in [(320,740),(768,1024),(1024,768),(1366,768),(1920,1080)]:
                cdp('Emulation.setDeviceMetricsOverride',{'width':width,'height':height,'deviceScaleFactor':1,'mobile':width<=560})
                assert execute('return document.documentElement.scrollWidth<=innerWidth'),(width,height)
                assert execute('const n=document.getElementById("decoder-text");return n.scrollWidth<=n.clientWidth'),('decoder overflow',width,height)
                wait(lambda:execute('''const axis=document.getElementById('fullband-axis'),button=document.getElementById('fullband-axis-switch').getBoundingClientRect();
                    const labels=[...axis.querySelectorAll('.bandplan-label')].filter(n=>n.textContent);
                    const bounds=labels.map(n=>n.getBoundingClientRect()).sort((a,b)=>a.left-b.left);
                    return labels.some(n=>n.textContent.startsWith('CW')) && bounds.every((r,i)=>r.right<=button.left+1 && (!i || r.left>=bounds[i-1].right));'''))
                if width==320:save_screenshot(capture.with_name(capture.stem+'-bandplan-320.png'))
            click('#fullband-axis-switch')
            # Preferences survive a reload; transmitting is never restored.
            call('POST',f'/session/{session}/url',{'url':url})
            wait(lambda:execute('return document.getElementById("role").textContent')=='Bediener')
            assert execute('return document.getElementById("ptt-toggle").checked && document.getElementById("ptt-space").checked')
            assert execute('return [document.getElementById("waterfall-floor").value,document.getElementById("waterfall-ceiling").value]')==['-7','37']
            assert execute('return document.getElementById("spectrum-average").value')=='20'
            assert execute('return document.getElementById("display-color").value')=='green'
            assert execute('return document.getElementById("fullband-axis").dataset.mode')=='bandplan'
            assert not execute('return document.getElementById("decoder-panel").hidden'),'Decoder visibility was not restored'
            expect_ptt(False)
            # Backend restart must refresh the session with PTT off.
            old_session=snapshot()['session'];radio.terminate();radio.wait(timeout=8);radio=start_backend()
            restarted=snapshot(lambda s:s.get('ready') and s.get('session') not in (0,old_session))
            assert not restarted['tx']['ptt']
            wait(lambda:execute('return !document.getElementById("copy-rx-tx").disabled'))
            # Viewers can select a receiver and listen but never control the backend.
            owner=call('GET',f'/session/{session}/window')
            viewer=call('POST',f'/session/{session}/window/new',{'type':'tab'})['handle']
            call('POST',f'/session/{session}/window',{'handle':viewer})
            call('POST',f'/session/{session}/url',{'url':url})
            wait(lambda:execute('return document.getElementById("role").textContent')=='Zuschauer')
            decoder_socket.sendto('Zuschauer: RX2 Text\n'.encode(), ('127.0.0.1', decoder_text_port))
            wait(lambda:execute('return document.getElementById("decoder-text").textContent')=='Zuschauer: RX2 Text\n')
            click('#decoder-toggle')
            assert execute('return document.getElementById("decoder-panel").hidden')
            click('#fullband-axis-switch')
            assert execute('return document.getElementById("fullband-axis").dataset.mode')=='downlink'
            assert execute('return document.getElementById("ptt").disabled && document.getElementById("copy-rx-tx").disabled && document.getElementById("nr").disabled')
            click('#frequency2');assert execute('return document.getElementById("frequency2").getAttribute("aria-pressed")')=='true'
            wait(zoom_follows_rx)
            assert execute('return document.getElementById("zoom-display").dataset.stream')=='2'
            before=snapshot();wheel('#zoom')
            after=snapshot();assert after['rx']['2']['frequency_hz']==before['rx']['2']['frequency_hz']
            errors=call('POST',f'/session/{session}/log',{'type':'browser'})
            assert not [e for e in errors if e['level']=='SEVERE'],errors
            call('POST',f'/session/{session}/window',{'handle':owner})
            click('#connect');wait(lambda:execute('return document.getElementById("role").textContent')=='Getrennt')
            time.sleep(.5);assert execute('return document.getElementById("connect").textContent')=='Verbinden'
            errors=call('POST',f'/session/{session}/log',{'type':'browser'})
            assert not [e for e in errors if e['level']=='SEVERE'],errors
            print(f'WebGUI browser: spectra, split/right-click/wheel, waterfall history/floors, audio continuity ({continuity}), RX2 tuning/decoder text, copy, DSP/settings, {len(packets)} microphone packets, PTT hold/toggle/Space/timeout/blur, touch, layouts, preferences, restart, viewer and disconnect passed.')
            print('Screenshots:',capture,capture.with_name(capture.stem+'-mobile.png'))
        except BaseException:
            if session:
                try:
                    print('Browser log:',call('POST',f'/session/{session}/log',{'type':'browser'}))
                    print('UI:',execute('return {status:document.getElementById("status")?.textContent, role:document.getElementById("role")?.textContent, settings:document.getElementById("settings-message")?.textContent}'))
                    save_screenshot(Path('/tmp/qo100-webgui-failure.png'))
                except Exception:
                    pass
            raise
        finally:
            if session:
                call('DELETE',f'/session/{session}')
            if observer:
                observer.close()
            for process in [chrome,bridge,radio]:
                process.terminate();process.wait(timeout=8)
            proxy.close()
            decoder_socket.close()
            for log in logs:
                log.close()
            if sys.exc_info()[0]:
                print((root/'adapter.log').read_text())


if __name__=='__main__':
    main()
