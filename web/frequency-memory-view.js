import {MEMORY_LIMIT, FrequencyMemories, memoryMarkerGroups} from './frequency-memories.js';
import {frequencyText} from './tuning.js';
import {text as t, language} from './language.js';

const $ = id => document.getElementById(id);

export class FrequencyMemoryView {
  constructor({saved, persist, recall, notify}) {
    this.store = new FrequencyMemories(saved);
    this.persist = persist; this.recall = recall; this.notify = notify;
    this.strip = $('frequency-memory-strip'); this.saveButton = $('frequency-memory-save');
    this.picker = $('frequency-memory-picker'); this.list = $('frequency-memory-list');
    // Native button keys must not also reach the optional global Space-to-PTT binding.
    for (const node of [this.strip, this.saveButton, this.picker, $('frequency-memory-notice')])
      for (const type of ['keydown', 'keyup']) node.addEventListener(type, event => {
        if (event.code === 'Space' || event.key === ' ' || event.key === 'Enter') event.stopPropagation();
      });
    this.saveButton.onclick = () => this.save();
    $('frequency-memory-close').onclick = () => this.closePicker();
    $('frequency-memory-undo').onclick = () => {
      if (!this.undo) return;
      this.store = new FrequencyMemories(this.undo);
      this.changed();
    };
    this.observer = new ResizeObserver(() => this.render());
    this.observer.observe(this.strip);
    window.addEventListener('resize', () => this.closePicker());
  }
  update(context) {
    if (this.context?.channel !== context.channel || this.lang !== language) this.closePicker();
    this.context = context; this.lang = language; this.render();
  }
  changed() {
    this.undo = null; clearTimeout(this.undoTimer); $('frequency-memory-notice').hidden = true;
    this.closePicker(); this.persist(this.store.serialize()); this.render();
  }
  record(channel, target) {
    if (this.store.record(channel, target)) this.changed();
  }
  selection(channel) { return this.store.selection(channel); }
  refine(channel, ref, target, revision) {
    if (this.store.refine(channel, ref, target, revision)) this.changed();
  }
  select(channel, ref) {
    if (this.store.select(channel, ref)) this.changed();
  }
  label(entry) {
    const name = entry.kind === 'memories' ? 'Memory' : t('Verlauf', 'History');
    const active = this.store.isActive(this.context.channel, entry.kind, entry.index) ? t(' · aktiv für Zoom', ' · active for zoom') : '';
    return `${name} ${entry.index + 1} · RX${this.context.channel} ${frequencyText(entry.pair.rx)} MHz · TX${this.context.channel} ${frequencyText(entry.pair.tx)} MHz${active}`;
  }
  render() {
    if (!this.context) return;
    const {channel, pair, geometry, canSave, canRecall} = this.context;
    const count = this.store.receivers[channel].memories.filter(Boolean).length;
    this.saveButton.textContent = t(`Speichern RX${channel} · ${count}/${MEMORY_LIMIT}`, `Save RX${channel} · ${count}/${MEMORY_LIMIT}`);
    this.saveButton.disabled = !canSave;
    this.saveButton.title = t('Aktuelles RX/TX-Paar blau speichern und für die Zoom-Feinabstimmung aktivieren. Rot: Fullband-Ziele.', 'Save the current RX/TX pair in blue and activate it for zoom fine tuning. Red: fullband destinations.');
    this.strip.setAttribute('aria-label', t(`Frequenzspeicher RX${channel}: Rot = Verlauf, Blau = Memory`, `RX${channel} frequency memories: red = history, blue = memory`));
    this.strip.title = t('Rot: Verlauf · Blau: Memory · Linksklick: Abrufen · Rechtsklick: Löschen', 'Red: history · Blue: memory · Left click: recall · Right click: delete');
    const width = this.strip.clientWidth;
    const signature = JSON.stringify([channel, pair, geometry, width, canRecall, language, this.store.serialize()]);
    if (signature === this.signature) return;
    this.signature = signature;
    const groups = memoryMarkerGroups(this.store.entries(channel), geometry?.center, geometry?.span, width);
    this.strip.replaceChildren(...groups.map(group => {
      const button = document.createElement('button');
      const memory = group.entries.some(entry => entry.kind === 'memories');
      const history = group.entries.some(entry => entry.kind === 'history');
      const current = group.entries.some(entry => this.store.isActive(channel, entry.kind, entry.index));
      const halfWidth = (group.entries.at(-1).x - group.entries[0].x) / 2 + 9;
      const x = Math.max(halfWidth, Math.min(width - halfWidth, group.x));
      button.type = 'button'; button.className = 'frequency-memory-marker';
      button.dataset.kind = memory && history ? 'mixed' : memory ? 'memories' : 'history';
      button.classList.toggle('current', current);
      button.dataset.active = String(current);
      button.style.left = `${x}px`; button.style.width = `${2 * halfWidth}px`;
      button.title = group.entries.map(entry => this.label(entry)).join('\n') + '\n' + this.strip.title;
      button.setAttribute('aria-label', button.title);
      button.setAttribute('aria-disabled', String(!canRecall));
      button.setAttribute('aria-pressed', String(current));
      if (group.entries.length === 1) {
        button.dataset.rx = group.entries[0].pair.rx; button.dataset.tx = group.entries[0].pair.tx;
        button.dataset.index = group.entries[0].index;
      }
      for (const entry of group.entries) {
        const tick = document.createElement('i'); tick.className = `frequency-memory-tick ${entry.kind}`;
        // Offset each stroke to its real frequency, even within a combined click target.
        tick.style.left = `${entry.x - x + halfWidth}px`;
        button.append(tick);
      }
      if (group.entries.length > 1) {
        const badge = document.createElement('span'); badge.textContent = group.entries.length; button.append(badge);
      }
      button.onclick = () => group.entries.length > 1 ? this.choose(group.entries, button) : this.activate(group.entries[0]);
      button.oncontextmenu = event => {
        event.preventDefault();
        if (group.entries.length > 1) this.choose(group.entries, button);
        else this.remove(group.entries[0]);
      };
      return button;
    }));
    // Keep an open chooser usable when transmission/control availability changes.
    for (const button of this.list.querySelectorAll('[data-recall]')) button.disabled = !canRecall;
  }
  save(slot = null) {
    const {channel, pair, canSave} = this.context;
    if (!canSave) return;
    const snapshot = this.store.serialize();
    const result = this.store.save(channel, pair, slot);
    if (result === 'full') {
      this.openPicker(t('Memory ersetzen', 'Replace memory'), this.saveButton);
      const captured = {...pair};
      this.list.replaceChildren(...this.store.receivers[channel].memories.map((stored, index) => {
        const button = document.createElement('button'); button.type = 'button'; button.dataset.replace = index;
        button.textContent = this.label({kind: 'memories', index, pair: stored});
        button.onclick = () => {
          if (this.context.channel !== channel || !this.context.canSave) return;
          const before = this.store.serialize();
          this.store.save(channel, captured, index); this.changed();
          this.offerUndo(before, t('Memory ersetzt.', 'Memory replaced.'));
        };
        return button;
      }));
      this.focusPicker();
    } else if (result === 'saved') {
      this.changed();
      if (slot !== null) this.offerUndo(snapshot, t('Memory ersetzt.', 'Memory replaced.'));
      else this.notify(t('Frequenzpaar gespeichert.', 'Frequency pair saved.'));
    }
  }
  activate(entry) {
    if (!this.context.canRecall) return;
    this.closePicker();
    const ref = this.store.reference(this.context.channel, entry.kind, entry.index);
    this.recall(this.context.channel, {...entry.pair}, ref);
  }
  remove(entry) {
    const snapshot = this.store.serialize();
    if (!this.store.remove(this.context.channel, entry.kind, entry.index)) return;
    this.changed(); this.offerUndo(snapshot, t('Speicher gelöscht.', 'Memory deleted.'));
  }
  offerUndo(snapshot, message) {
    this.undo = snapshot;
    $('frequency-memory-message').textContent = message;
    $('frequency-memory-notice').hidden = false;
    this.undoTimer = setTimeout(() => { this.undo = null; $('frequency-memory-notice').hidden = true; }, 8000);
  }
  choose(entries, anchor) {
    this.openPicker(t('Frequenzpaar auswählen', 'Choose frequency pair'), anchor);
    this.list.replaceChildren(...entries.map(entry => {
      const row = document.createElement('div'); row.className = 'frequency-memory-choice';
      const recall = document.createElement('button'); recall.type = 'button'; recall.dataset.recall = '';
      recall.dataset.kind = entry.kind; recall.textContent = this.label(entry); recall.disabled = !this.context.canRecall;
      recall.dataset.index = entry.index;
      recall.onclick = () => this.activate(entry);
      recall.oncontextmenu = event => { event.preventDefault(); this.remove(entry); };
      const remove = document.createElement('button'); remove.type = 'button';
      remove.textContent = '×'; remove.title = t('Löschen', 'Delete'); remove.setAttribute('aria-label', `${remove.title}: ${this.label(entry)}`);
      remove.onclick = () => this.remove(entry);
      row.append(recall, remove); return row;
    }));
    this.focusPicker();
  }
  openPicker(title, anchor) {
    $('frequency-memory-picker-title').textContent = title;
    const bounds = anchor.getBoundingClientRect();
    this.picker.style.left = `${Math.max(8, Math.min(innerWidth - 568, bounds.left))}px`;
    this.picker.style.top = `${Math.max(8, Math.min(innerHeight - 200, bounds.bottom + 5))}px`;
    if (!this.picker.matches(':popover-open')) this.picker.showPopover();
  }
  focusPicker() { this.list.querySelector('button:not(:disabled)')?.focus(); }
  closePicker() { if (this.picker.matches(':popover-open')) this.picker.hidePopover(); }
}
