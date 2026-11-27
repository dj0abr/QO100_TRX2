import {countryFlagPosition, dxccForCall} from './dxcc.js';
import {language, text as t} from './language.js';

const $ = id => document.getElementById(id);
const labels = {
  'calling-cq': ['CQ rufen', 'Calling CQ'],
  report: ['Warte auf Rapport', 'Waiting for report'],
  'roger-report': ['Warte auf bestätigten Rapport', 'Waiting for roger report'],
  acknowledgment: ['Warte auf Bestätigung', 'Waiting for acknowledgment'],
  closing: ['QSO abschließen', 'Closing QSO'],
  'free-text': ['Freitext senden', 'Sending free text'],
};
const regionNames = Object.fromEntries(['de', 'en'].map(lang =>
  [lang, new Intl.DisplayNames(lang, {type: 'region', fallback: 'none'})]));

function utcLabel(ms) {
  return new Date(ms).toISOString().slice(11, 19) + (ms % 1000 ? '.5' : '');
}
function updateCountry(node) {
  const {region, entity} = node.dataset;
  node.querySelector('.ft-country-name').textContent =
    region === 'GB' ? 'UK' : (region && regionNames[language].of(region)) || entity;
}
function decodeFields(line, metadata, {text, source, destination, value}) {
  const words = text.split(' ');
  const index = words.findIndex((word, i) => i > 0 && word.replace(/^<|>$/g, '') === source);
  line.dataset.message = text;
  if (!source || !destination || index < 1) {
    line.textContent = metadata + text;
    return;
  }
  line.classList.add('ft-decode-fields');
  const info = document.createElement('span');
  info.className = 'ft-decode-info';
  info.textContent = metadata;
  const body = document.createElement('span');
  body.className = 'ft-message-body';
  const exchange = words.slice(index + 1).join(' ') || value || '';
  for (const columnText of [words.slice(0, index).join(' ') + ' ', words[index] + (exchange ? ' ' : ''), exchange]) {
    const column = document.createElement('span');
    column.textContent = columnText;
    body.append(column);
  }
  line.append(info, body);
  const location = dxccForCall(source);
  if (!location) return;
  const country = document.createElement('span');
  country.className = 'ft-country';
  country.dataset.region = location.code;
  country.dataset.entity = location.entity;
  country.title = location.entity;
  const position = countryFlagPosition(location.code);
  if (position) {
    const flag = document.createElement('span');
    flag.className = 'ft-country-flag';
    flag.setAttribute('aria-hidden', 'true');
    flag.style.backgroundPosition = position;
    country.append(flag);
  }
  const name = document.createElement('span');
  name.className = 'ft-country-name';
  country.append(name);
  updateCountry(country);
  line.append(country);
}

