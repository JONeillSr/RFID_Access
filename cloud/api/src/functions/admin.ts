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

    await t('People').upsertEntity(
      {
        partitionKey: 'person',
        rowKey: personId,
        personId,
        name: String(body.name),
        email: String(body.email ?? ''),
        active: body.active !== false,
        groups: groups.join(','),
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
