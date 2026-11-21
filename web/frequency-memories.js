export const MEMORY_LIMIT = 3;

export function validPair(pair) {
  return pair && Number.isSafeInteger(pair.rx) && pair.rx > 0 && Number.isSafeInteger(pair.tx) && pair.tx > 0;
}

export function samePair(a, b) { return !!a && !!b && a.rx === b.rx && a.tx === b.tx; }
const copyPair = pair => ({rx: pair.rx, tx: pair.tx});

// Red entries remember fullband destinations; blue entries have stable slots.
// References retain their identity while zoom tuning edits the active pair.
export class FrequencyMemories {
  constructor(saved) {
    let source;
    try { source = JSON.parse(saved); } catch {}
    this.receivers = {}; this.active = {1: null, 2: null};
    for (const channel of [1, 2]) {
      const data = source?.version === 1 ? source.receivers?.[channel] : null;
      const memories = Array.from({length: MEMORY_LIMIT}, (_, slot) => validPair(data?.memories?.[slot]) ? copyPair(data.memories[slot]) : null);
      const history = [];
      for (const pair of Array.isArray(data?.history) ? data.history : []) {
        if (validPair(pair) && !history.some(item => samePair(item, pair))) history.push(copyPair(pair));
        if (history.length === MEMORY_LIMIT) break;
      }
      this.receivers[channel] = {history, memories};
      const active = source?.version === 1 ? source.active?.[channel] : null;
      if (active) this.select(channel, this.reference(channel, active.kind, active.index));
    }
  }
  serialize() {
    const active = {};
    for (const channel of [1, 2]) {
      const ref = this.selection(channel);
      active[channel] = ref ? {kind: ref.kind, index: this.receivers[channel][ref.kind].indexOf(ref.pair)} : null;
    }
    return JSON.stringify({version: 1, receivers: this.receivers, active});
  }
  reference(channel, kind, index) {
    const pair = ['history', 'memories'].includes(kind) && this.receivers[channel][kind][index];
    return pair ? {kind, pair} : null;
  }
  select(channel, ref) {
    if (!ref || !['history', 'memories'].includes(ref.kind) || !this.receivers[channel][ref.kind].includes(ref.pair)) return false;
    const previous = this.active[channel];
    if (previous?.kind !== ref.kind || previous.pair !== ref.pair) this.active[channel] = ref;
    return true;
  }
  selection(channel) {
    const ref = this.active[channel];
    if (ref && !this.receivers[channel][ref.kind].includes(ref.pair)) this.active[channel] = null;
    return this.active[channel];
  }
  isActive(channel, kind, index) {
    const ref = this.selection(channel);
    return !!ref && ref.kind === kind && ref.pair === this.receivers[channel][kind][index];
  }
  save(channel, pair, slot = null) {
    if (!validPair(pair)) return 'invalid';
    const memories = this.receivers[channel].memories;
    if (slot === null) slot = memories.indexOf(null);
    if (!Number.isInteger(slot) || slot < 0 || slot >= MEMORY_LIMIT) return 'full';
    memories[slot] = copyPair(pair);
    this.select(channel, this.reference(channel, 'memories', slot));
    return 'saved';
  }
  record(channel, target) {
    if (!validPair(target)) return false;
    const receiver = this.receivers[channel];
    const pair = receiver.history.find(item => samePair(item, target)) ?? copyPair(target);
    receiver.history = [pair, ...receiver.history.filter(item => item !== pair)].slice(0, MEMORY_LIMIT);
    this.select(channel, this.reference(channel, 'history', 0));
    return true;
  }
  refine(channel, ref, target, revision = 0) {
    if (!ref || this.selection(channel) !== ref || !validPair(target) || revision < (ref.revision ?? 0)) return false;
    ref.revision = revision;
    if (samePair(ref.pair, target)) return false;
    Object.assign(ref.pair, copyPair(target));
    // Keep the edited red entry when it converges with an older one. Blue slots stay stable.
    if (ref.kind === 'history') this.receivers[channel].history = this.receivers[channel].history.filter(pair => pair === ref.pair || !samePair(pair, target));
    return true;
  }
  remove(channel, kind, index) {
    const receiver = this.receivers[channel];
    if (!['history', 'memories'].includes(kind) || !receiver[kind][index]) return false;
    if (this.isActive(channel, kind, index)) this.active[channel] = null;
    if (kind === 'history') receiver.history.splice(index, 1);
    else receiver.memories[index] = null;
    return true;
  }
  entries(channel) {
    const {history, memories} = this.receivers[channel];
    return [
      ...memories.flatMap((pair, index) => pair ? [{kind: 'memories', index, pair}] : []),
      ...history.map((pair, index) => ({kind: 'history', index, pair})),
    ];
  }
}

// Group overlapping click targets, keeping each frequency's exact position in the group.
export function memoryMarkerGroups(entries, center, span, width, hitWidth = 18) {
  if (!Number.isFinite(center) || !(span > 0) || !(width > 0)) return [];
  const markers = entries.map(entry => ({...entry, x: ((entry.pair.rx - center) / span + .5) * width}))
    .filter(entry => entry.x >= 0 && entry.x <= width).sort((a, b) => a.x - b.x);
  const groups = [];
  for (const entry of markers) {
    let group = groups.at(-1);
    if (!group || entry.x - group.entries.at(-1).x >= hitWidth) {
      group = {x: entry.x, entries: []}; groups.push(group);
    }
    group.entries.push(entry);
    group.x = (group.entries[0].x + entry.x) / 2;
  }
  return groups;
}