export function createFtPanel({request, readPreference, writePreference, tuneDefault}) {
  const panel = $('ft8-panel');
  const all = {output: $('ft8-log'), lines: [], follow: true};
  const own = {output: $('ft8-own-log'), lines: [], follow: true};
  const logs = [all, own];
  const mode = $('ft-mode'), parity = $('ft8-parity');
  const callsign = $('ft-own-call'), locator = $('ft-own-grid');
  const offset = $('ft8-audio-hz'), level = $('ft8-level');
  const settings = $('ft-settings'), save = $('ft-save');
  const arm = $('ft-arm'), automatic = $('ft-auto'), cq = $('ft-cq'), abort = $('ft8-stop');
  const cqOnly = $('ft-cq-only'), freeText = $('ft8-message'), send = $('ft8-send');
  const adif = $('ft-adif-status'), download = $('ft-adif-download');
  const status = $('ft8-status'), qso = $('ft-contact'), nextTx = $('ft-next-tx');
  let state = {}, radio = {}, clock, error = '', outcome = '', settingsDirty = false;
  let revision = 0;
  const pending = new Set();
  const busy = () => !!state.busy || radio.phase === 'scheduled' || radio.phase === 'sending' ||
    ['ft_cq', 'ft_free_text', 'ft_start_qso', 'ft_auto'].some(command => pending.has(command));

  function scroll(log) {
    if (log.frame) return;
    log.frame = requestAnimationFrame(() => {
      log.frame = 0;
      const {output} = log;
      const width = output.clientWidth, height = output.clientHeight;
      const resized = width !== log.width || height !== log.height;
      log.width = width;
      log.height = height;
      if (height && log.follow) output.scrollTop = output.scrollHeight - height;
      if (log.follow || resized) log.top = output.scrollTop;
    });
  }
  function follow(log) {
    log.follow = true;
    log.output.removeAttribute('data-ft-reading-history');
    scroll(log);
  }
  function scrollToEnd() { logs.forEach(follow); }
  function clear() {
    for (const log of logs) {
      log.lines.length = 0;
      log.output.textContent = '';
      follow(log);
    }
  }
  for (const log of logs) {
    const {output} = log;
    new ResizeObserver(([entry]) => {
      const width = entry.borderBoxSize[0].inlineSize;
      output.dataset.ftLayout = width < 510 ? 'stacked' : width < 670 ? 'compact' : 'wide';
      scroll(log);
    }).observe(output, {box: 'border-box'});
    output.addEventListener('scroll', () => {
      if (!log.height || log.height !== output.clientHeight || log.width !== output.clientWidth ||
          log.top === output.scrollTop) return;
      log.top = output.scrollTop;
      log.follow = output.scrollHeight - output.clientHeight - log.top < 2;
      output.toggleAttribute('data-ft-reading-history', !log.follow);
    }, {passive: true});
    document.querySelector('.ft-log-end[aria-controls="' + output.id + '"]').onclick = () => follow(log);
  }
  function append(log, line) {
    log.lines.push(line);
    log.output.append(line);
    if (log.lines.length > 50) log.lines.shift().remove();
    if (log.follow) scroll(log);
  }
  function filterCQ() {
    all.output.toggleAttribute('data-cq-only', cqOnly.checked);
    all.output.setAttribute('aria-label', cqOnly.checked ? t('FT4/8-CQ-Rufe', 'FT4/8 CQ calls') :
      t('Alle FT4/8-Nachrichten', 'All FT4/8 messages'));
    scroll(all);
  }
  cqOnly.checked = readPreference('ftCqOnly') === 'true';
  cqOnly.onchange = () => { writePreference('ftCqOnly', cqOnly.checked); filterCQ(); };

  const canStart = () => radio.operator && radio.active && state.armed && state.tx_ready &&
    !radio.tuning && !busy() && !pending.size;

  async function submit(command, fields = {}) {
    if (pending.has(command)) return;
    const current = revision;
    pending.add(command);
    error = outcome = '';
    render();
    try {
      const reply = await request(command, fields);
      if (current !== revision) return;
      if (command === 'ft_config') {
        settingsDirty = false;
        for (const [key, value] of Object.entries({ftOwnCall: callsign.value, ftOwnGrid: locator.value,
          ftAudioHz: offset.value, ftTxLevel: level.value})) writePreference(key, value);
      }
      return reply;
    } catch (failure) {
      if (current === revision) error = failure.message;
    } finally {
      if (current === revision) {
        pending.delete(command);
        render();
      }
    }
  }

  function renderClock() {
    if (!radio.visible) return;
    const label = $('ft-utc-clock'), progress = $('ft-slot-progress');
    const now = clock ? clock.utc + performance.now() - clock.at : 0;
    const utc = now ? new Date(now).toISOString().slice(11, 19) : '--:--:--';
    label.textContent = utc + ' UTC';
    progress.max = state.slot_ms || 15000;
    progress.value = now % progress.max;
    const seconds = state.armed && radio.phase !== 'sending' ? Math.ceil((state.next_tx_ms - now) / 1000) : 0;
    nextTx.textContent = now && seconds > 0 ? (state.next_tx_planned
      ? t('TX in ', 'TX in ') : t('TX frühestens in ', 'Earliest TX in ')) + seconds + ' s' : '';
  }
  function render() {
    const control = radio.operator && radio.active && state.available;
    const transmitting = radio.phase === 'sending';
    mode.value = state.mode || 'FT8';
    mode.disabled = !control || pending.size > 0;
    arm.checked = !!state.armed;
    arm.disabled = !control || pending.has('ft_arm') ||
      (!state.armed && (!state.tx_ready || settingsDirty || radio.tuning || pending.size > 0));
    automatic.value = state.auto || 'off';
    automatic.disabled = !control || !state.armed || pending.has('ft_auto');
    cq.disabled = send.disabled = parity.disabled = !canStart();
    abort.disabled = !control || pending.has('ft_abort') || (!busy() && (!state.auto || state.auto === 'off'));
    $('ft8-default-qrg').disabled = !control || radio.tuning || busy();
    const edit = control && !state.armed && !busy() && !pending.size;
    for (const field of [callsign, locator, offset, level, save]) field.disabled = !edit;
    freeText.disabled = !control || busy();
    for (const log of logs) for (const line of log.lines) if (line.tagName === 'BUTTON') line.disabled = !canStart();
    const qsoLabel = [state.peer, labels[state.state] ? t(...labels[state.state]) : '',
      state.retries ? t('Wiederholungen ', 'Retries ') + state.retries : ''].filter(Boolean).join(' · ');
    $('ft-qso-status').textContent = qsoLabel;
    qso.hidden = !qsoLabel && !state.next_tx_ms;
    status.textContent = error || (!state.available && radio.active ? t('FT4/8 an RX2 ist deaktiviert.', 'FT4/8 on RX2 is disabled.') :
      !state.configured && radio.operator && radio.active ? t('Für FT-TX Rufzeichen und QTH-Locator speichern.', 'Save your callsign and grid locator to enable FT TX.') :
      transmitting ? t('Sendet …', 'Transmitting …') : outcome);
    status.classList.toggle('error', !!error);
    status.classList.toggle('sending', transmitting && !error);
    adif.hidden = !state.log_enabled;
    download.disabled = !control || pending.has('ft_adif');
    renderClock();
  }
  function fillSettings() {
    const saved = state.configured;
    callsign.value = state.callsign || readPreference('ftOwnCall') || '';
    locator.value = state.locator || readPreference('ftOwnGrid') || '';
    offset.value = saved ? state.tx_hz : Math.max(200, Math.min(Number(offset.max), Number(readPreference('ftAudioHz')) || 1500));
    level.value = saved ? state.tx_level : readPreference('ftTxLevel') || 25;
  }
  function parityLabels() {
    const labels = state.mode === 'FT4' ?
      [':00 / :15 / :30 / :45', ':07.5 / :22.5 / :37.5 / :52.5'] : [':00 / :30', ':15 / :45'];
    const selected = parity.value;
    parity.replaceChildren(new Option(t('Automatisch', 'Automatic'), 'auto'),
      ...labels.map((label, index) => new Option(label, String(index))));
    parity.value = selected || 'auto';
  }

  function update(next, context) {
    if (radio.connected && !context.connected) reset();
    const refresh = next.callsign !== state.callsign || next.locator !== state.locator ||
      next.tx_hz !== state.tx_hz || next.tx_level !== state.tx_level || next.mode !== state.mode;
    if (context.session !== radio.session || next.receive_revision !== state.receive_revision) clear();
    if (next.utc_ms && next.utc_ms !== state.utc_ms) clock = {utc: next.utc_ms, at: performance.now()};
    const modeChanged = next.mode !== state.mode;
    state = next;
    radio = context;
    if (modeChanged) {
      offset.max = state.mode === 'FT4' ? 3930 : 3950;
      parityLabels();
    }
    if (refresh && !settingsDirty) fillSettings();
    render();
  }
  function reset() {
    ++revision;
    pending.clear();
    state = {};
    radio = {};
    clock = undefined;
    error = outcome = '';
    settingsDirty = false;
    $('ft-adif-message').textContent = '';
    $('ft-qrz-status').textContent = '';
    clear();
    fillSettings();
    render();
  }

  function message(row) {
    if (!radio.active) return;
    if (row.type === 'ft_notice') {
      const failed = !['completed', 'aborted'].includes(row.outcome);
      if (row.kind === 'adif' || row.kind === 'qrz') {
        const node = $(row.kind === 'adif' ? 'ft-adif-message' : 'ft-qrz-status');
        node.textContent = row.text;
        node.classList.toggle('setting-error', failed);
        if (row.kind === 'adif' && !failed) for (const line of all.lines) {
          if (line.dataset.cqCall === row.peer) line.removeAttribute('data-new-cq');
        }
      } else if (row.kind === 'slot') {
        outcome = t('Sendefenster verpasst.', 'Transmit slot missed.');
      } else if (row.outcome === 'clock-changed') {
        error = t('UTC-Uhr wurde verstellt.', 'UTC clock changed.');
      } else if (row.kind === 'error' || failed) {
        error = row.text;
      } else {
        error = '';
        outcome = row.kind === 'qso' && row.outcome === 'completed' ?
          [row.peer, t('QSO abgeschlossen', 'QSO completed')].filter(Boolean).join(' · ') : row.text;
      }
      render();
      return;
    }
    if (row.session !== radio.session || row.receive_revision !== state.receive_revision ||
        row.mode !== state.mode || !Number.isFinite(row.slot_utc_ms)) return;
    if (row.type === 'ft_tx') {
      const line = document.createElement('div');
      line.className = 'ft-decode-line ft-sent-line';
      line.textContent = utcLabel(row.slot_utc_ms) + ' TX  ' + Math.round(row.audio_hz) + ' Hz  ' + row.text;
      append(own, line);
      return;
    }
    const clickable = row.source && row.decode_id && row.source !== state.callsign;
    const line = document.createElement(clickable ? 'button' : 'div');
    line.className = 'ft-decode-line';
    line.toggleAttribute('data-ft-cq', row.cq === true);
    if (row.cq) {
      line.dataset.cqCall = row.source || '';
      line.toggleAttribute('data-new-cq', row.worked_recently === false);
    }
    const meta = utcLabel(row.slot_utc_ms) + ' ' + String(row.snr_db).padStart(3) + ' dB ' +
      row.dt_s.toFixed(2).padStart(5) + ' s ' + row.audio_hz.toFixed(0).padStart(4) + ' Hz  ';
    decodeFields(line, meta, row);
    if (clickable) {
      line.type = 'button';
      line.disabled = !canStart();
      line.onclick = () => { if (canStart()) submit('ft_start_qso', {decode_id: row.decode_id}); };
    }
    append(all, line);
    if (state.callsign && (row.source === state.callsign || row.destination === state.callsign)) {
      const copy = line.cloneNode(true);
      copy.onclick = line.onclick;
      copy.removeAttribute('data-new-cq');
      append(own, copy);
    }
  }

  settings.oninput = () => { settingsDirty = true; error = ''; render(); };
  settings.onsubmit = event => {
    event.preventDefault();
    submit('ft_config', {callsign: callsign.value, locator: locator.value, tx_hz: offset.valueAsNumber, tx_level: level.valueAsNumber});
  };
  mode.onchange = () => submit('ft_mode', {mode: mode.value});
  arm.onchange = () => submit('ft_arm', {armed: arm.checked});
  automatic.onchange = () => submit('ft_auto', {auto: automatic.value});
  cq.onclick = () => submit('ft_cq', parity.value === 'auto' ? {} : {parity: Number(parity.value)});
  abort.onclick = () => submit('ft_abort');
  $('ft8-tx-form').onsubmit = event => {
    event.preventDefault();
    if (canStart()) submit('ft_free_text', {text: freeText.value});
  };
  $('ft8-default-qrg').onclick = tuneDefault;
  download.onclick = async () => {
    const ticket = await submit('ft_adif');
    if (!ticket?.url) return;
    try {
      const response = await fetch(ticket.url);
      if (!response.ok) throw new Error(await response.text());
      const url = URL.createObjectURL(await response.blob());
      const link = document.createElement('a');
      link.href = url;
      link.download = 'ft4-ft8.adi';
      link.click();
      setTimeout(() => URL.revokeObjectURL(url), 30000);
    } catch (failure) {
      error = failure.message || t('ADIF-Download fehlgeschlagen.', 'ADIF download failed.');
      render();
    }
  };

  function languageChanged() {
    parityLabels();
    filterCQ();
    panel.querySelectorAll('.ft-country').forEach(updateCountry);
    render();
  }
  fillSettings();
  languageChanged();
  return {update, message, renderClock, languageChanged, clear, scrollToEnd, reset, busy};
}
