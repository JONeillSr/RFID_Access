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
  const [editPerson, setEditPerson] = useState(null);
  const [editFob, setEditFob] = useState(null);
  const [busy, setBusy] = useState(false);

  const load = async () => {
    try {
      const [p, c, g, d] = await Promise.all([
        api('/v1/admin/people'),
        api('/v1/admin/credentials'),
        api('/v1/admin/groups'),
        api('/v1/admin/doors'),
      ]);
      setPeople(p?.people ?? []);
      setCreds(c?.credentials ?? []);
      setGroups(g?.groups ?? []);
      setSilentDoors((d?.doors ?? []).filter((x) => x.silentMinutes !== null && x.silentMinutes > 10));
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

      <Check
        label="Active"
        checked={rec.active}
        onChange={(v) => set('active', v)}
        hint="Inactive suspends every fob this person holds."
      />

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
