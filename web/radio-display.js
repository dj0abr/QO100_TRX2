const COLORS = {rx1: '#55d9ee', rx2: '#e8bd68', tx: '#fa8f82'};
export const AXIS_MODES = [
  {value: 'bandplan', label: 'Bandplan'},
  {value: 'downlink', label: 'Downlink'},
  {value: 'uplink', label: 'Uplink'},
  {value: 'pluto-rx', label: 'Pluto RX'},
  {value: 'pluto-tx', label: 'Pluto TX'},
];
// AMSAT-DL NB operating guidelines, checked 2026-11-06:
// https://amsat-dl.org/p4-a-nb-transponder-bandplan-und-betriebsrichtlinien/
export const NB_BANDPLAN = [
  {start:10489500000,end:10489505000,kind:'beacon',labels:['B'],description:'Untere CW-Bake und Schutzband'},
  {start:10489505000,end:10489540000,kind:'cw',labels:['CW only','CW'],description:'Nur CW'},
  {start:10489540000,end:10489580000,kind:'nb-digital',labels:['NB digital','NB dig','NB'],description:'Schmalband-Digital, maximal 500 Hz'},
  {start:10489580000,end:10489650000,kind:'digital',labels:['Digital','Dig'],description:'Digital, maximal 2700 Hz'},
  {start:10489650000,end:10489745000,kind:'ssb',labels:['SSB only','SSB'],description:'Nur SSB, maximal 2700 Hz'},
  {start:10489745000,end:10489755000,kind:'beacon',labels:['B'],description:'Mittlere BPSK-Bake und Schutzbänder'},
  {start:10489755000,end:10489850000,kind:'ssb',labels:['SSB only','SSB'],description:'Nur SSB, maximal 2700 Hz'},
  {start:10489850000,end:10489857500,kind:'broadcast',labels:['Rundspruch','RS','R'],description:'Rundspruch 10489,855 MHz und Schutzbänder'},
  {start:10489857500,end:10489865000,kind:'emergency',labels:['Notfunk','NF','N'],description:'Notfunk 10489,860 MHz und Schutzbänder'},
  {start:10489865000,end:10489990000,kind:'mixed',labels:['Mixed + Contest','Mix+Contest','Mixed','Mix'],description:'Gemischte Betriebsarten und Sonderzwecke einschließlich Contest, maximal 2700 Hz'},
  // Adjacent multimedia (990–997) and upper CW (997–1000) beacon areas share one B.
  {start:10489990000,end:10490000000,kind:'beacon',labels:['B'],description:'Multimedia-Bake 10489,9935 MHz (990–997 kHz) und obere CW-Bake 10490,000 MHz, mit Schutzbändern'},
];

export function bandplanSegments(center, span, width, measure, reservedRight = 0, language = 'de') {
  const left = center - span / 2, right = center + span / 2;
  const segments = NB_BANDPLAN.filter(item => item.end > left && item.start < right).map(item => {
    const position = (Math.max(left, item.start) - left) / span, size = (Math.min(right, item.end) - Math.max(left, item.start)) / span;
    const labels = language === 'en' && item.kind === 'broadcast' ? ['Broadcast', 'BC', 'B'] : language === 'en' && item.kind === 'emergency' ? ['Emergency', 'EM', 'E'] : item.labels;
    return {...item,labels,position,size,text:''};
  });
  const occupied = [];
  for (const item of [...segments].sort((a,b) => Number(b.kind === 'beacon') - Number(a.kind === 'beacon') || a.start - b.start)) {
    const text = item.kind === 'beacon' ? 'B' : item.labels.find(label => measure(label) + 4 <= item.size * width);
    if (!text) continue;
    const middle = (item.position + item.size / 2) * width;
    const bounds = {left:middle - measure(text)/2 - .5,right:middle + measure(text)/2 + .5};
    if (bounds.left < 0 || bounds.right > width - reservedRight || occupied.some(other => bounds.left < other.right && bounds.right > other.left)) continue;
    item.text = text; occupied.push(bounds);
  }
  return segments;
}
export function axisFrequency(downlink, mode, settings = {}) {
  const uplink = downlink - 8_089_500_000;
  if (mode === 'uplink') return uplink;
  if (mode === 'pluto-rx') return downlink - Number(settings.lnb_reference_hz) * 390;
  if (mode === 'pluto-tx') return uplink - (2_400_250_000 - Number(settings.pluto_tx_base_hz));
  return downlink;
}
const clamp = value => Math.max(0, Math.min(1, value));
export function displayOffsets(floor, ceiling) {
  const snap = (value, fallback, min, max) => Math.round(Math.max(min, Math.min(max, value != null && Number.isFinite(Number(value)) ? Number(value) : fallback)));
  return {floor: snap(floor, -5, -10, 10), ceiling: snap(ceiling, 40, 30, 40)};
}

