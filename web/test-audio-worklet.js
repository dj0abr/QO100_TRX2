// The AudioContext runs at 12 kHz; Web Audio converts microphone/device rates.
class RadioAudio extends AudioWorkletProcessor {
  constructor() {
    super();this.ring=new Float32Array(12000);this.read=0;this.write=0;this.phase=0;
    this.playing=false;this.next=null;this.identity='';this.gain=1;this.capture=false;this.mic=[];
    this.port.onmessage=({data:d})=>{
      if(d.type==='reset') {this.read=this.write=0;this.phase=0;this.playing=false;this.next=null;this.identity='';}
      if(d.type==='gain') this.gain=d.value;
      if(d.type==='capture') {this.capture=d.enabled;this.mic=[];}
      if(d.type!=='samples') return;
      if(d.identity!==this.identity) {this.read=this.write=0;this.phase=0;this.playing=false;this.next=null;this.identity=d.identity;}
      if(this.next!==null && d.index<this.next) return;
      if(this.next!==null && d.index-this.next>2400) {this.read=this.write=0;this.playing=false;this.next=d.index;}
      for(let i=this.next??d.index;i<d.index;i++) this.ring[this.write++%this.ring.length]=0;
      for(const value of d.samples) this.ring[this.write++%this.ring.length]=value;
      this.next=d.index+d.samples.length;
      if(this.write-this.read>2400) {this.read=this.write-960;this.phase=0;}
    };
  }
  process(inputs,outputs) {
    const output=outputs[0][0];
    if(this.write-this.read>=720) this.playing=true;
    const step=Math.max(.995,Math.min(1.005,1+(this.write-this.read-960)*.00001));
    for(let i=0;i<output.length;i++) {
      if(this.playing && this.write-this.read>=2) {
        const a=this.ring[this.read%this.ring.length],b=this.ring[(this.read+1)%this.ring.length];
        output[i]=(a+(b-a)*this.phase)*this.gain;
        this.phase+=step;const advance=Math.floor(this.phase);this.read+=advance;this.phase-=advance;
      } else {output[i]=0;this.playing=false;}
    }
    const mic=inputs[0]?.[0];
    if(this.capture && mic) {
      for(const value of mic) {
        this.mic.push(Math.max(-1,Math.min(1,value)));
        if(this.mic.length===240) {const samples=new Float32Array(this.mic);this.port.postMessage(samples,[samples.buffer]);this.mic=[];}
      }
    }
    return true;
  }
}
registerProcessor('radio-audio',RadioAudio);
