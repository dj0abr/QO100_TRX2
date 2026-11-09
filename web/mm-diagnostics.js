import {text as t} from './language.js';

const finite = n => typeof n === 'number' && Number.isFinite(n);
const hz = n => finite(n) ? `${n.toFixed(1)} Hz` : '—';
export function diagnosticState(data, elapsed = 0) {
  const receiver = data?.receiver || {};
  const fresh = !!data?.audio_fresh && finite(data.audio_age_s) && finite(data.status_age_s)
    && Math.max(data.audio_age_s, data.status_age_s) + elapsed < 2;
  const pilot = fresh && finite(data?.pilot_hz) ? data.pilot_hz : null;
  const center = fresh && finite(receiver.center_hz) ? receiver.center_hz : null;
  const locked = fresh && !!receiver.locked;
  const signalCenter = locked && center !== null ? center + (finite(receiver.pll_hz) ? receiver.pll_hz : 0) : null;
  const nearEdge = pilot !== null && pilot < 100 || signalCenter !== null && (signalCenter - 1440 < 100 || signalCenter + 1440 > 3900);
  return {fresh, locked, pilot, center, signalCenter, nearEdge,
    warning: !fresh ? 'stale' : pilot === null ? 'pilot_missing' : nearEdge ? 'edge' : ''};
}

