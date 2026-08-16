/**
 * Reports — the read side. Nothing here mutates anything, so every route is
 * Viewer-level.
 *
 * WHY THE QUERIES LOOK LIKE THIS
 * Table Storage has exactly one index (PartitionKey + RowKey). Every report
 * below is therefore a *partition scan*, never a table scan, which is the whole
 * reason events are written twice under two different partition schemes:
 *
 *   EventsByPerson   {personId}-{yyyyMM}   "where has this person been"
 *   EventsByDoor     {deviceId}-{yyyyMM}   "who came through this door"
 *
 * Row keys begin with an inverted timestamp, so a partition already returns
 * newest-first with no sorting, and a time window becomes a RowKey range rather
 * than a filter applied after reading everything.
 *
 * A range spanning several months fans out across one partition per month and
 * merges. That is bounded and predictable; it is also the point at which, if you
 * ever wanted open-ended analytics across the whole fleet, you would export
 * rather than contort this schema further.
 */

import { app, HttpRequest, HttpResponseInit, InvocationContext } from '@azure/functions';
import { TableClient, odata } from '@azure/data-tables';
import { DefaultAzureCredential } from '@azure/identity';
import { requireRole, isDenied } from '../adminAuth';
import { invertedTs, monthKey } from '../storage';
import { EventType } from '../../../shared/types';

const account = process.env.STORAGE_ACCOUNT_NAME!;
const endpoint = `https://${account}.table.core.windows.net`;
const credential = new DefaultAzureCredential();
const t = (name: string) => new TableClient(endpoint, name, credential);

const bad = (error: string): HttpResponseInit => ({ status: 400, jsonBody: { error } });

/** Default window when none is given: a week is the usual "what happened lately". */
const DEFAULT_DAYS = 7;
const MAX_ROWS = 500;

interface Range {
  from: Date;
  to: Date;
  months: string[];
}

function parseRange(req: HttpRequest): Range | string {
  const toRaw = req.query.get('to');
  const fromRaw = req.query.get('from');
  const to = toRaw ? new Date(toRaw) : new Date();
  if (isNaN(to.getTime())) return 'invalid "to" date';
  const from = fromRaw
    ? new Date(fromRaw)
    : new Date(to.getTime() - DEFAULT_DAYS * 86400000);
  if (isNaN(from.getTime())) return 'invalid "from" date';
  if (from > to) return '"from" is after "to"';

  // One partition per calendar month the window touches.
  const months: string[] = [];
  const cursor = new Date(Date.UTC(from.getUTCFullYear(), from.getUTCMonth(), 1));
  while (cursor <= to) {
    months.push(monthKey(cursor));
    cursor.setUTCMonth(cursor.getUTCMonth() + 1);
  }
  return { from, to, months };
}

interface Row {
  at: string;
  deviceId: string;
  doorName: string;
  personId?: string;
  personName?: string;
  cred: string;
  type: number;
  reason: number;
  granted: boolean;
  timeApprox: boolean;
}

function toRow(e: any): Row {
  return {
    at: e.at,
    deviceId: e.deviceId,
    doorName: e.doorName,
    personId: e.personId || undefined,
    personName: e.personName || undefined,
    cred: e.cred ?? '',
    type: e.type,
    reason: e.reason,
    granted: e.granted === true,
    timeApprox: e.timeApprox === true,
  };
}

/**
 * Scan one partition per month for the window, newest-first.
 *
 * The RowKey range does the time filtering server-side. Because the key begins
 * with (MAX - timestamp), a LATER time produces a SMALLER key — hence `to` is
 * the lower bound and `from` the upper, which reads backwards but is correct.
 */
async function scan(
  table: string,
  partitionFor: (month: string) => string,
  range: Range,
  limit: number
): Promise<Row[]> {
  const lo = invertedTs(range.to.getTime());
  const hi = invertedTs(range.from.getTime());
  const out: Row[] = [];

  for (const month of range.months) {
    const pk = partitionFor(month);
    for await (const e of t(table).listEntities<any>({
      queryOptions: {
        filter: odata`PartitionKey eq ${pk} and RowKey ge ${lo} and RowKey le ${hi}`,
      },
    })) {
      out.push(toRow(e));
      if (out.length >= limit * range.months.length) break;
    }
  }

  // Each partition arrives newest-first, but several partitions interleave.
  out.sort((a, b) => (a.at < b.at ? 1 : a.at > b.at ? -1 : 0));
  return out.slice(0, limit);
}

// ---------------------------------------------------------------------------
// Person across the fleet — the headline report
// ---------------------------------------------------------------------------

app.http('reportPerson', {
  methods: ['GET'],
  authLevel: 'anonymous',
  route: 'v1/admin/reports/person',
  handler: async (req: HttpRequest): Promise<HttpResponseInit> => {
    const auth = requireRole(req, 'Viewer');
    if (isDenied(auth)) return auth.denied;

    const personId = req.query.get('personId');
    if (!personId) return bad('personId is required');
    const range = parseRange(req);
    if (typeof range === 'string') return bad(range);

    let rows = await scan('EventsByPerson', (m) => `${personId}-${m}`, range, MAX_ROWS);

    // "This person at this door" is the same partition with a filter, not a
    // separate index — the partition is small enough that this stays cheap.
    const deviceId = req.query.get('deviceId');
    if (deviceId) rows = rows.filter((r) => r.deviceId === deviceId);
    if (req.query.get('grantedOnly') === 'true') rows = rows.filter((r) => r.granted);

    return {
      status: 200,
      jsonBody: {
        personId,
        deviceId: deviceId ?? null,
        from: range.from.toISOString(),
        to: range.to.toISOString(),
        count: rows.length,
        truncated: rows.length >= MAX_ROWS,
        events: rows,
      },
    };
  },
});

