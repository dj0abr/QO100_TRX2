const $=id=>document.getElementById(id);
let socket,operator=false,state={},request=0,audioContext,audioNode,microphone;
let audioPromise,micBusy=false,guiLoaded=false;
let pttHeld=false,txIdentity='',txIndex=0,capture=false,lastActivity=0,session=0,loopGeneration=-1,cutoff=0n;
const frames=new Map(),lastFrames=new Map(),displays=new Map();
function status(text,error=false) {$('status').textContent=text;$('status').classList.toggle('error',error);}
function send(message) {if(socket?.readyState===WebSocket.OPEN) socket.send(JSON.stringify(message));}
function command(type,values={}) {if(operator) send({type,id:++request,...values});}
function controls() {
  for(const node of document.querySelectorAll('[data-control]')) node.disabled=!operator||!state.ready;
  $('ptt').disabled=!operator||!state.ready||!microphone||!!(state.flags&6);
  $('microphone').disabled=!operator||!state.ready||!!microphone||micBusy;
}
function stopPtt() {
  if(pttHeld) command('tx',{frequency_hz:state.tx?.frequency_hz||Math.round(Number($('tx').value)*1e6),ptt:false});
  pttHeld=false;capture=false;audioNode?.port.postMessage({type:'capture',enabled:false});$('ptt').classList.remove('sending');
}
function stopMic() {
  microphone?.getTracks().forEach(track=>track.stop());microphone=null;
  $('microphone').textContent='Mikrofon einschalten';
}
function connect() {
  if(socket && socket.readyState<2) return;
  const url=new URL('/ws',location.href);url.protocol=location.protocol==='https:'?'wss:':'ws:';
  socket=new WebSocket(url);socket.binaryType='arraybuffer';state={};operator=false;controls();
  $('connect').disabled=true;
  socket.onopen=()=>status('Verbindung hergestellt. Warte auf Backend …');
  socket.onmessage=event=>{
    if(typeof event.data!=='string') {packet(event.data);return;}
    const message=JSON.parse(event.data);
    if(message.type==='role') {
      operator=message.role==='operator';$('role').textContent=operator?'Bediener':'Zuschauer';
      if(!operator) {stopPtt();stopMic();}controls();
    } else if(message.type==='state') update(message);
    else if(message.type==='error') status(`${message.code}: ${message.message}`,true);
    else if(message.type==='ack' && message.status) status(`${message.command}: Backend-Status ${message.status}`,true);
    else if(message.type==='ack' && message.command==='settings_set') status('Settings gespeichert.');
    else if(message.type==='closed') status(message.reason==='idle'?'Nach fünf Minuten ohne Bedienung abgemeldet. Erneut verbinden, um sich neu anzumelden.':`Abgemeldet: ${message.reason}`,true);
  };
  socket.onclose=()=>{stopPtt();stopMic();operator=false;controls();$('role').textContent='Getrennt';$('connect').disabled=false;};
  socket.onerror=()=>status('WebSocket-Verbindung fehlgeschlagen.',true);
}
function update(next) {
  if(next.session!==session) {
    session=next.session;frames.clear();lastFrames.clear();loopGeneration=-1;cutoff=0n;
    guiLoaded=false;
    stopPtt();audioNode?.port.postMessage({type:'reset'});
  }
  const generation=next.loop?.generation;
  if(generation!==undefined && generation!==loopGeneration) {
    if(loopGeneration!==-1) cutoff=BigInt(next.loop.timestamp_ns);
    loopGeneration=generation;frames.clear();lastFrames.clear();audioNode?.port.postMessage({type:'reset'});
  }
  state=next;controls();
  if(!guiLoaded && next.settings?.gui_audio_gain!==undefined) {
    guiLoaded=true;$('volume').value=next.settings.gui_audio_gain;
    audioNode?.port.postMessage({type:'gain',value:Number($('volume').value)});
  }
  if(!next.ready) {stopPtt();status('Warte auf Backend und bestätigten sicheren Zustand …');}
  else if(next.flags&2) status('Hardware wird umgestellt …');
  else if(next.flags&4) status(`Hardware offline: ${next.settings?.hardware_error||next.settings?.error||'Verbindung prüfen'}`,true);
  else if(!next.tx?.ptt) status(operator?'Bedienbereit.':'Zuschauer: Empfang läuft. Steuerrecht nur nach neuer Anmeldung, wenn der Platz frei ist.');
  const fields={rx1:next.rx?.['1']?.frequency_hz,rx2:next.rx?.['2']?.frequency_hz,tx:next.tx?.frequency_hz,
    bandwidth:next.filter?.bandwidth_hz,center:next.filter?.center_hz,nr:next.nr?.percent,compressor:next.compressor?.percent,loop:next.loop?.mode};
  for(const [key,value] of Object.entries(fields)) if(value!==undefined && document.activeElement!==$(key))
    $(key).value=['rx1','rx2','tx'].includes(key)?(value/1e6).toFixed(6):value;
  $('nr-value').value=$('nr').value;$('compressor-value').value=$('compressor').value;
  $('settings').textContent=JSON.stringify(next.settings,null,2);
  const identity=`${next.session}:${next.tx?.epoch}`;
  if(identity!==txIdentity) {txIdentity=identity;txIndex=0;}
  const enabled=!!(pttHeld && operator && next.ready && next.tx?.ptt);
  if(capture!==enabled) {capture=enabled;audioNode?.port.postMessage({type:'capture',enabled});}
  $('ptt').classList.toggle('sending',!!next.tx?.ptt);$('tx-status').textContent=next.tx?.ptt?'Senden':'Empfang';
}
function packet(buffer) {
  if(buffer.byteLength<64||buffer.byteLength>1200) return;
  const v=new DataView(buffer),u=new Uint8Array(buffer);
  if(String.fromCharCode(...u.subarray(0,4))!=='QTR2'||u[4]!==1||v.getUint16(6)!==64||v.getUint32(60)!==session) return;
  const type=u[5],stream=v.getUint32(8),id=v.getUint32(12),time=v.getBigUint64(16),index=v.getBigUint64(24);
  if(time<cutoff) return;
  if(type===5 && buffer.byteLength===544 && v.getUint16(54)===2 && v.getUint32(40)===12000) {
    const desired=state.loop?.mode===1?4:Number($('audio-stream').value);
    const epoch=stream===4?state.tx?.epoch:state.rx?.[String(stream)]?.epoch;
    if(!audioNode||stream!==desired||id!==epoch) return;
    const samples=new Float32Array(240);for(let i=0;i<240;i++) samples[i]=v.getInt16(64+2*i)/32768;
    audioNode.port.postMessage({type:'samples',identity:`${session}:${stream}:${id}`,index:Number(index),samples},[samples.buffer]);return;
  }
  if(type!==2||stream>1||v.getUint16(54)!==1) return;
  const n=v.getUint32(44),first=v.getUint32(48),count=v.getUint16(52),rate=v.getUint32(40),center=Number(v.getBigUint64(32));
  if(!n||n>65536||(n&(n-1))||v.getUint32(56)!==n||first+count>n||buffer.byteLength!==64+2*count) return;
  if(stream===1 && center!==state.rx?.['1']?.frequency_hz) return;
  if(lastFrames.has(stream) && time<=lastFrames.get(stream)) return;
  const key=`${stream}:${id}`,now=performance.now();
  for(const [key,f] of frames) if(now-f.started>500) frames.delete(key);
  if(!frames.has(key)) {
    if(frames.size>6) frames.delete(frames.keys().next().value);
    frames.set(key,{stream,id,n,rate,center,time,index,values:new Int16Array(n),seen:new Uint8Array(n),received:0,started:now});
  }
  const f=frames.get(key);
  if(f.n!==n||f.rate!==rate||f.center!==center||f.time!==time||f.index!==index) {frames.delete(key);return;}
  for(let k=0;k<count;k++) if(!f.seen[first+k]) {f.seen[first+k]=1;f.values[first+k]=v.getInt16(64+2*k);f.received++;}
  if(f.received===n) {frames.delete(key);lastFrames.set(stream,time);draw(f);}
}
function draw(frame) {
  const canvas=$(frame.stream?'zoom':'wide'),ctx=canvas.getContext('2d');
  ctx.drawImage(canvas,0,1);const row=ctx.createImageData(canvas.width,1);
  const span=frame.stream?50000:frame.rate,floor=Number(state.settings?.gui_display_floor??-100);
  for(let x=0;x<canvas.width;x++) {
    const bin=Math.min(frame.n-1,Math.max(0,Math.floor(frame.n/2+(x/canvas.width-.5)*span/frame.rate*frame.n)));
    const level=Math.min(1,Math.max(0,(frame.values[bin]/100-floor)/65));
    row.data.set([Math.round(255*Math.max(0,(level-.5)*2)),Math.round(255*Math.min(1,level*1.8)),Math.round(255*Math.min(1,level*3)),255],x*4);
  }
  ctx.putImageData(row,0,0);displays.set(frame.stream,{center:frame.center,span});
  $(frame.stream?'zoom-frequency':'wide-frequency').textContent=`${((frame.center-span/2)/1e6).toFixed(6)} – ${((frame.center+span/2)/1e6).toFixed(6)} MHz`;
}
async function audio() {
  if(!audioPromise) audioPromise=(async()=>{
    audioContext=new AudioContext({sampleRate:12000,latencyHint:'interactive'});
    await audioContext.audioWorklet.addModule('/audio-worklet.js');
    audioNode=new AudioWorkletNode(audioContext,'radio-audio',{outputChannelCount:[1]});audioNode.connect(audioContext.destination);
    audioNode.port.onmessage=({data:pcm})=>{
      if(!capture||!operator||!state.tx?.ptt||socket?.readyState!==WebSocket.OPEN) return;
      const data=new ArrayBuffer(544),v=new DataView(data);new Uint8Array(data).set([81,84,82,50,1,8,0,64]);
      v.setUint32(8,3);v.setUint32(12,state.tx.epoch);v.setBigUint64(24,BigInt(txIndex));v.setBigUint64(32,BigInt(state.tx.frequency_hz));
      v.setUint32(40,12000);v.setUint16(52,240);v.setUint16(54,2);v.setUint32(56,240);v.setUint32(60,state.session);
      for(let i=0;i<240;i++) v.setInt16(64+2*i,Math.round(Math.max(-1,Math.min(1,pcm[i]))*32767));
      // A stalled TCP connection must not accumulate seconds of microphone audio.
      if(socket.bufferedAmount>16384) {stopPtt();status('Senden wegen Verbindungsrückstau beendet.',true);return;}
      socket.send(data);txIndex+=240;
    };
    audioNode.port.postMessage({type:'gain',value:Number($('volume').value)});
  })().catch(e=>{audioContext?.close();audioContext=null;audioPromise=null;throw e;});
  await audioPromise;
  await audioContext.resume();$('audio').textContent='Audio aktiv';
}
async function enableMic() {
  if(microphone||micBusy) return;micBusy=true;controls();
  try {
    await audio();if(!operator) return;
    microphone=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:false,noiseSuppression:false,autoGainControl:false}});
    if(!operator) {microphone.getTracks().forEach(t=>t.stop());microphone=null;return;}
    audioContext.createMediaStreamSource(microphone).connect(audioNode);$('microphone').textContent='Mikrofon aktiv';controls();
  } catch(e) {status(`Audio/Mikrofon: ${e.message}. Über die HTTPS-Adresse der OPNsense öffnen.`,true);}
  finally {micBusy=false;controls();}
}
function startPtt(event) {
  if(!operator||!state.ready||!microphone) return;
  event.preventDefault();$('ptt').setPointerCapture(event.pointerId);pttHeld=true;
  command('tx',{frequency_hz:Math.round(Number($('tx').value)*1e6),ptt:true});
}
$('connect').onclick=connect;$('audio').onclick=()=>audio().catch(e=>status(`Audio: ${e.message}`,true));
$('microphone').onclick=enableMic;
$('tune').onclick=()=>{
  stopPtt();command('tune',{stream:1,frequency_hz:Math.round(Number($('rx1').value)*1e6)});
  command('tune',{stream:2,frequency_hz:Math.round(Number($('rx2').value)*1e6)});
  command('tx',{frequency_hz:Math.round(Number($('tx').value)*1e6),ptt:false});
};
$('filter').onclick=()=>command('filter',{bandwidth_hz:Number($('bandwidth').value),center_hz:Number($('center').value)});
$('nr').onchange=()=>command('nr',{percent:Number($('nr').value)});
$('compressor').onchange=()=>command('compressor',{percent:Number($('compressor').value)});
for(const key of ['nr','compressor']) $(key).oninput=()=>$(key+'-value').value=$(key).value;
$('loop').onchange=()=>{stopPtt();command('loop',{mode:Number($('loop').value)});};
$('save').onclick=()=>{try {command('settings_set',{revision:state.revision,values:JSON.parse($('patch').value)});}catch(e){status(e.message,true);}};
$('volume').oninput=()=>audioNode?.port.postMessage({type:'gain',value:Number($('volume').value)});
$('audio-stream').onchange=()=>audioNode?.port.postMessage({type:'reset'});
$('ptt').onpointerdown=startPtt;for(const event of ['pointerup','pointercancel','lostpointercapture']) $('ptt').addEventListener(event,stopPtt);
window.addEventListener('blur',stopPtt);document.addEventListener('visibilitychange',()=>{if(document.hidden) stopPtt();});
for(const [stream,id] of [[0,'wide'],[1,'zoom']]) $(id).onclick=event=>{
  if(!operator||!displays.has(stream)) return;const d=displays.get(stream),bounds=$(id).getBoundingClientRect();
  command('tune',{stream:1,frequency_hz:Math.round(d.center+((event.clientX-bounds.left)/bounds.width-.5)*d.span)});
};
for(const event of ['pointermove','pointerdown','wheel','keydown','touchstart']) document.addEventListener(event,e=>{
  if(!e.isTrusted||e.repeat||!operator||performance.now()-lastActivity<1000) return;
  lastActivity=performance.now();send({type:'activity'});
},{passive:true});
setInterval(()=>send({type:'heartbeat'}),1000);
setInterval(()=>{if(pttHeld&&operator) send({type:'ptt_keepalive'});},250);
window.addEventListener('pagehide',()=>{stopPtt();socket?.close();});
controls();connect();
