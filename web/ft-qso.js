const grid = /^[A-R]{2}\d{2}$/;
const report = /^[+-]\d{2}$/;
const ackReport = /^R[+-]\d{2}$/;

export function ftPeriod(mode) {
  return mode === 'FT4' ? 7.5 : mode === 'FT8' ? 15 : 0;
}

export function canStartFtAuto(contact, own) {
  return !!own && contact.peer !== own && (contact.kind === 'cq' ||
    (contact.recipient === own && ((grid.test(contact.kind) && contact.kind !== 'RR73') || report.test(contact.kind))));
}

// An automatic reply may advance the exchange or repeat the same step.
export function nextFtAutoStep(sentStep, replyStep) {
  if (sentStep === 'locator') return replyStep === 'ackReport';
  if (sentStep === 'report') return ['report', 'ack', 'signoff'].includes(replyStep);
  if (sentStep === 'ack') return ['ack', 'signoff', 'complete'].includes(replyStep);
  if (sentStep === 'ackReport') return ['ackReport', 'signoff', 'complete'].includes(replyStep);
  return false;
}

export function ftCall(value) {
  const call = value.trim().toUpperCase();
  return /^(?:[A-Z]|[A-Z][A-Z0-9]|[0-9][A-Z])\d[A-Z]{1,3}$/.test(call) ? call : '';
}

export function ftGrid(value) {
  const locator = value.trim().toUpperCase();
  return /^[A-R]{2}\d{2}(?:[A-X]{2})?$/.test(locator) ? locator.slice(0, 4) : '';
}

export function parseFtContact(packet) {
  const {message, mode, slot_utc: slot, snr_db: snr, frequency_hz: audioHz} = packet;
  const slotIndex = slot / ftPeriod(mode);
  if (typeof slot !== 'number' || !Number.isSafeInteger(slotIndex) || slot <= 0 ||
      typeof message !== 'string' || !Number.isFinite(snr) || !Number.isFinite(audioHz)) return null;
  const words = message.trim().split(/\s+/);
  const result = {slot, parity: 1 - (slotIndex % 2), snr, audioHz};
  if (words[0] === 'CQ') {
    const directed = /^(?:[A-Z]{1,4}|\d{3})$/.test(words[1] ?? '');
    const [peer, locator, ...rest] = words.slice(directed ? 2 : 1);
    if (!peer || !ftCall(peer) || rest.length || (locator && !grid.test(locator))) return null;
    return {...result, peer, kind: 'cq', locator};
  }
  const [recipient, peer, exchange, ...rest] = words;
  if (!recipient || !peer || !ftCall(recipient) || !ftCall(peer) || !exchange || rest.length) return null;
  if (!grid.test(exchange) && !report.test(exchange) && !ackReport.test(exchange) &&
      !['RRR', 'RR73', '73'].includes(exchange)) return null;
  return {...result, recipient, peer, kind: exchange};
}

export function ftReply(received, ownCall, locator) {
  if (received.peer === ownCall || (received.kind !== 'cq' && received.recipient !== ownCall)) return null;
  const db = Math.max(-50, Math.min(49, Math.round(received.snr)));
  const snr = (db < 0 ? '-' : '+') + String(Math.abs(db)).padStart(2, '0');
  const {kind, peer, slot, parity, audioHz} = received;
  let step, reply;
  if (kind === 'cq') [step, reply] = ['locator', locator];
  else if (grid.test(kind) && kind !== 'RR73') [step, reply] = ['report', snr];
  else if (report.test(kind)) [step, reply] = ['ackReport', 'R' + snr];
  else if (ackReport.test(kind)) [step, reply] = ['ack', 'RR73'];
  else if (kind === '73') [step, reply] = ['complete', ''];
  else [step, reply] = ['signoff', '73'];
  return {peer, slot, parity, audioHz, step, received, text: reply ? `${peer} ${ownCall} ${reply}` : ''};
}
