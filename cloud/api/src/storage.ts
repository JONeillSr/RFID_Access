/**
 * Table Storage access layer.
 *
 * All persistence goes through here so the storage choice stays swappable: if
 * the reporting ever outgrows Table Storage's single index, only this file
 * changes.
 *
 * Authentication is DefaultAzureCredential against the Function App's
 * system-assigned managed identity. The storage account has
 * allowSharedKeyAccess=false, so there is no account key to leak, rotate, or
 * accidentally commit — and no code path here can fall back to one.
 */

import { TableClient, odata } from '@azure/data-tables';
import { DefaultAzureCredential } from '@azure/identity';
import type {
  Person,
  Credential,
  Door,
  DeviceEvent,
  RosterEntry,
  StoredEvent,
} from '../../shared/types';
import { EventType } from '../../shared/types';

const account = process.env.STORAGE_ACCOUNT_NAME;
if (!account) throw new Error('STORAGE_ACCOUNT_NAME is not set');

const credential = new DefaultAzureCredential();
const endpoint = `https://${account}.table.core.windows.net`;

const clients = new Map<string, TableClient>();
function table(name: string): TableClient {
  let c = clients.get(name);
  if (!c) {
    c = new TableClient(endpoint, name, credential);
    clients.set(name, c);
  }
  return c;
}

// ---------------------------------------------------------------------------
// Row-key helpers
// ---------------------------------------------------------------------------

/**
 * Table Storage sorts row keys lexically, ascending, with no way to reverse it
 * in a query. Newest-first therefore has to be baked into the key: subtract the
 * timestamp from a large constant and zero-pad to a fixed width so string order
 * equals reverse chronological order.
 *
 * The constant is milliseconds at 9999-12-31, which keeps every key 15 digits
 * wide for any date this system will ever see.
 */
const MAX_MS = 253402300799999;

function invertedTs(epochMs: number): string {
  return String(MAX_MS - epochMs).padStart(15, '0');
}

/** Monthly partitions keep any single partition small and bound a query's fan-out. */
function monthKey(d: Date): string {
  return `${d.getUTCFullYear()}${String(d.getUTCMonth() + 1).padStart(2, '0')}`;
}

// ---------------------------------------------------------------------------
// Roster
// ---------------------------------------------------------------------------

/** Global revision. Any change to people, credentials, groups or doors bumps it. */
export async function getRosterRev(): Promise<number> {
  try {
    const e = await table('Meta').getEntity<{ value: number }>('meta', 'rosterRev');
    return e.value ?? 0;
  } catch {
    return 0;
  }
}

export async function bumpRosterRev(): Promise<number> {
  const next = (await getRosterRev()) + 1;
  await table('Meta').upsertEntity(
    { partitionKey: 'meta', rowKey: 'rosterRev', value: next },
    'Replace'
  );
  return next;
}

/**
 * The flat credential list a specific door checks taps against.
 *
 * Group logic is evaluated HERE, never on the device: a door receives an
 * already-resolved list and needs no concept of people or groups. That keeps the
 * ESP32 simple and means permission-model changes never require new firmware.
 *
 * A credential is included when its holder shares at least one group with the
 * door, and both the person and the credential are active and in date.
 */
export async function effectiveRoster(deviceId: string): Promise<RosterEntry[]> {
  const door = await getDoor(deviceId);
  if (!door) return [];

  const doorGroups = new Set(door.groups ?? []);
  if (doorGroups.size === 0) return [];

  const people = new Map<string, Person>();
  for await (const p of table('People').listEntities<Person>()) {
    if (p.active) people.set(p.personId, p);
  }

  const now = Date.now();
  const out: RosterEntry[] = [];

  for await (const c of table('Credentials').listEntities<Credential>()) {
    if (!c.active || !c.personId) continue;

    const person = people.get(c.personId);
    if (!person) continue;                       // inactive or deleted holder

    const groups = Array.isArray(person.groups)
      ? person.groups
      : String(person.groups ?? '').split(',').filter(Boolean);
    if (!groups.some((g) => doorGroups.has(g))) continue;

    if (c.validFrom && Date.parse(c.validFrom) > now) continue;
    if (c.validTo && Date.parse(c.validTo) < now) continue;

    out.push({ cred: c.number, name: person.name });
  }

  return out;
}

// ---------------------------------------------------------------------------
// Doors
// ---------------------------------------------------------------------------

export async function getDoor(deviceId: string): Promise<Door | undefined> {
  try {
    const e = await table('Doors').getEntity<any>('door', deviceId);
    return {
      deviceId,
      name: e.name ?? deviceId,
      site: e.site ?? '',
      board: e.board ?? '',
      groups: String(e.groups ?? '').split(',').filter(Boolean),
      config: e.config ? JSON.parse(e.config) : { relayHoldMs: 3000, resultHoldMs: 4000 },
      lastSeen: e.lastSeen,
      firmware: e.firmware,
      rosterRev: e.rosterRev,
    };
  } catch {
    return undefined;
  }
}

