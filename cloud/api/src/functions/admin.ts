/**
 * Admin API — people, credentials, groups and doors.
 *
 * Every route enforces a role. The split follows one idea: an **Operator works
 * within** the access model, an **Admin redefines it**. An Operator can give a
 * person a fob and put them in an existing group; only an Admin can change which
 * doors that group opens.
 *
 * Every mutation bumps `rosterRev`, which is what actually pushes the change out
 * — doors compare their revision on each sync and pull a new roster when it
 * moves. Forgetting to bump it produces the worst kind of bug: the UI says the
 * change was saved, the database agrees, and the doors never hear about it.
 *
 * Mutations are logged with the acting user. An access-control system where
 * changes are anonymous is worth far less than one where "who removed Carl's
 * access on Tuesday" is answerable.
 */

import { app, HttpRequest, HttpResponseInit, InvocationContext } from '@azure/functions';
import { TableClient } from '@azure/data-tables';
import { DefaultAzureCredential } from '@azure/identity';
import { requireRole, isDenied, actor } from '../adminAuth';
import { bumpRosterRev, getDoor, effectiveRoster } from '../storage';
import { issueEnrollCode } from '../auth';
import {
  getSweepConfig, SWEEP_MIN_MINUTES, SWEEP_MAX_MINUTES,
} from './entraSweep';

const account = process.env.STORAGE_ACCOUNT_NAME!;
const endpoint = `https://${account}.table.core.windows.net`;
const credential = new DefaultAzureCredential();
const t = (name: string) => new TableClient(endpoint, name, credential);

const ok = (jsonBody: unknown): HttpResponseInit => ({ status: 200, jsonBody: jsonBody as any });
const bad = (error: string): HttpResponseInit => ({ status: 400, jsonBody: { error } });

/** Table Storage has no array type; groups are stored comma-separated. */
const splitGroups = (v: unknown): string[] =>
  String(v ?? '').split(',').map((s) => s.trim()).filter(Boolean);

/**
 * Door config is stored as a JSON string. Tolerate an unparseable value rather
 * than failing the whole listing: one malformed row would otherwise take the
 * doors page down, and that page is how you discover something is wrong.
 */
function parseConfig(v: unknown): Record<string, unknown> {
  if (typeof v !== 'string' || !v.trim()) return {};
  try {
    const parsed = JSON.parse(v);
    return parsed && typeof parsed === 'object' ? parsed : {};
  } catch {
    return {};
  }
}

// ---------------------------------------------------------------------------
// People
// ---------------------------------------------------------------------------

