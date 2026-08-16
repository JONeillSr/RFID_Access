/**
 * Fleet status: what every door is running, and whether it took the latest image.
 *
 *   npm run fleet
 *
 * Answers the two questions you actually have between rollouts:
 *   - did each door take the update?
 *   - is any door quietly not checking in?
 *
 * Compares each door's REPORTED firmware against what is published for its
 * board, so "behind" is computed rather than eyeballed. Doors are matched to
 * firmware by board, because that is how firmware is published -- an image for
 * one ESP32 variant is meaningless to another.
 *
 * A door only appears here once it has synced at least once; the row is created
 * by pairing and updated on every check-in.
 */

import { TableClient } from '@azure/data-tables';
import { DefaultAzureCredential } from '@azure/identity';
import { listFirmware, boardKey } from '../storage';

const account = process.env.STORAGE_ACCOUNT_NAME;
if (!account) {
  console.error('STORAGE_ACCOUNT_NAME is not set.');
  console.error('  PowerShell:  $env:STORAGE_ACCOUNT_NAME = "jtcprodrfidaccessst"');
  process.exit(1);
}

const credential = new DefaultAzureCredential();
const endpoint = `https://${account}.table.core.windows.net`;

/** A door silent for this long is worth noticing; it syncs every ~30 s. */
const SILENT_AFTER_MIN = 10;

function ago(iso?: string): { text: string; minutes: number } {
  if (!iso) return { text: 'never', minutes: Number.MAX_SAFE_INTEGER };
  const ms = Date.now() - Date.parse(iso);
  const mins = Math.floor(ms / 60000);
  if (mins < 1) return { text: `${Math.floor(ms / 1000)}s ago`, minutes: 0 };
  if (mins < 60) return { text: `${mins}m ago`, minutes: mins };
  const hrs = Math.floor(mins / 60);
  if (hrs < 24) return { text: `${hrs}h ago`, minutes: mins };
  return { text: `${Math.floor(hrs / 24)}d ago`, minutes: mins };
}

async function main() {
  const doors = new TableClient(endpoint, 'Doors', credential);

  // Published image per board, keyed the same way the sync endpoint keys it.
  const published = new Map<string, string>();
  for (const f of await listFirmware()) published.set(boardKey(f.board), f.version);

  interface Row {
    name: string; deviceId: string; board: string;
    fw: string; want: string; status: string;
    seen: string; seenMins: number; rev: string; hold: boolean;
  }
  const rows: Row[] = [];

  for await (const d of doors.listEntities<any>()) {
    const fw = String(d.firmware ?? '?');
    const want = published.get(boardKey(String(d.board ?? ''))) ?? '-';
    const hold = d.fwHold === true;
    const seen = ago(d.lastSeen);

    let status: string;
    if (want === '-') status = 'no image published';
    else if (fw === want) status = 'up to date';
    else if (hold) status = `HELD at ${fw}`;
    else status = `BEHIND (wants ${want})`;

    rows.push({
      name: String(d.name ?? d.rowKey),
      deviceId: String(d.rowKey),
      board: String(d.board ?? '?'),
      fw, want, status,
      seen: seen.text, seenMins: seen.minutes,
      rev: String(d.rosterRev ?? '?'),
      hold,
    });
  }

  if (!rows.length) {
    console.log('\nNo doors have paired yet.\n');
    return;
  }

  rows.sort((a, b) => a.name.localeCompare(b.name));

  const w = (k: keyof Row, min: number) =>
    Math.max(min, ...rows.map((r) => String(r[k]).length));
  const wName = w('name', 4), wId = w('deviceId', 9),
        wBoard = w('board', 5), wFw = w('fw', 7);

  console.log('');
  console.log(
    `  ${'DOOR'.padEnd(wName)}  ${'DEVICE'.padEnd(wId)}  ${'BOARD'.padEnd(wBoard)}  ` +
      `${'RUNNING'.padEnd(wFw)}  ${'REV'.padEnd(4)}  ${'LAST SEEN'.padEnd(10)}  STATUS`
  );
  console.log('  ' + '-'.repeat(wName + wId + wBoard + wFw + 40));

  for (const r of rows) {
    // Flag a door that has stopped checking in: it is still deciding access
    // locally from its cached roster, so nothing looks broken at the door --
    // which is exactly why it needs pointing out here.
    const silent = r.seenMins > SILENT_AFTER_MIN ? '  <-- NOT CHECKING IN' : '';
    console.log(
      `  ${r.name.padEnd(wName)}  ${r.deviceId.padEnd(wId)}  ${r.board.padEnd(wBoard)}  ` +
        `${r.fw.padEnd(wFw)}  ${r.rev.padEnd(4)}  ${r.seen.padEnd(10)}  ${r.status}${silent}`
    );
  }

  console.log('');
  const behind = rows.filter((r) => r.status.startsWith('BEHIND')).length;
  const held = rows.filter((r) => r.hold).length;
  const silent = rows.filter((r) => r.seenMins > SILENT_AFTER_MIN).length;
  console.log(
    `  ${rows.length} door(s): ${rows.length - behind - held} up to date, ` +
      `${behind} behind, ${held} held` + (silent ? `, ${silent} not checking in` : '')
  );

  console.log('\n  Published images:');
  const all = await listFirmware();
  if (!all.length) console.log('    (none)');
  for (const f of all) {
    console.log(`    ${f.board.padEnd(20)} ${f.version.padEnd(10)} ${f.publishedAt}`);
  }
  console.log('');
}

main().catch((err) => {
  console.error('\nFailed:', err?.message ?? err);
  process.exit(1);
});
