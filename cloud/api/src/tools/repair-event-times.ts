/**
 * Repair event times stored by the old boot-epoch rule.
 *
 *   npm run repair-event-times                         # dry run: shows every change
 *   npm run repair-event-times -- --apply --backup <file-outside-the-repo>
 *   options:  --device rfid-6f24f0   --since 2026-08-18T00:00:00Z
 *
 * WHAT WENT WRONG
 * Events recorded with no clock were dated (start of the CURRENT boot + uptime)
 * even when they came from an EARLIER boot -- see ../eventTime.ts. A door that
 * flushed an offline stretch after rebooting stored those events up to sixteen
 * days in the future.
 *
 * HOW THE BAD ROWS ARE FOUND
 * Every sync writes its events within a second or two; syncs are 30 s apart. So
 * rows are grouped into the sync that wrote them by gaps in their storage write
 * time. Within one sync the highest boot number is the boot that reported it,
 * and any derived-time row from a LOWER boot was dated with the wrong boot's
 * start. Separately, any derived-time row dated after it was even received is
 * wrong by definition.
 *
 * WHAT THEY BECOME
 * Nothing invented. Each is marked time-unknown and bounded by its neighbours in
 * the door's own sequence -- the last event before it whose time is trustworthy,
 * and the next -- exactly as ingest now does for new events. It is moved to the
 * partition its placement falls in, in both event tables.
 *
 * SAFETY
 * - Dry run by default. --apply requires --backup, written BEFORE any change.
 * - The backup holds raw card numbers, which are credentials, so it is refused
 *   anywhere inside the repository.
 * - New rows are written before old ones are deleted: an interruption leaves a
 *   duplicate, which is recoverable, never a loss, which is not.
 * - Only rows written since --since are touched. Sequence-based bounds assume a
 *   door's boot counter climbs, and a door whose counter reset inside that window
 *   is skipped rather than guessed at.
 * - Idempotent: repaired rows are marked time-unknown and skipped on a rerun.
 */

import { TableClient } from '@azure/data-tables';
import { DefaultAzureCredential } from '@azure/identity';
import { writeFileSync } from 'fs';
import { resolve, relative, isAbsolute } from 'path';
import { boundUnknowns, compareSequence } from '../eventTime';
import { invertedTs, monthKey, isPersonless } from '../storage';

const account = process.env.STORAGE_ACCOUNT_NAME;
if (!account) {
  console.error('Set STORAGE_ACCOUNT_NAME first.');
  process.exit(1);
}
const credential = new DefaultAzureCredential();
const client = (name: string) =>
  new TableClient(`https://${account}.table.core.windows.net`, name, credential);

function arg(name: string): string | undefined {
  const i = process.argv.indexOf('--' + name);
  return i >= 0 ? process.argv[i + 1] : undefined;
}
const apply = process.argv.includes('--apply');
const backupPath = arg('backup');
const onlyDevice = arg('device');
const since = Date.parse(arg('since') ?? '2026-08-18T00:00:00Z');

/** Syncs are 30 s apart; one sync's writes land within a few seconds. */
const SYNC_GAP_MS = 10_000;

const DATA_FIELDS = [
  'deviceId', 'doorName', 'bootId', 'idx', 'uptimeMs', 'epoch', 'type', 'reason',
  'granted', 'cred', 'credId', 'personId', 'personName', 'at', 'timeApprox',
];

interface Row {
  entity: any;
  deviceId: string;
  bootId: number;
  idx: number;
  atMs: number;
  writtenMs: number;
  approx: boolean;
  unknown: boolean;
}

interface Change {
  row: Row;
  reason: string;
  newAtMs: number;
  notBeforeMs?: number;
  notAfterMs?: number;
}

const fmt = (ms?: number) => (ms === undefined ? '—' : new Date(ms).toISOString().slice(0, 19) + 'Z');

