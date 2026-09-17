/**
 * Checks for eventTime.ts -- the logic that decides when an audited event
 * happened.
 *
 *   npm run build && npm run check-event-time
 *
 * Scenario 3 is the real one: Front Door's outage of Aug 30 - Sep 16 2026, where
 * the old rule stored sixteen days of taps in the future. Everything else pins a
 * property the fix depends on. Exits non-zero on any failure.
 */

import { resolveBatch, EventTimeInput, ResolvedTime } from '../eventTime';

let failures = 0;
function check(name: string, ok: boolean, detail = ''): void {
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}${ok || !detail ? '' : `\n        ${detail}`}`);
  if (!ok) failures++;
}

const T = (iso: string) => Date.parse(iso);
const DAY = 86_400_000;
const iso = (ms?: number) => (ms === undefined ? 'undefined' : new Date(ms).toISOString());

function inOrder(events: EventTimeInput[], out: ResolvedTime[]): boolean {
  const idx = events.map((_, i) => i).sort((a, b) =>
    events[a]!.bootId - events[b]!.bootId || events[a]!.idx - events[b]!.idx);
  for (let k = 1; k < idx.length; k++) {
    if (out[idx[k]!]!.atMs < out[idx[k - 1]!]!.atMs) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
console.log('\n1. observed times are left exactly as the door recorded them');
{
  const ev = [{ bootId: 5, idx: 3, epoch: 1_788_000_000, uptimeMs: 999 }];
  const r = resolveBatch(ev, { currentBootId: 5, currentBootStartMs: 1, nowMs: T('2026-09-16T00:00:00Z') })[0]!;
  check('atMs = epoch * 1000', r.atMs === 1_788_000_000_000);
  check('not approx, not unknown', !r.approx && !r.unknown);
}

// ---------------------------------------------------------------------------
console.log('\n2. no clock, CURRENT boot: derived from this boot\'s start');
{
  const start = T('2026-09-16T20:00:00Z');
  const ev = [{ bootId: 9, idx: 0, epoch: 0, uptimeMs: 5 * 60_000 }];
  const r = resolveBatch(ev, { currentBootId: 9, currentBootStartMs: start, nowMs: T('2026-09-16T21:00:00Z') })[0]!;
  check('atMs = boot start + uptime', r.atMs === start + 5 * 60_000);
  check('approx, but not unknown', r.approx && !r.unknown);
}

// ---------------------------------------------------------------------------
console.log('\n3. Front Door, Aug 30 - Sep 16: earlier boots with no clock');
{
  const lastKnownTap = T('2026-08-29T14:53:52Z');   // boot 15, observed
  const boot18Start  = T('2026-09-16T20:41:40Z');
  const now          = T('2026-09-16T20:42:10Z');

  const ev: EventTimeInput[] = [
    { bootId: 16, idx: 0, epoch: 0, uptimeMs: 0 },           // boot
    { bootId: 16, idx: 1, epoch: 0, uptimeMs: 1 * DAY },     // taps days into the outage
    { bootId: 16, idx: 2, epoch: 0, uptimeMs: 7 * DAY },
    { bootId: 16, idx: 3, epoch: 0, uptimeMs: 15 * DAY },
    { bootId: 17, idx: 0, epoch: 0, uptimeMs: 0 },           // boot
    { bootId: 17, idx: 1, epoch: 0, uptimeMs: 2 * DAY },
    { bootId: 18, idx: 0, epoch: 0, uptimeMs: 0 },           // current boot's own boot event
  ];

  // What the OLD rule produced, to show the test reproduces the bug.
  const oldWorst = boot18Start + 15 * DAY;
  check('old rule put a tap in the future (bug reproduced)', oldWorst > now, iso(oldWorst));

  const out = resolveBatch(ev, {
    currentBootId: 18, currentBootStartMs: boot18Start, nowMs: now, anchorBeforeMs: lastKnownTap,
  });

  check('no event is placed in the future', out.every((r) => r.atMs <= now),
        out.map((r) => iso(r.atMs)).join(', '));
  check('boots 16 and 17 are unknown', out.slice(0, 6).every((r) => r.unknown));
  check('boot 18 boot event is derived, not unknown', !out[6]!.unknown && out[6]!.atMs === boot18Start);
  check('unknowns are floored at the last known tap',
        out.slice(0, 6).every((r) => r.notBeforeMs === lastKnownTap));
  check('unknowns are capped at the start of boot 18',
        out.slice(0, 6).every((r) => r.notAfterMs === boot18Start));
  check('every placement sits inside its own window',
        out.slice(0, 6).every((r) => r.atMs >= r.notBeforeMs! && r.atMs <= r.notAfterMs!));
  check('sequence order survives a sort by time', inOrder(ev, out));
}

// ---------------------------------------------------------------------------
console.log('\n4. retries resolve identically (so storage keys cannot duplicate)');
{
  const start = T('2026-09-16T20:41:40Z');
  const ev: EventTimeInput[] = [
    { bootId: 16, idx: 4, epoch: 0, uptimeMs: 3 * DAY },
    { bootId: 18, idx: 0, epoch: 0, uptimeMs: 0 },
    { bootId: 18, idx: 1, epoch: 0, uptimeMs: 90_000 },
  ];
  const a = resolveBatch(ev, { currentBootId: 18, currentBootStartMs: start, nowMs: T('2026-09-16T20:43:00Z') });
  const b = resolveBatch(ev, { currentBootId: 18, currentBootStartMs: start, nowMs: T('2026-09-16T20:43:30Z') });
  check('same batch 30 s later gives the same atMs for every event',
        a.every((r, i) => r.atMs === b[i]!.atMs),
        `${a.map((r) => r.atMs)} vs ${b.map((r) => r.atMs)}`);
}

// ---------------------------------------------------------------------------
console.log('\n5. unknown between two observed events is bounded by both');
{
  const before = T('2026-09-01T10:00:00Z');
  const after  = T('2026-09-01T12:00:00Z');
  const ev: EventTimeInput[] = [
    { bootId: 3, idx: 0, epoch: before / 1000, uptimeMs: 0 },
    { bootId: 3, idx: 1, epoch: 0, uptimeMs: 0 },            // clock lost mid-sequence
    { bootId: 4, idx: 0, epoch: after / 1000, uptimeMs: 0 },
  ];
  const r = resolveBatch(ev, { currentBootId: 4, currentBootStartMs: after, nowMs: T('2026-09-01T13:00:00Z') })[1]!;
  check('unknown', r.unknown);
  check('not before the earlier observed event', r.notBeforeMs === before, iso(r.notBeforeMs));
  check('not after the later observed event', r.notAfterMs === after, iso(r.notAfterMs));
}

// ---------------------------------------------------------------------------
console.log('\n6. nothing known before: no floor is invented');
{
  const now = T('2026-09-16T12:00:00Z');
  const r = resolveBatch([{ bootId: 2, idx: 0, epoch: 0, uptimeMs: 1000 }],
                         { currentBootId: 3, currentBootStartMs: T('2026-09-16T11:00:00Z'), nowMs: now })[0]!;
  check('notBefore is undefined', r.notBeforeMs === undefined);
  check('placed no later than the start of the reporting boot', r.atMs <= T('2026-09-16T11:00:00Z'));
}

// ---------------------------------------------------------------------------
console.log('\n7. contradictory bounds drop the floor rather than publish an empty window');
{
  const ceiling = T('2026-09-10T00:00:00Z');
  const r = resolveBatch([{ bootId: 2, idx: 0, epoch: 0, uptimeMs: 0 }], {
    currentBootId: 3, currentBootStartMs: ceiling, nowMs: T('2026-09-16T00:00:00Z'),
    anchorBeforeMs: T('2026-09-12T00:00:00Z'),                // "later" than the ceiling
  })[0]!;
  check('floor dropped', r.notBeforeMs === undefined, iso(r.notBeforeMs));
  check('ceiling kept', r.notAfterMs === ceiling);
}

// ---------------------------------------------------------------------------
console.log('\n8. a derived time in the future is disbelieved, not stored');
{
  const now = T('2026-09-16T12:00:00Z');
  const r = resolveBatch([{ bootId: 7, idx: 0, epoch: 0, uptimeMs: 30 * DAY }],
                         { currentBootId: 7, currentBootStartMs: T('2026-09-16T11:00:00Z'), nowMs: now })[0]!;
  check('treated as unknown', r.unknown);
  check('not placed in the future', r.atMs <= now, iso(r.atMs));
}

// ---------------------------------------------------------------------------
console.log('\n9. door has no clock yet: current-boot events are unknown too');
{
  const now = T('2026-09-16T12:00:00Z');
  const r = resolveBatch([{ bootId: 7, idx: 2, epoch: 0, uptimeMs: 4000 }],
                         { currentBootId: 7, currentBootStartMs: undefined, nowMs: now })[0]!;
  check('unknown', r.unknown);
  check('capped at receipt', r.notAfterMs === now && r.atMs <= now);
}

// ---------------------------------------------------------------------------
console.log('\n10. results come back in INPUT order, whatever order the batch arrived in');
{
  const start = T('2026-09-16T10:00:00Z');
  const ev: EventTimeInput[] = [
    { bootId: 5, idx: 2, epoch: 0, uptimeMs: 2000 },
    { bootId: 5, idx: 0, epoch: 0, uptimeMs: 0 },
    { bootId: 5, idx: 1, epoch: 0, uptimeMs: 1000 },
  ];
  const out = resolveBatch(ev, { currentBootId: 5, currentBootStartMs: start, nowMs: T('2026-09-16T11:00:00Z') });
  check('each result matches its own input', out[0]!.atMs === start + 2000 &&
        out[1]!.atMs === start && out[2]!.atMs === start + 1000);
}

console.log(failures === 0 ? '\nRESULT: ALL PASS\n' : `\nRESULT: ${failures} FAILURE(S)\n`);
process.exit(failures === 0 ? 0 : 1);
