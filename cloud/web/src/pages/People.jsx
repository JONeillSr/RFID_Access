import { useEffect, useState } from 'preact/hooks';
import { api, atLeast } from '../auth';
import { Table, Pill } from '../components/Table';
import {
  Modal, Field, Text, Check, GroupPicker, Actions,
  useRecord, suggestPersonId, suggestCredId,
} from '../components/Form';

/**
 * People and the fobs they carry — the page where access is actually granted
 * and revoked.
 *
 * WHY FOBS ARE EDITED FROM THE CREDENTIALS FEED, NOT THE PEOPLE FEED
 * `GET /people` attaches a *summary* of each person's fobs (id, number, label,
 * active) so the table can be drawn in one request. It omits `validFrom` and
 * `validTo`. Since `POST /credentials` replaces the whole row, saving an edit
 * built from that summary would silently erase both dates. So the full records
 * are fetched separately and used as the basis for every edit.
 */
export function People({ notify, flash }) {
  const [people, setPeople] = useState([]);
  const [creds, setCreds] = useState([]);
  const [groups, setGroups] = useState([]);
  const [silentDoors, setSilentDoors] = useState([]);
  const [entra, setEntra] = useState(null);
  const [editPerson, setEditPerson] = useState(null);
  const [editFob, setEditFob] = useState(null);
  const [busy, setBusy] = useState(false);

  const load = async () => {
    try {
      const [p, c, g, d, e] = await Promise.all([
        api('/v1/admin/people'),
        api('/v1/admin/credentials'),
        api('/v1/admin/groups'),
        api('/v1/admin/doors'),
        api('/v1/admin/entra-status').catch(() => null),
      ]);
      setPeople(p?.people ?? []);
      setCreds(c?.credentials ?? []);
      setGroups(g?.groups ?? []);
      setSilentDoors((d?.doors ?? []).filter((x) => x.silentMinutes !== null && x.silentMinutes > 10));
      setEntra(e);
    } catch (e) { notify(e); }
  };
  useEffect(() => { load(); }, []);

  const canWrite = atLeast('Operator');
  const canDelete = atLeast('Admin');

  /** Every mutation goes through here so refresh + messaging are never forgotten. */
  const mutate = async (fn, describe) => {
    setBusy(true);
    try {
      const r = await fn();
      await load();
      setEditPerson(null);
      setEditFob(null);
      flash(`${describe} — doors will pick this up on their next sync (roster rev ${r?.rosterRev ?? '?'}).`);
    } catch (e) { notify(e); } finally { setBusy(false); }
  };

  const fullCred = (credId) => creds.find((c) => c.credId === credId);

  return (
    <>
      <div class="toolbar">
        <h2 style="margin:0">People</h2>
        <div class="spacer" />
        {canWrite && (
          <button class="primary" onClick={() => setEditPerson({ isNew: true, active: true, groups: [] })}>
            Add person
          </button>
        )}
      </div>

      {entra && <EntraPanel entra={entra} notify={notify} flash={flash} onSaved={load} />}

      <Table
        headers={['Person', 'Groups', 'Fobs', 'Status', '']}
        rows={people.map((p) => [
          <div>
            <strong>{p.name}</strong>
            {p.email && <div class="muted">{p.email}</div>}
            <div class="muted"><code>{p.personId}</code></div>
          </div>,
          p.groups.length ? p.groups.join(', ') : <span class="muted">none</span>,
          <div>
            {(p.credentials ?? []).map((c) => (
              <div key={c.credId} class={canWrite ? 'clickable' : ''}
                   onClick={canWrite ? () => setEditFob(fullCred(c.credId) ?? c) : undefined}
                   title={canWrite ? 'Edit this fob' : undefined}>
                <code>{c.number}</code>
                {c.label && <span class="muted"> {c.label}</span>}
                {!c.active && <Pill kind="warn">inactive</Pill>}
              </div>
            ))}
            {!(p.credentials ?? []).length && <span class="muted">no fobs</span>}
          </div>,
          p.active ? <Pill kind="ok">active</Pill> : <Pill kind="warn">inactive</Pill>,
          canWrite ? (
            <div class="rowacts">
              <button class="small" onClick={() => setEditPerson({ ...p })}>Edit</button>
              <button class="small" onClick={() =>
                setEditFob({ isNew: true, active: true, personId: p.personId, _personName: p.name })}>
                Add fob
              </button>
            </div>
          ) : null,
        ])}
      />

      {!people.length && <p class="muted">No people yet. Add one to get started.</p>}

      {editPerson && (
        <PersonDialog
          person={editPerson} groups={groups} people={people}
          canDelete={canDelete} busy={busy}
          onClose={() => setEditPerson(null)}
          onSave={(rec) => mutate(
            () => api('/v1/admin/people', { method: 'POST', body: JSON.stringify(rec) }),
            `Saved ${rec.name}`)}
          onDelete={(rec) => mutate(
            () => api(`/v1/admin/people?personId=${encodeURIComponent(rec.personId)}`, { method: 'DELETE' }),
            `Deleted ${rec.name}`)}
        />
      )}

      {editFob && (
        <FobDialog
          fob={editFob} people={people} creds={creds}
          silentDoors={silentDoors} canDelete={canDelete} busy={busy}
          onClose={() => setEditFob(null)}
          onSave={(rec) => mutate(
            () => api('/v1/admin/credentials', { method: 'POST', body: JSON.stringify(rec) }),
            `Saved fob ${rec.number}`)}
          onDelete={(rec) => mutate(
            () => api(`/v1/admin/credentials?credId=${encodeURIComponent(rec.credId)}`, { method: 'DELETE' }),
            `Deleted fob ${rec.number}`)}
        />
      )}
    </>
  );
}