export class MmDiagnostics {
  constructor(element) {
    this.element = element; this.visible = false; this.data = null; this.at = 0;
    this.spectrum = element.querySelector('#mm-spectrum'); this.constellation = element.querySelector('#mm-constellation');
    this.observer = new ResizeObserver(() => { if (this.visible) this.render(); });
    this.observer.observe(this.spectrum); this.observer.observe(this.constellation);
    setInterval(() => { if (this.visible) this.render(); }, 250);
  }
  show(visible) { this.visible = visible; this.element.hidden = !visible; if (visible) this.render(); }
  update(data) { this.data = data; this.at = performance.now(); }
  reset() { this.data = null; this.at = 0; if (this.visible) this.render(); }
  render() {
    const data = this.data, state = diagnosticState(data, (performance.now() - this.at) / 1000);
    const receiver = data?.receiver || {}, node = id => this.element.querySelector(`#${id}`);
    const modes = {searching:t('Suche', 'Searching'), acquiring:t('Prüfe Träger', 'Acquiring'),
      tracking:t('Nachführen', 'Tracking'), hold:t('Halte Frequenz', 'Hold'), manual:t('Manuell', 'Manual')};
    const status = node('mm-lock');
    status.textContent = !state.fresh ? t('Keine aktuellen RX5-Daten', 'No current RX5 data') : state.locked ? t('Synchronisiert · FEC/CRC gültig', 'Locked · valid FEC/CRC') : t('Suche · noch nicht synchronisiert', 'Searching · not yet locked');
    status.dataset.state = !state.fresh ? 'stale' : state.locked ? 'locked' : 'searching';
    node('mm-pilot').textContent = hz(state.pilot);
    node('mm-afc').textContent = state.center === null ? '—' : `${state.center >= 1500 ? '+' : ''}${(state.center - 1500).toFixed(1)} Hz`;
    node('mm-pll').textContent = state.locked ? hz(receiver.pll_hz) : '—';
    node('mm-level').textContent = state.fresh && finite(receiver.rms_dbfs) ? `${receiver.rms_dbfs.toFixed(1)} dBFS` : '—';
    node('mm-frequency').textContent = finite(data?.frequency_hz) ? `${data.frequency_hz} Hz` : '—';
    node('mm-afc-state').textContent = state.fresh ? modes[receiver.afc_state] || '—' : '—';
    const warning = node('mm-warning'); warning.dataset.warning = state.warning;
    warning.textContent = ({stale:t('Warte auf aktuelle Messdaten.', 'Waiting for current measurements.'),
      pilot_missing:t('Kein eindeutiger Hilfsträger erkannt.', 'No clear pilot detected.'),
      edge:t('Signal nahe der Filtergrenze. Pilot um 500 Hz anstreben.', 'Signal near the filter edge. Aim for a pilot around 500 Hz.')})[state.warning]
      || t('Pilot im Durchlassbereich · Pilotsuche 20–900 Hz', 'Pilot inside the passband · pilot search 20–900 Hz');
    this.drawSpectrum(data, state); this.drawConstellation(data, state);
  }
  canvas(canvas) {
    const bounds = canvas.getBoundingClientRect(), dpr = Math.min(2, window.devicePixelRatio || 1);
    const w = Math.max(1, Math.round(bounds.width * dpr)), h = Math.max(1, Math.round(bounds.height * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    const ctx = canvas.getContext('2d'); ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = '#0b151e'; ctx.fillRect(0, 0, bounds.width, bounds.height);
    ctx.font = '10px monospace'; ctx.lineWidth = 1;
    return {ctx, w: bounds.width, h: bounds.height};
  }
  drawSpectrum(data, state) {
    const {ctx, w, h} = this.canvas(this.spectrum), left = 35, right = Math.max(left + 1, w - 9), top = 23, bottom = Math.max(top + 1, h - 20);
    const x = f => left + f / 4000 * (right - left), y = db => top + Math.max(0, Math.min(140, -db)) / 140 * (bottom - top);
    ctx.fillStyle = '#122b2c'; ctx.fillRect(x(20), top, x(900) - x(20), bottom - top);
    ctx.strokeStyle = '#263a49'; ctx.fillStyle = '#93a9bb';
    for (let db = -120; db <= 0; db += 40) { ctx.beginPath(); ctx.moveTo(left, y(db)); ctx.lineTo(right, y(db)); ctx.stroke(); ctx.fillText(db, 3, y(db) + 3); }
    for (let f = 0; f <= 4000; f += 1000) { ctx.beginPath(); ctx.moveTo(x(f), top); ctx.lineTo(x(f), bottom); ctx.stroke(); ctx.textAlign = f === 4000 ? 'right' : f === 0 ? 'left' : 'center'; ctx.fillText(f, x(f), h - 5); }
    ctx.textAlign = 'left'; ctx.fillText(t('RX5 Audio · dBFS/Bin', 'RX5 audio · dBFS/bin'), 4, 12);
    ctx.textAlign = 'right'; ctx.fillText('Hz', right, 12); ctx.textAlign = 'left';
    const bins = data?.spectrum?.dbfs;
    if (state.fresh && Array.isArray(bins) && bins.length > 1 && bins.length <= 1024) {
      ctx.strokeStyle = '#62d1e7'; ctx.beginPath();
      bins.forEach((db, i) => { if (finite(db)) { const px = x((i + .5) * 4000 / bins.length); if (i) ctx.lineTo(px, y(db)); else ctx.moveTo(px, y(db)); } });
      ctx.stroke();
    }
    const marker = (frequency, color, label) => {
      if (!finite(frequency) || frequency < 0 || frequency > 4000) return;
      ctx.strokeStyle = color; ctx.setLineDash([3, 3]); ctx.beginPath(); ctx.moveTo(x(frequency), top); ctx.lineTo(x(frequency), bottom); ctx.stroke(); ctx.setLineDash([]);
      if (label) { ctx.fillStyle = color; ctx.fillText(label, Math.min(x(frequency) + 3, right - 90), top + 10); }
    };
    if (state.signalCenter !== null) { marker(state.signalCenter - 1440, '#d5b869'); marker(state.signalCenter + 1440, '#d5b869'); }
    marker(state.pilot, '#71ecad', state.pilot === null ? '' : `Pilot ${state.pilot.toFixed(1)}`);
    if (!state.fresh) { ctx.fillStyle = '#8299aa'; ctx.fillText(t('Keine aktuellen Daten', 'No current data'), left + 5, top + 25); }
  }
  drawConstellation(data, state) {
    const {ctx, w, h} = this.canvas(this.constellation), cx = w / 2, cy = (h + 15) / 2, radius = Math.max(1, Math.min(w, h - 25) * .43);
    ctx.strokeStyle = '#304454'; ctx.beginPath(); ctx.moveTo(cx - radius, cy); ctx.lineTo(cx + radius, cy); ctx.moveTo(cx, cy - radius); ctx.lineTo(cx, cy + radius); ctx.stroke();
    ctx.fillStyle = '#93a9bb'; ctx.fillText('8APSK', 5, 12);
    const points = state.fresh && Array.isArray(data?.points) ? data.points.slice(-480).filter(p => Array.isArray(p) && p.length === 2 && p.every(finite)) : [];
    const levels = points.map(p => Math.hypot(...p)).sort((a, b) => a - b), extent = Math.max(1.3, (levels[Math.floor(levels.length * .98)] || 1) * 1.1);
    ctx.fillStyle = state.locked ? '#71ecad' : '#69b9df';
    for (const [re, im] of points) { const x = cx + re / extent * radius, y = cy - im / extent * radius; if (x >= 0 && x <= w && y >= 20 && y <= h) ctx.fillRect(x - 1, y - 1, 2, 2); }
    if (!points.length) { ctx.fillStyle = '#8299aa'; ctx.fillText('—', cx - 3, cy + 15); }
  }
}