export class NoiseFloor {
  constructor() { this.reset(); }
  reset() { this.value = null; this.reliable = false; this.geometry = null; this.updated = null; }
  add(frame, span = frame.rate, now = performance.now()) {
    const {rate, n, values} = frame;
    if (this.geometry && (rate !== this.geometry.rate || n !== this.geometry.n || span !== this.geometry.span)) this.reset();
    this.geometry = {rate, n, span};
    // Use only the visible passband. Discard edge roll-off, the DC notch and clipped bins.
    const visible = Math.min(n, Math.floor(n * span / rate)), edge = Math.max(1, Math.ceil(visible * .01));
    const start = Math.ceil((n - visible) / 2) + edge, end = Math.floor((n + visible) / 2) - edge;
    const block = Math.max(8, Math.round(5000 * n / rate)), dc = Math.max(2, Math.ceil(100 * n / rate));
    const medians = [];
    for (let first = start; first < end; first += block) {
      const section = [];
      for (let i = first; i < Math.min(end, first + block); i++) {
        const value = values[i];
        if (Math.abs(i - n / 2) > dc && value > -16000 && value < 2000 && Number.isFinite(value)) section.push(value / 100);
      }
      if (section.length < Math.max(4, block / 2)) continue;
      section.sort((a,b) => a-b);
      medians.push((section[Math.floor((section.length-1)/2)] + section[Math.floor(section.length/2)]) / 2);
    }
    medians.sort((a,b) => a-b);
    const quantile = fraction => medians[Math.floor((medians.length-1) * fraction)];
    // An inconsistent lower tail cannot reliably identify a common noise floor.
    this.reliable = medians.length >= 3 && quantile(.35) - quantile(.05) <= 6;
    if (this.reliable) {
      const candidate = quantile(.2);
      const alpha = this.value === null ? 1 : 1 - Math.exp(-Math.max(0, Math.min(1000, now - this.updated)) / 3000);
      this.value = this.value === null ? candidate : this.value + alpha * (candidate - this.value);
    }
    this.updated = now;
    return this.value;
  }
}

export class SpectrumPeaks {
  constructor() { this.reset(); }
  reset() { this.frames = []; this.geometry = null; this.cache = null; }
  prune(now) {
    while (this.frames.length && this.frames[0].time < now - 1000) { this.frames.shift(); this.cache = null; }
  }
  add(frame, noise, now = performance.now()) {
    const {center, rate, n} = frame;
    if (this.geometry && (center !== this.geometry.center || rate !== this.geometry.rate || n !== this.geometry.n)) this.reset();
    this.geometry = {center, rate, n}; this.prune(now);
    if (!Number.isFinite(noise)) return;
    this.frames.push({time: now, noise, values: frame.values.slice()}); this.cache = null;
  }
  get(span = this.geometry?.rate, now = performance.now()) {
    this.prune(now);
    if (!this.frames.length) return [];
    if (this.cache?.span === span) return this.cache.peaks;
    const {rate, n} = this.geometry, visible = Math.min(n, n * span / rate);
    const first = Math.max(0, Math.ceil((n - visible) / 2)), end = Math.min(n, Math.ceil((n + visible) / 2));
    const strengths = new Float32Array(n).fill(-Infinity), levels = new Float32Array(n);
    for (const frame of this.frames) for (let i = first; i < end; i++) {
      const level = frame.values[i] / 100, strength = level - frame.noise;
      if (strength > strengths[i]) { strengths[i] = strength; levels[i] = level; }
    }
    const peaks = [];
    for (let i = first + 1; i < end - 1; i++) {
      const strength = strengths[i];
      if (strength <= 15 || strength <= strengths[i - 1]) continue;
      const start = i;
      let level = levels[i];
      // A flat-topped carrier is one peak, placed in the middle of its plateau.
      while (i + 1 < end && strengths[i + 1] === strength) { i++; level = Math.max(level, levels[i]); }
      if (i < end - 1 && strength > strengths[i + 1]) {
        peaks.push({position: ((start + i) / 2 - (n - visible) / 2) / visible, level, strength});
      }
    }
    this.cache = {span, peaks}; return peaks;
  }
}