app.http('adminPeople', {
  methods: ['GET', 'POST', 'DELETE'],
  authLevel: 'anonymous',
  route: 'v1/admin/people',
  handler: async (req: HttpRequest, ctx: InvocationContext): Promise<HttpResponseInit> => {
    if (req.method === 'GET') {
      const auth = await requireRole(req, 'Viewer');
      if (isDenied(auth)) return auth.denied;

      const people: any[] = [];
      for await (const p of t('People').listEntities<any>()) {
        people.push({
          personId: p.rowKey,
          name: p.name,
          email: p.email ?? '',
          active: p.active !== false,
          groups: splitGroups(p.groups),
          // Absent means 'manual': a record that never opted in is never swept.
          managedBy: String(p.managedBy ?? 'manual'),
          entraObjectId: p.entraObjectId ?? '',
          deactivatedReason: p.deactivatedReason ?? '',
          deactivatedAt: p.deactivatedAt ?? '',
        });
      }
      // Credentials are attached so the UI can show "who holds what" without a
      // second round trip -- the commonest thing an operator needs to see.
      for await (const c of t('Credentials').listEntities<any>()) {
        const owner = people.find((x) => x.personId === c.personId);
        if (owner) {
          (owner.credentials ??= []).push({
            credId: c.rowKey, number: c.number, label: c.label ?? '',
            active: c.active !== false,
          });
        }
      }
      people.sort((a, b) => String(a.name).localeCompare(String(b.name)));
      return ok({ people });
    }

    // Operators may create and edit people, and move them between EXISTING
    // groups. They cannot invent groups -- see the groups route.
    const auth = await requireRole(req, 'Operator');
    if (isDenied(auth)) return auth.denied;

    if (req.method === 'DELETE') {
      // Deleting a person is Admin-only: it is destructive, and history already
      // carries their name frozen into past events, so an operator wanting to
      // stop someone's access should deactivate instead.
      const adm = await requireRole(req, 'Admin');
      if (isDenied(adm)) return adm.denied;

      const personId = req.query.get('personId');
      if (!personId) return bad('personId is required');

      // Orphaned credentials would silently keep working: effectiveRoster only
      // skips a credential when its person is missing or inactive, so leaving
      // them behind is exactly the kind of quiet failure to avoid.
      let orphaned = 0;
      for await (const c of t('Credentials').listEntities<any>()) {
        if (c.personId === personId) {
          await t('Credentials').deleteEntity('cred', c.rowKey as string);
          orphaned++;
        }
      }
      await t('People').deleteEntity('person', personId);
      const rev = await bumpRosterRev();
      ctx.log(`admin: ${actor(adm.principal)} deleted person ${personId} ` +
              `and ${orphaned} credential(s), rev -> ${rev}`);
      return ok({ deleted: personId, credentialsRemoved: orphaned, rosterRev: rev });
    }

    const body = (await req.json().catch(() => ({}))) as any;
    const personId = String(body.personId ?? '').trim();
    if (!personId) return bad('personId is required');
    if (!body.name) return bad('name is required');

    // Reject unknown groups rather than silently storing them: a typo would
    // otherwise produce a person who appears configured but opens nothing.
    const groups = Array.isArray(body.groups) ? body.groups.map(String) : [];
    if (groups.length) {
      const known = new Set<string>();
      for await (const g of t('Groups').listEntities<any>()) known.add(String(g.rowKey));
      const unknown = groups.filter((g: string) => !known.has(g));
      if (unknown.length) return bad(`unknown group(s): ${unknown.join(', ')}`);
    }

    // Entra linkage. 'entra' means the sweep may revoke this person's access;
    // anything else means only a human ever changes it.
    const managedBy = String(body.managedBy ?? 'manual') === 'entra' ? 'entra' : 'manual';
    const entraObjectId = String(body.entraObjectId ?? '').trim();

    // Reject a malformed object id rather than storing it. A link that does not
    // resolve is worse than no link: the person reads as covered by automatic
    // revocation and is not, which is precisely the state this feature exists
    // to eliminate.
    if (managedBy === 'entra' && entraObjectId &&
        !/^[0-9a-fA-F-]{36}$/.test(entraObjectId)) {
      return bad('entraObjectId must be a GUID (the Entra object id, not an email)');
    }

    await t('People').upsertEntity(
      {
        partitionKey: 'person',
        rowKey: personId,
        personId,
        name: String(body.name),
        email: String(body.email ?? ''),
        active: body.active !== false,
        groups: groups.join(','),
        managedBy,
        entraObjectId,
        // Reactivating by hand clears the sweep's note, so a stale "disabled in
        // Entra" reason cannot linger against someone who now has access.
        deactivatedReason: body.active !== false ? '' : String(body.deactivatedReason ?? ''),
        deactivatedAt: body.active !== false ? '' : String(body.deactivatedAt ?? ''),
      },
      'Replace'
    );
    const rev = await bumpRosterRev();
    ctx.log(`admin: ${actor(auth.principal)} saved person ${personId} ` +
            `[${groups.join(', ')}] active=${body.active !== false}, rev -> ${rev}`);
    return ok({ personId, rosterRev: rev });
  },
});

// ---------------------------------------------------------------------------
// Credentials
// ---------------------------------------------------------------------------

