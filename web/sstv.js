import {text as t} from './language.js';
import {PLOT_COLORS} from './radio-display.js';

const LINE = 21, SPECTRUM = 22, HEADER = 64;
const modes = {M1: 'Martin M1', S1: 'Scottie 1'};
const signed = hz => `${hz > 0 ? '+' : ''}${hz}`;

export class SstvView {
  constructor(element) {
    this.element = element;
    const node = id => element.querySelector(`#sstv-${id}`);
    this.status = node('status');
    this.count = node('line-count');
    this.afc = node('afc');
    this.marker = node('sync-marker');
    this.image = node('image');
    this.imageContext = this.image.getContext('2d');
    this.line = this.imageContext.createImageData(this.image.width, 1);
    this.waterfall = node('waterfall');
    this.waterfallContext = this.waterfall.getContext('2d');
    this.waterfallRow = this.waterfallContext.createImageData(this.waterfall.width, 1);
    this.imageKey = null;
    this.color = 'blue';
    this.reset();
  }

  get visible() { return !this.element.hidden; }

  show(visible) {
    if (this.visible === visible) return;
    this.element.hidden = !visible;
  }

  reset(newConnection = false) {
    if (newConnection) this.imageKey = null;
    this.frame = null;
    this.audio = false;
    this.end = null;
    this.offset = null;
    this.synchronized = false;
    this.waterfallContext.clearRect(0, 0, this.waterfall.width, this.waterfall.height);
    this.render();
  }

  message(message, session) {
    if (message.session !== session) return;
    if (message.type === 'sstv_reset') {
      this.reset();
    } else if (message.type === 'sstv_begin' && Object.hasOwn(modes, message.mode)) {
      const key = `${session}:${message.image}`;
      if (key !== this.imageKey) {
        this.imageKey = key;
        this.lines = 0;
        this.imageContext.clearRect(0, 0, this.image.width, this.image.height);
      }
      this.frame = message;
      this.end = null;
      this.offset = null;
      this.synchronized = false;
    } else if (message.type === 'sstv_end' && message.image === this.frame?.image) {
      this.end = message.complete;
      this.offset = null;
    }
    this.render();
  }

  binary(data, session) {
    if (data.byteLength < HEADER) return false;
    const view = new DataView(data), type = view.getUint8(5);
    if (type !== LINE && type !== SPECTRUM) return false;
    if (view.getUint32(0) !== 0x51545232 || view.getUint8(4) !== 1 ||
        view.getUint16(6) !== HEADER || view.getUint32(8) !== 2 || view.getUint32(60) !== session) return true;
    if (type === LINE) {
      const width = view.getUint16(52), row = view.getUint32(28);
      if (this.end !== null || view.getUint32(12) !== this.frame?.image || view.getUint32(24) ||
          width !== this.image.width || row >= this.image.height || data.byteLength !== HEADER + 3 * width) return true;
      const rgb = new Uint8Array(data, HEADER);
      for (let x = 0; x < width; ++x) {
        this.line.data[4 * x] = rgb[3 * x];
        this.line.data[4 * x + 1] = rgb[3 * x + 1];
        this.line.data[4 * x + 2] = rgb[3 * x + 2];
        this.line.data[4 * x + 3] = 255;
      }
      this.imageContext.putImageData(this.line, 0, row);
      this.lines = row + 1;
      this.offset = view.getInt32(44);
      this.synchronized = !!view.getUint32(48);
      this.render();
    } else {
      if (view.getUint16(52) !== this.waterfall.width || data.byteLength !== HEADER + this.waterfall.width ||
          view.getUint32(40) !== 12000 || view.getUint32(56) !== 1024) return true;
      this.drawWaterfall(new Uint8Array(data, HEADER));
      if (!this.audio) { this.audio = true; this.render(); }
    }
    return true;
  }

  drawWaterfall(levels) {
    const {width, height} = this.waterfall;
    const context = this.waterfallContext, palette = PLOT_COLORS[this.color].lut;
    const peak = Math.max(46, ...levels), floor = peak - 45;
    for (let x = 0; x < width; ++x) {
      const level = Math.max(0, Math.min(255, Math.round((levels[x] - floor) * 255 / 45)));
      this.waterfallRow.data.set(palette[level], 4 * x);
      this.waterfallRow.data[4 * x + 3] = 255;
    }
    context.drawImage(this.waterfall, 0, 0, width, height - 1, 0, 1, width, height - 1);
    context.putImageData(this.waterfallRow, 0, 0);
  }

  render() {
    const mode = modes[this.frame?.mode];
    const label = mode ? `${mode} · ` : '';
    const status = this.end === true ? t('Bild vollständig', 'Image complete') :
      this.end === false ? t('Teilbild', 'Partial image') :
      this.synchronized ? t('Synchronisiert', 'Locked') :
      this.audio ? t('Suche Sync', 'Searching for sync') : t('Warte auf RX2-Audio', 'Waiting for RX2 audio');
    if (this.status.textContent !== label + status) this.status.textContent = label + status;
    this.status.dataset.state = this.end === true || this.synchronized && this.end === null ? 'locked' :
      this.audio && this.end === null ? 'searching' : '';
    this.count.textContent = mode && this.lines ? `${this.lines}${this.frame.partial ? '' : ' / 256'}` : '';
    const sync = this.offset === null ? null : 1200 + this.offset;
    this.afc.textContent = sync === null ? '' : `Sync ${sync} Hz · AFC ${signed(this.offset)} Hz`;
    this.marker.hidden = !this.synchronized || sync === null || sync < 0 || sync > 4000;
    if (!this.marker.hidden) this.marker.style.left = `${sync / 40}%`;
  }
}
