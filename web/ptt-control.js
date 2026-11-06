export const PTT_TIMEOUT_MS = 5 * 60 * 1000;

export class PttControl {
  constructor({start, stop, isActive, timeout, mode = 'hold', spaceEnabled = false,
    now = () => performance.now(), schedule = (fn, ms) => setTimeout(fn, ms), cancel = id => clearTimeout(id)}) {
    Object.assign(this, {start, stop, isActive, timeout, now, schedule, cancel});
    this.mode = mode === 'toggle' ? 'toggle' : 'hold'; this.spaceEnabled = spaceEnabled;
    this.sources = new Set(); this.deadline = 0; this.timer = null;
  }
  configure(mode, spaceEnabled) {
    mode = mode === 'toggle' ? 'toggle' : 'hold';
    if (mode === this.mode && spaceEnabled === this.spaceEnabled) return;
    this.reset(); this.stop(); this.mode = mode; this.spaceEnabled = spaceEnabled;
  }
  press(source) {
    if (source === 'Space' && !this.spaceEnabled || this.sources.has(source)) return false;
    this.sources.add(source);
    if (this.mode === 'toggle' && this.isActive()) { this.reset(); this.stop(); return true; }
    if (!this.isActive() && this.start() === true && this.mode === 'toggle') {
      this.deadline = this.now() + PTT_TIMEOUT_MS;
      this.timer = this.schedule(() => this.checkTimeout(), PTT_TIMEOUT_MS);
    }
    return true;
  }
  release(source) {
    if (!this.sources.delete(source)) return false;
    if (this.mode === 'hold' && !this.sources.size) { this.reset(); this.stop(); }
    return true;
  }
  checkTimeout() {
    if (!this.deadline || this.now() < this.deadline) return false;
    this.reset(); this.stop(); this.timeout(); return true;
  }
  reset() {
    if (this.timer !== null) this.cancel(this.timer);
    this.timer = null; this.deadline = 0; this.sources.clear();
  }
  get remainingMs() { return Math.max(0, this.deadline - this.now()); }
}