/** Record that a door checked in. Merge, so it never clobbers admin-set fields. */
export async function touchDoor(
  deviceId: string,
  patch: { board?: string; firmware?: string; rosterRev?: number }
): Promise<void> {
  await table('Doors').upsertEntity(
    {
      partitionKey: 'door',
      rowKey: deviceId,
      lastSeen: new Date().toISOString(),
      ...patch,
    },
    'Merge'
  );
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

/** Credential number -> {credId, personId, personName}, resolved once per batch. */
async function credentialIndex(): Promise<
  Map<string, { credId: string; personId?: string; personName?: string }>
> {
  const people = new Map<string, string>();
  for await (const p of table('People').listEntities<Person>()) {
    people.set(p.personId, p.name);
  }

  const index = new Map<string, { credId: string; personId?: string; personName?: string }>();
  for await (const c of table('Credentials').listEntities<Credential>()) {
    index.set(c.number, {
      credId: c.credId,
      personId: c.personId,
      personName: c.personId ? people.get(c.personId) : undefined,
    });
  }
  return index;
}

/** Events that describe the door itself rather than a person's movement. */
function isPersonless(type: EventType): boolean {
  return (
    type === EventType.Exit ||
    type === EventType.Boot ||
    type === EventType.ScheduleOn ||
    type === EventType.ScheduleOff ||
    type === EventType.SyncFail ||
    type === EventType.Config
  );
}

/**
 * Ingest a batch from one door.
 *
 * Each event is written TWICE, under two different partition schemes, because
 * Table Storage has exactly one index. EventsByPerson answers "where has this
 * person been" in a single partition scan; EventsByDoor answers "who came
 * through this door". Storage is irrelevant at this volume; a cross-partition
 * scan for the primary report would not be.
 *
 * The two writes cannot be atomic — entity-group transactions are single
 * partition by definition, and these are different partitions on purpose. Both
 * are deterministic upserts keyed on (deviceId, bootId, idx), so a retry after a
 * partial failure converges rather than duplicating. EventsByPerson is written
 * first because it backs the primary report.
 *
 * Identity is resolved and FROZEN into each row: personName and doorName are
 * copied in now and never recomputed. Reassigning a fob later must not rewrite
 * who held it last Tuesday.
 */
export async function ingestEvents(
  deviceId: string,
  doorName: string,
  events: DeviceEvent[],
  bootEpoch: number
): Promise<{ written: number; partial: number }> {
  if (events.length === 0) return { written: 0, partial: 0 };

  const index = await credentialIndex();
  let written = 0;
  let partial = 0;

  for (const ev of events) {
    // Resolve the timestamp. epoch === 0 means the device had no trusted clock
    // when this happened, so derive it from the boot epoch and mark it derived
    // rather than presenting a guess as an observation.
    let epochMs: number;
    let approx = false;
    if (ev.epoch > 0) {
      epochMs = ev.epoch * 1000;
    } else if (bootEpoch > 0) {
      epochMs = bootEpoch * 1000 + ev.uptimeMs;
      approx = true;
    } else {
      epochMs = Date.now();          // last resort: ingest time
      approx = true;
    }

    const at = new Date(epochMs);
    const month = monthKey(at);
    const inv = invertedTs(epochMs);
    const boot = String(ev.bootId).padStart(10, '0');
    const idx = String(ev.idx).padStart(10, '0');

    const resolved = ev.cred ? index.get(ev.cred) : undefined;

    const row: Omit<StoredEvent, 'at'> & { at: string } = {
      deviceId,
      doorName,
      bootId: ev.bootId,
      idx: ev.idx,
      uptimeMs: ev.uptimeMs,
      epoch: Math.floor(epochMs / 1000),
      type: ev.type,
      reason: ev.reason,
      granted: ev.granted,
      cred: ev.cred,
      credId: resolved?.credId,
      personId: resolved?.personId,
      personName: resolved?.personName,
      at: at.toISOString(),
      timeApprox: approx,
    };

    // Personless events (exit presses, boots, schedule changes) have no sensible
    // person partition. They live only in EventsByDoor; the door timeline is the
    // complete record and the person timeline is a filtered view of it.
    // Unknown cards DO get a partition, because "unknown-YYYYMM" is exactly the
    // feed the admin UI enrolls from.
    const personPartition = isPersonless(ev.type)
      ? undefined
      : `${row.personId ?? 'unknown'}-${month}`;

    let byPersonOk = true;
    if (personPartition) {
      try {
        await table('EventsByPerson').upsertEntity(
          {
            partitionKey: personPartition,
            rowKey: `${inv}-${deviceId}-${boot}-${idx}`,
            ...row,
          },
          'Replace'
        );
      } catch {
        byPersonOk = false;
      }
    }

    let byDoorOk = true;
    try {
      await table('EventsByDoor').upsertEntity(
        {
          partitionKey: `${deviceId}-${month}`,
          rowKey: `${inv}-${boot}-${idx}`,
          ...row,
        },
        'Replace'
      );
    } catch {
      byDoorOk = false;
    }

    if (byPersonOk && byDoorOk) written++;
    else partial++;          // caller logs this; the device will retry the batch
  }

  return { written, partial };
}
