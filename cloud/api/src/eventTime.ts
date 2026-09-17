/**
 * When did a door event happen?
 *
 * A door records every event with its own sequence -- (bootId, idx) -- and, if
 * its clock was trustworthy at that moment, the wall-clock time. The sequence is
 * always right. The time may be missing: a door has no battery-backed clock, so
 * a power loss resets it to 1970 and every event until the next NTP sync is
 * recorded with epoch = 0.
 *
 * THE BUG THIS REPLACES
 * Events with no clock were given (start of the door's CURRENT boot + uptime).
 * That is right for events from the current boot and wrong for every other one.
 * A door that spent sixteen days offline in one boot, then rebooted onto Wi-Fi,
 * uploaded that boot's events with the NEW boot's start time plus up to sixteen
 * days of the OLD boot's uptime. Taps that happened between Aug 30 and Sep 16
 * were stored as Sep 17 to Sep 29 -- in the future, some in the wrong monthly
 * partition, and so wrong in every report that read them.
 *
 * THE RULE NOW
 *   observed   epoch > 0                        the door's own clock
 *   derived    epoch = 0, the CURRENT boot,     start of this boot + uptime
 *              and that boot's start is known
 *   unknown    anything else                    no time is invented
 *
 * An unknown event keeps its place in the sequence and gets honest bounds: not
 * before the nearest earlier event whose time is known, and not after the
 * nearest later one -- nor after the moment it could last have happened, such
 * as the start of the boot that reported it. Its stored `at` is a PLACEMENT
 * inside that window so it can be partitioned and sorted. It is never a claim
 * about when the event happened, and never later than the backend received it.
 *
 * This module is pure: no storage, no clock of its own. That is deliberate --
 * it decides what an audit trail says happened, so it has to be testable
 * without anything else in the way (see tools/check-event-time.ts).
 */

export interface EventTimeInput {
  bootId: number;
  idx: number;
  /** Unix seconds from the door's clock, or 0 if it had no trusted clock. */
  epoch: number;
  uptimeMs: number;
}

export interface ResolvedTime {
  /** Where to store and sort the event. For an unknown event, a placement only. */
  atMs: number;
  /** Not read directly off the door's clock. */
  approx: boolean;
  /** No time could be established; only the bounds mean anything. */
  unknown: boolean;
  notBeforeMs?: number;
  notAfterMs?: number;
}

export function compareSequence(
  a: { bootId: number; idx: number },
  b: { bootId: number; idx: number }
): number {
  return a.bootId - b.bootId || a.idx - b.idx;
}

/** A derived time may run this far ahead of the backend's clock before it is disbelieved. */
const FUTURE_TOLERANCE_MS = 60_000;

export interface BoundItem {
  /** A time trusted for this item, or undefined when it is unknown. */
  knownMs?: number;
  /** The latest instant this item can possibly have happened. */
  ceilingMs: number;
}

export interface Bounded {
  atMs: number;
  notBeforeMs?: number;
  notAfterMs?: number;
}

/**
 * Bound and place the unknown items in a sequence.
 *
 * `items` MUST already be in sequence order. `anchorBeforeMs` is the latest
 * known time of anything earlier in the sequence than the first item.
 *
 * Unknowns are placed just before the next known time -- or their ceiling --
 * stepping back a millisecond each, so sequence order survives a sort by time
 * and no placement ever lands after its ceiling.
 */
