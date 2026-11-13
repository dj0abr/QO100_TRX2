import {Rx2Waterfall} from './rx2-waterfall.js';

const historyLimit = 16384;
const spaceHz = 830, markHz = 1000, waterfallSpan = 800;
const leftOffset = (spaceHz + markHz - waterfallSpan) / 2;

class RttyWaterfall extends Rx2Waterfall {
  constructor(element) {
    super(element.querySelector('canvas'), leftOffset, waterfallSpan);
    this.tones = [...element.querySelectorAll('.rtty-tone')];
    for (const [index, hz] of [spaceHz, markHz].entries()) {
      this.tones[index].style.setProperty('--tone-position', `${100 * (hz - leftOffset) / waterfallSpan}%`);
    }
    this.update(undefined, false);
  }

  update(frequency, reverse) {
    super.update(frequency);
    if (reverse !== this.reverse) {
      this.reverse = reverse;
      for (const [index, hz] of [spaceHz, markHz].entries()) {
        const kind = (index === 1) !== reverse ? 'Mark' : 'Space';
        this.tones[index].dataset.kind = kind;
        this.tones[index].firstElementChild.textContent = `${kind} ${hz} Hz`;
      }
    }
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
    this.panel.hidden = !visible;
  }
  resetRx() { this.afterCr = false; }
  clearRx() { this.rx.value = ''; this.resetRx(); }

  receive(message, session) {
    if (message.session !== session) return;
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
