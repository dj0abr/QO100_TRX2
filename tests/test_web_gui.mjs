import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';
const moduleAt = async file => import('data:text/javascript;base64,' + Buffer.from(await readFile(new URL(`../web/${file}`, import.meta.url))).toString('base64'));
const {DOWNLINK_UPLINK_HZ, frequencyText, parseMHz, validateFrequency, tuneTargets, wheelTurns, wheelTargets, filterValues} = await moduleAt('tuning.js');
const rx1 = 10489700000, tx = 2400205000;
const base = {selected:1, frequency:rx1+12345, rx1, tx, rxCenter:10489750000, txCenter:2400250000, together:true};
assert.deepEqual(tuneTargets(base), {rx:1,frequency:rx1+12345,tx:tx+12345});
assert.equal(tuneTargets({...base,together:false}).tx, undefined);
assert.deepEqual(tuneTargets({...base,selected:2}), {rx:2,frequency:rx1+12345});
assert.throws(()=>tuneTargets({...base,tx:2400525000}), /TX/);
assert.throws(()=>validateFrequency(10490100000,10489750000), /275/);
assert.equal(parseMHz('10 489.700 001'),10489700001);
assert.equal(parseMHz('2400,205000'),2400205000);
assert.throws(()=>parseMHz('10489.1234567'));
assert.throws(()=>parseMHz('1e4'));
assert.equal(frequencyText(rx1),'10 489.700 000');
assert.equal(tx+DOWNLINK_UPLINK_HZ,10489705000);
assert.equal(wheelTurns({deltaY:-120,deltaMode:0}),1);
assert.equal(wheelTurns({deltaY:100,deltaMode:0}),-1);
assert.equal(wheelTurns({deltaY:-3,deltaMode:1}),1);
const wheelBase={selected:1,rx:rx1,tx,delta:250,rxCenter:base.rxCenter,txCenter:base.txCenter};
assert.deepEqual(wheelTargets(wheelBase),{rx:1,frequency:rx1+250});
assert.deepEqual(wheelTargets({...wheelBase,shift:true}),{tx:tx+250});
assert.deepEqual(wheelTargets({...wheelBase,ctrl:true}),{rx:1,frequency:rx1+250,tx:tx+250});
assert.deepEqual(wheelTargets({...wheelBase,ctrl:true,shift:true,delta:-25}),{rx:1,frequency:rx1-25,tx:tx-25});
assert.deepEqual(wheelTargets({...wheelBase,selected:2,ctrl:true}),{rx:2,frequency:rx1+250,tx:tx+250});
assert.throws(()=>wheelTargets({...wheelBase,ctrl:true,tx:base.txCenter+275000}),/TX/);
assert.throws(()=>wheelTargets({...wheelBase,ctrl:true,rx:base.rxCenter+275000}),/RX1/);
assert.deepEqual(filterValues(100,4000),{low_hz:100,high_hz:4000});
assert.deepEqual(filterValues(301,2700),{low_hz:301,high_hz:2700});
assert.deepEqual(filterValues(2500,2700,'low'),{low_hz:2200,high_hz:2700});
assert.deepEqual(filterValues(301,500,'high'),{low_hz:301,high_hz:801});
assert.deepEqual(filterValues(449.5,950.5),{low_hz:449.5,high_hz:950.5});