// ---------------------------------------------------------------------------

/**
 * Entra sweep: whether it is enforcing, and how often.
 *
 * The staleness line is the point of this panel. The sweep fails OPEN — it
 * changes nothing when Graph cannot be reached, so a Graph outage never locks a
 * building — which means silence and success look identical. Time since the last
 * CLEAN run is the only thing that tells them apart.
 */
function EntraPanel({ entra, notify, flash, onSaved }) {
  const [mins, setMins] = useState(String(entra.intervalMinutes ?? 15));
  const [enabled, setEnabled] = useState(entra.enabled !== false);
  const [busy, setBusy] = useState(false);
  const canEdit = atLeast('Admin');
  const c = entra.counts ?? {};

  const save = async () => {
    setBusy(true);
    try {
      await api('/v1/admin/entra-status', {
        method: 'POST',
        body: JSON.stringify({ intervalMinutes: Number(mins), enabled }),
      });
      flash(`Entra sweep set to every ${mins} minutes${enabled ? '' : ' (disabled)'}.`);
      onSaved();
    } catch (e) { notify(e); } finally { setBusy(false); }
  };

  const n = Number(mins);
  const valid = Number.isFinite(n) && n >= (entra.minMinutes ?? 5) && n <= (entra.maxMinutes ?? 1440);

  return (
    <div class="card">
      <h3>Entra account sweep</h3>

      {!entra.enabled && (
        <div class="consequence warn">
          <strong>Turned off.</strong> Disabling an Entra account will not revoke
          any fobs until this is switched back on.
        </div>
      )}

      {entra.enabled && entra.stale && (
        <div class="consequence warn">
          <strong>Not currently enforcing.</strong>{' '}
          {entra.lastSuccessAt
            ? <>Last clean check was {entra.minutesSinceSuccess} minutes ago.</>
            : <>It has never completed a clean check.</>}
          {entra.error && <> Last error: {entra.error}</>}
          {' '}Access is unchanged — this sweep never revokes on doubt — but a
          disabled account would not have been picked up.
        </div>
      )}

      {entra.enabled && !entra.stale && (
        <p class="muted">
          Last clean check {entra.minutesSinceSuccess === 0 ? 'just now'
            : `${entra.minutesSinceSuccess} minutes ago`}. Checking every{' '}
          {entra.intervalMinutes} minutes.
        </p>
      )}

      <p class="muted">
        <strong>{c.entraManaged ?? 0}</strong> governed by Entra ·{' '}
        <strong>{c.manual ?? 0}</strong> managed here (guests, contractors)
        {c.unlinked > 0 && <> · <span class="bad">{c.unlinked} marked as Entra but not linked</span></>}
      </p>

      {c.unlinked > 0 && (
        <div class="consequence warn">
          {c.unlinked} {c.unlinked === 1 ? 'person is' : 'people are'} marked as
          governed by Entra with no object ID, so <strong>nothing will be revoked
          for them</strong>. They read as covered without being covered.
        </div>
      )}

      {entra.lastRevoked?.length > 0 && (
        <p class="muted">Last run revoked: {entra.lastRevoked.join(', ')}</p>
      )}

      {canEdit && (
        <div class="toolbar" style="margin-top:12px;margin-bottom:0">
          <label class="check" style="margin:0">
            <input type="checkbox" checked={enabled}
                   onChange={(e) => setEnabled(e.currentTarget.checked)} />
            <span>Enabled</span>
          </label>
          <span class="muted">check every</span>
          <input type="number" value={mins} min={entra.minMinutes} max={entra.maxMinutes}
                 style="width:90px" onInput={(e) => setMins(e.currentTarget.value)} />
          <span class="muted">
            minutes ({entra.minMinutes}–{entra.maxMinutes}; the lower bound is the
            timer heartbeat)
          </span>
          <button class="primary" disabled={!valid || busy} onClick={save}>
            {busy ? 'Saving…' : 'Save'}
          </button>
        </div>
      )}

      <p class="muted" style="margin-top:10px">{entra.note}</p>
    </div>
  );
}