app.http('adminCredentials', {
  methods: ['GET', 'POST', 'DELETE'],
  authLevel: 'anonymous',
  route: 'v1/admin/credentials',
  handler: async (req: HttpRequest, ctx: InvocationContext): Promise<HttpResponseInit> => {
    if (req.method === 'GET') {
      const auth = await requireRole(req, 'Viewer');
      if (isDenied(auth)) return auth.denied;
      const creds: any[] = [];
      for await (const c of t('Credentials').listEntities<any>()) {
        creds.push({
          credId: c.rowKey, number: c.number, personId: c.personId ?? null,
          label: c.label ?? '', active: c.active !== false,
          validFrom: c.validFrom ?? '', validTo: c.validTo ?? '',
        });
      }
      return ok({ credentials: creds });
    }

    const auth = await requireRole(req, 'Operator');
    if (isDenied(auth)) return auth.denied;

    if (req.method === 'DELETE') {
      // Admin-only, for the same reason as deleting a person: it destroys
      // attribution rather than merely revoking access.
      //
      // credentialIndex() resolves events against EVERY credential regardless of
      // `active`, so a DEACTIVATED fob still names its holder on future taps --
      // "Carl - DENIED" at 2am is exactly the record you want. Delete the row and
      // that same tap resolves to nobody: it lands in the unknown-card feed and
      // is offered up for enrolment, which is precisely backwards for a
      // credential someone revoked on purpose.
      //
      // Nothing an Operator legitimately needs requires this. A mistyped number
      // is fixed by editing it (credId is the key, so a corrected POST replaces
      // it in place), a wrong assignment by reassigning personId, and a lost fob
      // by deactivating -- which stops it just as fast and can be undone.
      const adm = await requireRole(req, 'Admin');
      if (isDenied(adm)) return adm.denied;

      const credId = req.query.get('credId');
      if (!credId) return bad('credId is required');
      await t('Credentials').deleteEntity('cred', credId);
      const rev = await bumpRosterRev();
      ctx.log(`admin: ${actor(adm.principal)} deleted credential ${credId}, rev -> ${rev}`);
      return ok({ deleted: credId, rosterRev: rev });
    }

    const body = (await req.json().catch(() => ({}))) as any;
    const credId = String(body.credId ?? '').trim();
    const number = String(body.number ?? '').trim();
    if (!credId) return bad('credId is required');
    if (!/^\d+$/.test(number)) return bad('number must be numeric');

    // A number held by two credential rows makes event attribution
    // non-deterministic -- the ingest path indexes by number, so whichever row
    // enumerates last wins. Reject it here rather than discovering it in a report.
    for await (const c of t('Credentials').listEntities<any>()) {
      if (c.number === number && c.rowKey !== credId) {
        return bad(`number ${number} is already assigned to credential "${c.rowKey}"`);
      }
    }

    await t('Credentials').upsertEntity(
      {
        partitionKey: 'cred', rowKey: credId, credId, number,
        personId: String(body.personId ?? ''),
        label: String(body.label ?? ''),
        active: body.active !== false,
        validFrom: String(body.validFrom ?? ''),
        validTo: String(body.validTo ?? ''),
      },
      'Replace'
    );
    const rev = await bumpRosterRev();
    ctx.log(`admin: ${actor(auth.principal)} saved credential ${credId} -> ` +
            `person ${body.personId || '(unassigned)'}, active=${body.active !== false}, rev -> ${rev}`);
    return ok({ credId, rosterRev: rev });
  },
});

// ---------------------------------------------------------------------------
// Groups — Admin only. Changing groups changes who can open what.
// ---------------------------------------------------------------------------

app.http('adminGroups', {
  methods: ['GET', 'POST', 'DELETE'],
  authLevel: 'anonymous',
  route: 'v1/admin/groups',
  handler: async (req: HttpRequest, ctx: InvocationContext): Promise<HttpResponseInit> => {
    if (req.method === 'GET') {
      const auth = await requireRole(req, 'Viewer');
      if (isDenied(auth)) return auth.denied;
      const groups: any[] = [];
      for await (const g of t('Groups').listEntities<any>()) {
        groups.push({ groupId: g.rowKey, name: g.name });
      }
      return ok({ groups });
    }

    const auth = await requireRole(req, 'Admin');
    if (isDenied(auth)) return auth.denied;

    if (req.method === 'DELETE') {
      const groupId = req.query.get('groupId');
      if (!groupId) return bad('groupId is required');

      // Refuse while anything references it. Deleting a group out from under a
      // person or a door silently revokes access with no obvious cause.
      const usedByPeople: string[] = [];
      for await (const p of t('People').listEntities<any>()) {
        if (splitGroups(p.groups).includes(groupId)) usedByPeople.push(String(p.name ?? p.rowKey));
      }
      const usedByDoors: string[] = [];
      for await (const d of t('Doors').listEntities<any>()) {
        if (splitGroups(d.groups).includes(groupId)) usedByDoors.push(String(d.name ?? d.rowKey));
      }
      if (usedByPeople.length || usedByDoors.length) {
        return {
          status: 409,
          jsonBody: {
            error: 'group is still in use',
            people: usedByPeople,
            doors: usedByDoors,
          },
        };
      }

      await t('Groups').deleteEntity('group', groupId);
      const rev = await bumpRosterRev();
      ctx.log(`admin: ${actor(auth.principal)} deleted group ${groupId}, rev -> ${rev}`);
      return ok({ deleted: groupId, rosterRev: rev });
    }

    const body = (await req.json().catch(() => ({}))) as any;
    const groupId = String(body.groupId ?? '').trim();
    if (!groupId) return bad('groupId is required');
    await t('Groups').upsertEntity(
      { partitionKey: 'group', rowKey: groupId, groupId, name: String(body.name ?? groupId) },
      'Replace'
    );
    const rev = await bumpRosterRev();
    ctx.log(`admin: ${actor(auth.principal)} saved group ${groupId}, rev -> ${rev}`);
    return ok({ groupId, rosterRev: rev });
  },
});

