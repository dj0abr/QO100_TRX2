// RF frequencies are integer Hz. The satellite mapping is independent of LO corrections.
export const DOWNLINK_UPLINK_HZ = 8_089_500_000;
export const CHANNEL_LIMIT_HZ = 275_000;

export function frequencyText(hz) {
  if (!Number.isSafeInteger(hz)) return '—';
  const mhz = Math.floor(hz / 1e6).toLocaleString('de-DE').replaceAll('.', ' ');
  const fraction = String(hz % 1e6).padStart(6, '0');
  return `${mhz}.${fraction.slice(0, 3)} ${fraction.slice(3)}`;
}

export function parseMHz(text) {
  const value = String(text).trim().replace(/\s/g, '').replace(',', '.');
  if (!/^\d+(?:\.\d{1,6})?$/.test(value)) throw new Error('Frequenz in MHz mit höchstens sechs Nachkommastellen eingeben.');
  const [whole, fraction = ''] = value.split('.');
  const hz = Number(whole) * 1e6 + Number(fraction.padEnd(6, '0'));
  if (!Number.isSafeInteger(hz)) throw new Error('Frequenz ist ungültig.');
  return hz;
}

export function validateFrequency(hz, center, label = 'Frequenz') {
  if (!Number.isSafeInteger(hz) || !Number.isSafeInteger(center) || Math.abs(hz - center) > CHANNEL_LIMIT_HZ) {
    throw new Error(`${label}: außerhalb des abstimmbaren Bereichs (Mitte ±275 kHz).`);
  }
  return hz;
}

export function tuneTargets({selected, frequency, rx1, rx = rx1, tx, rxCenter, txCenter, together}) {
  validateFrequency(frequency, rxCenter, selected === 2 ? 'RX2' : 'RX1');
  const result = {rx: selected, frequency};
  if (together) {
    result.tx = validateFrequency(tx + frequency - rx, txCenter, `TX${selected}`);
  }
  return result;
}

export function wheelTurns(event) {
  if (!Number.isFinite(event.deltaY) || !event.deltaY) return 0;
  return -Math.sign(event.deltaY) * Math.max(1, Math.round(Math.abs(event.deltaY) / (event.deltaMode === 1 ? 3 : 100)));
}

export function wheelTargets({selected, rx, tx, delta, rxCenter, txCenter, shift = false, ctrl = false}) {
  const targets = {};
  if (ctrl || !shift) {
    targets.rx = selected;
    targets.frequency = validateFrequency(rx + delta, rxCenter, `RX${selected}`);
  }
  if (ctrl || shift) targets.tx = validateFrequency(tx + delta, txCenter, 'TX');
  return targets;
}

export function filterValues(low, high, changed = 'low') {
  let low_hz = Math.max(100, Math.min(3500, low));
  let high_hz = Math.max(600, Math.min(4000, high));
  if (high_hz - low_hz < 500) {
    if (changed === 'high') high_hz = low_hz + 500;
    else low_hz = high_hz - 500;
  }
  return {low_hz, high_hz};
}
