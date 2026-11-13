import {displayOffsets, NoiseFloor, PLOT_COLORS, zoomMatchesFrequency} from './radio-display.js';

export class Rx2Waterfall {
  constructor(canvas, leftOffset, span) {
    this.canvas = canvas;
    this.leftOffset = leftOffset;
    this.span = span;
    this.context = this.canvas.getContext('2d');
    this.row = this.context.createImageData(this.canvas.width, 1);
    this.noise = new NoiseFloor();
    this.offsets = displayOffsets();
    this.color = 'blue';
  }

  update(frequency) {
    if (frequency !== this.frequency) {
      if (Number.isSafeInteger(frequency) && Number.isSafeInteger(this.frequency)) {
        // Keep the existing traces at their RF frequencies when the dial moves.
        const {width, height} = this.canvas;
        const shift = Math.round((this.frequency - frequency) * width / this.span);
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
    const first = frame.n / 2 + (this.frequency + this.leftOffset - frame.center) / hzPerBin;
    const step = this.span / (width * hzPerBin);
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