// ---------------------------------------------------------------------------
// Doors
// ---------------------------------------------------------------------------

app.http('adminDoors', {
  methods: ['GET', 'POST'],
  authLevel: 'anonymous',
  route: 'v1/admin/doors',
  handler: async (req: HttpRequest, ctx: InvocationContext): Promise<HttpResponseInit> => {
    if (req.method === 'GET') {
      const auth = await requireRole(req, 'Viewer');
      if (isDenied(auth)) return auth.denied;
      const doors: any[] = [];
      for await (const d of t('Doors').listEntities<any>()) {
        const lastSeen = d.lastSeen ? Date.parse(d.lastSeen) : 0;
        doors.push({
          deviceId: d.rowKey, name: d.name, site: d.site ?? '',
          board: d.board ?? '', groups: splitGroups(d.groups),
          firmware: d.firmware ?? '', rosterRev: d.rosterRev ?? 0,
          lastSeen: d.lastSeen ?? null,
          // Surfaced so the UI can flag it: a door that stopped checking in
          // still grants access from its cached roster, so nothing looks wrong
          // at the door itself.
          silentMinutes: lastSeen ? Math.floor((Date.now() - lastSeen) / 60000) : null,
          fwHold: d.fwHold === true,
          paired: !!d.keyHash,
          // Returned so an editor can round-trip it. POST replaces the whole
          // config blob, so a UI that could not read the current values would
          // overwrite them with whatever its blank form happened to hold.
          config: parseConfig(d.config),
        });
      }
      doors.sort((a, b) => String(a.name).localeCompare(String(b.name)));
      return ok({ doors });
    }

    const auth = await requireRole(req, 'Admin');
    if (isDenied(auth)) return auth.denied;

    const body = (await req.json().catch(() => ({}))) as any;
    const deviceId = String(body.deviceId ?? '').trim();
    if (!deviceId) return bad('deviceId is required');

    const patch: Record<string, unknown> = { partitionKey: 'door', rowKey: deviceId };
    if (body.name !== undefined) patch.name = String(body.name);
    if (body.site !== undefined) patch.site = String(body.site);
    if (body.fwHold !== undefined) patch.fwHold = body.fwHold === true;
    if (body.groups !== undefined) {
      const groups = Array.isArray(body.groups) ? body.groups.map(String) : [];
      const known = new Set<string>();
      for await (const g of t('Groups').listEntities<any>()) known.add(String(g.rowKey));
      const unknown = groups.filter((g: string) => !known.has(g));
      if (unknown.length) return bad(`unknown group(s): ${unknown.join(', ')}`);
      patch.groups = groups.join(',');
    }
    if (body.config !== undefined) patch.config = JSON.stringify(body.config);

    // Merge, never Replace: keyHash and pairedAt must survive an edit, or the
    // door silently loses its ability to authenticate.
    await t('Doors').upsertEntity(patch as any, 'Merge');
    const rev = await bumpRosterRev();
    ctx.log(`admin: ${actor(auth.principal)} updated door ${deviceId} ` +
            `(${Object.keys(patch).filter((k) => k !== 'partitionKey' && k !== 'rowKey').join(', ')}), rev -> ${rev}`);
    return ok({ deviceId, rosterRev: rev });
  },
});

/** Preview what a door would actually receive. Answers "why can't X get in?" */
app.http('adminDoorRoster', {
  methods: ['GET'],
  authLevel: 'anonymous',
  route: 'v1/admin/doors/roster',
  handler: async (req: HttpRequest): Promise<HttpResponseInit> => {
    const auth = await requireRole(req, 'Viewer');
    if (isDenied(auth)) return auth.denied;
    const deviceId = req.query.get('deviceId');
    if (!deviceId) return bad('deviceId is required');
    const door = await getDoor(deviceId);
    if (!door) return { status: 404, jsonBody: { error: 'no such door' } };
    return ok({ deviceId, door: door.name, roster: await effectiveRoster(deviceId) });
  },
});

/**
 * Health of the Entra sweep.
 *
 * Exists because the sweep fails OPEN: if Graph is unreachable it changes
 * nothing, which keeps a Graph outage from locking a building. The cost is that
 * silence is indistinguishable from success, so the time since the last CLEAN
 * run has to be visible. "Has checked" and "is checking" are different claims
 * and only one of them is a control.
 */
