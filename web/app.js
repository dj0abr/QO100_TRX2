import {DOWNLINK_UPLINK_HZ, frequencyText, parseMHz, validateFrequency, tuneTargets, wheelTurns, wheelTargets, filterValues} from './tuning.js';
import {FrameAssembler, RadioDisplay, displayOffsets, AXIS_MODES} from './radio-display.js';
import {RadioAudio} from './audio.js';
import {PttControl} from './ptt-control.js';
import {MmDiagnostics} from './mm-diagnostics.js';
import {SstvView} from './sstv.js';
import {SstvTxView} from './sstv-tx.js';
import {RttyPresets, RttyView} from './rtty.js';
import {Rx2Waterfall} from './rx2-waterfall.js';
import {drawWaterfallPreview, waterfallRaster} from './waterfall-tx.js';
import {canStartFtAuto, ftCall, ftGrid, ftPeriod, ftReply, nextFtAutoStep, parseFtContact} from './ft-qso.js';
import {text as t, language, applyLanguage, setLanguagePreference, languagePreference} from './language.js';

const $ = id => document.getElementById(id);
let socket, operator = false, state = {}, selected = 1, together = true, request = 0;
let session = 0, loopGeneration = null, cutoff = 0n, guiLoaded = false, lastActivity = 0;
let pttHeld = false, pttAcked = false, pttIntent = 0, txIdentity = '', txIndex = 0;
let noticeUntil = 0, settingsBase = {}, editingFrequency = '1', settingsTail = Promise.resolve();
let filterBusy = 0;
let menuDismissedAt = -Infinity;
let cursor = null, cursorFrame = 0;
let txMonitorPrevious = 0;
const pending = new Map(), wanted = new Map(), timers = new Map(), stateWaiters = new Set();
const views = [new RadioDisplay($('fullband')), new RadioDisplay($('zoom-display'), true)];
const frames = new FrameAssembler(frame => {
  const view = views[frame.stream === 0 ? 0 : 1];
  view.accept(frame);
  if (digitalView === 'rtty') rtty.waterfall.accept(frame);
  if (ft8Visible()) ftWaterfall.accept(frame);
  if (cursor?.view === view) queueCursorUpdate();
});
const radio = new RadioAudio(transmitSamples, mediaChanged);
const mmDiagnostics = new MmDiagnostics($('mm-diagnostics'));
const sstv = new SstvView($('sstv-panel'));
const sstvTx = new SstvTxView($('sstv-panel'), $('sstv-tx-editor'), renderSstvTxControls);
let sstvUpload = null, sstvStopRequested = false;
let waterfallFont = null, waterfallFontLoading = false, waterfallRows = [];
let waterfallStartId = null, waterfallStopId = null, waterfallError = '';
const rtty = new RttyView($('rtty-panel'), writeRttyText);
const rttyPresets = new RttyPresets($('rtty-preset-editor'), $('rtty-preset-slot'), readPreference('rttyPresets'));
const rttyInputLimit = 1024;
let rttyTx = null;
let ft8Pending = false, ftModePending = false, ft8Error = '';
let ftQso = null, ftQsoTransmission = null, ftAutoQso = null;
const ftAutoMaxAttempts = 3;
const ftAutoReplySlots = 2;
let ftQsoLog = null, ftLogError = false;
const ft8Log = {output: $('ft8-log'), lines: [], follow: true};
const ftOwnLog = {output: $('ft8-own-log'), lines: [], follow: true};
const ft8Visible = () => !$('ft8-panel').hidden;
const ftWaterfall = new Rx2Waterfall($('ft-waterfall'), 0, 4000);
const ftStandardRxHz = 10_489_540_000;
const ptt = new PttControl({start: startPtt, stop: stopPtt, isActive: () => pttHeld,
  mode: readPreference('pttMode'), spaceEnabled: readPreference('pttSpace') === 'true',
  timeout: () => notify('PTT nach 5 Minuten automatisch ausgeschaltet.', false, 8000)});
let waterfallOffsets = displayOffsets(readPreference('waterfallFloorOffset') ?? readPreference('fullbandFloorOffset'), readPreference('waterfallCeilingOffset') ?? readPreference('fullbandCeilingOffset'));

function readPreference(key) { try { return localStorage.getItem(`qo100.${key}`); } catch { return null; } }
function writePreference(key, value) { try { localStorage.setItem(`qo100.${key}`, String(value)); } catch {} }

function isOpen() { return socket?.readyState === WebSocket.OPEN; }
// An offline Pluto must still allow Setup corrections; only TX requires online hardware.
function canControl() { return operator && isOpen() && state.ready && !(state.flags & 2) && (selected === 1 || !!state.tx2?.frequency_hz); }
function send(message) {
  if (!isOpen()) return false;
  try { socket.send(JSON.stringify(message)); return true; } catch { return false; }
}
function notify(text, error = false, duration = 5000) {
  $('status').textContent = text; $('status').classList.toggle('error', error);
  $('status').title = text; noticeUntil = performance.now() + duration;
}
function report(error) { if (error?.code !== 'superseded') notify(error.message || String(error), true); }
function fail(message, code = 'invalid') { return Object.assign(new Error(message), {code}); }
function command(type, values = {}) {
  if (!canControl()) return Promise.reject(fail(operator ? 'Backend ist nicht bedienbereit.' : 'Zuschauer können nur empfangen.', 'not_ready'));
  const id = request = (request + 1) >>> 0;
  const promise = new Promise((resolve, reject) => {
    const timeout = setTimeout(() => { pending.delete(id); reject(fail('Keine Bestätigung vom Backend.', 'timeout')); }, 14000);
    pending.set(id, {resolve, reject, timeout, type});
    if (!send({type, id, ...values})) { clearTimeout(timeout); pending.delete(id); reject(fail('Verbindung getrennt.')); }
  });
  promise.id = id;
  return promise;
}
function checked(type, values) {
  return command(type, values).then(answer => {
    if (answer.status !== 0) throw fail(`${type}: Backend-Status ${answer.status}.`);
    return answer;
  });
}
function debounce(key, callback, delay = 100) {
  clearTimeout(timers.get(key));
  timers.set(key, setTimeout(() => { timers.delete(key); callback(); }, delay));
}
function clearTimers() { for (const timer of timers.values()) clearTimeout(timer); timers.clear(); }
function waitState(predicate, timeout = 2500) {
  if (predicate(state)) return Promise.resolve(state);
  return new Promise((resolve, reject) => {
    const waiter = next => { if (predicate(next)) { clearTimeout(timer); stateWaiters.delete(waiter); resolve(next); } };
    const timer = setTimeout(() => { stateWaiters.delete(waiter); reject(fail('Zustandsbestätigung fehlt. Bitte erneut versuchen.')); }, timeout);
    stateWaiters.add(waiter);
  });
}

function connect() {
  if (socket && socket.readyState < WebSocket.CLOSING) return;
  operator = false; state = {}; wanted.clear(); guiLoaded = false; session = 0; clearTimers();
  frames.reset(); views.forEach(view => view.clear()); radio.reset(); sstv.reset(); rtty.resetRx(); rtty.waterfall.clear();
  cursor = null; queueCursorUpdate();
  const url = new URL('/ws', location.href); url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = socket = new WebSocket(url); ws.binaryType = 'arraybuffer';
  notify('Verbindung wird hergestellt …', false, 0); controls();
  ws.onopen = () => { if (ws === socket) { rx2AudioSubscription = null; notify('Adapter verbunden. Warte auf Backend …', false, 0); send({type:'mm_diagnostics',enabled:!$('mm-diagnostics').hidden}); subscribedDigitalView = undefined; controls(); } };
  ws.onmessage = event => {
    if (ws !== socket) return;
    if (typeof event.data !== 'string') {
      if (sstv.binary(event.data, session)) return;
      frames.accept(event.data, session, cutoff);
      radio.receive(event.data, state, selected, cutoff);
      return;
    }
    let message;
    try { message = JSON.parse(event.data); } catch { notify('Ungültige Nachricht vom Adapter.', true); return; }
    if (message.type === 'role') {
      const wasOperator = operator; operator = message.role === 'operator';
      $('role').textContent = operator ? t('Bediener', 'Operator') : t('Zuschauer', 'Viewer');
      $('role').title = `${message.clients} Verbindung(en) · ${message.idle_seconds ? `Inaktivitätslimit ${message.idle_seconds} s` : t('Keine Abmeldung bei Inaktivität', 'No inactivity logout')}`;
      if (wasOperator && !operator) { ftAutoCq.checked = false; stopPtt(); resetRttyTx(); clearFtQso(); radio.stopMic(); clearTimers(); }
      controls();
    } else if (message.type === 'state') update(message);
    else if (message.type === 'decoder_text' && [3, 4].includes(message.stream) && typeof message.text === 'string') appendDecoderText(message.stream, message.text);
    else if (message.type === 'decoder_text' && message.stream === 5 && typeof message.text === 'string' && ['dx-cluster', 'cw-skimmer', 'bulletin'].includes(message.channel)) appendDecoderText(`5-${message.channel}`, message.text, true);
    else if (message.type === 'mm_diagnostics' && message.stream === 5) mmDiagnostics.update(message);
    else if (['sstv_begin', 'sstv_end', 'sstv_reset'].includes(message.type)) sstv.message(message, session);
    else if (message.type === 'ft8_decode' && digitalView === 'ft8' && message.session === session && message.mode === state.ft_mode) appendFt8(message);
    else if (message.type === 'ft_qso_logged') {
      for (const {line} of ft8Log.lines)
        if (line.dataset.cqCall === message.call) line.removeAttribute('data-new-cq');
    }
    else if (message.type === 'rtty_text' || message.type === 'rtty_reset') rtty.receive(message, session);
    else if (message.type === 'ack' || message.type === 'error') {
      const item = pending.get(message.id);
      if (item) {
        pending.delete(message.id); clearTimeout(item.timeout);
        if (message.type === 'ack') item.resolve(message);
        else item.reject(fail(({read_only: 'Nur der Bediener darf diese Einstellung ändern.', not_ready: 'Backend noch nicht bereit.', busy: 'Einstellungen werden gerade verarbeitet.', tx2_busy: t('TX2 ist belegt.', 'TX2 is busy.'), tx_queue_full: t('RTTY-Sendepuffer voll. Warte kurz und versuche es erneut.', 'RTTY send queue full. Wait a moment and try again.'), backend_reset: 'Backend wurde neu gestartet.', timeout: 'Backend antwortet nicht.'})[message.code] || message.message, message.code));
      } else if (message.type === 'error' && message.code !== 'superseded' && message.code !== 'not_ready') notify(message.message, true);
    } else if (message.type === 'closed') {
      notify(message.reason === 'idle' ? 'Wegen Inaktivität abgemeldet. Mit „Verbinden“ erneut anmelden.' : 'Verbindung wegen fehlendem Heartbeat beendet. Bitte erneut verbinden.', true, Infinity);
    }
  };
  ws.onerror = () => { if (ws === socket) notify('WebSocket-Verbindung fehlgeschlagen.', true); };
  ws.onclose = event => {
    if (ws !== socket) return;
    stopPtt(); resetRttyTx(); operator = false; state = {}; wanted.clear(); clearTimers(); radio.stopMic(); radio.reset(); mmDiagnostics.reset(); sstv.reset(true); rtty.waterfall.clear();
    sstvUpload = null; sstvStopRequested = false;
    waterfallStartId = waterfallStopId = null;
    for (const item of pending.values()) { clearTimeout(item.timeout); item.reject(fail('Verbindung getrennt.', 'closed')); }
    ftAutoCq.checked = false;
    ft8Error = ''; clearFt8(); ftWaterfall.clear(); clearFtQso(); pending.clear(); frames.reset(); views.forEach(view => view.clear());
    cursor = null; queueCursorUpdate();
    $('role').textContent = t('Getrennt', 'Disconnected'); controls();
    if (noticeUntil !== Infinity) {
      const detail = event.reason || ([1000, 1005].includes(event.code) ? '' : `WebSocket-Code ${event.code}`);
      notify(`Verbindung getrennt${detail ? ` (${detail})` : ''}. Mit „Verbinden“ erneut anmelden.`, !!detail, Infinity);
    }
  };
}