export function peakLabels(peaks, {width, height, scale = 1, measure}) {
  const gap = 3 * scale, font = 11 * scale;
  const occupied = [
    {left: 0, right: 96 * scale, top: 0, bottom: 16 * scale},
    {left: 0, right: 75 * scale, top: height - 17 * scale, bottom: height},
  ];
  const labels = [];
  for (const peak of [...peaks].sort((a,b) => b.strength - a.strength || a.position - b.position)) {
    const text = `+${Math.round(peak.strength)}dB`, x = peak.position * width - measure(text) / 2;
    const y = font + 3 * scale;
    const box = {left: x - gap, right: x + measure(text) + gap, top: y - font - gap, bottom: y + gap};
    if (box.left < 0 || box.right > width || occupied.some(other => box.left < other.right && box.right > other.left && box.top < other.bottom && box.bottom > other.top)) continue;
    occupied.push(box); labels.push({text, x, y, box});
  }
  return labels;
}
const PLOT_COLORS = {
  blue: {line: '#55d9ee', stops: [[5,15,26],[7,32,52],[15,84,116],[41,159,177],[171,197,145],[249,209,115]]},
  red: {line: '#ff6870', stops: [[20,5,8],[50,7,15],[115,15,28],[185,40,40],[230,120,90],[255,235,220]]},
  green: {line: '#62ef91', stops: [[5,20,12],[7,45,24],[14,100,45],[35,170,80],[150,215,130],[230,255,200]]},
};
for (const theme of Object.values(PLOT_COLORS)) {
  const palette = [0,.16,.37,.6,.81,1].map((position,i) => [position,theme.stops[i]]);
  theme.lut = Array.from({length: 256}, (_, n) => {
    const level = n / 255;
    const i = Math.max(0, palette.findIndex((p, i) => i < palette.length - 1 && level >= p[0] && level <= palette[i + 1][0]));
    const [a, c] = palette[i], [b, d] = palette[i + 1];
    return c.map((v, j) => Math.round(v + (d[j] - v) * (level - a) / (b - a)));
  });
}

export class SpectrumAverage {
  constructor(count = 1) { this.setCount(count); }
  setCount(count) {
    const value = Number(count);
    count = Number.isFinite(value) ? Math.max(1, Math.min(20, Math.round(value))) : 1;
    if (count === this.count) return false;
    this.count = count; this.reset(); return true;
  }
  reset() { this.frames = []; this.geometry = null; this.sum = this.values = null; }
  add(frame) {
    if (this.count === 1) return frame.values;
    const {center, rate, n} = frame;
    if (!this.geometry || center !== this.geometry.center || rate !== this.geometry.rate || n !== this.geometry.n) {
      this.reset(); this.geometry = {center, rate, n};
      this.sum = new Float64Array(n); this.values = new Float32Array(n);
    }
    // Average linear power, then convert back to hundredths of dBFS.
    this.frames.push(Float64Array.from(frame.values, value => 10 ** (value / 1000)));
    if (this.frames.length > this.count) this.frames.shift();
    // Re-sum the small window so a departed strong signal cannot leave rounding residue.
    this.sum.fill(0);
    for (const powers of this.frames) for (let i = 0; i < n; i++) this.sum[i] += powers[i];
    for (let i = 0; i < n; i++) this.values[i] = 1000 * Math.log10(this.sum[i] / this.frames.length);
    return this.values;
  }
}