// ---------------------------------------------------------------------------

function PersonDialog({ person, groups, people, canDelete, busy, onClose, onSave, onDelete }) {
  const isNew = !!person.isNew;
  const [rec, set] = useRecord({
    personId: '', name: '', email: '', active: true, groups: [], ...person,
  });
  const [confirmDelete, setConfirmDelete] = useState(false);
  const taken = people.map((p) => p.personId);

  // Suggest an id from the name while creating, until the user types their own.
  const [idTouched, setIdTouched] = useState(false);
  const setName = (v) => {
    set('name', v);
    if (isNew && !idTouched) set('personId', suggestPersonId(v, taken));
  };

  const dupId = isNew && taken.includes(rec.personId);
  const valid = rec.name.trim() && rec.personId.trim() && !dupId;
  const fobCount = (person.credentials ?? []).length;

  return (
    <Modal title={isNew ? 'Add person' : `Edit ${person.name}`} onClose={onClose}>
      <Field label="Full name">
        <Text value={rec.name} onInput={setName} placeholder="Avery O'Neill" autofocus />
      </Field>

      <Field
        label="Person ID"
        hint={isNew
          ? 'Permanent. Used in logs and to key every event, so it cannot be changed later.'
          : 'Permanent — renaming would create a new person and leave this one granting access.'}
        error={dupId ? 'That ID is already taken.' : null}
      >
        <Text
          value={rec.personId}
          onInput={(v) => { setIdTouched(true); set('personId', v.toLowerCase().replace(/[^a-z0-9-]/g, '')); }}
          disabled={!isNew}
        />
      </Field>

      <Field label="Email" hint="Optional. Not used for sign-in — this is the person who holds the fob, not an app user.">
        <Text value={rec.email} onInput={(v) => set('email', v)} placeholder="someone@example.com" />
      </Field>

      <Field label="Groups">
        <GroupPicker all={groups} selected={rec.groups} onChange={(g) => set('groups', g)} />
      </Field>

      <Field label="Access governed by"
             hint="Stated, not guessed. A contractor with no account and an employee nobody linked look identical otherwise — and only one of them is a problem.">
        <select value={rec.managedBy ?? 'manual'}
                onChange={(e) => set('managedBy', e.currentTarget.value)}>
          <option value="manual">A person here — guest, contractor, one-off</option>
          <option value="entra">Their Entra account</option>
        </select>
      </Field>

      {rec.managedBy === 'entra' && (
        <>
          <Field
            label="Entra object ID"
            hint="The object id (a GUID) from the user's Entra profile — not their email. Emails change with names and rebrands; the object id never does."
            error={rec.entraObjectId && !/^[0-9a-fA-F-]{36}$/.test(rec.entraObjectId.trim())
              ? 'That is not a GUID. Copy the Object ID from the Entra user page.' : null}
          >
            <Text value={rec.entraObjectId} onInput={(v) => set('entraObjectId', v)}
                  placeholder="00000000-0000-0000-0000-000000000000" />
          </Field>
          {!String(rec.entraObjectId ?? '').trim() && (
            <div class="consequence warn">
              Marked as governed by Entra but not linked, so <strong>nothing will be
              revoked automatically</strong>. This reads as covered without being
              covered — the one state worth avoiding.
            </div>
          )}
        </>
      )}

      <Check
        label="Active"
        checked={rec.active}
        onChange={(v) => set('active', v)}
        hint="Inactive suspends every fob this person holds."
      />

      {person.deactivatedReason && !rec.active && (
        <div class="consequence warn">
          Deactivated automatically: <strong>{person.deactivatedReason}</strong>
          {person.deactivatedAt && <> on {new Date(person.deactivatedAt).toLocaleString()}</>}.
          Re-activating here is deliberate and will stick — the sweep only ever
          revokes, and never restores access on its own.
        </div>
      )}

      {!rec.active && fobCount > 0 && (
        <div class="consequence warn">
          Deactivating <strong>{rec.name}</strong> stops all {fobCount} of their
          fob{fobCount === 1 ? '' : 's'} working at every door. Their history is kept.
        </div>
      )}

      <Actions>
        {!isNew && canDelete && (
          <button class="danger left" disabled={busy} onClick={() => setConfirmDelete(true)}>
            Delete
          </button>
        )}
        <button onClick={onClose} disabled={busy}>Cancel</button>
        <button class="primary" disabled={!valid || busy} onClick={() => onSave(stripUi(rec))}>
          {busy ? 'Saving…' : 'Save'}
        </button>
      </Actions>

      {confirmDelete && (
        <Modal title={`Delete ${person.name}?`} onClose={() => setConfirmDelete(false)}>
          <div class="consequence warn">
            This also deletes {fobCount} fob{fobCount === 1 ? '' : 's'}. Past events
            keep this person's name, so history stays readable.
          </div>
          <p class="muted">
            To stop their access without losing the record, deactivate them instead —
            that is reversible and keeps the fobs attached.
          </p>
          <Actions>
            <button onClick={() => setConfirmDelete(false)} disabled={busy}>Cancel</button>
            <button class="danger" disabled={busy} onClick={() => onDelete(rec)}>
              {busy ? 'Deleting…' : 'Delete permanently'}
            </button>
          </Actions>
        </Modal>
      )}
    </Modal>
  );
}

