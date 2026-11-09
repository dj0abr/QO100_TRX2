import {DOWNLINK_UPLINK_HZ, frequencyText, parseMHz, validateFrequency, tuneTargets, wheelTurns, wheelTargets, filterValues} from './tuning.js';
import {FrameAssembler, RadioDisplay, displayOffsets, AXIS_MODES} from './radio-display.js';
import {RadioAudio} from './audio.js';
import {PttControl} from './ptt-control.js';
import {MmDiagnostics} from './mm-diagnostics.js';
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
  if (cursor?.view === view) queueCursorUpdate();
});
const radio = new RadioAudio(transmitSamples, mediaChanged);
const mmDiagnostics = new MmDiagnostics($('mm-diagnostics'));
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
  frames.reset(); views.forEach(view => view.clear()); radio.reset();
  cursor = null; queueCursorUpdate();
  const url = new URL('/ws', location.href); url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = socket = new WebSocket(url); ws.binaryType = 'arraybuffer';
  notify('Verbindung wird hergestellt …', false, 0); controls();
  ws.onopen = () => { if (ws === socket) { notify('Adapter verbunden. Warte auf Backend …', false, 0); send({type:'mm_diagnostics',enabled:!$('mm-diagnostics').hidden}); controls(); } };
  ws.onmessage = event => {
    if (ws !== socket) return;
    if (typeof event.data !== 'string') {
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
      if (wasOperator && !operator) { stopPtt(); radio.stopMic(); clearTimers(); }
      controls();
    } else if (message.type === 'state') update(message);
    else if (message.type === 'decoder_text' && [3, 4].includes(message.stream) && typeof message.text === 'string') appendDecoderText(message.stream, message.text);
    else if (message.type === 'decoder_text' && message.stream === 5 && typeof message.text === 'string' && ['dx-cluster', 'cw-skimmer', 'bulletin'].includes(message.channel)) appendDecoderText(`5-${message.channel}`, message.text, true);
    else if (message.type === 'mm_diagnostics' && message.stream === 5) mmDiagnostics.update(message);
    else if (message.type === 'ack' || message.type === 'error') {
      const item = pending.get(message.id);
      if (item) {
        pending.delete(message.id); clearTimeout(item.timeout);
        if (message.type === 'ack') item.resolve(message);
        else item.reject(fail(({read_only: 'Nur der Bediener darf diese Einstellung ändern.', not_ready: 'Backend noch nicht bereit.', busy: 'Einstellungen werden gerade verarbeitet.', backend_reset: 'Backend wurde neu gestartet.', timeout: 'Backend antwortet nicht.'})[message.code] || message.message, message.code));
      } else if (message.type === 'error' && message.code !== 'superseded' && message.code !== 'not_ready') notify(message.message, true);
    } else if (message.type === 'closed') {
      notify(message.reason === 'idle' ? 'Wegen Inaktivität abgemeldet. Mit „Verbinden“ erneut anmelden.' : 'Verbindung wegen fehlendem Heartbeat beendet. Bitte erneut verbinden.', true, Infinity);
    }
  };
  ws.onerror = () => { if (ws === socket) notify('WebSocket-Verbindung fehlgeschlagen.', true); };
  ws.onclose = event => {
    if (ws !== socket) return;
    stopPtt(); operator = false; state = {}; wanted.clear(); clearTimers(); radio.stopMic(); radio.reset(); mmDiagnostics.reset();
    for (const item of pending.values()) { clearTimeout(item.timeout); item.reject(fail('Verbindung getrennt.', 'closed')); }
    pending.clear(); frames.reset(); views.forEach(view => view.clear());
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
function setFrequency(channel, hz) {
  const tx = channel === 'tx', pair = selected, key = tx && pair === 2 ? 'tx2' : String(channel);
  validateFrequency(hz, Number(state.settings?.[tx ? 'tx_center_hz' : 'rx_center_hz']), tx ? 'TX' : `RX${channel}`);
  if (tx && pair === 1) stopPtt();
  const promise = command(tx ? 'tx' : 'tune', tx ? {frequency_hz: hz, ptt: false, channel: pair} : {stream: Number(channel), frequency_hz: hz});
  wanted.set(key, {hz, id: promise.id}); renderFrequencies();
  promise.then(answer => { if (answer.status !== 0) throw fail(`Frequenz wurde nicht übernommen (Status ${answer.status}).`); }).catch(error => {
    if (wanted.get(key)?.id === promise.id) { wanted.delete(key); renderFrequencies(); report(error); }
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
  $('zoom-mobile-caption').textContent = `${rx}-Zoom · 50 kHz`;
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
  if (next.session !== session) {
    stopPtt(); session = next.session; wanted.clear(); guiLoaded = false;
    loopGeneration = null; cutoff = 0n; clearTimers(); frames.reset(); views.forEach(view => view.clear()); radio.reset();
  }
  if (next.loop?.generation !== undefined && next.loop.generation !== loopGeneration) {
    if (loopGeneration !== null) cutoff = BigInt(next.loop.timestamp_ns || '0');
    loopGeneration = next.loop.generation; frames.reset(); views.forEach(view => view.clear()); radio.reset();
  }
  if ((next.flags & 6) && !(state.flags & 6)) radio.reset();
  state = next;
  for (const [key, value] of wanted) {
    const actual = key === 'tx2' ? next.tx2?.frequency_hz : key === 'tx' ? next.tx?.frequency_hz : next.rx?.[key]?.frequency_hz;
    if (actual === value.hz) wanted.delete(key);
  }
  if (!guiLoaded && next.settings?.gui_audio_gain !== undefined) {
    guiLoaded = true; $('volume').value = next.settings.gui_audio_gain; volumeChanged(false);
  }
  if (!canControl() || (next.flags & 6) || !radio.microphone || !radio.running || (pttAcked && !next.tx?.ptt)) stopPtt();
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
function controls() {
  const writable = canControl();
  for (const element of document.querySelectorAll('[data-control]')) element.disabled = !writable;
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
  $('tx-status').textContent = transmitting ? (state.loop?.mode === 1 ? t('Audio-Test', 'Audio test') : t('Senden', 'Transmitting')) : state.loop?.mode ? t(`Empfang · ${state.loop.mode === 1 ? 'Audio' : 'HF'}-Loop`, `Receiving · ${state.loop.mode === 1 ? 'Audio' : 'RF'} loop`) : t('Empfang', 'Receiving');
  if (selected === 2) $('tx-status').textContent = 'RX2 / TX2 · Digital';
  $('tx-dot').className = `live-dot${transmitting ? ' sending' : isOpen() && state.ready ? '' : ' offline'}`;
  $('ptt').classList.toggle('sending', transmitting); $('ptt').setAttribute('aria-pressed', String(transmitting));
  applyMonitorGain();
  renderPttHint();
  $('audio-details').textContent = radio.starting ? t('Audio wird gestartet …', 'Starting audio …') : radio.micBusy ? t('Mikrofon wird geöffnet …', 'Opening microphone …') : radio.running ? t(`Audio aktiv · Gerät ${radio.context.sampleRate / 1000} kHz · Funk 12 kHz · Mikrofon ${radio.microphone ? 'aktiv' : 'aus'}`, `Audio active · Device ${radio.context.sampleRate / 1000} kHz · Radio 12 kHz · Microphone ${radio.microphone ? 'active' : 'off'}`) : t('Audio ausgeschaltet', 'Audio off');
  $('audio-message').hidden = !(radio.error || radio.warning);
  $('audio-message').textContent = radio.error || radio.warning;
  $('audio-message').classList.toggle('warning', !radio.error && !!radio.warning);
  renderDevices();
  if (performance.now() >= noticeUntil) {
    const error = !!(state.flags & 4);
    const message = !isOpen() ? 'Getrennt. Zum Anmelden „Verbinden“ wählen.' : !state.ready ? 'Warte auf Backend und bestätigte Abschaltung …' : error ? `Hardware offline: ${state.settings?.hardware_error || state.settings?.error || 'Verbindung prüfen'}` : !operator ? 'Zuschauer · Empfang aktiv. Für Steuerrecht bei freiem Platz neu verbinden.' : selected === 2 ? (radio.running ? t('Bedienbereit · RX2-Audiokontrolle aktiv. TX2 erhält externes Audio.', 'Ready · RX2 audio monitoring active. TX2 receives external audio.') : t('Bedienbereit · Audio starten, um RX2 zu hören. TX2 erhält externes Audio.', 'Ready · Start audio to listen to RX2. TX2 receives external audio.')) : radio.running && radio.microphone ? 'Bedienbereit · Audio und Mikrofon aktiv.' : 'Bedienbereit · Audio starten und Mikrofon freigeben, um PTT zu verwenden.';
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
  if (!radio.microphone || !radio.running) stopPtt();
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
const zoomSources = ['zoom', ...decoderSources, '5-diagnostics'];
let decoderSource = decoderSources.find(source => String(source) === readPreference('decoderSource')) ?? 3;
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
  const monitoring = source !== 'zoom';
  const diagnostics = source === '5-diagnostics';
  if (monitoring && !diagnostics) decoderSource = source;
  for (const channel of decoderSources) {
    decoderOutput(channel).hidden = channel !== decoderSource;
    $(`decoder-source${channel}`).checked = channel === source;
  }
  $('decoder-sourcezoom').checked = !monitoring;
  $('decoder-source5-diagnostics').checked = diagnostics;
  $('decoder-panel').hidden = !monitoring || diagnostics;
  $('decoder-clear').hidden = !monitoring || diagnostics;
  mmDiagnostics.show(diagnostics);
  send({type:'mm_diagnostics',enabled:diagnostics});
  $('zoom-plots').setAttribute('aria-hidden', String(monitoring));
  $('zoom-plots').inert = monitoring;
  $('zoom-output').dataset.view = String(source);
  writePreference('zoomView', source);
  if (monitoring && !diagnostics) { writePreference('decoderSource', source); restoreDecoderScroll(); }
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
$('decoder-clear').onclick = () => { decoderPending[decoderSource] = null; decoderOutput(decoderSource).textContent = ''; decoderScroll[decoderSource] = {top: 0, follow: decoderSource !== '5-bulletin'}; };
for (const source of zoomSources) $(`decoder-source${source}`).onchange = () => selectDecoder(source);
// Keep local decoder controls and log scrolling independent of global Space PTT.
for (const id of ['zoom-sources', 'decoder-clear', 'decoder-panel', 'mm-diagnostics']) $(id).addEventListener('keydown', event => {
  if (event.code === 'Space' || event.key === ' ') event.stopPropagation();
});
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
  if (space && !ptt.spaceEnabled || event.altKey || event.ctrlKey || event.metaKey || document.querySelector('dialog[open]') || event.target?.isContentEditable || event.target?.closest?.('input,textarea,select')) return;
  event.preventDefault(); if (!event.repeat) ptt.press(space ? 'Space' : 'Enter');
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
setInterval(() => { if (!ptt.checkTimeout() && pttHeld && operator) send({type: 'ptt_keepalive'}); renderPttHint(); renderNoiseLevels(); }, 250);
$('language').value = languagePreference();
$('language').onchange = () => {
  setLanguagePreference($('language').value);
  txVolumeChanged(false);
  views.forEach(view => view.setLanguage(language));
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