const {FrameAssembler, displayOffsets, NoiseFloor, SpectrumPeaks, peakLabels, SpectrumAverage, axisFrequency, NB_BANDPLAN, bandplanSegments, AXIS_MODES} = await moduleAt('radio-display.js');
assert.deepEqual(AXIS_MODES.map(mode=>mode.value),['bandplan','downlink','uplink','pluto-rx','pluto-tx']);
assert.equal(NB_BANDPLAN[0].start,10489500000);assert.equal(NB_BANDPLAN.at(-1).end,10490000000);
for(let i=1;i<NB_BANDPLAN.length;i++)assert.equal(NB_BANDPLAN[i-1].end,NB_BANDPLAN[i].start,'Bandplan has a gap or overlap');
assert.equal(NB_BANDPLAN.find(item=>item.kind==='broadcast').end,10489857500);
const bands=bandplanSegments(10489750000,600000,1200,text=>text.length*6,30);
assert.equal(bands.length,11);assert(Math.abs(bands[0].position-1/12)<1e-12);
assert.equal(bands.filter(band=>band.kind==='beacon' && band.text==='B').length,3);
assert.equal(bands.find(band=>band.kind==='mixed').text,'Mixed + Contest');
assert(bandplanSegments(10489750000,600000,300,text=>text.length*5.4,24).some(band=>band.text==='CW'));
const clippedBands=bandplanSegments(10489850000,100000,500,text=>text.length*6);
assert(clippedBands.every(band=>band.position>=0 && band.position+band.size<=1));
assert.equal(bandplanSegments(1000000000,600000,1000,text=>text.length*6).length,0);
const axisSettings={lnb_reference_hz:25000000,pluto_tx_base_hz:1290250000,rx_correction_hz:123,tx_correction_hz:-456};
assert.equal(axisFrequency(10489750000,'downlink',axisSettings),10489750000);
assert.equal(axisFrequency(10489750000,'uplink',axisSettings),2400250000);
assert.equal(axisFrequency(10489750000,'pluto-rx',axisSettings),739750000);
assert.equal(axisFrequency(10489750000,'pluto-tx',axisSettings),1290250000);
assert.equal(axisFrequency(10489450000,'pluto-rx',{...axisSettings,lnb_reference_hz:24999999}),739450390);
assert.equal(axisFrequency(10490050000,'pluto-tx',{...axisSettings,pluto_tx_base_hz:2400250000}),2400550000);
assert(Number.isNaN(axisFrequency(10489750000,'pluto-rx')));
assert(Number.isNaN(axisFrequency(10489750000,'pluto-tx')));
const fft = (value, center=100, rate=8, n=8) => ({center,rate,n,values:new Int16Array(n).fill(value)});
const average = new SpectrumAverage();
const raw = fft(-1000);assert.equal(average.add(raw),raw.values);
average.setCount(2);
assert.equal(average.add(fft(0))[0],0);
assert(Math.abs(average.add(fft(-1000))[0]-1000*Math.log10(.55))<.001);
assert(Math.abs(average.add(fft(-2000))[0]-1000*Math.log10(.055))<.001,'Average is not a finite sliding window');
assert.equal(average.frames.length,2);
assert.equal(average.add(fft(-4000,125))[0],-4000,'Tuning mixed different RF bins');
assert.equal(average.add(fft(-6000,125,16))[0],-6000);
assert.equal(average.add(fft(-7000,125,16,16))[0],-7000);
average.setCount(20);average.add(fft(0));
for(let i=0;i<20;i++)average.add(fft(-4000));
assert.equal(average.frames.length,20);assert.equal(average.values[0],-4000,'A departed signal left a smoothing tail');
average.reset();assert.equal(average.count,20);assert.equal(average.add(fft(-5000))[0],-5000);
average.setCount(1);assert.equal(average.add(raw),raw.values);assert.equal(raw.values[0],-1000);
average.setCount(100);assert.equal(average.count,20);average.setCount(NaN);assert.equal(average.count,1);
assert.deepEqual(displayOffsets(), {floor:-5,ceiling:40});
assert.deepEqual(displayOffsets(null,null), {floor:-5,ceiling:40});
assert.deepEqual(displayOffsets(-7,37), {floor:-7,ceiling:37});
assert.deepEqual(displayOffsets(-999,999), {floor:-10,ceiling:40});
for(let floor=-20;floor<=20;floor++) for(const ceiling of [0,30,33,38,45]) {
  const range=displayOffsets(floor,ceiling);
  assert(Number.isInteger(range.floor) && Number.isInteger(range.ceiling) && range.ceiling-range.floor>=20);
}
const noiseFrame=(level,rate=600000)=>({center:10489750000,rate,n:8192,values:Int16Array.from({length:8192},(_,i)=>level*100+(i%11-5)*10)});
const noise=new NoiseFloor(), occupied=noiseFrame(-60);
occupied.values.fill(-14000,0,50);occupied.values.fill(-14000,8142); // Edge roll-off.
occupied.values.fill(-15500,4094,4099); // DC notch.
occupied.values.fill(-2000,1500,3200); // Broad occupied part of the band.
for(let i=70;i<8192;i+=97)occupied.values[i]=-1000; // Narrow carriers.
assert(Math.abs(noise.add(occupied,600000,0)+60)<.2,'Signals, DC or edges changed the noise estimate');
const original=noise.value;
noise.add(noiseFrame(-50),600000,100);
assert(noise.value>original && noise.value<original+1,'A single frame made the display pump');
for(let now=200;now<=3000;now+=100)noise.add(noiseFrame(-50),600000,now);
assert(Math.abs(noise.value-(-50-10/Math.E))<.3,'Noise tracking has the wrong time constant');
const previous=noise.value, uncertain=noiseFrame(-60);
for(let i=0;i<8192;i++)uncertain.values[i]=-6000+Math.round(4000*i/8192);
assert.equal(noise.add(uncertain,600000,3100),previous);assert(!noise.reliable);
assert.equal(noise.add(noiseFrame(-160),600000,3200),previous);assert(!noise.reliable);
const zoomNoise=new NoiseFloor(), zoomFrame=noiseFrame(-80,75000);
zoomFrame.values.fill(-15000,0,1366);zoomFrame.values.fill(-15000,6827);
zoomFrame.values.fill(-3000,4000,4200);
assert(Math.abs(zoomNoise.add(zoomFrame,50000,0)+80)<.2,'Invisible zoom skirts influenced the noise floor');
assert(Math.abs(noise.add(noiseFrame(-90,75000),50000,3300)+90)<.2,'Changed FFT geometry retained the previous calibration');
zoomNoise.reset();assert.equal(zoomNoise.value,null);assert(!zoomNoise.reliable);
const peaks=new SpectrumPeaks(), peakFrame=noiseFrame(-60);
peakFrame.values[1000]=-4500; // Exactly +15 dB: no label.
peakFrame.values[2000]=-3500;
peakFrame.values.fill(-3000,4000,4005); // A plateau is one peak.
peaks.add(peakFrame,-60,0);
assert.deepEqual(peaks.get(600000,0).map(p=>p.strength),[25,30]);
assert.equal(peaks.get(600000,0)[1].position,4002/8192);
const quietFrame=noiseFrame(-60);quietFrame.values[2000]=-4200;
peaks.add(quietFrame,-60,250);
assert.equal(peaks.get(600000,999)[0].strength,25,'A brief maximum was lost before one second');
assert.equal(peaks.get(600000,1001)[0].strength,18,'A departed peak outlived its measurement window');
assert.equal(peaks.get(600000,1251).length,0,'Stopped FFTs left stale labels');
peaks.add(peakFrame,null,1300);assert.equal(peaks.get(600000,1300).length,0,'Uncalibrated noise produced peak measurements');
peaks.add(peakFrame,-70,1400);assert.equal(peaks.get(600000,1400).find(p=>p.position===1000/8192).strength,25,'Peak values are not relative to the measured noise');
peaks.add({...quietFrame,center:peakFrame.center+25},-60,1450);
assert.deepEqual(peaks.get(600000,1450).map(p=>p.strength),[18],'Retuning retained peaks at old RF bins');
peaks.reset();assert.equal(peaks.get().length,0);
const zoomPeaks=new SpectrumPeaks(), peakZoom=noiseFrame(-80,75000);
peakZoom.values[100]=-1000;peakZoom.values[3000]=-5500;peakZoom.values[8000]=-1000;
zoomPeaks.add(peakZoom,-80,0);
assert.deepEqual(zoomPeaks.get(50000,0).map(p=>p.strength),[25],'Invisible zoom peaks received labels');
const labelOptions={width:800,height:120,floor:-65,ceiling:-20,measure:text=>text.length*7};
const crowded=[{position:.25,level:-35,strength:25},{position:.27,level:-38,strength:22},{position:.5,level:-28,strength:32},{position:.999,level:-40,strength:20}];
const labels=peakLabels(crowded,labelOptions);
assert.deepEqual(labels.map(label=>label.text),['+32dB','+25dB'],'Labels overlap or extend beyond the right edge');
assert(labels.every(label=>/^\+\d+dB$/.test(label.text) && label.x>0 && label.box.right<=800));
assert(labels.every(label=>label.y===14),'Labels are not aligned at the upper edge');
assert.equal(labels.find(label=>label.text==='+25dB').x+labelOptions.measure('+25dB')/2,200,'Label is not centered above its peak');
assert.deepEqual(peakLabels(crowded.map(peak=>({...peak,level:peak.level-15})),labelOptions),labels,'Changing peak height made labels jump');
assert.equal(peakLabels([{position:.01,level:-20,strength:45}],labelOptions).length,0,'Peak labels cover the dBFS axis text');
const mobileLabels=peakLabels(crowded,{...labelOptions,width:320,height:85});
assert(mobileLabels.length<crowded.length && mobileLabels.every(label=>label.box.right<=320));
for(let i=0;i<mobileLabels.length;i++)for(let j=i+1;j<mobileLabels.length;j++){
  const a=mobileLabels[i].box,b=mobileLabels[j].box;
  assert(a.right<=b.left || a.left>=b.right || a.bottom<=b.top || a.top>=b.bottom,'Mobile labels overlap');
}
const complete = [], assembler = new FrameAssembler(frame=>complete.push(frame));
function fragment(first, {session=7,stream=0,id=1,time=10n,n=8,rate=600000}={}) {
  const buffer=new ArrayBuffer(72),v=new DataView(buffer);
  v.setUint32(0,0x51545232);v.setUint8(4,1);v.setUint8(5,2);v.setUint16(6,64);
  v.setUint32(8,stream);v.setUint32(12,id);v.setBigUint64(16,time);v.setBigUint64(32,10489750000n);
  v.setUint32(40,rate);v.setUint32(44,n);v.setUint32(48,first);v.setUint16(52,4);v.setUint16(54,1);v.setUint32(56,n);v.setUint32(60,session);
  for(let i=0;i<4;i++)v.setInt16(64+i*2,(first+i)*100);
  return buffer;
}
assembler.accept(fragment(4),7);assembler.accept(fragment(4),7);assert.equal(complete.length,0);
assembler.accept(fragment(0),7);assert.equal(complete.length,1);assert.deepEqual([...complete[0].values],[0,100,200,300,400,500,600,700]);
assembler.accept(fragment(0),7);assembler.accept(fragment(4),7);assert.equal(complete.length,1);
assembler.reset();assembler.accept(fragment(0),8);assembler.accept(fragment(4),8);assert.equal(assembler.frames.size,0);
assembler.accept(fragment(0),7,11n);assert.equal(assembler.frames.size,0);
assembler.accept(fragment(0),7);assembler.accept(fragment(4,{rate:75000}),7);assert.equal(assembler.frames.size,0);
// Both zooms may share frame IDs and timestamps; fragments and duplicate rejection stay per stream.
assembler.reset();complete.length=0;
for(const first of [4,0])for(const stream of [0,1,2])assembler.accept(fragment(first,{stream,rate:stream?75000:600000}),7);
assert.deepEqual(complete.map(frame=>frame.stream),[0,1,2]);
for(const frame of complete)assert.deepEqual([...frame.values],[0,100,200,300,400,500,600,700]);
for(const stream of [1,2])for(const first of [0,4])assembler.accept(fragment(first,{stream,rate:75000}),7);
assert.equal(complete.length,3);
assembler.accept(fragment(0,{stream:3}),7);assert.equal(assembler.frames.size,0);
for(const first of [0,4])assembler.accept(fragment(first,{stream:2,time:11n,id:2,rate:75000}),7);
assert.equal(complete.length,4);assert.equal(complete.at(-1).stream,2);