export class FrameAssembler {
  constructor(onFrame) { this.onFrame = onFrame; this.reset(); }
  reset() { this.frames = new Map(); this.last = new Map(); }
  accept(buffer, session, cutoff = 0n) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < 64 || buffer.byteLength > 1200) return;
    const v = new DataView(buffer);
    if (v.getUint32(0) !== 0x51545232 || v.getUint8(4) !== 1 || v.getUint8(5) !== 2 || v.getUint16(6) !== 64 || v.getUint32(60) !== session) return;
    const stream = v.getUint32(8), id = v.getUint32(12), time = v.getBigUint64(16), index = v.getBigUint64(24);
    const center = Number(v.getBigUint64(32)), rate = v.getUint32(40), n = v.getUint32(44), first = v.getUint32(48), count = v.getUint16(52);
    if (stream > 2 || time < cutoff || v.getUint16(54) !== 1 || !rate || !n || n > 65536 || (n & (n - 1)) || v.getUint32(56) !== n || !count || first + count > n || buffer.byteLength !== 64 + count * 2) return;
    if (this.last.has(stream) && time <= this.last.get(stream)) return;
    const now = performance.now();
    for (const [key, frame] of this.frames) if (now - frame.started > 500) this.frames.delete(key);
    const key = `${stream}:${id}`;
    if (!this.frames.has(key)) {
      if (this.frames.size >= 8) this.frames.delete(this.frames.keys().next().value);
      this.frames.set(key, {stream, id, time, index, center, rate, n, values: new Int16Array(n), seen: new Uint8Array(n), received: 0, started: now});
    }
    const frame = this.frames.get(key);
    if (frame.n !== n || frame.center !== center || frame.rate !== rate || frame.time !== time || frame.index !== index) { this.frames.delete(key); return; }
    for (let i = 0; i < count; i++) if (!frame.seen[first + i]) {
      frame.seen[first + i] = 1;
      frame.values[first + i] = v.getInt16(64 + 2 * i);
      frame.received++;
    }
    if (frame.received === n) {
      this.frames.delete(key);
      this.last.set(stream, time);
      this.onFrame(frame);
    }
  }
}

