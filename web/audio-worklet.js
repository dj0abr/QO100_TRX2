// The network always uses 12 kHz. The context uses the sound device's native
// rate, and this processor resamples playback and microphone audio accordingly.
class RadioAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ring = new Float32Array(12000);
    this.gain = 1; this.capture = false; this.mic = []; this.capturePhase = 0;
    this.reset();
    this.port.onmessage = ({data: d}) => {
      if (d.type === 'reset') this.reset();
      if (d.type === 'gain') this.gain = Math.max(0, Math.min(20, d.value));
      if (d.type === 'capture') { this.capture = d.enabled; this.generation = d.generation; this.mic = []; this.capturePhase = 0; }
      if (d.type !== 'samples') return;
      if (d.identity !== this.identity) {
        // A tuning epoch restarts packet indices, not the receiver's playback.
        const retuned = d.source && d.source === this.source && d.frequency !== this.frequency;
        if (!retuned) this.reset();
        this.next = null; this.identity = d.identity; this.source = d.source; this.frequency = d.frequency;
      }
      if (this.next !== null && d.index < this.next) return;
      if (this.next !== null && d.index - this.next > 2400) {
        this.reset(); this.identity = d.identity; this.source = d.source; this.frequency = d.frequency; this.next = d.index;
      }
      for (let i = this.next ?? d.index; i < d.index; i++) this.ring[this.write++ % this.ring.length] = 0;
      for (const value of d.samples) this.ring[this.write++ % this.ring.length] = value;
      this.next = d.index + d.samples.length;
      if (this.write - this.read > 2400) { this.read = this.write - 960; this.phase = 0; }
    };
  }
  reset() { this.read = this.write = 0; this.phase = 0; this.playing = false; this.next = null; this.identity = ''; this.source = ''; this.frequency = undefined; }
  process(inputs, outputs) {
    const output = outputs[0][0];
    if (this.write - this.read >= 720) this.playing = true;
    const correction = Math.max(.995, Math.min(1.005, 1 + (this.write - this.read - 960) * .00001));
    const step = 12000 / sampleRate * correction;
    for (let i = 0; i < output.length; i++) {
      if (this.playing && this.write - this.read >= 2) {
        const a = this.ring[this.read % this.ring.length], b = this.ring[(this.read + 1) % this.ring.length];
        output[i] = (a + (b - a) * this.phase) * this.gain;
        this.phase += step;
        const advance = Math.floor(this.phase); this.read += advance; this.phase -= advance;
      } else { output[i] = 0; this.playing = false; }
    }
    const mic = inputs[0]?.[0];
    if (this.capture && mic) for (const value of mic) {
      this.capturePhase += 12000 / sampleRate;
      if (this.capturePhase >= 1 - 1e-10) {
        this.capturePhase = Math.max(0, this.capturePhase - 1); this.mic.push(Math.max(-1, Math.min(1, value)));
        if (this.mic.length === 240) {
          const samples = new Float32Array(this.mic);
          this.port.postMessage({samples, generation: this.generation}, [samples.buffer]); this.mic = [];
        }
      }
    }
    return true;
  }
}
registerProcessor('radio-audio', RadioAudioProcessor);