// ---------------------------------------------------------------------------

function FobDialog({ fob, people, creds, silentDoors, canDelete, busy, onClose, onSave, onDelete }) {
  const isNew = !!fob.isNew;
  const owner = people.find((p) => p.personId === fob.personId);
  const [rec, set] = useRecord({
    credId: '', number: '', label: '', personId: '', active: true,
    validFrom: '', validTo: '', ...fob,
  });
  const [confirmDelete, setConfirmDelete] = useState(false);
  const [idTouched, setIdTouched] = useState(false);

  useEffect(() => {
    if (isNew && !idTouched && !rec.credId) {
      const name = fob._personName || owner?.name || 'card';
      set('credId', suggestCredId(name, 'fob', creds.map((c) => c.credId)));
    }
  }, []);

  const takenBy = creds.find((c) => c.number === rec.number.trim() && c.credId !== rec.credId);
  const numberOk = /^\d+$/.test(rec.number.trim());
  const dupId = isNew && creds.some((c) => c.credId === rec.credId);
  const valid = rec.credId.trim() && numberOk && !takenBy && !dupId;

  // Was active, now being switched off -- the revocation case.
  const revoking = !isNew && fob.active && !rec.active;

  return (
    <Modal title={isNew ? 'Add fob' : `Edit fob ${fob.number}`} onClose={onClose}>
      <Field
        label="Card number"
        hint="Exactly as the reader sees it. Take it from the unknown-taps list on the dashboard rather than typing it off the card."
        error={
          rec.number && !numberOk ? 'Digits only.'
            : takenBy ? `Already assigned to "${takenBy.credId}".`
            : null
        }
      >
        {/* Placeholder is deliberately a dummy number. Fob numbers are
            credentials -- anyone holding one can clone a working card -- so a
            real one must never appear in tracked source. Same reason seed.json
            is gitignored and seed.example.json uses zeroes. */}
        <Text value={rec.number} onInput={(v) => set('number', v.trim())}
              placeholder="0000000000" autofocus={isNew} />
      </Field>

      <Field label="Fob ID" hint="Permanent identifier for this physical card."
             error={dupId ? 'That ID is already taken.' : null}>
        <Text value={rec.credId}
              onInput={(v) => { setIdTouched(true); set('credId', v.toLowerCase().replace(/[^a-z0-9-]/g, '')); }}
              disabled={!isNew} />
      </Field>

      <Field label="Label" hint="What this physically is, so a lost one can be identified.">
        <Text value={rec.label} onInput={(v) => set('label', v)} placeholder="keychain fob" />
      </Field>

      <Field label="Assigned to" hint="Unassigned fobs never open anything.">
        <select value={rec.personId} onChange={(e) => set('personId', e.currentTarget.value)}>
          <option value="">— unassigned —</option>
          {people.map((p) => (
            <option key={p.personId} value={p.personId}>{p.name}</option>
          ))}
        </select>
      </Field>

      <Check label="Active" checked={rec.active} onChange={(v) => set('active', v)}
             hint="Deactivate a lost fob rather than deleting it." />

      {revoking && (
        <div class="consequence warn">
          This fob stops working at each door on that door's next sync.
          {silentDoors.length > 0 && (
            <> <strong>{silentDoors.length} door{silentDoors.length === 1 ? ' is' : 's are'} not
            checking in</strong> ({silentDoors.map((d) => d.name).join(', ')}) and will keep
            accepting it until they reconnect.</>
          )}
        </div>
      )}

      <Actions>
        {!isNew && canDelete && (
          <button class="danger left" disabled={busy} onClick={() => setConfirmDelete(true)}>
            Delete
          </button>
        )}
        <button onClick={onClose} disabled={busy}>Cancel</button>
        <button class="primary" disabled={!valid || busy} onClick={() => onSave(stripUi(rec))}>
          {busy ? 'Saving…' : 'Save'}
        </button>
      </Actions>

      {confirmDelete && (
        <Modal title={`Delete fob ${fob.number}?`} onClose={() => setConfirmDelete(false)}>
          <div class="consequence warn">
            Deleting removes the fob record. Past events keep the number, but the
            fob stops being attributable to anyone in future reports.
          </div>
          <p class="muted">
            For a lost or stolen card, <strong>deactivate instead</strong> — it stops
            working just as fast, stays attributable, and can be reversed if it turns up.
          </p>
          <Actions>
            <button onClick={() => setConfirmDelete(false)} disabled={busy}>Cancel</button>
            <button class="danger" disabled={busy} onClick={() => onDelete(rec)}>
              {busy ? 'Deleting…' : 'Delete permanently'}
            </button>
          </Actions>
        </Modal>
      )}
    </Modal>
  );
}

/** Drop UI-only keys so they are never persisted. */
function stripUi(rec) {
  const out = { ...rec };
  delete out.isNew;
  delete out.credentials;   // people: server-derived, not a stored column
  for (const k of Object.keys(out)) if (k.startsWith('_')) delete out[k];
  return out;
}
