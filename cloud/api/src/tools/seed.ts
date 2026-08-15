/**
 * Seed groups, people, credentials and doors from seed.json.
 *
 *   npm run seed              -- apply seed.json, then print each door's roster
 *   npm run seed -- --dry-run -- show what would change, write nothing
 *   npm run seed -- --code rfid-275044   -- issue a pairing code for one door
 *
 * Authenticates as YOU (whoever ran `az login`) via DefaultAzureCredential, so
 * it needs the Storage Table Data Contributor role that cloud/infra/README.md
 * grants. There is no account key to pass, because the account has none.
 *
 * Idempotent: re-running with the same file converges rather than duplicating.
 * Doors are written with Merge so that re-seeding never clobbers the keyHash of
 * an already-paired device — that would silently lock a working door out of the
 * backend until someone re-paired it in person.
 *
 * After seeding it prints the EFFECTIVE ROSTER each door would receive. That is
 * the real point of this tool: it exercises the group-intersection logic before
 * any device depends on it, so "the door got an empty roster" gets diagnosed at
 * a desk instead of on a ladder.
 */

import { TableClient } from '@azure/data-tables';
import { DefaultAzureCredential } from '@azure/identity';
import { readFileSync, existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { issueEnrollCode } from '../auth';

interface SeedCredential {
  credId: string;
  number: string;
  label?: string;
  validFrom?: string;
  validTo?: string;
}
interface SeedPerson {
  personId: string;
  name: string;
  email?: string;
  groups: string[];
  credentials: SeedCredential[];
}
interface SeedDoor {
  deviceId: string;
  name: string;
  site: string;
  board: string;
  groups: string[];
  config?: { relayHoldMs: number; resultHoldMs: number };
}
interface SeedFile {
  groups: { groupId: string; name: string }[];
  people: SeedPerson[];
  doors: SeedDoor[];
}

const account = process.env.STORAGE_ACCOUNT_NAME;
if (!account) {
  console.error('STORAGE_ACCOUNT_NAME is not set.');
  console.error('  PowerShell:  $env:STORAGE_ACCOUNT_NAME = "jtcprodrfidaccessst"');
  process.exit(1);
}

const credential = new DefaultAzureCredential();
const endpoint = `https://${account}.table.core.windows.net`;
const t = (name: string) => new TableClient(endpoint, name, credential);

const args = process.argv.slice(2);
const dryRun = args.includes('--dry-run');
const prune = args.includes('--prune');
const codeFor = args.includes('--code') ? args[args.indexOf('--code') + 1] : undefined;

async function main() {
  // --- pairing-code mode: issue and exit -----------------------------------
  if (codeFor) {
    const doors = t('Doors');
    let name = codeFor;
    let site = '';
    try {
      const row = await doors.getEntity<any>('door', codeFor);
      name = row.name ?? codeFor;
      site = row.site ?? '';
    } catch {
      console.log(`(no Doors row for ${codeFor} yet — pairing will create one)`);
    }
    const code = await issueEnrollCode(name, site);
    console.log(`\nPairing code for ${codeFor} (${name}): ${code}`);
    console.log('Valid 15 minutes, single use. Type it into the device /setup page.\n');
    return;
  }

  // --- seed mode -----------------------------------------------------------
  // Resolved from the working directory, not __dirname: npm scripts run with
  // cwd set to the package root, whereas __dirname points into dist/ and would
  // have to count directory levels that change whenever the build layout does.
  const path = resolve(process.cwd(), 'seed.json');
  if (!existsSync(path)) {
    console.error(`seed.json not found at ${path}`);
    console.error('Copy seed.example.json to seed.json and fill it in.');
    process.exit(1);
  }

  const seed = JSON.parse(readFileSync(path, 'utf8')) as SeedFile;

  // Validate before writing anything: a half-applied seed is worse than none.
  const groupIds = new Set(seed.groups.map((g) => g.groupId));
  const problems: string[] = [];
  const seenCred = new Map<string, string>();

  for (const p of seed.people) {
    for (const g of p.groups) {
      if (!groupIds.has(g)) problems.push(`person "${p.personId}" references unknown group "${g}"`);
    }
    for (const c of p.credentials) {
      if (!/^\d+$/.test(c.number)) {
        problems.push(`credential "${c.credId}" number "${c.number}" is not numeric`);
      }
      const prior = seenCred.get(c.number);
      if (prior) {
        // Two people holding the same number means one of them silently never
        // matches — the roster is keyed by the credential, not the person.
        problems.push(`credential number ${c.number} assigned to both "${prior}" and "${p.personId}"`);
      }
      seenCred.set(c.number, p.personId);
    }
  }
  for (const d of seed.doors) {
    for (const g of d.groups) {
      if (!groupIds.has(g)) problems.push(`door "${d.deviceId}" references unknown group "${g}"`);
    }
    if (d.groups.length === 0) {
      problems.push(`door "${d.deviceId}" has no groups — it would grant nobody`);
    }
  }

  if (problems.length) {
    console.error('\nSeed file has problems:\n');
    for (const p of problems) console.error(`  - ${p}`);
    console.error('\nNothing was written.\n');
    process.exit(1);
  }

  console.log(
    `\n${dryRun ? '[DRY RUN] ' : ''}Seeding ${seed.groups.length} group(s), ` +
      `${seed.people.length} person/people, ` +
      `${seed.people.reduce((n, p) => n + p.credentials.length, 0)} credential(s), ` +
      `${seed.doors.length} door(s)\n`
  );

  if (!dryRun) {
    for (const g of seed.groups) {
      await t('Groups').upsertEntity(
        { partitionKey: 'group', rowKey: g.groupId, groupId: g.groupId, name: g.name },
        'Replace'
      );
      console.log(`  group       ${g.groupId.padEnd(16)} ${g.name}`);
    }

    for (const p of seed.people) {
      await t('People').upsertEntity(
        {
          partitionKey: 'person',
          rowKey: p.personId,
          personId: p.personId,
          name: p.name,
          email: p.email ?? '',
          active: true,
          groups: p.groups.join(','),   // Table Storage has no array type
        },
        'Replace'
      );
      console.log(`  person      ${p.personId.padEnd(16)} ${p.name}  [${p.groups.join(', ')}]`);

      for (const c of p.credentials) {
        await t('Credentials').upsertEntity(
          {
            partitionKey: 'cred',
            rowKey: c.credId,
            credId: c.credId,
            number: c.number,
            personId: p.personId,
            label: c.label ?? '',
            active: true,
            validFrom: c.validFrom ?? '',
            validTo: c.validTo ?? '',
          },
          'Replace'
        );
        console.log(`  credential  ${c.credId.padEnd(16)} ${c.number}  (${c.label ?? 'no label'})`);
      }
    }

    for (const d of seed.doors) {
      // Merge, NOT Replace: preserves keyHash/pairedAt on an already-paired door.
      await t('Doors').upsertEntity(
        {
          partitionKey: 'door',
          rowKey: d.deviceId,
          name: d.name,
          site: d.site,
          board: d.board,
          groups: d.groups.join(','),
          config: JSON.stringify(d.config ?? { relayHoldMs: 3000, resultHoldMs: 4000 }),
        },
        'Merge'
      );
      console.log(`  door        ${d.deviceId.padEnd(16)} ${d.name}  [${d.groups.join(', ')}]`);
    }

    // Bump the revision LAST, so a door that syncs mid-seed either sees the old
    // roster or the complete new one, never a half-applied state.
    const meta = t('Meta');
    let rev = 0;
    try {
      rev = (await meta.getEntity<{ value: number }>('meta', 'rosterRev')).value ?? 0;
    } catch { /* first run */ }
    rev += 1;
    await meta.upsertEntity({ partitionKey: 'meta', rowKey: 'rosterRev', value: rev }, 'Replace');
    console.log(`\n  rosterRev -> ${rev}`);
  }

  // --- reconcile: find rows in the tables that the seed file no longer -----
  // describes. Upserting alone does NOT converge: renaming a personId or credId
  // leaves the old row behind, and a stale credential row sharing a number with
  // a live one makes event attribution non-deterministic (credentialIndex is
  // keyed by number, so whichever row enumerates last wins). Worse, a stale
  // PERSON row can carry an old group and silently grant access that was
  // deliberately revoked.
  const wantPeople = new Set(seed.people.map((p) => p.personId));
  const wantCreds = new Set(seed.people.flatMap((p) => p.credentials.map((c) => c.credId)));
  const wantGroups = new Set(seed.groups.map((g) => g.groupId));
  const wantDoors = new Set(seed.doors.map((d) => d.deviceId));

  const orphans: { table: string; key: string; detail: string }[] = [];

  for await (const e of t('People').listEntities<any>()) {
    if (!wantPeople.has(e.rowKey as string)) {
      orphans.push({ table: 'People', key: e.rowKey as string, detail: `${e.name} [${e.groups}]` });
    }
  }
  const numbersInUse = new Map<string, string[]>();
  for await (const e of t('Credentials').listEntities<any>()) {
    const key = e.rowKey as string;
    const num = String(e.number ?? '');
    if (!numbersInUse.has(num)) numbersInUse.set(num, []);
    numbersInUse.get(num)!.push(key);
    if (!wantCreds.has(key)) {
      orphans.push({ table: 'Credentials', key, detail: `${num} -> ${e.personId}` });
    }
  }
  for await (const e of t('Groups').listEntities<any>()) {
    if (!wantGroups.has(e.rowKey as string)) {
      orphans.push({ table: 'Groups', key: e.rowKey as string, detail: String(e.name ?? '') });
    }
  }
  for await (const e of t('Doors').listEntities<any>()) {
    if (!wantDoors.has(e.rowKey as string)) {
      const paired = e.keyHash ? ' (PAIRED — pruning would unpair this device)' : '';
      orphans.push({ table: 'Doors', key: e.rowKey as string, detail: `${e.name}${paired}` });
    }
  }

  const dupes = [...numbersInUse.entries()].filter(([n, keys]) => n && keys.length > 1);
  if (dupes.length) {
    console.log('\n  *** DUPLICATE CREDENTIAL NUMBERS IN THE TABLES ***');
    for (const [num, keys] of dupes) {
      console.log(`      ${num} is held by ${keys.length} rows: ${keys.join(', ')}`);
    }
    console.log(
      '      Event attribution for these fobs is non-deterministic until resolved.\n' +
        '      Run with --prune to remove the rows the seed file no longer declares.'
    );
  }

  if (orphans.length) {
    console.log(`\n  ${orphans.length} row(s) in the tables are not in seed.json:\n`);
    for (const o of orphans) {
      console.log(`      ${o.table.padEnd(12)} ${o.key.padEnd(22)} ${o.detail}`);
    }
    if (prune && !dryRun) {
      console.log('\n  --prune given: deleting them');
      for (const o of orphans) {
        const pk =
          o.table === 'People' ? 'person' : o.table === 'Credentials' ? 'cred'
          : o.table === 'Groups' ? 'group' : 'door';
        await t(o.table).deleteEntity(pk, o.key);
        console.log(`      deleted ${o.table}/${o.key}`);
      }
    } else {
      console.log('\n  Left in place. Re-run with --prune to delete them.');
    }
  } else {
    console.log('\n  Tables match seed.json exactly (no orphans).');
  }

  // --- verification: what would each door actually receive? -----------------
  console.log('\nEffective roster per door (this is what a device would be sent):\n');
  for (const d of seed.doors) {
    const doorGroups = new Set(d.groups);
    const entries: string[] = [];
    for (const p of seed.people) {
      if (!p.groups.some((g) => doorGroups.has(g))) continue;
      for (const c of p.credentials) entries.push(`${c.number}  ${p.name}`);
    }
    console.log(`  ${d.name} (${d.deviceId})`);
    if (entries.length === 0) {
      console.log('      (empty — this door would deny everyone)');
    } else {
      for (const e of entries) console.log(`      ${e}`);
    }
    console.log('');
  }
}

main().catch((err) => {
  console.error('\nSeeding failed:', err?.message ?? err);
  if (String(err?.message ?? '').includes('AuthorizationPermissionMismatch')) {
    console.error(
      '\nThat is an RBAC failure, not a bad seed file. You need Storage Table\n' +
        'Data Contributor on the account — see cloud/infra/README.md. Role\n' +
        'assignments can take a couple of minutes to propagate.\n'
    );
  }
  process.exit(1);
});