// ---------------------------------------------------------------------------
// Door timeline — the complete record for one door, attributable or not
// ---------------------------------------------------------------------------

app.http('reportDoor', {
  methods: ['GET'],
  authLevel: 'anonymous',
  route: 'v1/admin/reports/door',
  handler: async (req: HttpRequest): Promise<HttpResponseInit> => {
    const auth = requireRole(req, 'Viewer');
    if (isDenied(auth)) return auth.denied;

    const deviceId = req.query.get('deviceId');
    if (!deviceId) return bad('deviceId is required');
    const range = parseRange(req);
    if (typeof range === 'string') return bad(range);

    let rows = await scan('EventsByDoor', (m) => `${deviceId}-${m}`, range, MAX_ROWS);
    if (req.query.get('grantedOnly') === 'true') rows = rows.filter((r) => r.granted);

    return {
      status: 200,
      jsonBody: {
        deviceId,
        from: range.from.toISOString(),
        to: range.to.toISOString(),
        count: rows.length,
        truncated: rows.length >= MAX_ROWS,
        events: rows,
      },
    };
  },
});

// ---------------------------------------------------------------------------
// Unknown taps — the enrolment feed
// ---------------------------------------------------------------------------

app.http('reportUnknown', {
  methods: ['GET'],
  authLevel: 'anonymous',
  route: 'v1/admin/reports/unknown',
  handler: async (req: HttpRequest): Promise<HttpResponseInit> => {
    const auth = requireRole(req, 'Viewer');
    if (isDenied(auth)) return auth.denied;

    const range = parseRange(req);
    if (typeof range === 'string') return bad(range);

    const rows = await scan('EventsByPerson', (m) => `unknown-${m}`, range, MAX_ROWS);

    // Collapse to distinct cards: the same unrecognised fob tapped eleven times
    // is one thing to enrol, not eleven. Keep the most recent sighting and where.
    const seen = new Map<string, { cred: string; lastSeen: string; door: string; taps: number }>();
    for (const r of rows) {
      if (!r.cred) continue;
      // ONLY card taps. Other event types reuse the detail field for their own
      // purposes -- firmware events put a version change there -- and a
      // misclassification upstream would otherwise offer "2.5.1>2.5.2" as a card
      // to enrol. Belt and braces: ingest already files these under the door, but
      // this feed leads directly to granting someone access, so it verifies the
      // type itself rather than trusting the partition it was found in.
      if (r.type !== EventType.Tap) continue;
      if (!/^[0-9]+$/.test(r.cred)) continue;
      const e = seen.get(r.cred);
      if (e) { e.taps++; }
      else seen.set(r.cred, { cred: r.cred, lastSeen: r.at, door: r.doorName, taps: 1 });
    }

    return {
      status: 200,
      jsonBody: {
        from: range.from.toISOString(),
        to: range.to.toISOString(),
        cards: [...seen.values()],
      },
    };
  },
});

// ---------------------------------------------------------------------------
// Unattributed exits
// ---------------------------------------------------------------------------

app.http('reportUnattributedExits', {
  methods: ['GET'],
  authLevel: 'anonymous',
  route: 'v1/admin/reports/unattributed-exits',
  handler: async (req: HttpRequest, ctx: InvocationContext): Promise<HttpResponseInit> => {
    const auth = requireRole(req, 'Viewer');
    if (isDenied(auth)) return auth.denied;

    const range = parseRange(req);
    if (typeof range === 'string') return bad(range);
    const windowSec = Number(req.query.get('windowSec') ?? 120);
    if (!Number.isFinite(windowSec) || windowSec <= 0) return bad('windowSec must be positive');

    // Which doors to examine. All of them unless one is named.
    const only = req.query.get('deviceId');
    const doors: { id: string; name: string }[] = [];
    for await (const d of t('Doors').listEntities<any>()) {
      if (!only || d.rowKey === only) {
        doors.push({ id: String(d.rowKey), name: String(d.name ?? d.rowKey) });
      }
    }

    const findings: any[] = [];
    for (const door of doors) {
      const rows = await scan('EventsByDoor', (m) => `${door.id}-${m}`, range, MAX_ROWS);
      const exits = rows.filter((r) => r.type === EventType.Exit);
      const grants = rows
        .filter((r) => r.type === EventType.Tap && r.granted)
        .map((r) => Date.parse(r.at))
        .sort((a, b) => a - b);

      for (const ex of exits) {
        const at = Date.parse(ex.at);
        // Any granted tap in the preceding window explains this exit: someone
        // badged in and later pressed the button to get out.
        const explained = grants.some((g) => g <= at && at - g <= windowSec * 1000);
        if (!explained) {
          findings.push({
            deviceId: door.id, doorName: door.name, at: ex.at,
            windowSec, timeApprox: ex.timeApprox,
          });
        }
      }
    }

    findings.sort((a, b) => (a.at < b.at ? 1 : -1));
    ctx.log(`report: ${findings.length} unattributed exit(s) across ${doors.length} door(s)`);

    return {
      status: 200,
      jsonBody: {
        from: range.from.toISOString(),
        to: range.to.toISOString(),
        windowSec,
        count: findings.length,
        // Said plainly because the number is not, by itself, a problem.
        note:
          'An exit-button press with no granted tap at that door in the preceding ' +
          'window. The exit button opens the door with no record of who, by design, ' +
          'so legitimate hits are expected (a visitor let out, someone following ' +
          'another person in). The value is making an invisible gap reviewable. A ' +
          'door-position sensor would sharpen this considerably.',
        exits: findings,
      },
    };
  },
});