export class RadioDisplay {
  constructor(element, zoom = false) {
    this.element = element; this.zoom = zoom;
    this.spectrum = element.querySelector('[data-spectrum]');
    this.waterfall = element.querySelector('[data-waterfall]');
    this.overlay = element.querySelector('[data-overlay]');
    this.axis = element.querySelector('.frequency-axis');
    this.empty = element.querySelector('.display-empty');
    this.state = {}; this.selected = 1; this.axisMode = zoom ? 'downlink' : 'bandplan'; this.floor = -100; this.ceiling = -35; this.waterfallShift = 0;
    this.axisMeasure = document.createElement('canvas').getContext('2d'); this.axisLayout = null;
    this.average = new SpectrumAverage(); this.spectrumValues = null;
    this.peaks = new SpectrumPeaks();
    this.noise = new NoiseFloor(); this.offsets = displayOffsets(); this.refreshRange();
    this.color = 'blue'; this.language = 'de';
    this.observer = new ResizeObserver(() => this.resize());
    this.observer.observe(this.spectrum); this.observer.observe(this.waterfall);
    this.resize();
  }
  resize() {
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    for (const canvas of [this.spectrum, this.waterfall, this.overlay]) {
      const bounds = canvas.getBoundingClientRect();
      const width = Math.max(1, Math.round(bounds.width * dpr)), height = Math.max(1, Math.round(bounds.height * dpr));
      if (canvas.width === width && canvas.height === height) continue;
      let old;
      if (canvas === this.waterfall && canvas.width > 1) {
        old = document.createElement('canvas'); old.width = canvas.width; old.height = canvas.height;
        old.getContext('2d').drawImage(canvas, 0, 0);
        this.waterfallShift *= width / canvas.width;
      }
      canvas.width = width; canvas.height = height;
      if (old) canvas.getContext('2d').drawImage(old, 0, 0, width, height);
    }
    this.dpr = dpr;
    this.drawSpectrum(); this.drawMarkers(); this.drawAxis();
  }
  clear() {
    this.frame = null; this.geometry = null; this.waterfallShift = 0;
    this.average.reset(); this.spectrumValues = null;
    this.peaks.reset();
    this.noise.reset(); this.refreshRange();
    for (const canvas of [this.spectrum, this.waterfall, this.overlay]) canvas.getContext('2d').clearRect(0, 0, canvas.width, canvas.height);
    this.axis.replaceChildren(); this.empty.hidden = false;
    this.axisLayout = null;
    delete this.element.dataset.stream; delete this.element.dataset.center;
  }
  update(state, selected) {
    // Receiver changes start a new history and noise/average estimate; retuning keeps history.
    if (this.zoom && selected !== this.selected) this.clear();
    this.state = state; this.selected = selected;
    if (this.zoom) this.element.dataset.receiver = String(selected);
    this.drawSpectrum(); this.drawMarkers(); this.drawAxis();
  }
  setAxisMode(mode) {
    this.axisMode = this.zoom ? 'downlink' : AXIS_MODES.some(item => item.value === mode) ? mode : 'bandplan';
    this.drawAxis(); return this.axisMode;
  }
  setLanguage(language) {
    this.language = language; this.axisLayout = null; this.drawAxis();
  }
  refreshRange() {
    const noise = this.noise.value ?? -100;
    this.floor = noise + this.offsets.floor; this.ceiling = noise + this.offsets.ceiling;
  }
  setOffsets(floor, ceiling) {
    this.offsets = displayOffsets(floor, ceiling); this.refreshRange();
    this.drawSpectrum();
  }
  setAverage(count) {
    if (this.average.setCount(count)) {
      this.spectrumValues = this.frame ? this.average.add(this.frame) : null;
      this.drawSpectrum();
    }
    return this.average.count;
  }
  setColor(color) {
    color = Object.hasOwn(PLOT_COLORS, color) ? color : 'blue';
    if (color === this.color) return color;
    if (this.frame) {
      const ctx = this.waterfall.getContext('2d'), picture = ctx.getImageData(0, 0, this.waterfall.width, this.waterfall.height);
      const previous = PLOT_COLORS[this.color].lut, next = PLOT_COLORS[color].lut;
      const indices = new Map(previous.map(([r,g,b],i) => [(r << 16) | (g << 8) | b,i]));
      for (let i = 0; i < picture.data.length; i += 4) {
        if (!picture.data[i + 3]) continue;
        const r = picture.data[i], g = picture.data[i + 1], b = picture.data[i + 2], key = (r << 16) | (g << 8) | b;
        if (!indices.has(key)) {
          // Resized history may contain interpolated colors between palette entries.
          let closest = 0, distance = Infinity;
          for (let n = 0; n < previous.length; n++) {
            const [pr,pg,pb] = previous[n], d = (pr-r)**2 + (pg-g)**2 + (pb-b)**2;
            if (d < distance) { closest = n; distance = d; }
          }
          indices.set(key, closest);
        }
        picture.data.set(next[indices.get(key)], i);
      }
      ctx.putImageData(picture, 0, 0);
    }
    this.color = color; this.drawSpectrum(); return color;
  }
  accept(frame) {
    if (this.zoom && (frame.stream !== this.selected || frame.center !== this.state.rx?.[String(this.selected)]?.frequency_hz)) return;
    const geometry = {center: frame.center, span: this.zoom ? 50000 : frame.rate};
    if (this.geometry) {
      const canvas = this.waterfall, ctx = canvas.getContext('2d');
      if (geometry.span !== this.geometry.span) {
        ctx.clearRect(0, 0, canvas.width, canvas.height); this.waterfallShift = 0;
      } else {
        // Retain history at its RF frequencies as the zoom follows the selected RX.
        const shift = (this.geometry.center - geometry.center) / geometry.span * canvas.width + this.waterfallShift;
        const pixels = Math.round(shift); this.waterfallShift = shift - pixels;
        if (pixels) {
          ctx.drawImage(canvas, pixels, 0);
          ctx.clearRect(pixels > 0 ? 0 : Math.max(0, canvas.width + pixels), 0, Math.min(canvas.width, Math.abs(pixels)), canvas.height);
        }
      }
    }
    this.frame = frame;
    this.geometry = geometry;
    const now = performance.now();
    this.noise.add(frame, geometry.span, now); this.refreshRange();
    if (!this.zoom) this.peaks.add(frame, this.noise.value, now);
    this.spectrumValues = this.average.add(frame);
    this.empty.hidden = true;
    this.element.dataset.frames = String(Number(this.element.dataset.frames || 0) + 1);
    this.element.dataset.stream = String(frame.stream); this.element.dataset.center = String(frame.center);
    this.drawSpectrum(); this.drawWaterfall(); this.drawAxis(); this.drawMarkers();
  }
  levels(width, values = this.frame.values) {
    const {frame, geometry} = this;
    const result = new Float32Array(width);
    for (let x = 0; x < width; x++) {
      const start = Math.max(0, Math.floor(frame.n / 2 + (x / width - .5) * geometry.span / frame.rate * frame.n));
      const end = Math.min(frame.n, Math.max(start + 1, Math.ceil(frame.n / 2 + ((x + 1) / width - .5) * geometry.span / frame.rate * frame.n)));
      let level = -16000;
      for (let i = start; i < end; i++) level = Math.max(level, values[i]);
      result[x] = level / 100;
    }
    return result;
  }
  drawSpectrum() {
    const canvas = this.spectrum, ctx = canvas.getContext('2d'), w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    ctx.strokeStyle = '#263c4b'; ctx.lineWidth = 1;
    ctx.beginPath();
    for (let y = 1; y < 5; y++) { ctx.moveTo(0, y * h / 5); ctx.lineTo(w, y * h / 5); }
    for (let x = 1; x < 12; x++) { ctx.moveTo(x * w / 12, 0); ctx.lineTo(x * w / 12, h); }
    ctx.stroke();
    if (this.frame) {
      const levels = this.levels(w, this.spectrumValues ?? this.frame.values);
      ctx.beginPath(); ctx.moveTo(0, h);
      for (let x = 0; x < w; x++) ctx.lineTo(x, h * (1 - clamp((levels[x] - this.floor) / (this.ceiling - this.floor))));
      ctx.lineTo(w, h); ctx.closePath(); ctx.fillStyle = `${PLOT_COLORS[this.color].line}14`; ctx.fill();
      ctx.beginPath();
      for (let x = 0; x < w; x++) ctx.lineTo(x, h * (1 - clamp((levels[x] - this.floor) / (this.ceiling - this.floor))));
      ctx.strokeStyle = PLOT_COLORS[this.color].line; ctx.lineWidth = this.dpr || 1; ctx.stroke();
      this.paintMarkers(ctx, w, h);
      this.drawPeakLabels(ctx, w, h);
    }
    ctx.font = `${9 * (this.dpr || 1)}px monospace`; ctx.fillStyle = '#819bad';
    ctx.fillText(`${this.ceiling.toFixed(1)} dBFS`, 7 * this.dpr, 12 * this.dpr);
    ctx.fillText(this.floor.toFixed(1), 7 * this.dpr, h - 5 * this.dpr);
  }
  drawPeakLabels(ctx, width, height) {
    if (this.zoom) return;
    ctx.save();
    ctx.font = `${11 * this.dpr}px monospace`; ctx.textBaseline = 'bottom';
    const labels = peakLabels(this.peaks.get(this.geometry.span), {
      width, height, scale: this.dpr,
      measure: text => ctx.measureText(text).width,
    });
    ctx.fillStyle = '#c4a64b'; ctx.strokeStyle = '#080f16'; ctx.lineWidth = 3 * this.dpr; ctx.lineJoin = 'round';
    for (const label of labels) { ctx.strokeText(label.text, label.x, label.y); ctx.fillText(label.text, label.x, label.y); }
    ctx.restore();
  }
  drawWaterfall() {
    const c = this.waterfall, ctx = c.getContext('2d'), rowHeight = Math.max(1, Math.round(this.dpr));
    ctx.drawImage(c, 0, rowHeight);
    const row = ctx.createImageData(c.width, rowHeight), levels = this.levels(c.width);
    for (let x = 0; x < c.width; x++) {
      const color = PLOT_COLORS[this.color].lut[Math.round(clamp((levels[x] - this.floor) / (this.ceiling - this.floor)) * 255)];
      for (let y = 0; y < rowHeight; y++) row.data.set([...color, 255], (y * c.width + x) * 4);
    }
    ctx.putImageData(row, 0, 0);
  }
  paintMarkers(ctx, w, h) {
    if (!this.geometry) return;
    const left = this.geometry.center - this.geometry.span / 2;
    const x = hz => (hz - left) / this.geometry.span * w;
    const rx1 = this.state.rx?.['1']?.frequency_hz;
    const filter = this.state.filter;
    if (this.zoom && rx1 && filter?.bandwidth_hz) {
      const low = x(rx1 + filter.center_hz - filter.bandwidth_hz / 2);
      const high = x(rx1 + filter.center_hz + filter.bandwidth_hz / 2);
      ctx.fillStyle = '#55d9ee15'; ctx.fillRect(low, 0, high - low, h);
      ctx.strokeStyle = '#55d9ee60'; ctx.lineWidth = 1; ctx.strokeRect(low, 0, high - low, h);
    }
    ctx.save();
    if (!this.zoom) ctx.globalAlpha = 0.5;
    const markers = [[rx1, COLORS.rx1, false], [this.state.rx?.['2']?.frequency_hz, COLORS.rx2, false],
      [(this.selected === 2 ? this.state.tx2 : this.state.tx)?.frequency_hz + 8089500000, COLORS.tx, true]];
    for (const [hz, color, dashed] of markers) if (Number.isSafeInteger(hz) && x(hz) >= 0 && x(hz) <= w) {
      ctx.strokeStyle = color; ctx.lineWidth = this.dpr || 1;
      ctx.setLineDash(dashed ? [4 * this.dpr, 4 * this.dpr] : []);
      ctx.beginPath(); ctx.moveTo(x(hz), 0); ctx.lineTo(x(hz), h); ctx.stroke();
    }
    ctx.setLineDash([]);
    ctx.restore();
  }
  drawMarkers() {
    const ctx = this.overlay.getContext('2d'); ctx.clearRect(0, 0, this.overlay.width, this.overlay.height);
    this.paintMarkers(ctx, this.overlay.width, this.overlay.height);
  }
  drawAxis() {
    const bandplan = !this.zoom && this.axisMode === 'bandplan';
    this.axis.classList.toggle('bandplan-axis', bandplan);
    if (!this.geometry) return;
    if (bandplan) { this.drawBandplan(); return; }
    this.axisLayout = null;
    const width = this.axis.clientWidth, {center, span} = this.geometry, left = center - span / 2;
    const wanted = span / Math.max(2, Math.floor(width / 135));
    const power = 10 ** Math.floor(Math.log10(wanted));
    const step = [1, 2, 2.5, 5, 10].find(n => n * power >= wanted) * power;
    const points = [{hz: left, position: 0}];
    for (let hz = Math.ceil(left / step) * step; hz < left + span; hz += step) {
      const position = (hz - left) / span * 100;
      if (position * width / 100 > 120 && (100 - position) * width / 100 > 140) points.push({hz, position});
    }
    points.push({hz: left + span, position: 100});
    this.axis.replaceChildren(...points.map(({hz, position}, i) => {
      const label = document.createElement('span');
      label.style.setProperty('--position', `${position}%`);
      // The zoom follows the selected RX at 1-Hz resolution; never round edge labels to kHz.
      const displayed = axisFrequency(hz, this.zoom ? 'downlink' : this.axisMode, this.state.settings);
      label.textContent = Number.isFinite(displayed) ? (displayed / 1e6).toFixed(step < 1000 || displayed % 1000 !== 0 ? 6 : 3) : '—';
      if (i === points.length - 1) label.textContent += ' MHz';
      return label;
    }));
  }
  drawBandplan() {
    const {center,span} = this.geometry, width = this.axis.clientWidth, style = getComputedStyle(this.axis);
    // The font shorthand may be empty with tabular-nums; match the actual label font explicitly.
    const font = `${style.fontSize} ${style.fontFamily}`;
    const reserved = this.element.querySelector('.axis-switch')?.getBoundingClientRect().width ?? 0;
    const layout = `${center}:${span}:${width}:${font}:${reserved}:${this.language}`;
    if (this.axisLayout === layout) return;
    this.axisLayout = layout; this.axisMeasure.font = font;
    const segments = bandplanSegments(center, span, width, text => this.axisMeasure.measureText(text).width, reserved, this.language);
    this.axis.replaceChildren(...segments.map(item => {
      const section = document.createElement('div');
      section.className = `bandplan-segment bandplan-${item.kind}`;
      section.style.left = `${item.position * 100}%`; section.style.width = `${item.size * 100}%`;
      section.dataset.start = item.start; section.dataset.end = item.end;
      section.title = `${item.description}\n${(item.start/1e6).toFixed(6)} – ${(item.end/1e6).toFixed(6)} MHz Downlink · AMSAT-DL`;
      const label = document.createElement('span'); label.className = 'bandplan-label'; label.textContent = item.text;
      section.append(label); return section;
    }));
  }
  frequencyAt(event) {
    if (!this.geometry) return null;
    if (this.zoom && this.geometry.center !== this.state.rx?.[String(this.selected)]?.frequency_hz) return null;
    const bounds = event.currentTarget.getBoundingClientRect();
    return Math.round(this.geometry.center + ((event.clientX - bounds.left) / bounds.width - .5) * this.geometry.span);
  }
}
