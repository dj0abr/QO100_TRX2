import {displayOffsets, NoiseFloor, PLOT_COLORS, zoomMatchesFrequency} from './radio-display.js';

const historyLimit = 16384;
const spaceHz = 830, markHz = 1000, waterfallSpan = 800;
const leftOffset = (spaceHz + markHz - waterfallSpan) / 2;

class RttyWaterfall {
  constructor(element) {
    this.canvas = element.querySelector('canvas');
    this.context = this.canvas.getContext('2d');
    this.row = this.context.createImageData(this.canvas.width, 1);
    this.tones = [...element.querySelectorAll('.rtty-tone')];
    for (const [index, hz] of [spaceHz, markHz].entries()) {
      this.tones[index].style.setProperty('--tone-position', `${100 * (hz - leftOffset) / waterfallSpan}%`);
    }
    this.noise = new NoiseFloor();
    this.offsets = displayOffsets();
    this.color = 'blue';
    this.update(undefined, false);
  }

  update(frequency, reverse) {
    if (frequency !== this.frequency) {
      if (Number.isSafeInteger(frequency) && Number.isSafeInteger(this.frequency)) {
        // Keep the existing traces at their RF frequencies when the dial moves.
        const {width, height} = this.canvas;
        const shift = this.frequency - frequency; // The canvas has one pixel per Hz.
        if (Math.abs(shift) >= width) this.context.clearRect(0, 0, width, height);
        else {
          this.context.drawImage(this.canvas, shift, 0);
          this.context.clearRect(shift > 0 ? 0 : width + shift, 0, Math.abs(shift), height);
        }
      } else {
        this.clear();
      }
      this.frequency = frequency;
    }
    if (reverse !== this.reverse) {
      this.reverse = reverse;
      for (const [index, hz] of [spaceHz, markHz].entries()) {
        const kind = (index === 1) !== reverse ? 'Mark' : 'Space';
        this.tones[index].dataset.kind = kind;
        this.tones[index].firstElementChild.textContent = `${kind} ${hz} Hz`;
      }
    }
  }

  clear() {
    this.noise.reset();
    this.context.clearRect(0, 0, this.canvas.width, this.canvas.height);
  }

  setColor(color) {
    if (color === this.color) return;
    this.color = color;
    this.clear();
  }

  accept(frame) {
    if (frame.stream !== 2 || !zoomMatchesFrequency(frame.center, this.frequency)) return;
    const {width} = this.canvas;
    const hzPerBin = frame.rate / frame.n;
    const first = frame.n / 2 + (this.frequency + leftOffset - frame.center) / hzPerBin;
    const step = waterfallSpan / (width * hzPerBin);
    if (first < 0 || first + width * step >= frame.n - 1) return;

    const noise = this.noise.add(frame) ?? -100;
    const floor = noise + this.offsets.floor, ceiling = noise + this.offsets.ceiling;
    const palette = PLOT_COLORS[this.color].lut;
    for (let x = 0; x < width; x++) {
      const bin = first + (x + .5) * step, lower = Math.floor(bin);
      // Interpolate the source bins without rounding the RX dial to the FFT grid.
      const a = frame.values[lower], b = frame.values[lower + 1];
      const level = (a + (b - a) * (bin - lower)) / 100;
      const color = Math.max(0, Math.min(255, Math.round(255 * (level - floor) / (ceiling - floor))));
      this.row.data.set(palette[color], 4 * x);
      this.row.data[4 * x + 3] = 255;
    }
    this.context.drawImage(this.canvas, 0, 1);
    this.context.putImageData(this.row, 0, 0);
  }
}

export class RttyPresets {
  constructor(dialog, select, stored) {
    this.dialog = dialog;
    this.select = select;
    this.fields = [...dialog.querySelectorAll('[data-rtty-preset]')];
    let saved;
    try { saved = JSON.parse(stored); } catch {}
    if (Array.isArray(saved?.fields)) {
      for (const [index, field] of this.fields.entries()) {
        if (typeof saved.fields[index] === 'string') field.value = saved.fields[index];
      }
    }
    if (['1', '2', '3', '4', '5'].includes(saved?.slot)) this.select.value = saved.slot;
  }

  get text() {
    const [own, other, ...texts] = this.fields.map(field => field.value);
    const replacements = {'%': own.trim(), '$': other.trim(), '&': '\n'};
    return (texts[Number(this.select.value) - 1] ?? '').replace(/[%$&]/g, key => replacements[key]);
  }

  serialize() {
    return JSON.stringify({slot: this.select.value, fields: this.fields.map(field => field.value)});
  }
}

export class RttyView {
  constructor(panel, write) {
    this.panel = panel;
    this.rx = panel.querySelector('#rtty-rx-text');
    this.tx = panel.querySelector('#rtty-tx-text');
    this.error = panel.querySelector('#rtty-error');
    this.waterfall = new RttyWaterfall(panel.querySelector('.rtty-waterfall'));
    this.txText = '';
    this.afterCr = false;
    this.composing = false;
    this.tx.addEventListener('beforeinput', event => {
      if (event.isComposing || !event.cancelable) return;
      event.preventDefault();
      if (event.inputType === 'insertLineBreak' || event.inputType === 'insertParagraph') write('\n');
      else if (event.inputType === 'insertText' && event.data) write(event.data);
    });
    this.tx.addEventListener('paste', event => {
      event.preventDefault();
      const text = event.clipboardData?.getData('text/plain');
      if (text) write(text);
    });
    this.tx.addEventListener('drop', event => event.preventDefault());
    this.tx.addEventListener('compositionstart', () => { this.composing = true; });
    this.tx.addEventListener('compositionend', event => {
      this.composing = false;
      if (event.data) write(event.data);
      this.renderTx();
    });
    this.tx.addEventListener('input', event => {
      if (event.isComposing || this.composing) return;
      if (event.inputType === 'insertText' && event.data) write(event.data);
      this.renderTx();
    });
  }

  get visible() { return !this.panel.hidden; }
  show(visible) {
    if (this.visible !== visible) {
      this.resetRx();
      this.waterfall.clear();
    }
    this.panel.hidden = !visible;
  }
  resetRx() { this.afterCr = false; }
  clearRx() { this.rx.value = ''; this.resetRx(); }

  receive(message, session) {
    if (!this.visible || message.session !== session) return;
    if (message.type === 'rtty_reset') { this.resetRx(); return; }
    if (typeof message.text !== 'string') return;
    const follow = this.rx.scrollHeight - this.rx.clientHeight - this.rx.scrollTop < 4;
    const top = this.rx.scrollTop;
    const text = this.afterCr ? message.text.replace(/^\n/, '') : message.text;
    this.afterCr = message.text.endsWith('\r');
    this.rx.value = (this.rx.value + text.replace(/\r\n?/g, '\n').replaceAll('\x07', '')).slice(-historyLimit);
    this.rx.scrollTop = follow ? this.rx.scrollHeight : top;
  }

  beginTx(live) {
    this.txText = '';
    this.renderTx();
    this.error.textContent = '';
    this.tx.readOnly = !live;
    if (live) this.tx.focus({preventScroll: true});
  }

  endTx() {
    this.tx.readOnly = true;
    this.composing = false;
    this.renderTx();
  }

  appendTx(text) {
    this.txText = (this.txText + text).slice(-historyLimit);
    this.renderTx();
  }

  renderTx() {
    if (this.composing || this.tx.value === this.txText) return;
    this.tx.value = this.txText;
    this.tx.setSelectionRange(this.txText.length, this.txText.length);
    this.tx.scrollTop = this.tx.scrollHeight;
  }
}
