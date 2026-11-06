import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const source = await readFile(new URL('../web/ptt-control.js', import.meta.url), 'utf8');
const {PttControl, PTT_TIMEOUT_MS} = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'));
let now = 0, active = false, allowed = true, starts = 0, timeouts = 0, serial = 0;
const timers = new Map();
const ptt = new PttControl({
  start: () => { if (!allowed) return false; active = true; starts++; return true; },
  stop: () => { active = false; }, isActive: () => active, timeout: () => { timeouts++; },
  now: () => now, schedule: (fn, ms) => { const id = ++serial; timers.set(id, {fn, at: now + ms}); return id; },
  cancel: id => timers.delete(id),
});
assert.equal(PTT_TIMEOUT_MS, 300000);
ptt.press('Space');assert(!active, 'Space must be opt-in');
ptt.press('pointer');assert(active);assert.equal(timers.size, 0);
ptt.press('pointer');assert.equal(starts, 1, 'Repeated key/pointer down retriggered PTT');
ptt.release('pointer');assert(!active);
ptt.configure('hold', true);
ptt.press('pointer');ptt.press('Space');ptt.release('pointer');assert(active);
ptt.release('Space');assert(!active);
ptt.configure('toggle', true);
ptt.press('click');ptt.release('click');assert(active);assert.equal(ptt.remainingMs, 300000);
ptt.press('click');ptt.release('click');assert(!active);assert.equal(timers.size, 0);
ptt.press('Space');ptt.press('Space');ptt.release('Space');assert(active);
now += PTT_TIMEOUT_MS - 1;assert(!ptt.checkTimeout());assert(active);
// Key release and unrelated activity cannot extend the maximum TX duration.
ptt.release('Space');now++;assert(ptt.checkTimeout());assert(!active);assert.equal(timeouts, 1);
assert.equal(timers.size, 0);assert(!ptt.checkTimeout());
ptt.press('click');ptt.release('click');
const timer = [...timers.values()][0];now = timer.at;timer.fn();
assert(!active);assert.equal(timeouts, 2);
ptt.press('click');ptt.release('click');assert(active);
ptt.configure('hold', true);assert(!active);assert.equal(ptt.deadline, 0);
ptt.configure('toggle', true);allowed = false;ptt.press('click');ptt.release('click');
assert(!active);assert.equal(timers.size, 0, 'A refused start armed a timeout');
console.log('PTT: hold/toggle, opt-in Space, repeated keys, mixed inputs, cancellation and exact five-minute timeout passed.');