const source = await readFile(new URL('../web/audio-worklet.js',import.meta.url),'utf8');
for(const rate of [12000,44100,48000]) {
  let Processor;const messages=[];
  const context=vm.createContext({sampleRate:rate,Float32Array,Math,
    AudioWorkletProcessor:class{constructor(){this.port={postMessage:m=>messages.push(m)};}},
    registerProcessor:(_,implementation)=>{Processor=implementation;}});
  vm.runInContext(source,context);const p=new Processor();
  p.port.onmessage({data:{type:'capture',enabled:true,generation:'current'}});
  let remaining=rate*.04;
  while(remaining>0){const n=Math.min(128,remaining);p.process([[new Float32Array(n).fill(.25)]],[[new Float32Array(n)]]);remaining-=n;}
  assert.equal(messages.length,2);assert.equal(messages[0].samples.length,240);assert.equal(messages[0].generation,'current');
  p.port.onmessage({data:{type:'capture',enabled:false}});p.process([[new Float32Array(128)]],[[new Float32Array(128)]]);assert.equal(messages.length,2);
  const samples=new Float32Array(240).fill(.3);
  for(const index of [0,240,480,480])p.port.onmessage({data:{type:'samples',identity:'rx',index,samples}});
  assert.equal(p.write,720);const output=new Float32Array(128);p.process([],[[output]]);assert(output.some(v=>v>0));
  p.port.onmessage({data:{type:'samples',identity:'new-epoch',index:0,samples}});assert.equal(p.write,240);
  // Retuning the same RX must not discard queued PCM or prime playback again.
  const continuous=new Processor();
  const packet=(epoch,index,frequency=10489700000,source='7:1')=>continuous.port.onmessage({data:{type:'samples',identity:`${source}:${epoch}`,source,frequency,index,samples}});
  for(let i=0;i<4;i++)packet(1,i*240);
  continuous.process([],[[new Float32Array(128)]]);
  const before={read:continuous.read,write:continuous.write,phase:continuous.phase};
  assert(continuous.playing);
  packet(2,0,10489700025);
  assert.equal(continuous.read,before.read);assert.equal(continuous.write,before.write+240);
  assert.equal(continuous.phase,before.phase);assert(continuous.playing);
  packet(2,0,10489700025);assert.equal(continuous.write,before.write+240);
  const tunedOutput=new Float32Array(128);continuous.process([],[[tunedOutput]]);
  assert(tunedOutput.every(v=>v>0),'Retuning introduced silence into buffered audio');
  // Unrelated epoch changes and receiver/session switches still discard old audio.
  packet(3,0,10489700025);assert.equal(continuous.write,240);assert(!continuous.playing);
  packet(1,0,10489700100,'7:2');assert.equal(continuous.write,240);assert(!continuous.playing);
  packet(1,0,10489700125,'8:2');assert.equal(continuous.write,240);assert(!continuous.playing);
}
const {RadioAudio} = await moduleAt('audio.js');
Object.defineProperty(globalThis,'navigator',{value:{mediaDevices:{addEventListener(){}}},configurable:true});
globalThis.localStorage={getItem:()=>null,setItem:()=>{}};
const audio=new RadioAudio(()=>{},()=>{});
audio.devices=[
  {kind:'audioinput',deviceId:'default',label:'Default - USB Advanced Audio Device'},
  {kind:'audioinput',deviceId:'built-in',label:'Built-in microphone'},
  {kind:'audioinput',deviceId:'usb-monitor',label:'Monitor of USB Advanced Audio Device Analog Stereo'},
  {kind:'audioinput',deviceId:'usb-in',label:'USB Advanced Audio Device Analog Stereo'},
  {kind:'audiooutput',deviceId:'speakers',label:'Built-in speakers'},
  {kind:'audiooutput',deviceId:'usb-out',label:'USB Advanced Audio Device Analog Stereo'},
];
let routed;
audio.context={setSinkId:async id=>{routed=id;}};
assert.equal(audio.deviceId('audioinput'),'usb-in');
assert.equal(audio.deviceId('audiooutput'),'usb-out');
await audio.routeOutput();assert.equal(routed,'usb-out');
await audio.selectDevice('audiooutput','default');assert.equal(routed,'');
audio.outputId='';audio.context={};assert.equal(audio.deviceId('audiooutput'),'default');
// The existing default USB output must not be switched to an opaque sink ID.
audio.devices.push({kind:'audiooutput',deviceId:'default',label:'Default - USB Advanced Audio Device'});
let switched=0;
audio.context={sinkId:'',setSinkId:async()=>{switched++;}};
await audio.routeOutput();assert.equal(switched,0);assert.equal(audio.activeOutputId,'default');
// A stale saved ID is discarded only after labeled devices have been exposed.
const stored=new Map([['qo100.audioInput','old-input'],['qo100.audioOutput','old-output']]);
globalThis.localStorage={getItem:key=>stored.get(key),setItem:(key,value)=>stored.set(key,value),removeItem:key=>stored.delete(key)};
const freshDevices=audio.devices.filter(d=>d.deviceId!=='default');
navigator.mediaDevices.enumerateDevices=async()=>freshDevices;
const recovered=new RadioAudio(()=>{},()=>{});
await recovered.refreshDevices();
assert.equal(recovered.deviceId('audioinput'),'usb-in');assert.equal(recovered.deviceId('audiooutput'),'usb-out');
assert(!stored.has('qo100.audioInput'));assert(!stored.has('qo100.audioOutput'));
// An invalid output sink is a playback warning; it must not abort input setup.
recovered.context={sinkId:'',setSinkId:async()=>{throw new DOMException('Invalid sink','NotFoundError');}};
await recovered.routeOutput();assert.match(recovered.warning,/Wiedergabe/);
let getCalls=0,stopped=false,startedAfterPermission=false;
const track={label:'USB Advanced Audio Device Analog Stereo',getSettings:()=>({deviceId:'permission-scoped-id'}),stop:()=>{stopped=true;}};
const stream={getTracks:()=>[track],getAudioTracks:()=>[track]};
navigator.mediaDevices.getUserMedia=async constraints=>{getCalls++;assert.equal(constraints.audio.deviceId?.exact,'usb-in');return stream;};
const capture=new RadioAudio(()=>{},()=>{});
capture.context={sampleRate:44100,createMediaStreamSource:()=>({connect(){},disconnect(){}}),createBiquadFilter:()=>({frequency:{},Q:{},connect(){},disconnect(){}})};
capture.node={};
capture.start=async()=>{startedAfterPermission=getCalls>0;capture.enabled=true;};
await capture.enableMic();
assert.equal(getCalls,1);assert(startedAfterPermission);assert.equal(capture.microphone,stream);assert(!stopped);
// Brave may identify the same USB input as "Standard" with a default device ID.
freshDevices.find(d=>d.deviceId==='usb-in').groupId='usb-group';
track.label='Standard';track.getSettings=()=>({deviceId:'default',groupId:'usb-group'});
const defaultCapture=new RadioAudio(()=>{},()=>{});
defaultCapture.context=capture.context;defaultCapture.node={};defaultCapture.start=async()=>{};
await defaultCapture.enableMic();
assert.equal(getCalls,2);assert(!stopped);
assert.equal(defaultCapture.deviceLabel('audioinput'),'USB Advanced Audio Device Analog Stereo');
// A browser may ignore an ideal device ID and open its camera microphone instead.
// Explicit switching must request an exact ID and use the matching track.
const cameraDevice={kind:'audioinput',deviceId:'camera-in',label:'USB 2.0 Camera Analog Stereo',groupId:'camera-group'};
const inputDevices=[...freshDevices,cameraDevice];
const inputStream=device=>{
  const track={label:device.label,stopped:false,getSettings:()=>({deviceId:device.deviceId,groupId:device.groupId}),stop(){this.stopped=true;}};
  return {getTracks:()=>[track],getAudioTracks:()=>[track]};
};
const mic=()=>{
  const audio=new RadioAudio(()=>{},()=>{});audio.inputId='';audio.outputId='';
  audio.context={...capture.context,state:'running'};audio.node={port:{postMessage(){}}};audio.start=async()=>{audio.enabled=true;};
  return audio;
};
const usbDevice=inputDevices.find(d=>d.deviceId==='usb-in');
navigator.mediaDevices.enumerateDevices=async()=>inputDevices;
let requests=[];
navigator.mediaDevices.getUserMedia=async ({audio})=>{
  requests.push(audio);
  return inputStream(inputDevices.find(d=>d.deviceId===audio.deviceId?.exact) || cameraDevice);
};
const switchedInput=mic();switchedInput.inputId='camera-in';
await switchedInput.enableMic();const oldCamera=switchedInput.microphone.getAudioTracks()[0];
await switchedInput.selectDevice('audioinput','usb-in');
assert(oldCamera.stopped);assert.equal(requests.at(-1).deviceId.exact,'usb-in');
assert.equal(switchedInput.deviceLabel('audioinput'),usbDevice.label);assert.equal(stored.get('qo100.audioInput'),'usb-in');
switchedInput.stopMic();
// First grant: expose devices using a temporary default stream, then open USB exactly.
let permission=false,probe;
requests=[];
navigator.mediaDevices.enumerateDevices=async()=>permission?inputDevices:[];
navigator.mediaDevices.getUserMedia=async ({audio})=>{
  requests.push(audio);permission=true;
  if(!audio.deviceId) return probe=inputStream(cameraDevice);
  assert.equal(audio.deviceId.exact,'usb-in');return inputStream(usbDevice);
};
const firstGrant=mic();await firstGrant.enableMic();
assert.equal(requests.length,2);assert(!requests[0].deviceId);assert(probe.getTracks()[0].stopped);
assert.equal(firstGrant.deviceLabel('audioinput'),usbDevice.label);firstGrant.stopMic();
// Stale IDs / Brave permission mapping may require a default permission probe.
navigator.mediaDevices.enumerateDevices=async()=>inputDevices;
requests=[];
navigator.mediaDevices.getUserMedia=async ({audio})=>{
  requests.push(audio);
  if(requests.length===1) throw new DOMException('Mapping not ready','NotFoundError');
  if(!audio.deviceId) return probe=inputStream(cameraDevice);
  assert.equal(audio.deviceId.exact,'usb-in');return inputStream(usbDevice);
};
const retryInput=mic();await retryInput.enableMic();
assert.equal(requests.length,3);assert(probe.getTracks()[0].stopped);
assert.equal(retryInput.deviceLabel('audioinput'),usbDevice.label);retryInput.stopMic();
// Never connect the camera when exact selection is rejected or ignored, even on retry.
for(const rejects of [false,true]){
  const opened=[];let connected=false;
  navigator.mediaDevices.getUserMedia=async ({audio})=>{
    if(rejects && audio.deviceId)throw new DOMException('USB unavailable','OverconstrainedError');
    const stream=inputStream(cameraDevice);opened.push(stream);return stream;
  };
  const wrongInput=mic();wrongInput.context.createMediaStreamSource=()=>{connected=true;throw new Error('Wrong microphone connected');};
  await assert.rejects(wrongInput.enableMic());
  assert(!connected);assert.equal(wrongInput.microphone,null);assert(wrongInput.running);
  assert(opened.every(stream=>stream.getTracks()[0].stopped));
  assert.match(wrongInput.error,rejects?/Gerätezuordnung/:/USB 2.0 Camera.*statt.*USB Advanced/);
}
const {resolveLanguage} = await moduleAt('language.js');
assert.equal(resolveLanguage('auto','de-DE'),'de');assert.equal(resolveLanguage('auto','de-AT'),'de');
assert.equal(resolveLanguage('auto','DE'),'de');assert.equal(resolveLanguage('auto','en-US'),'en');
assert.equal(resolveLanguage('auto','fr-FR'),'en');assert.equal(resolveLanguage('en','de-DE'),'en');
assert.equal(resolveLanguage('de','en-US'),'de');assert.equal(resolveLanguage('invalid','en-US'),'en');
console.log('WebGUI: RF mapping, split tuning, RX2, bounds, FFT assembly, robust noise estimation/tracking, relative display ranges, 12/44.1/48 kHz audio, USB device selection and language detection passed.');