async function main() {
  if (apply) {
    if (!backupPath) {
      console.error('--apply needs --backup <file>. The backup is written before anything changes.');
      process.exit(1);
    }
    // Compiled to cloud/api/dist/api/src/tools: six levels up is the repo root.
    // (Five only reaches cloud/, which would let a backup land in tools/.)
    const repoRoot = resolve(__dirname, '..', '..', '..', '..', '..', '..');
    const rel = relative(repoRoot, resolve(backupPath));
    if (!rel.startsWith('..') && !isAbsolute(rel)) {
      console.error('Refusing to write the backup inside the repository: it contains card numbers.');
      process.exit(1);
    }
  }

  // ---- load ---------------------------------------------------------------
  const byDoor = new Map<string, Row[]>();
  let sawTimestamp = true;
  for await (const e of client('EventsByDoor').listEntities<any>()) {
    const deviceId = String(e.deviceId ?? '');
    if (!deviceId || (onlyDevice && deviceId !== onlyDevice)) continue;
    const writtenMs = Date.parse(String(e.timestamp ?? ''));
    if (!Number.isFinite(writtenMs)) { sawTimestamp = false; continue; }
    const list = byDoor.get(deviceId) ?? [];
    list.push({
      entity: e, deviceId,
      bootId: Number(e.bootId), idx: Number(e.idx),
      atMs: Date.parse(String(e.at)), writtenMs,
      approx: e.timeApprox === true, unknown: e.timeUnknown === true,
    });
    byDoor.set(deviceId, list);
  }
  if (!sawTimestamp) {
    // Without the storage write time the rows cannot be grouped by sync, and
    // guessing the grouping is exactly how an audit repair makes things worse.
    console.error('Rows came back without a storage timestamp; cannot group them by sync. Nothing changed.');
    process.exit(1);
  }

  // ---- plan ---------------------------------------------------------------
  const changes: Change[] = [];
  for (const [deviceId, all] of byDoor) {
    const rows = all.filter((r) => r.writtenMs >= since && !r.unknown);
    if (rows.length === 0) continue;

    // Group by the sync that wrote them.
    const byWrite = [...rows].sort((a, b) => a.writtenMs - b.writtenMs);
    const batches: Row[][] = [];
    for (const r of byWrite) {
      const cur = batches[batches.length - 1];
      if (cur && r.writtenMs - cur[cur.length - 1]!.writtenMs <= SYNC_GAP_MS) cur.push(r);
      else batches.push([r]);
    }

    // A boot counter that goes backwards means sequence-based bounds would lie.
    let maxEarlier = -1;
    let reset = false;
    for (const b of batches) {
      const lo = Math.min(...b.map((r) => r.bootId));
      if (maxEarlier >= 0 && lo < maxEarlier - 1) { reset = true; break; }
      maxEarlier = Math.max(maxEarlier, ...b.map((r) => r.bootId));
    }
    if (reset) {
      console.log(`  ${deviceId}: boot counter goes backwards in this window — skipped, nothing changed`);
      continue;
    }

    const flagged = new Map<Row, string>();
    for (const b of batches) {
      const reportingBoot = Math.max(...b.map((r) => r.bootId));
      for (const r of b) {
        if (!r.approx) continue;                               // observed: trusted
        if (r.bootId < reportingBoot) {
          flagged.set(r, `dated from boot ${reportingBoot}'s start, but recorded in boot ${r.bootId}`);
        } else if (r.atMs > r.writtenMs + 60_000) {
          flagged.set(r, 'dated after it was received');
        }
      }
    }
    if (flagged.size === 0) continue;

    // Bound the flagged rows by their neighbours in the door's own sequence.
    const seq = [...rows].sort(compareSequence);
    const bounded = boundUnknowns(seq.map((r) => ({
      knownMs: flagged.has(r) ? undefined : r.atMs,
      ceilingMs: r.writtenMs,                                  // cannot postdate its own receipt
    })));
    seq.forEach((r, i) => {
      const why = flagged.get(r);
      if (!why) return;
      const b = bounded[i]!;
      changes.push({ row: r, reason: why, newAtMs: b.atMs, notBeforeMs: b.notBeforeMs, notAfterMs: b.notAfterMs });
    });
  }

  // ---- report -------------------------------------------------------------
  if (changes.length === 0) {
    console.log('\nNothing to repair.\n');
    return;
  }
  console.log(`\n${changes.length} event(s) to repair${apply ? '' : ' (DRY RUN — nothing is changed)'}:\n`);
  for (const c of changes) {
    const e = c.row.entity;
    console.log(
      `  ${String(e.doorName).padEnd(12)} boot ${String(c.row.bootId).padEnd(3)} idx ${String(c.row.idx).padEnd(4)} ` +
      `type ${String(e.type).padEnd(3)} stored ${fmt(c.row.atMs)}\n` +
      `      -> time unknown, between ${fmt(c.notBeforeMs)} and ${fmt(c.notAfterMs)}` +
      `   (placed ${fmt(c.newAtMs)})\n      because: ${c.reason}`
    );
  }
  if (!apply) {
    console.log('\nRe-run with --apply --backup <file outside the repo> to make these changes.\n');
    return;
  }

  // ---- backup -------------------------------------------------------------
  const backup: any[] = [];
  for (const c of changes) {
    const e = c.row.entity;
    const entry: any = { eventsByDoor: e };
    if (!isPersonless(e.type)) {
      const pk = `${e.personId || 'unknown'}-${monthKey(new Date(c.row.atMs))}`;
      const rk = `${invertedTs(c.row.atMs)}-${e.deviceId}-${String(e.bootId).padStart(10, '0')}-${String(e.idx).padStart(10, '0')}`;
      try { entry.eventsByPerson = await client('EventsByPerson').getEntity(pk, rk); }
      catch { entry.eventsByPerson = null; }
    }
    backup.push(entry);
  }
  writeFileSync(resolve(backupPath!), JSON.stringify({ takenAt: new Date().toISOString(), rows: backup }, null, 2));
  console.log(`\nBackup written: ${resolve(backupPath!)} (${backup.length} event(s))`);

  // ---- apply --------------------------------------------------------------
  let moved = 0;
  for (const c of changes) {
    const e = c.row.entity;
    const boot = String(e.bootId).padStart(10, '0');
    const idx = String(e.idx).padStart(10, '0');
    const at = new Date(c.newAtMs);

    const data: any = {};
    for (const f of DATA_FIELDS) if (e[f] !== undefined && e[f] !== null) data[f] = e[f];
    Object.assign(data, {
      at: at.toISOString(),
      epoch: Math.floor(c.newAtMs / 1000),
      timeApprox: true,
      timeUnknown: true,
      ...(c.notBeforeMs !== undefined ? { timeNotBefore: new Date(c.notBeforeMs).toISOString() } : {}),
      ...(c.notAfterMs !== undefined ? { timeNotAfter: new Date(c.notAfterMs).toISOString() } : {}),
    });

    const doorPk = `${e.deviceId}-${monthKey(at)}`;
    const doorRk = `${invertedTs(c.newAtMs)}-${boot}-${idx}`;

    // Write new first; delete old second. Never the other way round.
    await client('EventsByDoor').upsertEntity({ partitionKey: doorPk, rowKey: doorRk, ...data }, 'Replace');
    if (!isPersonless(e.type)) {
      const pPk = `${e.personId || 'unknown'}-${monthKey(at)}`;
      const pRk = `${invertedTs(c.newAtMs)}-${e.deviceId}-${boot}-${idx}`;
      await client('EventsByPerson').upsertEntity({ partitionKey: pPk, rowKey: pRk, ...data }, 'Replace');

      const oldPk = `${e.personId || 'unknown'}-${monthKey(new Date(c.row.atMs))}`;
      const oldRk = `${invertedTs(c.row.atMs)}-${e.deviceId}-${boot}-${idx}`;
      if (oldPk !== pPk || oldRk !== pRk) {
        try { await client('EventsByPerson').deleteEntity(oldPk, oldRk); } catch { /* already gone */ }
      }
    }
    if (e.partitionKey !== doorPk || e.rowKey !== doorRk) {
      await client('EventsByDoor').deleteEntity(e.partitionKey, e.rowKey);
    }
    moved++;
  }
  console.log(`Repaired ${moved} event(s).\n`);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