app.http('adminEntraStatus', {
  methods: ['GET', 'POST'],
  authLevel: 'anonymous',
  route: 'v1/admin/entra-status',
  handler: async (req: HttpRequest, ctx: InvocationContext): Promise<HttpResponseInit> => {
    // Changing how often access is checked -- or switching the check off -- is
    // changing the access model, not working within it. Admin.
    if (req.method === 'POST') {
      const adm = await requireRole(req, 'Admin');
      if (isDenied(adm)) return adm.denied;

      const body = (await req.json().catch(() => ({}))) as any;
      const raw = Number(body.intervalMinutes);
      if (!Number.isFinite(raw)) return bad('intervalMinutes must be a number');
      if (raw < SWEEP_MIN_MINUTES || raw > SWEEP_MAX_MINUTES) {
        return bad(
          `intervalMinutes must be between ${SWEEP_MIN_MINUTES} and ${SWEEP_MAX_MINUTES}. ` +
          `The lower bound is the timer heartbeat — a shorter interval cannot run more often ` +
          `than the timer ticks, and would only look like it was working.`
        );
      }
      const enabled = body.enabled !== false;

      await t('Meta').upsertEntity(
        {
          partitionKey: 'meta', rowKey: 'entraSweepConfig',
          intervalMinutes: Math.round(raw), enabled,
        },
        'Merge'
      );
      ctx.log(`admin: ${actor(adm.principal)} set entra sweep to ` +
              `${Math.round(raw)}m, enabled=${enabled}`);
      return ok({ intervalMinutes: Math.round(raw), enabled });
    }

    const auth = await requireRole(req, 'Viewer');
    if (isDenied(auth)) return auth.denied;

    let row: any = null;
    try {
      row = await t('Meta').getEntity('meta', 'entraSweep');
    } catch {
      row = null;                     // never run
    }

    let managed = 0, unlinked = 0, total = 0;
    for await (const p of t('People').listEntities<any>()) {
      total++;
      if (String(p.managedBy ?? 'manual') !== 'entra') continue;
      managed++;
      if (!String(p.entraObjectId ?? '').trim()) unlinked++;
    }

    const lastSuccessAt = row?.lastSuccessAt ?? null;
    const ageMins = lastSuccessAt
      ? Math.floor((Date.now() - Date.parse(String(lastSuccessAt))) / 60000)
      : null;

    const cfg = await getSweepConfig();

    return ok({
      lastRunAt: row?.lastRunAt ?? null,
      lastSuccessAt,
      lastOk: row?.lastOk ?? null,
      minutesSinceSuccess: ageMins,
      intervalMinutes: cfg.intervalMinutes,
      enabled: cfg.enabled,
      minMinutes: SWEEP_MIN_MINUTES,
      maxMinutes: SWEEP_MAX_MINUTES,
      // DERIVED from the configured interval, not a fixed number. A hardcoded
      // threshold silently becomes wrong the moment someone changes the cadence
      // -- too tight and it cries wolf on a healthy system, too loose and it
      // stops reporting a real outage. Three missed runs is a signal; one is a
      // blip.
      stale: ageMins === null || ageMins > cfg.intervalMinutes * 3,
      error: row?.error ?? '',
      lastRevoked: String(row?.revoked ?? '').split('; ').filter(Boolean),
      counts: { total, entraManaged: managed, manual: total - managed, unlinked },
      note:
        'The sweep only ever revokes; restoring access is always a deliberate action here. ' +
        'It changes nothing when Entra cannot be reached, so a stale check means the ' +
        'guarantee is not currently being enforced, not that everyone is fine.',
    });
  },
});

/** Issue a pairing code. Operator-level: routine, and the code is short-lived. */
app.http('adminPairingCode', {
  methods: ['POST'],
  authLevel: 'anonymous',
  route: 'v1/admin/doors/pairing-code',
  handler: async (req: HttpRequest, ctx: InvocationContext): Promise<HttpResponseInit> => {
    const auth = await requireRole(req, 'Operator');
    if (isDenied(auth)) return auth.denied;
    const body = (await req.json().catch(() => ({}))) as any;
    const name = String(body.doorName ?? '').trim();
    if (!name) return bad('doorName is required');
    const code = await issueEnrollCode(name, String(body.site ?? ''));
    ctx.log(`admin: ${actor(auth.principal)} issued a pairing code for "${name}"`);
    return ok({ code, doorName: name, expiresInMinutes: 15 });
  },
});