function frequency(channel) {
  const key = channel === 'tx' && selected === 2 ? 'tx2' : String(channel);
  return wanted.get(key)?.hz ?? (key === 'tx2' ? state.tx2?.frequency_hz : key === 'tx' ? state.tx?.frequency_hz : state.rx?.[key]?.frequency_hz);
}
function setFrequency(channel, hz, pair = selected) {
  const tx = channel === 'tx', key = tx && pair === 2 ? 'tx2' : String(channel);
  validateFrequency(hz, Number(state.settings?.[tx ? 'tx_center_hz' : 'rx_center_hz']), tx ? 'TX' : `RX${channel}`);
  if (tx && pair === 1) stopPtt();
  const promise = command(tx ? 'tx' : 'tune', tx ? {frequency_hz: hz, ptt: false, channel: pair} : {stream: Number(channel), frequency_hz: hz});
  wanted.set(key, {hz, id: promise.id}); renderFrequencies();
  promise.then(answer => { if (answer.status !== 0) throw fail(`Frequenz wurde nicht übernommen (Status ${answer.status}).`); }).catch(error => {
    if (wanted.get(key)?.id === promise.id) { wanted.delete(key); renderFrequencies(); renderFt8Controls(); renderWaterfallTxControls(); report(error); }
  });
}
function tune(hz, coupled = false) {
  if (!canControl()) return;
  try {
    const target = tuneTargets({selected, frequency: hz, rx: frequency(selected), tx: frequency('tx'), rxCenter: Number(state.settings.rx_center_hz), txCenter: Number(state.settings.tx_center_hz), together: coupled});
    if (target.tx !== undefined) stopPtt();
    setFrequency(target.rx, target.frequency);
    if (target.tx !== undefined) setFrequency('tx', target.tx);
  } catch (error) { report(error); }
}
function fineTune(channel, delta) {
  if (!canControl()) return;
  try { setFrequency(channel, frequency(channel) + delta); } catch (error) { report(error); }
}
function canFineTuneRx2() {
  return canControl() && Number.isSafeInteger(frequency(2));
}
function selectReceiver(channel) {
  if (selected !== channel) { stopPtt(); radio.reset(); }
  selected = channel;
  for (const stream of [1, 2]) {
    $(`frequency${stream}`).classList.toggle('selected', channel === stream);
    $(`frequency${stream}`).setAttribute('aria-pressed', String(channel === stream));
    $(`selected${stream}`).hidden = channel !== stream;
  }
  $('touch-together').hidden = false;
  $('touch-together').textContent = `RX${channel} + TX${channel}`;
  $('touch-together').classList.toggle('chosen', together);
  $('touch-together').setAttribute('aria-pressed', String(together));
  $('touch-rx').textContent = t(`nur RX${channel}`, `RX${channel} only`);
  $('touch-rx').classList.toggle('chosen', !together);
  $('touch-rx').setAttribute('aria-pressed', String(!together));
  $('wide-help').textContent = t(`Links: RX${channel} + TX${channel} · Rechts: nur RX${channel} · Strg + Rad: beide · Shift + Rad: TX${channel}`, `Left: RX${channel} + TX${channel} · Right: RX${channel} only · Ctrl + Wheel: both · Shift + Wheel: TX${channel}`);
  $('touch-help').textContent = t(`Tippen: ${together ? `RX${channel} + TX${channel}` : `nur RX${channel}`}`, `Tap: ${together ? `RX${channel} + TX${channel}` : `RX${channel} only`}`);
  $('tx-label').textContent = `TX${channel}`;
  $('rx1-filter-controls').hidden = channel === 2; $('rx2-filter-note').hidden = channel === 1;
  $('voice-compressor-control').hidden = channel === 2; $('digital-audio-control').hidden = channel === 1;
  const copyRx = $('copy-rx-tx').querySelectorAll('span'), copyTx = $('copy-tx-rx').querySelectorAll('span');
  copyRx[0].textContent = `RX${channel}`; copyRx[2].textContent = `TX${channel}`;
  copyTx[0].textContent = `TX${channel}`; copyTx[2].textContent = `RX${channel}`;
  $('copy-rx-tx').title = `RX${channel} → TX${channel}`; $('copy-tx-rx').title = `TX${channel} → RX${channel}`;
  syncCapture(); views.forEach(view => view.update(state, selected)); renderZoomLabels(); renderNoiseLevels(); renderFrequencies(); controls();
}

function renderFrequencies() {
  for (const [id, channel] of [['rx1', 1], ['rx2', 2], ['tx', 'tx']]) {
    const hz = frequency(channel); $(id).textContent = frequencyText(hz);
    $(id).dataset.hz = Number.isSafeInteger(hz) ? String(hz) : '';
    $(id).classList.toggle('pending', wanted.has(channel === 'tx' && selected === 2 ? 'tx2' : String(channel)));
  }
  $('sstv-frequency').textContent = `RX2 · ${frequencyText(frequency(2))}`;
  $('rtty-frequency').textContent = `RX2 ${frequencyText(frequency(2))} · TX2 ${frequencyText(wanted.get('tx2')?.hz ?? state.tx2?.frequency_hz)}`;
  $('ft8-frequency').textContent = $('rtty-frequency').textContent;
  const tx = frequency('tx'), rx = frequency(selected);
  $('tx-downlink').textContent = frequencyText(Number.isSafeInteger(tx) ? tx + DOWNLINK_UPLINK_HZ : undefined);
  const offset = tx - (rx - DOWNLINK_UPLINK_HZ);
  $('offset-status').textContent = t(`RX${selected} ausgewählt`, `RX${selected} selected`) + (Number.isSafeInteger(offset) ? ` · ${t('TX-Versatz', 'TX offset')} ${offset >= 0 ? '+' : '−'}${Math.abs(offset).toLocaleString('de-DE')} Hz` : '');
  queueCursorUpdate();
}
function queueCursorUpdate() {
  if (cursorFrame) return;
  cursorFrame = requestAnimationFrame(() => {
    cursorFrame = 0;
    const downlink = cursor?.view.frequencyAt({currentTarget: cursor.canvas, clientX: cursor.x});
    const node = $('cursor-downlink'), text = frequencyText(downlink);
    if (node.textContent !== text) node.textContent = text;
    node.dataset.hz = Number.isSafeInteger(downlink) ? String(downlink) : '';
  });
}
function rangePaint(input) {
  input.style.setProperty('--fill', `${100 * (Number(input.value) - Number(input.min)) / Math.max(1, Number(input.max) - Number(input.min))}%`);
}
function updateRange(id, value) { if (value !== undefined && document.activeElement !== $(id) && !timers.has(id)) $(id).value = value; rangePaint($(id)); }
function renderWaterfallSettings() {
  for (const bound of ['floor', 'ceiling']) {
    const input = $(`waterfall-${bound}`), value = waterfallOffsets[bound];
    input.value = value; $(`waterfall-${bound}-value`).textContent = `${value > 0 ? '+' : ''}${value} dB`;
    rangePaint(input);
  }
  views.forEach(view => view.setOffsets(waterfallOffsets.floor, waterfallOffsets.ceiling));
  rtty.waterfall.offsets = ftWaterfall.offsets = waterfallOffsets;
  renderNoiseLevels();
}
function renderNoiseLevels() {
  const format = value => value.toLocaleString('de-DE', {minimumFractionDigits: 1, maximumFractionDigits: 1});
  for (const [index, name] of ['fullband', 'zoom'].entries()) {
    const view = views[index], measured = view.noise.value !== null;
    $(`${name}-noise`).textContent = measured ? `${format(view.noise.value)} dBFS${view.noise.reliable ? '' : t(' (gehalten)', ' (held)')}` : view.frame ? t('keine sichere Schätzung', 'no reliable estimate') : t('warte auf FFT …', 'waiting for FFT …');
    $(`${name}-noise`).dataset.dbfs = measured ? view.noise.value : '';
    $(`${name}-range`).textContent = measured ? `${format(view.floor)} … ${format(view.ceiling)} dBFS` : '—';
    $(`${name}-range`).dataset.floor = measured ? view.floor : '';
    $(`${name}-range`).dataset.ceiling = measured ? view.ceiling : '';
  }
}
function setSpectrumAverage(value) {
  const count = views[0].setAverage(value);
  views[1].setAverage(count);
  $('spectrum-average').value = count; $('spectrum-average-value').textContent = count;
  rangePaint($('spectrum-average')); writePreference('spectrumAverage', count);
}
function setDisplayColor(value) {
  const color = views[0].setColor(value);
  views[1].setColor(color);
  sstv.color = color;
  rtty.waterfall.setColor(color);
  ftWaterfall.setColor(color);
  $('display-color').value = color; writePreference('displayColor', color);
}
function setFullbandAxis(mode) {
  const value = views[0].setAxisMode(mode), index = AXIS_MODES.findIndex(item => item.value === value);
  const label = item => item.label === 'Bandplan' ? t('Bandplan', 'Band plan') : item.label;
  const current = label(AXIS_MODES[index]), next = label(AXIS_MODES[(index + 1) % AXIS_MODES.length]);
  $('fullband-axis-mode').textContent = `600 kHz · ${current}`;
  $('fullband-axis').dataset.mode = value;
  const description = value === 'bandplan' ? t('QO-100 Narrowband-Bandplan nach AMSAT-DL', 'QO-100 narrowband band plan by AMSAT-DL') : t(`Fullband-Frequenzen: ${current} in MHz`, `Fullband frequencies: ${current} in MHz`);
  $('fullband-axis').setAttribute('aria-label', description);
  $('fullband-axis-switch').title = `${description} · ${t('Klicken für', 'Click for')} ${next}`;
  $('fullband-axis-switch').setAttribute('aria-label', `${description}. ${t('Wechseln zu', 'Switch to')} ${next}.`);
}
function renderFilter(changed = 'low') {
  const values = filterValues(Number($('filter-lower').value), Number($('filter-upper').value), changed);
  const low = values.low_hz, high = values.high_hz, bandwidth = high - low;
  $('filter-lower').value = low; $('filter-upper').value = high;
  $('filter-lower').max = high - 500; $('filter-upper').min = low + 500;
  $('filter-lower-value').textContent = `${low.toLocaleString('de-DE')} Hz`;
  $('filter-upper-value').textContent = `${high.toLocaleString('de-DE')} Hz`;
  $('filter-low').textContent = `${low} Hz`; $('filter-high').textContent = `${high} Hz`;
  const label = `USB · ${(bandwidth / 1000).toFixed(1)} kHz`;
  $('mobile-filter-summary').textContent = label; renderZoomLabels();
  for (const button of document.querySelectorAll('[data-bandwidth]')) button.classList.toggle('selected', Number(button.dataset.bandwidth) === bandwidth);
  rangePaint($('filter-lower')); rangePaint($('filter-upper'));
  return values;
}
function renderZoomLabels() {
  const rx = `RX${selected}`, low = Number($('filter-lower').value), high = Number($('filter-upper').value);
  $('zoom-title').textContent = `${rx}-ZOOM`;
  $('zoom-choice-label').textContent = `${rx}-Zoom`;
  $('zoom-filter').textContent = selected === 1 ? `USB · ${((high - low) / 1000).toFixed(1)} kHz` : 'RX2 · USB';
  $('filter-caption').textContent = selected === 1 ? `RX1 · Filter ${low}–${high} Hz` : 'RX2 · USB';
  $('zoom-mobile-caption').textContent = `${rx}-Zoom · 60 kHz`;
  const view = views[1];
  view.empty.textContent = t(`Warte auf ${rx}-Zoom …`, `Waiting for ${rx} zoom …`);
  view.spectrum.setAttribute('aria-label', t(`${rx}-Zoom-Spektrum, zum Abstimmen anklicken`, `${rx} zoom spectrum, click to tune`));
  view.waterfall.setAttribute('aria-label', t(`${rx}-Zoom-Wasserfall, zum Abstimmen anklicken`, `${rx} zoom waterfall, click to tune`));
  view.axis.setAttribute('aria-label', t(`${rx}-Zoom-Frequenzen in MHz`, `${rx} zoom frequencies in MHz`));
}
function applyFilter(values) {
  filterBusy++;
  return checked('filter', values).catch(report).finally(() => { filterBusy--; });
}
function update(next) {
  if (next.ft_mode !== state.ft_mode || next.session !== session || next.rx?.['2']?.epoch !== state.rx?.['2']?.epoch ||
      next.rx?.['2']?.frequency_hz !== state.rx?.['2']?.frequency_hz) {
    clearFt8();
    ftWaterfall.clear();
    clearFtQso();
  }
  if (next.tx2?.frequency_hz !== state.tx2?.frequency_hz || next.loop?.generation !== state.loop?.generation ||
      (ftAutoQso && (!next.ready || next.flags & 6 || next.loop?.mode))) clearFtQso();
  updateFtQsoTransmission(next.ft8_tx);
  if (next.session !== session) {
    ftAutoCq.checked = false;
    stopPtt(); resetRttyTx(); session = next.session; wanted.clear(); guiLoaded = false;
    loopGeneration = null; cutoff = 0n; clearTimers(); frames.reset(); views.forEach(view => view.clear()); radio.reset(); sstv.reset(); rtty.resetRx(); rtty.waterfall.clear();
  }
  if (next.loop?.generation !== undefined && next.loop.generation !== loopGeneration) {
    if (loopGeneration !== null) cutoff = BigInt(next.loop.timestamp_ns || '0');
    loopGeneration = next.loop.generation; frames.reset(); views.forEach(view => view.clear()); rtty.waterfall.clear(); radio.reset();
  }
  if ((next.flags & 6) && !(state.flags & 6)) radio.reset();
  updateSstvTx(next.sstv_tx);
  state = next;
  if (next.waterfall_tx?.tx_id === waterfallStartId) waterfallStartId = null;
  if (next.waterfall_tx?.phase === 'idle' && next.waterfall_tx?.tx_id === waterfallStopId) waterfallStopId = null;
  $('sstv-mode').value = next.sstv_mode ?? 'auto';
  $('rtty-reverse').checked = !!next.rtty_reverse;
  ftWaterfall.update(next.rx?.['2']?.frequency_hz);
  rtty.waterfall.update(next.rx?.['2']?.frequency_hz, !!next.rtty_reverse);
  updateRttyTx();
  for (const [key, value] of wanted) {
    const actual = key === 'tx2' ? next.tx2?.frequency_hz : key === 'tx' ? next.tx?.frequency_hz : next.rx?.[key]?.frequency_hz;
    if (actual === value.hz) wanted.delete(key);
  }
  if (!guiLoaded && next.settings?.gui_audio_gain !== undefined) {
    guiLoaded = true; $('volume').value = next.settings.gui_audio_gain; volumeChanged(false);
  }
  if (pttHeld && (!canControl() || (next.flags & 6) || !radio.microphone || !radio.running || (pttAcked && !next.tx?.ptt))) stopPtt();
  const identity = `${session}:${next.tx?.epoch}`;
  if (identity !== txIdentity) { txIdentity = identity; txIndex = 0; }
  syncCapture(); renderFrequencies();
  if (!filterBusy && !timers.has('filter') && next.filter?.bandwidth_hz !== undefined) {
    $('filter-lower').max = 3500; $('filter-upper').min = 600;
    updateRange('filter-lower', next.filter.low_hz ?? next.filter.center_hz - next.filter.bandwidth_hz / 2);
    updateRange('filter-upper', next.filter.high_hz ?? next.filter.center_hz + next.filter.bandwidth_hz / 2);
  }
  renderFilter();
  updateRange('nr', next.nr?.percent); $('nr-value').textContent = `${$('nr').value} %`;
  updateRange('compressor', next.compressor?.percent); compressorLabel();
  if (document.activeElement !== $('loop')) $('loop').value = next.loop?.mode ?? 0;
  $('hardware-details').textContent = next.settings?.pluto_rx_hz ? `Pluto RX: ${frequencyText(Number(next.settings.pluto_rx_hz))} MHz · Pluto TX: ${frequencyText(Number(next.settings.pluto_tx_hz))} MHz` : t('Hardwarewerte werden geladen.', 'Loading hardware values.');
  $('compressor-details').textContent = `${t('Verdichtung', 'Compression')}: ${Number(next.compressor?.compression_db || 0).toFixed(1)} dB · ${t('Begrenzung', 'Limiting')}: ${Number(next.compressor?.limiting_db || 0).toFixed(1)} dB`;
  $('software-version').textContent = next.software_version ? `Version ${next.software_version}` : 'Version —';
  $('settings-form').dataset.revision = next.revision || '';
  renderWaterfallSettings(); views.forEach(view => view.update(next, selected)); controls();
  for (const waiter of stateWaiters) waiter(next);
}
let rx2AudioSubscription = null;
function syncRx2Subscription() {
  if (!isOpen()) { rx2AudioSubscription = null; return; }
  const enabled = selected === 2 && radio.running;
  if (rx2AudioSubscription !== enabled) {
    send({type:'audio_subscription',stream:2,enabled}); rx2AudioSubscription = enabled;
  }
}
function controls() {
  syncRx2Subscription();
  subscribeDigitalView();
  const writable = canControl();
  for (const element of document.querySelectorAll('[data-control]')) element.disabled = !writable;
  for (const canvas of [$('sstv-waterfall'), $('rtty-waterfall')])
    canvas.parentElement.dataset.tunable = String(canFineTuneRx2());
  renderSstvTxControls();
  $('rtty-reverse').disabled = !writable;
  $('rtty-tx-level').disabled = !writable;
  renderRttyTxControls();
  renderFt8Controls();
  renderWaterfallTxControls();
  $('ptt').disabled = selected === 2 || !writable || !!(state.flags & 6) || !radio.microphone || !radio.running;
  $('ptt-label').textContent = selected === 2 ? 'TX2' : 'PTT';
  $('audio').disabled = !!radio.starting;
  $('audio').textContent = radio.starting ? t('Audio startet …', 'Starting audio …') : radio.enabled ? t('Audio stoppen', 'Stop audio') : operator && selected === 1 ? t('Audio / Mikrofon', 'Audio / Microphone') : t('Audio starten', 'Start audio');
  $('audio').classList.toggle('active', radio.running);
  $('connect').disabled = socket?.readyState === WebSocket.CONNECTING;
  $('connect').textContent = isOpen() ? t('Trennen', 'Disconnect') : t('Verbinden', 'Connect');
  $('role').textContent = isOpen() ? operator ? t('Bediener', 'Operator') : t('Zuschauer', 'Viewer') : t('Getrennt', 'Disconnected');
  $('backend-status').textContent = !isOpen() ? t('Backend getrennt', 'Backend disconnected') : session ? t('Backend verbunden', 'Backend connected') : t('Backend wird gesucht', 'Looking for backend');
  $('backend-dot').className = `live-dot${isOpen() && session ? '' : ' offline'}`;
  const hardwareOnline = state.settings?.hardware_online === '1' && !(state.flags & 6);
  $('hardware-status').textContent = state.flags & 2 ? t('Hardware startet', 'Hardware starting') : state.flags & 4 ? 'Hardware offline' : hardwareOnline ? t('Hardware bereit', 'Hardware ready') : t('Hardware unbekannt', 'Hardware unknown');
  $('hardware-dot').className = `live-dot${state.flags & 4 ? ' error' : hardwareOnline ? '' : ' offline'}`;
  const transmitting = !!state.tx?.ptt;
  const sstvSending = state.sstv_tx?.phase === 'sending';
  const rttySending = state.rtty_tx?.phase === 'sending';
  const ft8Sending = state.ft8_tx?.phase === 'sending';
  const waterfallSending = state.waterfall_tx?.phase === 'sending';
  $('tx-status').textContent = transmitting ? (state.loop?.mode === 1 ? t('Audio-Test', 'Audio test') : t('Senden', 'Transmitting')) : state.loop?.mode ? t(`Empfang · ${state.loop.mode === 1 ? 'Audio' : 'HF'}-Loop`, `Receiving · ${state.loop.mode === 1 ? 'Audio' : 'RF'} loop`) : t('Empfang', 'Receiving');
  if (selected === 2) $('tx-status').textContent = sstvSending ? t('SSTV · Senden', 'SSTV · Transmitting') : 'RX2 / TX2 · Digital';
  if (rttySending) $('tx-status').textContent = transmitting ?
    t('SSB + RTTY · Senden', 'SSB + RTTY · Transmitting') : t('RTTY · Senden', 'RTTY · Transmitting');
  if (ft8Sending) $('tx-status').textContent = `${state.ft_mode ?? 'FT8'} · ${t('Senden', 'Transmitting')}`;
  if (waterfallSending) $('tx-status').textContent = t('Waterfall-Text · Senden', 'Waterfall text · Transmitting');
  $('tx-dot').className = `live-dot${transmitting || waterfallSending || ft8Sending || rttySending || selected === 2 && sstvSending ? ' sending' : isOpen() && state.ready ? '' : ' offline'}`;
  $('ptt').classList.toggle('sending', transmitting);
  $('ptt').setAttribute('aria-pressed', String(transmitting));
  applyMonitorGain();
  renderPttHint();
  $('audio-details').textContent = radio.starting ? t('Audio wird gestartet …', 'Starting audio …') : radio.micBusy ? t('Mikrofon wird geöffnet …', 'Opening microphone …') : radio.running ? t(`Audio aktiv · Gerät ${radio.context.sampleRate / 1000} kHz · Funk 12 kHz · Mikrofon ${radio.microphone ? 'aktiv' : 'aus'}`, `Audio active · Device ${radio.context.sampleRate / 1000} kHz · Radio 12 kHz · Microphone ${radio.microphone ? 'active' : 'off'}`) : t('Audio ausgeschaltet', 'Audio off');
  $('audio-message').hidden = !(radio.error || radio.warning);
  $('audio-message').textContent = radio.error || radio.warning;
  $('audio-message').classList.toggle('warning', !radio.error && !!radio.warning);
  renderDevices();
  if (performance.now() >= noticeUntil) {
    const error = !!(state.flags & 4);
    const message = !isOpen() ? 'Getrennt. Zum Anmelden „Verbinden“ wählen.' : !state.ready ? 'Warte auf Backend und bestätigte Abschaltung …' : error ? `Hardware offline: ${state.settings?.hardware_error || state.settings?.error || 'Verbindung prüfen'}` : !operator ? 'Zuschauer · Empfang aktiv. Für Steuerrecht bei freiem Platz neu verbinden.' : rtty.visible ? (rttySending ? t('RTTY sendet über TX2.', 'RTTY is transmitting over TX2.') : t('RTTY-Empfang über RX2.', 'RTTY reception on RX2.')) : sstvSending ? t('SSTV sendet über TX2.', 'SSTV is transmitting over TX2.') : selected === 2 ? (radio.running ? t('Bedienbereit · RX2-Audiokontrolle aktiv. TX2 erhält externes Audio.', 'Ready · RX2 audio monitoring active. TX2 receives external audio.') : t('Bedienbereit · Audio starten, um RX2 zu hören. TX2 erhält externes Audio.', 'Ready · Start audio to listen to RX2. TX2 receives external audio.')) : radio.running && radio.microphone ? 'Bedienbereit · Audio und Mikrofon aktiv.' : 'Bedienbereit · Audio starten und Mikrofon freigeben, um PTT zu verwenden.';
    notify(message, error, 0);
  }
}