export function boundUnknowns(items: BoundItem[], anchorBeforeMs?: number): Bounded[] {
  const out: Bounded[] = new Array(items.length);

  // Forward: the latest known time before each unknown.
  const floors: (number | undefined)[] = new Array(items.length);
  let lastKnown = anchorBeforeMs;
  for (let i = 0; i < items.length; i++) {
    const k = items[i]!.knownMs;
    if (k !== undefined) lastKnown = k;
    else floors[i] = lastKnown;
  }

  // Backward: the earliest known time after each unknown, capped by its ceiling.
  let nextKnown = Number.POSITIVE_INFINITY;
  let cursor = Number.POSITIVE_INFINITY;
  for (let i = items.length - 1; i >= 0; i--) {
    const it = items[i]!;
    if (it.knownMs !== undefined) {
      nextKnown = it.knownMs;
      cursor = it.knownMs;
      out[i] = { atMs: it.knownMs };
      continue;
    }

    const notAfter = Math.min(nextKnown, it.ceilingMs);
    let notBefore = floors[i];
    // Bounds that contradict each other mean some anchor's clock was wrong.
    // Keep the ceiling and drop the floor, rather than publish a window that
    // cannot contain anything.
    if (notBefore !== undefined && notBefore > notAfter) notBefore = undefined;

    let at = Math.min(notAfter, cursor - 1);
    if (notBefore !== undefined && at < notBefore) at = notBefore;
    cursor = at;
    out[i] = { atMs: at, notBeforeMs: notBefore, notAfterMs: notAfter };
  }
  return out;
}

export interface BatchContext {
  /** The boot the door is in now, as it reported it. */
  currentBootId: number;
  /**
   * When the current boot began, in unix ms, or undefined if the door has no
   * clock yet. Should be STABLE across the whole boot (see sync.ts): recomputed
   * per request it jitters by a second, which would move a derived event's
   * storage key between retries and duplicate it.
   */
  currentBootStartMs?: number;
  /** When the backend received the batch. */
  nowMs: number;
  /** Latest known time of anything earlier in sequence than this batch. */
  anchorBeforeMs?: number;
}

/** Resolve a batch of events from one door. Results are in input order. */
export function resolveBatch(events: EventTimeInput[], ctx: BatchContext): ResolvedTime[] {
  const order = events
    .map((_, i) => i)
    .sort((a, b) => compareSequence(events[a]!, events[b]!));

  const bootStart = ctx.currentBootStartMs;
  const kinds: ('observed' | 'derived' | 'unknown')[] = [];

  const items: BoundItem[] = order.map((i) => {
    const ev = events[i]!;

    // An event from an EARLIER boot happened before this boot began. That is a
    // real bound, tighter and more meaningful than the time of receipt.
    const ceilingMs = ev.bootId < ctx.currentBootId && bootStart !== undefined
      ? Math.min(bootStart, ctx.nowMs)
      : ctx.nowMs;

    if (ev.epoch > 0) {
      kinds.push('observed');
      return { knownMs: ev.epoch * 1000, ceilingMs };
    }

    // Only the CURRENT boot's start can date an event with no clock. Applying
    // it to an earlier boot is precisely the bug described above.
    if (ev.bootId === ctx.currentBootId && bootStart !== undefined) {
      const derived = bootStart + ev.uptimeMs;
      if (derived <= ctx.nowMs + FUTURE_TOLERANCE_MS) {
        kinds.push('derived');
        // NOT clamped to nowMs. A door's clock a few seconds ahead of the
        // backend's is ordinary skew, and clamping would make the stored time
        // depend on when the batch happened to arrive -- so a retry would land
        // on a different storage key and duplicate the event. A stable key is
        // worth far more than hiding a few seconds of skew.
        return { knownMs: derived, ceilingMs };
      }
    }

    kinds.push('unknown');
    return { knownMs: undefined, ceilingMs };
  });

  const bounded = boundUnknowns(items, ctx.anchorBeforeMs);

  const out: ResolvedTime[] = new Array(events.length);
  order.forEach((inputIndex, pos) => {
    const b = bounded[pos]!;
    const kind = kinds[pos]!;
    out[inputIndex] = {
      atMs: b.atMs,
      approx: kind !== 'observed',
      unknown: kind === 'unknown',
      notBeforeMs: kind === 'unknown' ? b.notBeforeMs : undefined,
      notAfterMs: kind === 'unknown' ? b.notAfterMs : undefined,
    };
  });
  return out;
}