function renderDevices() {
  for (const [kind, id, value] of [['audioinput', 'audio-input', radio.inputId], ['audiooutput', 'audio-output', radio.outputId]]) {
    const node = $(id), devices = radio.devices.filter(d => d.kind === kind && !['default', 'communications'].includes(d.deviceId));
    const signature = JSON.stringify([devices.map(d => [d.deviceId, d.label]), value, language]);
    if (node.dataset.signature !== signature) {
      const options = [new Option(t('Systemstandard', 'System default'), 'default')];
      for (const [index, device] of devices.entries()) options.push(new Option(device.label || t(`Gerät ${index + 1} · Freigabe erforderlich`, `Device ${index + 1} · Permission required`), device.deviceId));
      if (value && value !== 'default' && !devices.some(d => d.deviceId === value)) options.push(new Option(t('Gespeichertes Gerät nicht verfügbar', 'Saved device unavailable'), value));
      node.replaceChildren(...options); node.value = value || 'default'; node.dataset.signature = signature;
    }
    node.disabled = !!radio.starting || radio.micBusy || (kind === 'audioinput' && !canControl());
  }
  $('audio-device-details').textContent = radio.running ? `${t('Wiedergabe', 'Playback')}: ${radio.deviceLabel('audiooutput')} · ${radio.microphone ? `${t('Aufnahme', 'Recording')}: ${radio.deviceLabel('audioinput')}` : t('Mikrofon aus', 'Microphone off')}. ${t('Auswahl gilt für diesen Browser.', 'Selection applies to this browser.')}` : t('Gerätenamen erscheinen nach Freigabe des Mikrofons. Die Geräteauswahl wird nur in diesem Browser gespeichert.', 'Device names appear after microphone permission is granted. Device selection is saved in this browser only.');
}
function syncCapture() {
  ptt.checkTimeout();
  radio.setCapture(selected === 1 && pttHeld && pttAcked && canControl() && !(state.flags & 6) && state.tx?.ptt && radio.microphone && radio.running ? `${txIdentity}:${pttIntent}` : null);
}
function stopPtt() {
  ptt.reset();
  const held = pttHeld; pttHeld = false; pttAcked = false; pttIntent++;
  radio.setCapture(null);
  applyMonitorGain();
  if (held && operator && isOpen() && Number.isSafeInteger(state.tx?.frequency_hz)) send({type: 'tx', id: request = (request + 1) >>> 0, frequency_hz: state.tx.frequency_hz, ptt: false});
}
function startPtt() {
  if (selected !== 1 || pttHeld || !canControl() || (state.flags & 6) || !radio.microphone || !radio.running || wanted.has('tx')) return false;
  pttHeld = true; pttAcked = false;
  applyMonitorGain();
  const intent = ++pttIntent;
  command('tx', {frequency_hz: state.tx.frequency_hz, ptt: true}).then(answer => {
    if (intent !== pttIntent || !pttHeld) return;
    if (answer.status !== 0) throw fail(`Sendefreigabe abgewiesen (Status ${answer.status}).`);
    pttAcked = true; send({type: 'state'});
  }).catch(error => { if (intent === pttIntent) { stopPtt(); report(error); } });
  return true;
}
function rttyTxActive() {
  return rttyTx !== null || (state.rtty_tx?.phase ?? 'idle') !== 'idle';
}
function canStartRtty() {
  return !rttyTxActive() && canControl() && Number.isSafeInteger(state.tx2?.frequency_hz) &&
    !(state.flags & 6) && !state.loop?.mode && !wanted.has('tx2') && state.rtty_tx?.phase === 'idle' &&
    !sstvTxActive() && !waterfallTxActive() && !ft8Pending && (state.ft8_tx?.phase ?? 'idle') === 'idle';
}
function resetRttyTx() {
  if (!rttyTx) return;
  rttyTx = null;
  rtty.endTx();
}
function stopRttyTx() {
  if (!operator || !rttyTxActive() || rttyTx?.stopping) return;
  const id = rttyTx?.id ?? state.rtty_tx?.tx_id;
  if (!send({type: 'rtty_tx_stop', tx_id: id})) return;
  rttyTx ??= {id, accepted: true, live: false};
  rttyTx.stopping = true;
  rtty.endTx();
  renderRttyTxControls();
}
function beginRttyTx(text) {
  if (!canStartRtty()) return;
  const live = text === undefined;
  if (!live && text.length > rttyInputLimit) {
    rtty.error.textContent = t('RTTY-Text ist zu lang.', 'RTTY text is too long.');
    return;
  }
  const values = {frequency_hz: state.tx2.frequency_hz, level: Number($('rtty-tx-level').value)};
  const opening = live ? command('rtty_tx_open', values) : command('rtty_tx_start', {...values, text});
  const tx = rttyTx = {id: opening.id, accepted: false, live};
  rtty.beginTx(live);
  controls();
  opening.then(answer => {
    if (rttyTx !== tx) return;
    if (answer.status !== 0) throw fail(t('RTTY-Sendung wurde abgewiesen.', 'RTTY transmission was rejected.'));
    tx.accepted = true;
    if (!live && !tx.stopping) rtty.appendTx(answer.text);
  }).catch(error => {
    if (rttyTx !== tx) return;
    if (operator) send({type: 'rtty_tx_stop', tx_id: tx.id});
    resetRttyTx();
    rtty.error.textContent = error.message;
    renderRttyTxControls();
  });
}
function rttyEndMarker(text) {
  const index = text.indexOf('§');
  return index === -1 ? {text, finish: false} : {text: text.slice(0, index), finish: true};
}
function writeRttyText(input) {
  const tx = rttyTx;
  if (!tx?.live || tx.stopping || tx.finishing) return;
  const {text, finish} = rttyEndMarker(input);
  if (text.length > rttyInputLimit) {
    rtty.error.textContent = t('RTTY-Text ist zu lang für die Sendewarteschlange.', 'RTTY text exceeds the send queue.');
    return;
  }
  if (socket.bufferedAmount > 16384) {
    stopRttyTx();
    rtty.error.textContent = t('RTTY wegen Verbindungsrückstau beendet.', 'RTTY stopped due to connection backlog.');
    return;
  }
  if (finish) {
    tx.finishing = true;
    rtty.endTx();
    renderRttyTxControls();
  }
  checked('rtty_tx_write', {tx_id: tx.id, text, finish}).then(answer => {
    if (rttyTx !== tx || tx.stopping) return;
    if (finish) tx.live = false;
    rtty.appendTx(answer.text);
    rtty.error.textContent = '';
  }).catch(error => {
    if (rttyTx !== tx || tx.stopping) return;
    if (finish) {
      tx.finishing = false;
      rtty.tx.readOnly = false;
      renderRttyTxControls();
    }
    rtty.error.textContent = error.message;
  });
}
function renderRttyTxControls() {
  const button = $('rtty-tx-toggle');
  const active = rttyTxActive(), canStart = canStartRtty();
  button.textContent = active ? 'Stop' : 'Start';
  button.dataset.active = String(active);
  button.disabled = active ? !!rttyTx?.stopping || !operator || !isOpen() : !canStart;
  button.title = active ? t('RTTY-Sendung abbrechen', 'Stop RTTY transmission') :
    t('RTTY über TX2 senden', 'Transmit RTTY over TX2');
  const {text, finish} = rttyEndMarker(rttyPresets.text);
  const canQueue = rttyTx?.live && !rttyTx.stopping && !rttyTx.finishing && canControl();
  $('rtty-preset-send').disabled = !(canQueue && (finish || text.trim()) || canStart && text.trim());
}
function updateRttyTx() {
  const tx = state.rtty_tx;
  if (!rttyTx?.accepted || tx?.tx_id !== rttyTx.id || tx?.phase !== 'idle') return;
  resetRttyTx();
  if (tx.result && !['stopped', 'complete'].includes(tx.result)) {
    const message = t(`RTTY beendet (${tx.result}).`, `RTTY ended (${tx.result}).`);
    if (rtty.visible) rtty.error.textContent = message;
    else notify(message, true);
  }
}
function renderPttHint() {
  if (selected === 2) {
    $('ptt-hint').textContent = t('Externes Audio', 'External audio');
    $('ptt').title = t('TX2 sendet Audio externer Programme automatisch.', 'TX2 transmits external program audio automatically.');
    return;
  }
  const seconds = Math.ceil(ptt.remainingMs / 1000);
  const remaining = `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
  $('ptt-hint').textContent = !operator ? t('Nur Bediener', 'Operator only') : !state.ready ? t('Nicht bereit', 'Not ready') : state.flags & 4 ? 'Hardware offline' : !radio.microphone ? t('Mikrofon fehlt', 'Microphone unavailable') : state.tx?.ptt ? (ptt.mode === 'toggle' && ptt.deadline ? t(`Klick: aus · ${remaining}`, `Click: off · ${remaining}`) : state.loop?.mode === 1 ? t('Audio-Loop', 'Audio loop') : t('Senden', 'Transmitting')) : ptt.mode === 'toggle' ? t('Klicken zum Senden', 'Click to transmit') : t('Gedrückt halten', 'Press and hold');
  $('ptt').title = `${ptt.mode === 'toggle' ? t('Klicken schaltet PTT um · maximal 5 Minuten', 'Click to toggle PTT · 5 minutes maximum') : t('Drücken und halten zum Senden', 'Press and hold to transmit')}${ptt.spaceEnabled ? t(' · Leertaste = PTT', ' · Space = PTT') : ''}`;
}
function mediaChanged() {
  if (pttHeld && (!radio.microphone || !radio.running)) stopPtt();
  controls();
}
function transmitSamples(pcm) {
  if (ptt.checkTimeout()) return;
  if (!pttHeld || !pttAcked || !canControl() || (state.flags & 6) || !state.tx?.ptt || !radio.captureId) return;
  if (socket.bufferedAmount > 16384) { stopPtt(); notify('Senden wegen Verbindungsrückstau beendet.', true); return; }
  const data = new ArrayBuffer(544), v = new DataView(data);
  v.setUint32(0, 0x51545232); v.setUint8(4, 1); v.setUint8(5, 8); v.setUint16(6, 64);
  v.setUint32(8, 3); v.setUint32(12, state.tx.epoch); v.setBigUint64(24, BigInt(txIndex)); v.setBigUint64(32, BigInt(state.tx.frequency_hz));
  v.setUint32(40, 12000); v.setUint16(52, 240); v.setUint16(54, 2); v.setUint32(56, 240); v.setUint32(60, session);
  for (let i = 0; i < 240; i++) v.setInt16(64 + 2 * i, Math.round(Math.max(-1, Math.min(1, pcm[i])) * 32767));
  socket.send(data); txIndex += 240;
}

async function saveSettings(values) {
  const task = settingsTail.catch(() => {}).then(async () => {
    if (!canControl()) throw fail('Einstellungen können gerade nicht gespeichert werden.');
    const expected = {...state.settings};
    for (let attempt = 0; attempt < 2; attempt++) {
      const answer = await command('settings_set', {revision: state.revision, values});
      send({type: 'state'});
      await waitState(next => next.revision && BigInt(next.revision) >= BigInt(answer.revision));
      if (answer.status === 0) return;
      if (answer.status === 2 && attempt === 0 && Object.keys(values).every(key => state.settings[key] === expected[key] || String(state.settings[key]) === String(values[key]))) continue;
      throw fail(answer.status === 2 ? 'Einstellungen wurden zwischenzeitlich geändert. Setup erneut öffnen und Änderungen prüfen.' : state.settings?.error || state.settings?.hardware_error || `Speichern fehlgeschlagen (Status ${answer.status}).`);
    }
  });
  settingsTail = task;
  return task;
}
function volumeChanged(persist = true) {
  const gain = Number($('volume').value); rangePaint($('volume'));
  $('volume-value').textContent = `${Math.round(gain / 20 * 100)} %`; $('volume').title = `Audioverstärkung ${gain.toFixed(2)}×`;
  applyMonitorGain();
  if (persist && canControl()) debounce('volume', () => saveSettings({gui_audio_gain: gain}).catch(report), 350);
}
function applyMonitorGain() {
  const transmitting = selected === 1 && (operator ? pttHeld : !!state.tx?.ptt);
  const gain = Number($(transmitting ? 'volume-tx' : 'volume').value);
  if (radio.gain !== gain) radio.setGain(gain);
  $('listen-volume').dataset.mode = transmitting ? 'tx' : 'rx';
}
function txVolumeChanged(persist = true) {
  const input = $('volume-tx'), gain = Number(input.value), muted = gain === 0;
  if (!muted) txMonitorPrevious = gain;
  rangePaint(input); $('volume-tx-value').textContent = `${Math.round(gain / 20 * 100)} %`;
  const button = $('tx-listen-mute');
  button.setAttribute('aria-pressed', String(muted));
  button.title = muted ? t('TX-Mithören einschalten', 'Unmute TX monitoring') : t('TX-Mithören stummschalten', 'Mute TX monitoring');
  button.setAttribute('aria-label', button.title);
  button.querySelector('use').setAttribute('href', muted ? '#speaker-muted' : '#speaker');
  if (persist) {
    writePreference('txMonitorVolume', gain);
    writePreference('txMonitorPrevious', txMonitorPrevious);
  }
  applyMonitorGain();
}
function toggleTxMonitorMute() {
  const input = $('volume-tx');
  input.value = Number(input.value) > 0 ? 0 : txMonitorPrevious || Number($('volume').value) || 4;
  txVolumeChanged();
}
function compressorLabel() { const value = Number($('compressor').value); $('compressor-value').textContent = value === 0 ? 'Bypass' : `${value.toLocaleString('de-DE', {maximumFractionDigits: 1})} %`; rangePaint($('compressor')); }
function openSetup() {
  stopPtt(); settingsBase = {...state.settings};
  for (const field of document.querySelectorAll('[data-setting]')) field.value = settingsBase[field.dataset.setting] ?? '';
  const center = Number(state.settings?.rx_center_hz);
  if (Number.isSafeInteger(center)) for (const key of ['rx3_frequency_hz', 'rx4_frequency_hz', 'rx5_frequency_hz']) {
    const field = document.querySelector(`[data-setting="${key}"]`); field.min = center - 275000; field.max = center + 275000;
  }
  $('settings-message').textContent = ''; $('settings-message').classList.remove('error');
  $('setup-dialog').showModal(); controls();
  radio.refreshDevices().catch(() => {});
}
function openFrequency(channel) {
  if (!canControl()) return;
  stopPtt(); editingFrequency = String(channel);
  $('frequency-title').textContent = channel === 'tx' ? t(`TX${selected}-Frequenz`, `TX${selected} frequency`) : t(`RX${channel}-Frequenz`, `RX${channel} frequency`);
  $('frequency-input').value = (frequency(channel) / 1e6).toFixed(6);
  $('frequency-error').textContent = ''; $('frequency-dialog').showModal(); $('frequency-input').select();
}
function setMenuOpen(name, open) {
  for (const menu of ['filter', 'waterfall']) {
    if (menu !== name && !open) continue;
    const expanded = menu === name && open;
    $(`${menu}-content`).hidden = !expanded;
    $(`${menu}-toggle`).classList.toggle('active', expanded);
    $(`${menu}-toggle`).setAttribute('aria-expanded', String(expanded));
  }
}

function bindPlots(view, step) {
  for (const canvas of [...view.element.querySelectorAll('[data-tune]'), view.axis]) {
    canvas.addEventListener('pointermove', event => {
      if (event.pointerType !== 'mouse') return;
      cursor = {view, canvas, x: event.clientX}; queueCursorUpdate();
    });
    canvas.addEventListener('pointerleave', () => {
      if (cursor?.canvas === canvas) { cursor = null; queueCursorUpdate(); }
    });
  }
  for (const canvas of view.element.querySelectorAll('[data-tune]')) {
    let touch, touchEndedAt = -Infinity;
    canvas.addEventListener('click', event => {
      if (event.pointerType === 'touch' || event.pointerType === 'pen' || performance.now() - touchEndedAt < 500 || performance.now() - menuDismissedAt < 400) return;
      const hz = view.frequencyAt(event); if (hz !== null) tune(hz, true);
    });
    canvas.addEventListener('contextmenu', event => { event.preventDefault(); const hz = view.frequencyAt(event); if (hz !== null) tune(hz, false); });
    canvas.addEventListener('wheel', event => {
      if (!canControl() || !view.geometry) return;
      event.preventDefault();
      const delta = wheelTurns(event) * step;
      if (!delta) return;
      try {
        const targets = wheelTargets({selected, rx: frequency(selected), tx: frequency('tx'), delta,
          rxCenter: Number(state.settings.rx_center_hz), txCenter: Number(state.settings.tx_center_hz), shift: event.shiftKey, ctrl: event.ctrlKey});
        if (targets.tx !== undefined) stopPtt();
        if (targets.rx !== undefined) setFrequency(targets.rx, targets.frequency);
        if (targets.tx !== undefined) setFrequency('tx', targets.tx);
      } catch (error) { report(error); }
    }, {passive: false});
    canvas.addEventListener('pointerdown', event => { if (event.pointerType !== 'mouse') touch = {id: event.pointerId, x: event.clientX, y: event.clientY}; });
    canvas.addEventListener('pointercancel', () => { touch = null; });
    canvas.addEventListener('pointerup', event => {
      if (!touch || touch.id !== event.pointerId) return;
      const tap = Math.hypot(event.clientX - touch.x, event.clientY - touch.y) < 10; touch = null;
      touchEndedAt = performance.now();
      if (tap && touchEndedAt - menuDismissedAt >= 400) { const hz = view.frequencyAt(event); if (hz !== null) tune(hz, together); }
    });
  }
}
function bindFrequency(channel, node) {
  let gesture, suppressClickUntil = 0;
  node.addEventListener('click', () => {
    if (performance.now() < suppressClickUntil) return;
    if (channel === 'tx') openFrequency('tx'); else selectReceiver(channel);
  });
  if (channel !== 'tx') node.addEventListener('dblclick', () => openFrequency(channel));
  node.addEventListener('pointerdown', event => {
    if (event.pointerType === 'mouse' || !canControl()) return;
    gesture = {id: event.pointerId, x: event.clientX, y: event.clientY, steps: 0, active: false};
    node.setPointerCapture(event.pointerId);
  });
  node.addEventListener('pointermove', event => {
    if (!gesture || gesture.id !== event.pointerId) return;
    const dx = event.clientX - gesture.x, dy = event.clientY - gesture.y;
    if (!gesture.active && Math.abs(dy) > Math.abs(dx) && Math.abs(dy) > 10) { gesture = null; return; }
    if (!gesture.active && Math.abs(dx) > 12 && Math.abs(dx) > Math.abs(dy)) {
      gesture.active = true; node.classList.add('swiping');
      if (channel !== 'tx') selectReceiver(channel);
    }
    if (gesture.active) {
      event.preventDefault(); suppressClickUntil = performance.now() + 600;
      const steps = Math.trunc(dx / 12), delta = steps - gesture.steps;
      if (delta) { gesture.steps = steps; fineTune(channel, delta * 25); }
    }
  }, {passive: false});
  const finish = () => { if (gesture?.active) suppressClickUntil = performance.now() + 600; gesture = null; node.classList.remove('swiping'); };
  node.addEventListener('pointerup', finish); node.addEventListener('pointercancel', finish); node.addEventListener('lostpointercapture', finish);
}

$('connect').onclick = () => { if (isOpen()) { stopPtt(); socket.close(); } else connect(); };
$('audio').onclick = async () => {
  try {
    if (radio.enabled) { stopPtt(); radio.stop(); }
    else if (canControl() && selected === 1) { await radio.enableMic(); if (!canControl()) radio.stopMic(); }
    else await radio.start();
  } catch (error) { notify(radio.error || `Audio / Mikrofon: ${error.message}`, true); }
};
for (const [id, kind] of [['audio-input', 'audioinput'], ['audio-output', 'audiooutput']]) $(id).onchange = async () => {
  stopPtt();
  try {
    await radio.selectDevice(kind, $(id).value);
    // Selecting a working input also recovers a failed/disconnected microphone.
    if (kind === 'audioinput' && radio.running && canControl()) {
      await radio.enableMic(); if (!canControl()) radio.stopMic();
    }
  } catch (error) { notify(radio.error || error.message, true); }
};
$('setup').onclick = openSetup;
for (const button of document.querySelectorAll('[data-close]')) button.onclick = () => $(button.dataset.close).close();
for (const name of ['filter', 'waterfall']) $(`${name}-toggle`).onclick = () => setMenuOpen(name, $(`${name}-content`).hidden);
const compactLayout = matchMedia('(max-width: 800px)');
function closeMenus() { setMenuOpen('filter', false); setMenuOpen('waterfall', false); }
if (compactLayout.matches) closeMenus();
compactLayout.addEventListener('change', event => { if (event.matches) closeMenus(); });
document.addEventListener('pointerdown', event => {
  if (compactLayout.matches && ['filter', 'waterfall'].some(name => !$(`${name}-content`).hidden) && !event.target.closest('.sidebar')) {
    menuDismissedAt = performance.now(); closeMenus();
  }
}, {capture: true});
for (const bound of ['floor', 'ceiling']) $(`waterfall-${bound}`).oninput = () => {
  waterfallOffsets = displayOffsets(Number($('waterfall-floor').value), Number($('waterfall-ceiling').value));
  writePreference('waterfallFloorOffset', waterfallOffsets.floor); writePreference('waterfallCeilingOffset', waterfallOffsets.ceiling); renderWaterfallSettings();
};
$('spectrum-average').oninput = () => setSpectrumAverage($('spectrum-average').value);
$('display-color').onchange = () => setDisplayColor($('display-color').value);
$('fullband-axis-switch').onclick = () => {
  const index = AXIS_MODES.findIndex(item => item.value === views[0].axisMode);
  setFullbandAxis(AXIS_MODES[(index + 1) % AXIS_MODES.length].value);
};
// Space/Enter operate the axis button without also triggering global PTT.
$('fullband-axis-switch').addEventListener('keydown', event => {
  if (event.code === 'Space' || event.key === ' ' || event.key === 'Enter') event.stopPropagation();
});
$('ptt-space').checked = ptt.spaceEnabled;
$(ptt.mode === 'toggle' ? 'ptt-toggle' : 'ptt-hold').checked = true;
for (const id of ['ptt-space', 'ptt-hold', 'ptt-toggle']) $(id).onchange = () => {
  ptt.configure($('ptt-toggle').checked ? 'toggle' : 'hold', $('ptt-space').checked);
  writePreference('pttMode', ptt.mode); writePreference('pttSpace', ptt.spaceEnabled); renderPttHint();
};
for (const id of ['filter-lower', 'filter-upper']) $(id).oninput = () => {
  renderFilter(id === 'filter-lower' ? 'low' : 'high');
  debounce('filter', () => applyFilter(renderFilter()));
};
for (const button of document.querySelectorAll('[data-bandwidth]')) button.onclick = () => {
  const bandwidth = Number(button.dataset.bandwidth);
  const low = Math.min(Number($('filter-lower').value), 4000 - bandwidth);
  $('filter-lower').max = 3500; $('filter-upper').min = 600;
  $('filter-lower').value = low; $('filter-upper').value = low + bandwidth;
  applyFilter(renderFilter());
};
$('nr').oninput = () => { $('nr-value').textContent = `${$('nr').value} %`; rangePaint($('nr')); debounce('nr', () => checked('nr', {percent: Number($('nr').value)}).catch(report)); };
$('compressor').oninput = () => { compressorLabel(); debounce('compressor', () => checked('compressor', {percent: Number($('compressor').value)}).catch(report)); };
$('volume').oninput = () => volumeChanged();
$('volume-tx').oninput = () => txVolumeChanged();
$('tx-listen-mute').onclick = toggleTxMonitorMute;
$('tx-listen-mute').addEventListener('keydown', e => {
  if (e.code === 'Space' || e.key === ' ' || e.key === 'Enter') e.stopPropagation();
});
const decoderSources = [3, 4, '5-dx-cluster', '5-cw-skimmer', '5-bulletin'];
const monitorViews = [...decoderSources, '5-diagnostics'];
const digitalReceivers = ['sstv', 'rtty', 'ft8'];
const digitalModes = [...digitalReceivers, 'waterfall'];
const digitalLabels = {sstv: 'SSTV', rtty: 'RTTY', ft8: 'FT4/8', waterfall: 'Waterfall-Text'};
const zoomSources = ['zoom', ...monitorViews, 'off', ...digitalModes];
let digitalView = null, subscribedDigitalView;
function subscribeDigitalView() {
  if (!isOpen()) return;
  const mode = digitalView;
  if (mode === subscribedDigitalView) return;
  for (const entry of digitalReceivers) send({type:`${entry}_subscribe`, enabled: entry === mode});
  subscribedDigitalView = mode;
}
function stopDigitalView() {
  if (digitalView === 'sstv' && sstvTxActive()) stopSstvTx();
  if (digitalView === 'rtty' && rttyTxActive()) stopRttyTx();
  if (digitalView === 'waterfall' && waterfallTxActive()) stopWaterfallTx();
  if (digitalView === 'ft8') {
    ftAutoCq.checked = false;
    if (!ftAutoQso && (ft8Pending || (state.ft8_tx?.phase ?? 'idle') !== 'idle'))
      send({type:'ft8_tx_stop'});
    clearFtQso();
  }
}
function renderDigitalViewStatus() {
  const active = ftAutoQso || ['sstv_tx', 'rtty_tx', 'ft8_tx', 'waterfall_tx'].some(key => (state[key]?.phase ?? 'idle') !== 'idle');
  $('decoder-digital-choice').toggleAttribute('data-running', !!active);
}
let decoderSource = decoderSources.find(source => String(source) === readPreference('decoderSource')) ?? 3;
let monitorView = monitorViews.find(source => String(source) === readPreference('monitorView')) ?? decoderSource;
const decoderScroll = Object.fromEntries(decoderSources.map(source => [source, {top: 0, follow: source !== '5-bulletin'}]));
const decoderPending = Object.fromEntries(decoderSources.map(source => [source, null]));
const decoderReplace = {};
let decoderRenderTimer = null;
const decoderOutput = source => $(source === 3 ? 'decoder-text' : `decoder-text-${source}`);
function rememberDecoderScroll() {
  if ($('decoder-panel').hidden) return;
  const output = decoderOutput(decoderSource);
  decoderScroll[decoderSource] = {top: output.scrollTop, follow: output.scrollHeight - output.clientHeight - output.scrollTop < 4};
}
function restoreDecoderScroll() {
  const output = decoderOutput(decoderSource), position = decoderScroll[decoderSource];
  output.scrollTop = position.follow ? output.scrollHeight : position.top;
}
function selectDecoder(source) {
  rememberDecoderScroll();
  const group = monitorViews.includes(source) ? 'monitor' :
    source === 'off' || digitalModes.includes(source) ? 'digital' : 'zoom';
  const showZoom = source === 'zoom' || source === 'off';
  const diagnostics = source === '5-diagnostics';
  const textView = decoderSources.includes(source), sstvView = source === 'sstv', rttyView = source === 'rtty';
  const ft8View = source === 'ft8';
  if (ft8View && !ft8Visible()) ftWaterfall.clear();
  const nextDigital = source === 'off' ? null : source;
  if (group === 'digital' && nextDigital !== digitalView) {
    stopDigitalView();
    digitalView = nextDigital;
  }
  if (group === 'monitor') {
    monitorView = source;
    writePreference('monitorView', source);
  }
  rtty.show(rttyView);
  if (textView) decoderSource = source;
  for (const channel of decoderSources) decoderOutput(channel).hidden = channel !== decoderSource;
  document.querySelector(`input[name="decoder-view"][value="${group}"]`).checked = true;
  document.querySelector(`input[name="decoder-monitor"][value="${monitorView}"]`).checked = true;
  document.querySelector(`input[name="decoder-digital"][value="${digitalView ?? 'off'}"]`).checked = true;
  $('decoder-monitor').hidden = group !== 'monitor';
  $('decoder-digital').hidden = group !== 'digital';
  const activeMode = $('decoder-active-mode');
  activeMode.hidden = !digitalView || group === 'digital';
  activeMode.textContent = digitalView ? `· ${digitalLabels[digitalView]}` : '';
  $('decoder-panel').hidden = !textView;
  $('ft8-panel').hidden = !ft8View;
  $('waterfall-tx-panel').hidden = source !== 'waterfall';
  if (source === 'waterfall') loadWaterfallFont();
  $('decoder-clear').hidden = !textView && !rttyView && !ft8View;
  sstv.show(sstvView);
  mmDiagnostics.show(diagnostics);
  send({type:'mm_diagnostics',enabled:diagnostics});
  $('zoom-output').dataset.view = String(source);
  controls();
  $('zoom-plots').setAttribute('aria-hidden', String(!showZoom));
  $('zoom-plots').inert = !showZoom;
  writePreference('zoomView', source);
  if (textView) { writePreference('decoderSource', source); restoreDecoderScroll(); }
}
function appendDecoderText(source, text, replace = false) {
  // FT8 returns a whole slot in a burst. Keep its per-line WebSocket handlers
  // free of layout work so they cannot hold up the 20-ms audio packets.
  const limit = typeof source === 'number' ? 16384 : 65542;
  decoderPending[source] = replace ? text.slice(0, limit) : ((decoderPending[source] || '') + text).slice(-limit);
  decoderReplace[source] = replace;
  if (decoderRenderTimer === null) decoderRenderTimer = setTimeout(flushDecoderText, 50);
}
function flushDecoderText() {
  decoderRenderTimer = null;
  const visible = decoderPending[decoderSource] !== null && !$('decoder-panel').hidden;
  if (visible) rememberDecoderScroll();
  for (const source of decoderSources) {
    if (decoderPending[source] === null) continue;
    const output = decoderOutput(source);
    let text = decoderReplace[source] ? decoderPending[source] : (output.textContent + decoderPending[source]).slice(-16384);
    if (source === 3) {
      const lines = text.split('\n');
      // A final newline terminates the last FT8 message; it adds no extra row.
      const excess = lines.length - (text.endsWith('\n') ? 1 : 0) - 50;
      if (excess > 0) text = lines.slice(excess).join('\n');
    }
    if (source === '5-bulletin' && output.textContent !== text) decoderScroll[source] = {top: 0, follow: false};
    output.textContent = text;
    decoderPending[source] = null;
  }
  if (visible) restoreDecoderScroll();
}
$('decoder-clear').onclick = () => { if (ft8Visible()) { clearFt8(); return; } if (rtty.visible) { rtty.clearRx(); return; } decoderPending[decoderSource] = null; decoderOutput(decoderSource).textContent = ''; decoderScroll[decoderSource] = {top: 0, follow: decoderSource !== '5-bulletin'}; };
$('zoom-sources').onchange = ({target}) => {
  const source = {zoom: 'zoom', monitor: monitorView, digital: digitalView ?? 'off'}[target.value];
  selectDecoder(source);
  if (source === 'ft8') {
    scrollFtLogToEnd(ft8Log);
    scrollFtLogToEnd(ftOwnLog);
  }
};
$('decoder-monitor').onchange = ({target}) => {
  selectDecoder(monitorViews.find(source => String(source) === target.value));
};
$('decoder-digital').onchange = ({target}) => selectDecoder(target.value);
// Keep local decoder controls and log scrolling independent of global Space PTT.
for (const id of ['zoom-sources', 'decoder-monitor', 'decoder-digital', 'decoder-clear', 'decoder-panel', 'mm-diagnostics', 'sstv-panel', 'rtty-panel', 'ft8-panel', 'waterfall-tx-panel']) $(id).addEventListener('keydown', event => {
  if (event.code === 'Space' || event.key === ' ') event.stopPropagation();
});
for (const log of [ft8Log, ftOwnLog]) {
  const {output} = log;
  const resize = new ResizeObserver(() => {
    log.width = output.clientWidth;
    log.height = output.clientHeight;
    if (log.height && log.follow) scrollFtLogToEnd(log);
    else log.scrollTop = output.scrollTop;
  });
  resize.observe(output);
  output.addEventListener('scroll', () => {
    if (!log.height || log.height !== output.clientHeight || log.width !== output.clientWidth ||
        log.scrollTop === output.scrollTop) return;
    log.scrollTop = output.scrollTop;
    log.follow = output.scrollHeight - output.clientHeight - log.scrollTop < 2;
  });
  document.querySelector(`.ft-log-end[aria-controls="${output.id}"]`).onclick = () => scrollFtLogToEnd(log);
}
function scrollFtLogToEnd(log) {
  log.follow = true;
  log.output.scrollTop = log.output.scrollHeight;
  log.scrollTop = log.output.scrollTop;
}
function clearFtLog(log) {
  log.lines.length = 0;
  log.output.textContent = '';
  scrollFtLogToEnd(log);
}
function clearFt8() {
  clearFtLog(ft8Log);
  clearFtLog(ftOwnLog);
}
function appendFtLine(log, entry) {
  const {output, lines} = log;
  lines.push(entry);
  output.append(entry.line);
  if (lines.length > 50) lines.shift().line.remove();
  if (log.follow) scrollFtLogToEnd(log);
}
function appendFtOwnLine(entry, own) {
  if (!own || !entry.text.toUpperCase().split(/[^A-Z0-9]+/).includes(own)) return;
  const line = entry.line.cloneNode(true);
  line.onclick = entry.line.onclick;
  line.removeAttribute('data-new-cq');
  line.removeAttribute('data-cq-call');
  appendFtLine(ftOwnLog, {...entry, line});
}
function appendFt8(message) {
  const {utc, snr_db, dt_s, frequency_hz, message: text} = message;
  if (typeof utc !== 'string' || typeof text !== 'string' ||
      ![snr_db, dt_s, frequency_hz].every(Number.isFinite)) return;
  const contact = parseFtContact(message);
  const line = document.createElement(contact ? 'button' : 'div');
  line.className = 'ft-decode-line';
  line.textContent = `${utc} ${String(snr_db).padStart(3)} dB ${dt_s.toFixed(2).padStart(5)} s ${frequency_hz.toFixed(0).padStart(4)} Hz  ${text}`;
  if (contact) {
    line.type = 'button';
    line.disabled = $('ft-cq').disabled || !canChooseFtContact(contact);
    line.onclick = () => {
      const own = ftCall($('ft-own-call').value);
      // For a third-party exchange, call the station that transmitted the selected line.
      const target = contact.recipient && contact.recipient !== own ? {...contact, kind:'cq'} : contact;
      if ($('ft-auto').checked && (target.kind === 'cq' || canStartFtAuto(target, own))) startFtAutoQso(target, true);
      else prepareFtReply(target, true);
    };
  }
  const entry = {line, contact, text};
  appendFtLine(ft8Log, entry);
  const own = ftCall($('ft-own-call').value);
  appendFtOwnLine(entry, own);
  if (typeof message.cq_call === 'string') {
    line.dataset.cqCall = message.cq_call;
    line.toggleAttribute('data-new-cq', message.worked_recently === false);
  }
  if (contact) {
    if (ftAutoQso) advanceFtAutoQso(contact);
    else if (isFtAutoReply(contact, own)) startFtAutoQso(contact);
    else prepareFtReply(contact, false);
  }
}
function isFtAutoReply(contact, own) {
  if (!ftAutoReply.checked || contact.recipient !== own || !canStartFtAuto(contact, own)) return false;
  if (ftGrid(contact.kind)) return true;
  return ftQso?.step === 'cq' && ftQso.waiting &&
    contact.parity === ftQso.parity && contact.slot === ftQso.sentSlot + ftPeriod(state.ft_mode);
}
function canChooseFtContact(contact) {
  const own = ftCall($('ft-own-call').value);
  return contact.peer !== own && !(contact.recipient === own && contact.kind === '73');
}
function ftDraftInForm(draft) {
  return !!draft?.text && $('ft8-message').value === draft.text && Number($('ft8-parity').value) === draft.parity;
}
function applyFtDraft(draft) {
  $('ft8-message').value = draft.text;
  if (draft.text) $('ft8-parity').value = String(draft.parity);
}
function clearFtQso() {
  if (ftAutoQso) endFtAutoQso(true);
  if (ftDraftInForm(ftQso)) $('ft8-message').value = '';
  ftQso = null;
  ftQsoTransmission = null;
  ftQsoLog = null;
}
function ftIdentity(showError) {
  const own = ftCall($('ft-own-call').value);
  const locator = ftGrid($('ft-own-grid').value);
  if (own && locator) return {own, locator};
  if (showError) {
    ft8Error = own ? t('QTH-Locator mit vier oder sechs Zeichen eingeben.', 'Enter a four- or six-character grid locator.') :
      t('Eigenes Standardrufzeichen eingeben.', 'Enter your standard callsign.');
    renderFt8Controls();
    $(own ? 'ft-own-grid' : 'ft-own-call').focus();
  }
  return null;
}
function setFtDraft(draft, selected) {
  if (ftQsoLog && (draft.peer !== ftQsoLog.call || (selected && draft.step === 'locator')))
    ftQsoLog = null;
  const use = selected || ftDraftInForm(ftQso) || !$('ft8-message').value;
  ftQso = draft;
  if (use) applyFtDraft(draft);
  ft8Error = '';
  renderFt8Controls();
}
function prepareFtReply(contact, selected) {
  if (!selected && (!operator || !ftQso?.peer || ftQso.step === 'complete' || contact.peer !== ftQso.peer || contact.kind === 'cq' ||
      contact.slot <= Math.max(ftQso.slot, ftQso.sentSlot ?? 0, ftQsoTransmission?.slot ?? 0) ||
      contact.parity !== ftQso.parity)) return;
  const identity = ftIdentity(selected);
  if (!identity) return;
  const draft = ftReply(contact, identity.own, identity.locator);
  if (!draft) return;
  if (!selected && draft.step === 'complete' && ftQsoLog?.acknowledged)
    saveFtQsoLog(contact.slot);
  setFtDraft(draft, selected);
}
function startFtAutoQso(contact, selected = false) {
  if ($('ft8-send').disabled || ftAutoQso) return;
  const identity = ftIdentity(true);
  if (!identity) return;
  const draft = ftReply(contact, identity.own, identity.locator);
  const period = ftPeriod(state.ft_mode);
  if (!draft || !period || (!selected && Date.now() / 1000 - contact.slot > 3 * period)) return;
  ftAutoQso = {own: identity.own, period, attempts: 1, timer: null};
  setFtDraft(draft, true);
  transmitFtDraft(ftAutoQso);
}
function endFtAutoQso(cancelTx) {
  clearTimeout(ftAutoQso.timer);
  ftAutoQso = null;
  if (cancelTx && (ft8Pending || state.ft8_tx?.phase === 'scheduled' || state.ft8_tx?.phase === 'sending'))
    send({type:'ft8_tx_stop'});
}
function advanceFtAutoQso(contact) {
  if (!ftQso?.waiting || contact.peer !== ftQso.peer || contact.recipient !== ftAutoQso.own ||
      contact.slot <= Math.max(ftQso.slot, ftQso.sentSlot) || contact.parity !== ftQso.parity) return;
  const draft = ftReply(contact, ftAutoQso.own, '');
  if (!draft || !nextFtAutoStep(ftQso.step, draft.step)) return;
  if (draft.step === 'complete') {
    if (ftQsoLog?.acknowledged) saveFtQsoLog(contact.slot);
    endFtAutoQso(false);
    setFtDraft(draft, true);
    return;
  }
  const sameStep = draft.step === ftQso.step;
  if (sameStep && ftAutoQso.attempts >= ftAutoMaxAttempts) return;
  clearTimeout(ftAutoQso.timer);
  sendFtAutoStep(draft, sameStep ? ftAutoQso.attempts + 1 : 1);
}
function sendFtAutoStep(draft, attempts) {
  if (!canSendFtDraft()) {
    stopFtAutoQso(t('TX2 ist belegt oder nicht bereit.', 'TX2 is busy or unavailable.'));
    return;
  }
  ftAutoQso.attempts = attempts;
  draft.waiting = false;
  if (draft !== ftQso) setFtDraft(draft, true);
  transmitFtDraft(ftAutoQso);
}
function stopFtAutoQso(message, cancelTx = false) {
  endFtAutoQso(cancelTx);
  clearFtQso();
  ft8Error = message;
  renderFt8Controls();
}
function trackFtQsoTransmission({draft, slot, values, identity, rxHz}) {
  if (!draft.peer || !identity) return;
  if (!ftQsoLog) {
    ftQsoLog = {
      call: draft.peer, own_call: identity.own, own_grid: identity.locator, mode: values.mode,
      start: Math.floor(slot), tx_hz: values.frequency_hz + values.audio_hz,
      rx_hz: rxHz + Math.round(draft.audioHz),
    };
  }
  const {received} = draft;
  if (received.locator || (/^[A-R]{2}\d{2}$/.test(received.kind) && received.kind !== 'RR73'))
    ftQsoLog.grid = received.locator ?? received.kind;
  if (/^R?[+-]\d{2}$/.test(received.kind))
    ftQsoLog.received_report = received.kind.replace(/^R/, '');
  if (draft.step === 'report' || draft.step === 'ackReport')
    ftQsoLog.sent_report = values.message.split(/\s+/).pop().replace(/^R/, '');
  if (draft.step === 'ack' || draft.step === 'signoff') saveFtQsoLog(slot);
  if (draft.step === 'ackReport') ftQsoLog.acknowledged = true;
}
async function saveFtQsoLog(slot) {
  const record = ftQsoLog;
  if (!record || record.saving || record.saved || !record.sent_report || !record.received_report) return;
  record.saving = true;
  try {
    const {call, own_call, grid, own_grid, mode, start, tx_hz, rx_hz, sent_report, received_report} = record;
    await checked('ft_qso_log', {call, own_call, grid, own_grid, mode, start, tx_hz, rx_hz, sent_report, received_report,
      end: Math.floor(slot)});
    record.saved = true;
    ftLogError = false;
  } catch (error) {
    ftLogError = error.code !== 'ft_log_disabled';
  } finally {
    record.saving = false;
    renderFt8Controls();
  }
}
function updateFtQsoTransmission(tx) {
  const sending = ftQsoTransmission;
  if (!sending || !tx) return;
  if (tx.phase !== 'idle') {
    if (tx.slot_utc === sending.slot) sending.observed = true;
    return;
  }
  if (!sending.observed) return;
  ftQsoTransmission = null;
  if (sending.draft !== ftQso) return;
  if (tx.result !== 'complete') {
    if (ftAutoQso) {
      endFtAutoQso(false);
      clearFtQso();
    }
    return;
  }
  ftQso.sentSlot = sending.slot;
  trackFtQsoTransmission(sending);
  if (ftQso.step === 'signoff') {
    ftQso.step = 'complete';
    if (ftDraftInForm(ftQso)) $('ft8-message').value = '';
    ftQso.text = '';
    if (ftAutoQso) endFtAutoQso(false);
  } else {
    ftQso.waiting = true;
    if (ftAutoQso) {
      const automatic = ftAutoQso;
      automatic.timer = setTimeout(() => {
        if (ftAutoQso !== automatic) return;
        if (automatic.attempts >= ftAutoMaxAttempts) {
          stopFtAutoQso(t('Auto-QSO beendet: keine passende Antwort nach drei Sendeversuchen.',
            'Auto QSO ended: no matching reply after three attempts.'));
          return;
        }
        sendFtAutoStep(ftQso, automatic.attempts + 1);
      }, ftAutoReplySlots * automatic.period * 1000);
    }
  }
}
function renderFtQsoControls(enabled) {
  const manual = enabled && !ftAutoQso;
  for (const id of ['ft-own-call', 'ft-own-grid', 'ft-cq', 'ft-apply-reply', 'ft-end-qso', 'ft-auto', 'ft-auto-reply']) $(id).disabled = !manual;
  for (const {lines} of [ft8Log, ftOwnLog]) {
    for (const {line, contact} of lines)
      if (contact) line.disabled = !manual || !canChooseFtContact(contact);
  }
  $('ft-contact').hidden = !ftQso;
  if (!ftQso) return;
  const {peer, audioHz, step, text, waiting} = ftQso;
  const ready = !!text && !ftDraftInForm(ftQso);
  $('ft-apply-reply').hidden = !ready;
  $('ft-end-qso').hidden = !!ftAutoQso;
  const name = peer ? `${peer} · RX ${Math.round(audioHz)} Hz` : 'CQ';
  const status = step === 'complete' ? t('QSO abgeschlossen', 'QSO complete') :
    waiting ? (peer || ftAutoReply.checked ? t('Warte auf Antwort', 'Waiting for reply') : t('Antwort anklicken', 'Select a reply')) :
    ready ? text : t('Sendung vorbereitet', 'Transmission prepared');
  const output = $('ft-qso-status');
  const label = `${ftAutoQso ? 'Auto · ' : ''}${name} · ${status}`;
  if (output.textContent !== label) output.textContent = label;
}
for (const [id, preference] of [['ft-own-call', 'ftOwnCall'], ['ft-own-grid', 'ftOwnGrid']]) {
  const input = $(id);
  input.value = readPreference(preference) ?? '';
  input.oninput = () => {
    clearFtQso();
    ft8Error = '';
    writePreference(preference, input.value.trim().toUpperCase());
    if (id === 'ft-own-call') {
      clearFtLog(ftOwnLog);
      const own = ftCall(input.value);
      for (const entry of ft8Log.lines) appendFtOwnLine(entry, own);
    }
    renderFt8Controls();
  };
}
const ftAutoReply = $('ft-auto-reply');
ftAutoReply.checked = readPreference('ftAutoReply') === 'true';
ftAutoReply.onchange = () => {
  writePreference('ftAutoReply', ftAutoReply.checked);
  renderFt8Controls();
};
const ftAutoCq = $('ft-auto-cq');
let ftAutoCqRetryAt = 0;
ftAutoCq.checked = false;
ftAutoCq.onchange = () => {
  if (ftAutoCq.checked) {
    if (!ftIdentity(true)) ftAutoCq.checked = false;
    ftAutoCqRetryAt = 0;
    sendIdleFtCq();
  } else if (ftQso?.autoCq && (ft8Pending || state.ft8_tx?.phase === 'scheduled')) {
    send({type:'ft8_tx_stop'});
    clearFtQso();
  }
  renderFt8Controls();
};
function prepareFtCq(identity, autoCq = false) {
  setFtDraft({step:'cq', autoCq, text:`CQ ${identity.own} ${identity.locator}`,
    parity:Number($('ft8-parity').value)}, true);
}
$('ft-cq').onclick = () => {
  const identity = ftIdentity(true);
  if (!identity) return;
  prepareFtCq(identity);
  $('ft8-tx-form').requestSubmit($('ft8-send'));
};
function sendIdleFtCq() {
  if (!ftAutoCq.checked || digitalView !== 'ft8' || ftAutoQso || !canSendFtDraft()) return;
  if (ftQso?.peer && ftQso.step !== 'complete') return;
  const period = ftPeriod(state.ft_mode);
  const now = Date.now() / 1000;
  if (now < ftAutoCqRetryAt ||
      (ftQso?.step === 'cq' && ftQso.waiting && now < ftQso.sentSlot + 2 * period + 1)) return;
  const identity = ftIdentity(false);
  if (!identity || !$('ft8-audio-hz').validity.valid || !$('ft8-level').validity.valid) return;
  prepareFtCq(identity, true);
  transmitFtDraft();
}
$('ft-apply-reply').onclick = () => {
  if (ftQso) applyFtDraft(ftQso);
  renderFt8Controls();
};
$('ft-end-qso').onclick = () => {
  clearFtQso();
  ft8Error = '';
  renderFt8Controls();
};
function canSendFtDraft() {
  return canControl() && state.ft8_enabled && !(state.flags & 6) && !state.loop?.mode &&
    !ft8Pending && !ftModePending && state.ft8_tx?.phase === 'idle' && !sstvTxActive() && !rttyTxActive() && !waterfallTxActive() &&
    !wanted.has('2') && !wanted.has('tx2') && !!state.tx2?.frequency_hz;
}
function renderFtSlotClock() {
  if (!ft8Visible()) return;
  const now = new Date();
  const clock = $('ft-utc-clock');
  const utc = now.toISOString();
  const label = utc.slice(11, 19) + ' UTC';
  if (clock.textContent !== label) {
    clock.textContent = label;
    clock.dateTime = utc.slice(11, 19) + 'Z';
  }
  const period = ftPeriod(state.ft_mode ?? 'FT8');
  const progress = $('ft-slot-progress');
  progress.max = period;
  progress.value = (now.getTime() % (period * 1000)) / 1000;
}
function renderFt8Controls() {
  renderFtSlotClock();
  renderDigitalViewStatus();
  const tx = state.ft8_tx;
  const active = !!tx && tx.phase !== 'idle';
  const writable = canControl() && state.ft8_enabled && !(state.flags & 6) && !state.loop?.mode;
  const busy = ft8Pending || ftModePending || active || sstvTxActive() || rttyTxActive();
  $('ft8-frequency').disabled = !writable || busy || !!ftAutoQso || wanted.has('2') || wanted.has('tx2');
  const mode = state.ft_mode ?? 'FT8';
  const modeSelect = $('ft-mode');
  if (!ftModePending) modeSelect.value = mode;
  modeSelect.disabled = !writable || !state.ft_mode || active || ft8Pending || ftModePending || !!ftAutoQso;
  const ft4 = mode === 'FT4';
  const parity = $('ft8-parity');
  parity.options[0].textContent = ft4 ? ':00 / :15 / :30 / :45' : ':00 / :30';
  parity.options[1].textContent = ft4 ? ':07.5 / :22.5 / :37.5 / :52.5' : ':15 / :45';
  $('ft8-audio-hz').max = ft4 ? 3930 : 3950;
  $('ft8-send').disabled = !canSendFtDraft() || !!ftAutoQso;
  ftAutoCq.disabled = !writable && !ftAutoCq.checked;
  $('ft8-stop').disabled = !operator || !isOpen() || (!ftAutoCq.checked && !ftAutoQso && (!active || ft8Pending));
  renderFtQsoControls(!$('ft8-send').disabled);
  for (const id of ['ft8-message', 'ft8-audio-hz', 'ft8-level', 'ft8-parity']) $(id).disabled = !writable || busy || !!ftAutoQso;
  renderFt8Status(tx);
}
function renderFt8Status(tx) {
  const time = tx?.slot_utc ?
    new Date(tx.slot_utc * 1000).toISOString().slice(11, Number.isInteger(tx.slot_utc) ? 19 : 21) + ' UTC' : '';
  const result = {
    complete: t('Sendung abgeschlossen', 'Transmission complete'),
    stopped: t('Sendung abgebrochen', 'Transmission cancelled'),
    missed_slot: t('Sendeslot verpasst', 'Missed transmit slot'),
    clock_changed: t('Systemzeit hat sich geändert', 'System clock changed'),
    retuned: t('Frequenz geändert', 'Frequency changed'),
    unavailable: t('TX2 nicht verfügbar', 'TX2 unavailable'),
    underrun: t('TX2-Audiopuffer unterbrochen', 'TX2 audio buffer underrun'),
  };
  const status = $('ft8-status');
  const logError = ftLogError && t('QSO konnte nicht im ADIF-Logbuch gespeichert werden.', 'Could not save QSO to the ADIF logbook.');
  status.classList.toggle('error', !!(ft8Error || logError));
  status.classList.toggle('sending', tx?.phase === 'sending' && !ft8Error && !logError);
  const seconds = Math.max(0, Math.ceil(tx?.slot_utc - Date.now() / 1000));
  const text = ft8Error || logError || (tx?.phase === 'scheduled' && time ?
    `${t('Geplant für', 'Scheduled for')} ${time} · ${t(`noch ${seconds} s`, `${seconds} s remaining`)}` :
    tx?.phase === 'sending' ? `${t('Sendet', 'Transmitting')} · ${time}` :
    result[tx?.result] ?? (state.ready && !state.ft8_enabled ? t('FT4/8 an RX2 ist deaktiviert.', 'RX2 FT4/8 is disabled.') : ''));
  if (status.textContent !== text) status.textContent = text;
}
$('ft8-frequency').onclick = () => {
  if ($('ft8-frequency').disabled) return;
  try {
    const tx = ftStandardRxHz - DOWNLINK_UPLINK_HZ;
    validateFrequency(ftStandardRxHz, Number(state.settings.rx_center_hz), 'RX2');
    validateFrequency(tx, Number(state.settings.tx_center_hz), 'TX2');
    setFrequency(2, ftStandardRxHz);
    setFrequency('tx', tx, 2);
    renderFt8Controls();
  } catch (error) { report(error); }
};
$('ft-mode').onchange = async event => {
  const mode = event.target.value;
  ftModePending = true;
  ft8Error = '';
  renderFt8Controls();
  try { await checked('ft_mode', {mode}); }
  catch (error) { ft8Error = error.message; }
  finally { ftModePending = false; renderFt8Controls(); }
};
$('ft8-tx-form').onsubmit = event => {
  event.preventDefault();
  if (!$('ft8-send').disabled) transmitFtDraft();
};
async function transmitFtDraft(automatic = null) {
  const values = {
    frequency_hz: state.tx2.frequency_hz,
    message: $('ft8-message').value,
    mode: state.ft_mode ?? 'FT8',
    audio_hz: Number($('ft8-audio-hz').value),
    level: Number($('ft8-level').value),
    parity: Number($('ft8-parity').value),
  };
  if (!Number.isInteger(values.audio_hz) || values.audio_hz < 100 ||
      values.audio_hz > Number($('ft8-audio-hz').max) ||
      !Number.isInteger(values.level) || values.level < 1 || values.level > 100) {
    const message = t('Audiofrequenz und TX-Pegel prüfen.', 'Check audio frequency and TX level.');
    if (automatic) stopFtAutoQso(message);
    else { ft8Error = message; renderFt8Controls(); }
    return;
  }
  ft8Error = '';
  ft8Pending = true;
  renderFt8Controls();
  const draft = ftDraftInForm(ftQso) ? ftQso : null;
  const identity = draft?.peer ? ftIdentity(false) : null;
  if (identity) identity.locator = $('ft-own-grid').value.trim().toUpperCase();
  const rxHz = state.rx?.['2']?.frequency_hz;
  try {
    const answer = await checked('ft8_tx_start', values);
    if (draft && draft === ftQso && Number.isFinite(answer.slot_utc))
      ftQsoTransmission = {draft, slot:answer.slot_utc, values, identity, rxHz,
        observed:state.ft8_tx?.slot_utc === answer.slot_utc};
  }
  catch (error) {
    if (automatic && automatic !== ftAutoQso) return;
    if (draft?.autoCq && draft === ftQso) {
      if (error.code === 'tx2_busy') ftAutoCqRetryAt = Date.now() / 1000 + ftPeriod(values.mode);
      else ftAutoCq.checked = false;
    }
    if (automatic) {
      endFtAutoQso(error.code === 'timeout');
      clearFtQso();
    }
    ft8Error = ({
      ft8_free_text_length: t('Freitext ist auf 13 Zeichen inklusive Leerzeichen begrenzt. Für längere Nachrichten ein Standardformat verwenden.',
        'Free text is limited to 13 characters including spaces. Longer messages need a standard message format.'),
      ft8_invalid_message: t('Diese Nachricht lässt sich so nicht kodieren.', 'Cannot encode this message as entered.'),
    })[error.code] ?? error.message;
  }
  finally { ft8Pending = false; renderFt8Controls(); }
}
for (const [id, preference] of [['ft8-audio-hz', 'ftAudioHz'], ['ft8-level', 'ftTxLevel']]) {
  const input = $(id);
  const saved = Number(readPreference(preference));
  if (Number.isInteger(saved) && saved >= Number(input.min) && saved <= Number(input.max)) {
    input.value = saved;
  }
  input.addEventListener('input', () => {
    if (input.validity.valid) writePreference(preference, input.value);
  });
}
$('ft8-tx-form').addEventListener('input', () => {
  ft8Error = '';
  renderFt8Controls();
});
$('ft8-stop').onclick = () => {
  ftAutoCq.checked = false;
  if (ftAutoQso) stopFtAutoQso(t('Auto-QSO abgebrochen.', 'Auto QSO cancelled.'), true);
  else {
    send({type: 'ft8_tx_stop'});
    renderFt8Controls();
  }
};

const rx2TuneStep = 10;
for (const canvas of [$('sstv-waterfall'), $('rtty-waterfall')]) {
  canvas.addEventListener('wheel', event => {
    if (!canFineTuneRx2() || event.ctrlKey || event.metaKey) return;
    const delta = wheelTurns(event) * rx2TuneStep;
    if (!delta) return;
    event.preventDefault();
    fineTune(2, delta);
  }, {passive: false});
  canvas.addEventListener('keydown', event => {
    if (!canFineTuneRx2() || event.altKey || event.ctrlKey || event.metaKey) return;
    if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') return;
    event.preventDefault();
    fineTune(2, event.key === 'ArrowUp' ? rx2TuneStep : -rx2TuneStep);
  });
}
const sstvTxLevel = $('sstv-tx-level');
function renderSstvTxLevel() {
  rangePaint(sstvTxLevel);
  $('sstv-tx-level-value').textContent = `${sstvTxLevel.value} %`;
  renderSstvTxControls();
}
const savedSstvTxLevel = Number(readPreference('sstvTxLevel') ?? sstvTxLevel.defaultValue);
if (Number.isInteger(savedSstvTxLevel) && savedSstvTxLevel >= 0 && savedSstvTxLevel <= 100) {
  sstvTxLevel.value = savedSstvTxLevel;
}
sstvTxLevel.addEventListener('input', () => {
  renderSstvTxLevel();
  writePreference('sstvTxLevel', sstvTxLevel.value);
  if (operator && sstvTxActive() && !sstvStopRequested) {
    send({type: 'sstv_tx_level', level: Number(sstvTxLevel.value)});
  }
});
renderSstvTxLevel();
function sstvTxActive() {
  return sstvUpload !== null || (state.sstv_tx?.phase ?? 'idle') !== 'idle';
}
function renderSstvTxControls() {
  const button = $('sstv-tx-toggle');
  const busy = sstvStopRequested || sstvTxActive();
  const available = state.sstv_tx && state.tx2?.frequency_hz && !(state.flags & 6) && !state.loop?.mode;
  const hasLevel = Number(sstvTxLevel.value) > 0;
  button.textContent = busy ? 'Stop' : 'Start';
  button.dataset.active = String(busy);
  button.disabled = busy ? sstvStopRequested || !operator || !isOpen() :
    !canControl() || !available || sstvTx.image.hidden || !hasLevel || rttyTxActive() || waterfallTxActive() || ft8Pending ||
    (state.ft8_tx?.phase ?? 'idle') !== 'idle';
  button.title = busy ? t('SSTV-Sendung abbrechen', 'Stop SSTV transmission') :
    sstvTx.image.hidden ? t('Ein TX-Bild auswählen', 'Choose a TX image') :
    !hasLevel ? t('Sendepegel einstellen', 'Set the TX level') : t('SSTV über TX2 senden', 'Transmit SSTV over TX2');
  sstvTxLevel.disabled = busy && !operator;
  sstvTx.setBusy(busy);
  $('sstv-mode').disabled = busy || !canControl();
}

function updateSstvTx(next) {
  if (!next) return;
  if (sstvUpload?.sent) sstvUpload = null;
  if (next.phase !== 'idle') return;
  sstvStopRequested = false;
  const previous = state.sstv_tx;
  if (!next.result || (previous?.phase === 'idle' && previous.result === next.result)) return;
  const status = {
    complete: t('SSTV-Bild gesendet.', 'SSTV image sent.'),
    stopped: t('SSTV-Sendung gestoppt.', 'SSTV transmission stopped.'),
    retuned: t('SSTV-Sendung wegen TX2-Abstimmung gestoppt.', 'SSTV transmission stopped after TX2 tuning.'),
    tx2_busy: t('TX2 ist belegt.', 'TX2 is busy.'),
    upload_error: t('TX-Bild konnte nicht übertragen werden.', 'Could not upload the TX image.'),
  };
  notify(status[next.result] || t('SSTV-Sendung unterbrochen.', 'SSTV transmission interrupted.'),
    !['complete', 'stopped'].includes(next.result));
}

function stopSstvTx() {
  if (!operator || !isOpen() || sstvStopRequested) return;
  sstvUpload = null;
  sstvStopRequested = send({type: 'sstv_tx_stop'});
  renderSstvTxControls();
}
$('sstv-tx-toggle').onclick = async () => {
  const button = $('sstv-tx-toggle');
  if (button.disabled) return;
  if (button.dataset.active === 'true') {
    stopSstvTx();
    return;
  }
  const upload = {socket, session, sent: false};
  sstvUpload = upload;
  renderSstvTxControls();
  try {
    const canvas = sstvTx.image;
    const rgba = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
    const rgb = new Uint8Array(rgba.length * 3 / 4);
    for (let i = 0, j = 0; i < rgba.length; i += 4, j += 3) {
      rgb[j] = rgba[i]; rgb[j + 1] = rgba[i + 1]; rgb[j + 2] = rgba[i + 2];
    }
    const ack = await checked('sstv_tx_start', {
      level: Number(sstvTxLevel.value), frequency_hz: state.tx2.frequency_hz,
    });
    if (sstvUpload !== upload || socket !== upload.socket || session !== upload.session) return;
    for (let offset = 0; offset < rgb.length; offset += 1024) {
      const frame = new Uint8Array(64 + 1024);
      const header = new DataView(frame.buffer);
      header.setUint32(0, 0x51545232); // QTR2
      header.setUint8(4, 1);
      header.setUint8(5, 23); // SSTV image upload
      header.setUint16(6, 64);
      header.setUint32(8, 7); // TX2
      header.setUint32(12, ack.id);
      header.setUint32(28, offset);
      header.setUint32(60, session);
      frame.set(rgb.subarray(offset, offset + 1024), 64);
      socket.send(frame);
    }
    upload.sent = true;
  } catch (error) {
    if (sstvUpload !== upload) return;
    sstvUpload = null;
    renderSstvTxControls();
    report(error);
  }
};
$('sstv-mode').onchange = () => checked('sstv_mode', {mode: $('sstv-mode').value}).catch(error => {
  $('sstv-mode').value = state.sstv_mode ?? 'auto';
  report(error);
});
$('rtty-reverse').onchange = () => checked('rtty_reverse', {enabled: $('rtty-reverse').checked}).catch(error => {
  $('rtty-reverse').checked = !!state.rtty_reverse;
  report(error);
});
$('rtty-preset-edit').onclick = () => rttyPresets.dialog.showModal();
function saveRttyPresets() {
  writePreference('rttyPresets', rttyPresets.serialize());
  renderRttyTxControls();
}
rttyPresets.dialog.addEventListener('close', saveRttyPresets);
rttyPresets.select.addEventListener('change', saveRttyPresets);
$('rtty-tx-toggle').onclick = () => {
  if (rttyTxActive()) stopRttyTx();
  else beginRttyTx();
};
$('rtty-preset-send').onclick = () => {
  if (rttyTx?.live) {
    writeRttyText(rttyPresets.text);
    if (!rtty.tx.readOnly) rtty.tx.focus({preventScroll: true});
  } else {
    const {text} = rttyEndMarker(rttyPresets.text);
    if (text.trim()) beginRttyTx(text);
  }
};
const rttyLevel = $('rtty-tx-level');
rttyLevel.value = readPreference('rttyTxLevel') ?? rttyLevel.defaultValue;
function renderRttyLevel() {
  rangePaint(rttyLevel);
  $('rtty-tx-level-value').textContent = `${rttyLevel.value} %`;
}
rttyLevel.oninput = () => {
  renderRttyLevel();
  writePreference('rttyTxLevel', rttyLevel.value);
  if (rttyTx && !rttyTx.stopping) send({type: 'rtty_tx_level', tx_id: rttyTx.id, level: Number(rttyLevel.value)});
};
renderRttyLevel();
function waterfallTxActive() {
  return waterfallStartId !== null || (state.waterfall_tx?.phase ?? 'idle') !== 'idle';
}

function canStartWaterfallTx() {
  return waterfallRows.length && canControl() && state.waterfall_tx &&
    Number($('waterfall-tx-frequency').value.replace(',', '.')) === state.tx2?.frequency_hz / 1e6 &&
    !(state.flags & 6) && !state.loop?.mode && !wanted.has('tx2') && !waterfallTxActive() &&
    !sstvTxActive() && !rttyTxActive() && !ft8Pending && (state.ft8_tx?.phase ?? 'idle') === 'idle';
}

function renderWaterfallTxControls() {
  const tx = state.waterfall_tx;
  const busy = waterfallTxActive();
  $('waterfall-tx-text').disabled = busy;
  $('waterfall-tx-row').disabled = busy;
  $('waterfall-tx-level').disabled = busy;
  const frequencyInput = $('waterfall-tx-frequency');
  const hz = wanted.get('tx2')?.hz ?? state.tx2?.frequency_hz;
  frequencyInput.disabled = !canControl() || busy;
  if (document.activeElement !== frequencyInput) frequencyInput.value = Number.isSafeInteger(hz) ? (hz / 1e6).toFixed(6) : '';
  $('waterfall-tx-send').disabled = !canStartWaterfallTx();
  $('waterfall-tx-stop').disabled = !operator || !isOpen() || !busy || waterfallStopId !== null;

  const result = {
    complete: t('Gesendet.', 'Sent.'),
    stopped: t('Sendung gestoppt.', 'Transmission stopped.'),
    retuned: t('Sendung durch TX2-Abstimmung beendet.', 'Transmission ended after retuning TX2.'),
    unavailable: t('TX2 nicht verfügbar.', 'TX2 unavailable.'),
    underrun: t('TX2-Audiopuffer leer. Sendung beendet.', 'TX2 audio buffer underrun. Transmission ended.'),
    backend_reset: t('Backend neu gestartet. Sendung beendet.', 'Backend restarted. Transmission ended.'),
  }[tx?.result];
  const text = $('waterfall-tx-text').value;
  const invalid = waterfallFont && text.trim() && !waterfallRows.length;
  const status = waterfallError || (busy ? (tx?.phase === 'sending' ?
    t('Sendet über TX2 …', 'Transmitting on TX2 …') : t('TX2 wird vorbereitet …', 'Preparing TX2 …')) :
    invalid ? t('Erlaubt sind A–Z, 0–9, Leerzeichen und /-.?+_', 'Use A–Z, 0–9, spaces and /-.?+_') : result ?? '');
  const node = $('waterfall-tx-status');
  if (node.textContent !== status) node.textContent = status;
}

function renderWaterfallDraft() {
  const row = $('waterfall-tx-row'), level = $('waterfall-tx-level');
  const rowMs = Number(row.value);
  waterfallRows = waterfallFont ? waterfallRaster($('waterfall-tx-text').value, waterfallFont) : [];
  $('waterfall-tx-row-value').textContent = `${rowMs} ms`;
  $('waterfall-tx-level-value').textContent = `${level.value} %`;
  rangePaint(row); rangePaint(level);
  const seconds = waterfallRows.length * rowMs / 1000;
  $('waterfall-tx-duration').textContent = seconds ? `${seconds.toLocaleString(language, {maximumFractionDigits: 2})} s` : '';
  drawWaterfallPreview($('waterfall-tx-preview'), waterfallRows, rowMs);
  renderWaterfallTxControls();
}

async function loadWaterfallFont() {
  if (waterfallFont || waterfallFontLoading) return;
  waterfallFontLoading = true;
  try {
    const response = await fetch('./waterfall-font.json');
    if (!response.ok) throw new Error('Failed to load waterfall font');
    waterfallFont = await response.json();
    waterfallError = '';
  } catch {
    waterfallError = t('Waterfall-Schrift konnte nicht geladen werden.', 'The waterfall font could not be loaded.');
  } finally {
    waterfallFontLoading = false;
    renderWaterfallDraft();
  }
}

function stopWaterfallTx() {
  const id = waterfallStartId ?? state.waterfall_tx?.tx_id;
  if (!operator || !waterfallTxActive() || waterfallStopId !== null || id === undefined) return;
  if (send({type: 'waterfall_tx_stop', tx_id: id})) waterfallStopId = id;
  renderWaterfallTxControls();
}

$('waterfall-tx-form').onsubmit = async event => {
  event.preventDefault();
  if (!canStartWaterfallTx()) return;
  waterfallError = '';
  const start = command('waterfall_tx_start', {
    text: $('waterfall-tx-text').value,
    row_ms: Number($('waterfall-tx-row').value),
    level: Number($('waterfall-tx-level').value),
    frequency_hz: state.tx2.frequency_hz,
  });
  waterfallStartId = start.id;
  renderWaterfallTxControls();
  try {
    await start;
  } catch (error) {
    if (waterfallStartId === start.id) waterfallStartId = null;
    if (waterfallStopId === start.id) waterfallStopId = null;
    waterfallError = error.message;
    renderWaterfallTxControls();
  }
};
$('waterfall-tx-stop').onclick = stopWaterfallTx;
$('waterfall-tx-frequency').addEventListener('keydown', event => {
  if (event.key !== 'Enter') return;
  event.preventDefault();
  event.target.blur();
});
$('waterfall-tx-frequency').addEventListener('change', event => {
  waterfallError = '';
  try {
    const hz = parseMHz(event.target.value);
    if (hz !== state.tx2?.frequency_hz) setFrequency('tx', hz, 2);
  } catch (error) {
    waterfallError = error.message;
  }
  renderWaterfallTxControls();
});
$('waterfall-tx-frequency').addEventListener('input', renderWaterfallTxControls);
$('waterfall-tx-frequency').addEventListener('blur', renderWaterfallTxControls);
for (const field of ['text', 'row', 'level']) {
  const input = $('waterfall-tx-' + field);
  const saved = readPreference(input.id);
  if (saved !== null) input.value = field === 'text' ? saved.slice(0, input.maxLength) : saved;
  input.addEventListener('input', () => {
    writePreference(input.id, input.value);
    if (waterfallFont) waterfallError = '';
    renderWaterfallDraft();
  });
}
renderWaterfallDraft();

const savedZoomView = zoomSources.find(source => String(source) === readPreference('zoomView'));
selectDecoder(savedZoomView ?? (readPreference('decoderVisible') === 'true' ? decoderSource : 'zoom'));
$('loop').onchange = () => { stopPtt(); checked('loop', {mode: Number($('loop').value)}).catch(report); };
$('copy-rx-tx').onclick = () => { try { setFrequency('tx', frequency(selected) - DOWNLINK_UPLINK_HZ); } catch (error) { report(error); } };
$('copy-tx-rx').onclick = () => { try { setFrequency(selected, frequency('tx') + DOWNLINK_UPLINK_HZ); } catch (error) { report(error); } };
$('touch-together').onclick = () => { together = true; $('touch-together').classList.add('chosen'); $('touch-together').setAttribute('aria-pressed', 'true'); selectReceiver(selected); };
$('touch-rx').onclick = () => { together = false; $('touch-together').classList.remove('chosen'); $('touch-together').setAttribute('aria-pressed', 'false'); selectReceiver(selected); };
$('settings-form').onsubmit = async event => {
  event.preventDefault();
  const values = {};
  for (const field of document.querySelectorAll('[data-setting]')) if (String(field.value) !== String(settingsBase[field.dataset.setting] ?? '')) values[field.dataset.setting] = field.type === 'number' ? Number(field.value) : field.value;
  if (!Object.keys(values).length) { $('settings-message').textContent = 'Keine Änderungen.'; return; }
  stopPtt(); $('save').disabled = true; $('settings-message').textContent = 'Änderungen werden übernommen …'; $('settings-message').classList.remove('error');
  try { await saveSettings(values); settingsBase = {...state.settings}; $('settings-message').textContent = 'Einstellungen dauerhaft gespeichert.'; }
  catch (error) { $('settings-message').textContent = error.message; $('settings-message').classList.add('error'); }
  finally { controls(); }
};
$('frequency-form').onsubmit = event => {
  event.preventDefault();
  try {
    const hz = parseMHz($('frequency-input').value);
    if (editingFrequency === 'tx') setFrequency('tx', hz);
    else { selectReceiver(Number(editingFrequency)); const target = tuneTargets({selected, frequency: hz, rx: frequency(selected), tx: frequency('tx'), rxCenter: Number(state.settings.rx_center_hz), txCenter: Number(state.settings.tx_center_hz), together: true}); if (target.tx !== undefined) stopPtt(); setFrequency(selected, hz); if (target.tx !== undefined) setFrequency('tx', target.tx); }
    $('frequency-dialog').close();
  } catch (error) { $('frequency-error').textContent = error.message; $('frequency-error').classList.add('error'); }
};
$('ptt').onpointerdown = event => {
  if (event.button !== 0 || ptt.mode !== 'hold') return;
  event.preventDefault(); $('ptt').setPointerCapture(event.pointerId); ptt.press('pointer');
};
for (const event of ['pointerup', 'lostpointercapture']) $('ptt').addEventListener(event, () => ptt.release('pointer'));
$('ptt').addEventListener('pointercancel', stopPtt);
$('ptt').onclick = () => { if (ptt.mode === 'toggle') { ptt.press('click'); ptt.release('click'); } };
$('ptt').oncontextmenu = event => event.preventDefault();
window.addEventListener('keydown', event => {
  const space = event.code === 'Space' || event.key === ' ';
  const enter = event.key === 'Enter' && event.target === $('ptt');
  if (!space && !enter) return;
  if (event.target === $('ptt')) event.preventDefault();
  if (space && !ptt.spaceEnabled || event.altKey || event.ctrlKey || event.metaKey || document.querySelector('dialog[open]') || event.target?.isContentEditable || event.target?.closest?.('input,textarea,select,#sstv-tx-edit')) return;
  event.preventDefault();
  if (!event.repeat) ptt.press(space ? 'Space' : 'Enter');
});
window.addEventListener('keyup', event => {
  const source = event.code === 'Space' || event.key === ' ' ? 'Space' : event.key === 'Enter' ? 'Enter' : null;
  if (source && ptt.release(source)) event.preventDefault();
});
window.addEventListener('blur', stopPtt);
document.addEventListener('visibilitychange', () => { if (document.hidden) stopPtt(); });
window.addEventListener('pagehide', () => { stopPtt(); radio.stopMic(); socket?.close(); });
for (const event of ['pointermove', 'pointerdown', 'wheel', 'keydown', 'touchstart']) document.addEventListener(event, e => {
  if (!e.isTrusted || e.repeat || !operator || performance.now() - lastActivity < 1000) return;
  lastActivity = performance.now(); send({type: 'activity'});
}, {passive: true});
setInterval(() => send({type: 'heartbeat'}), 1000);
setInterval(() => {
  if (!ptt.checkTimeout() && operator && pttHeld) send({type: 'ptt_keepalive'});
  if (operator && rttyTx?.live && !rttyTx.stopping) send({type: 'rtty_tx_keepalive', tx_id: rttyTx.id});
  renderPttHint(); renderNoiseLevels(); renderFtSlotClock();
  if (ft8Visible() && state.ft8_tx?.phase === 'scheduled') renderFt8Status(state.ft8_tx);
  sendIdleFtCq();
}, 250);
$('theme').checked = window.QO100Theme.value === 'light';
$('theme').onchange = () => window.QO100Theme.set($('theme').checked ? 'light' : 'dark');
$('language').value = languagePreference();
$('language').onchange = () => {
  setLanguagePreference($('language').value);
  txVolumeChanged(false);
  sstv.render();
  views.forEach(view => view.setLanguage(language));
  renderWaterfallDraft();
  selectReceiver(selected); renderFilter(); renderNoiseLevels();
  setFullbandAxis(views[0].axisMode); controls();
};
applyLanguage(); views.forEach(view => view.setLanguage(language));
bindPlots(views[0], 250); bindPlots(views[1], 25);
bindFrequency(1, $('frequency1')); bindFrequency(2, $('frequency2')); bindFrequency('tx', $('frequencytx'));
const savedTxMonitorVolume = Number(readPreference('txMonitorVolume') ?? 0);
$('volume-tx').value = Number.isFinite(savedTxMonitorVolume) ? savedTxMonitorVolume : 0;
const savedTxMonitorPrevious = Number(readPreference('txMonitorPrevious') ?? 0);
txMonitorPrevious = Number.isFinite(savedTxMonitorPrevious) ? Math.max(0, Math.min(20, savedTxMonitorPrevious)) : 0;
txVolumeChanged(false); volumeChanged(false); renderFilter(); renderWaterfallSettings(); setSpectrumAverage(readPreference('spectrumAverage') ?? 1); setDisplayColor(readPreference('displayColor') ?? 'blue'); setFullbandAxis('bandplan'); selectReceiver(1); controls(); connect();
