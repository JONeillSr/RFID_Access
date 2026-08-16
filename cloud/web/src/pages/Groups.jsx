import { useEffect, useState } from 'preact/hooks';
import { api, atLeast } from '../auth';
import { Table } from '../components/Table';
import { Modal, Field, Text, Actions, useRecord } from '../components/Form';

/**
 * Groups — Admin only, and rarely visited.
 *
 * A group is the join between people and doors: a person opens a door when they
 * share at least one group with it. That makes this the page where the shape of
 * the access model is decided, as opposed to People, where individuals are moved
 * around inside a shape that already exists. Hence the role split — an Operator
 * works within the model, an Admin redefines it.
 *
 * Because a group is meaningless on its own, the table shows what each one
 * actually connects. A group listing "3 people / 0 doors" opens nothing, which
 * is invisible if you only show names.
 */
export function Groups({ notify, flash }) {
  const [groups, setGroups] = useState([]);
  const [people, setPeople] = useState([]);
  const [doors, setDoors] = useState([]);
  const [editing, setEditing] = useState(null);
  const [busy, setBusy] = useState(false);

  const load = async () => {
    try {
      const [g, p, d] = await Promise.all([
        api('/v1/admin/groups'), api('/v1/admin/people'), api('/v1/admin/doors'),
      ]);
      setGroups(g?.groups ?? []);
      setPeople(p?.people ?? []);
      setDoors(d?.doors ?? []);
    } catch (e) { notify(e); }
  };
  useEffect(() => { load(); }, []);

  const mutate = async (fn, describe) => {
    setBusy(true);
    try {
      const r = await fn();
      await load();
      setEditing(null);
      flash(`${describe} — roster rev ${r?.rosterRev ?? '?'}.`);
    } catch (e) { notify(e); } finally { setBusy(false); }
  };

  const usage = (groupId) => ({
    people: people.filter((p) => p.groups.includes(groupId)),
    doors: doors.filter((d) => d.groups.includes(groupId)),
  });

  if (!atLeast('Admin')) {
    return <p class="muted">Managing groups requires the Admin role.</p>;
  }

  return (
    <>
      <div class="toolbar">
        <h2 style="margin:0">Groups</h2>
        <div class="spacer" />
        <button class="primary" onClick={() => setEditing({ isNew: true })}>Add group</button>
      </div>

      <p class="muted">
        A person can open a door when they share at least one group with it.
        Changing these changes who can go where.
      </p>

      <Table
        headers={['Group', 'People', 'Doors', '']}
        rows={groups.map((g) => {
          const u = usage(g.groupId);
          return [
            <div>
              <strong>{g.name || g.groupId}</strong>
              <div class="muted"><code>{g.groupId}</code></div>
            </div>,
            u.people.length
              ? u.people.map((p) => p.name).join(', ')
              : <span class="muted">nobody</span>,
            u.doors.length
              ? u.doors.map((d) => d.name).join(', ')
              : <span class="bad">no doors — opens nothing</span>,
            <div class="rowacts">
              <button class="small" onClick={() => setEditing({ ...g })}>Rename</button>
              <button class="small" onClick={() => setEditing({ ...g, confirmDelete: true })}>
                Delete
              </button>
            </div>,
          ];
        })}
      />

      {!groups.length && <p class="muted">No groups yet.</p>}

      {editing && (
        <GroupDialog
          group={editing} usage={editing.groupId ? usage(editing.groupId) : { people: [], doors: [] }}
          taken={groups.map((g) => g.groupId)} busy={busy}
          onClose={() => setEditing(null)}
          onSave={(rec) => mutate(
            () => api('/v1/admin/groups', { method: 'POST', body: JSON.stringify(rec) }),
            `Saved ${rec.name || rec.groupId}`)}
          onDelete={(rec) => mutate(
            () => api(`/v1/admin/groups?groupId=${encodeURIComponent(rec.groupId)}`, { method: 'DELETE' }),
            `Deleted ${rec.name || rec.groupId}`)}
        />
      )}
    </>
  );
}

function GroupDialog({ group, usage, taken, busy, onClose, onSave, onDelete }) {
  const isNew = !!group.isNew;
  const [rec, set] = useRecord({ groupId: '', name: '', ...group });
  const [idTouched, setIdTouched] = useState(false);

  const setName = (v) => {
    set('name', v);
    if (isNew && !idTouched) {
      set('groupId', v.toLowerCase().replace(/[^a-z0-9]/g, ''));
    }
  };

  const dupId = isNew && taken.includes(rec.groupId);
  const valid = rec.groupId.trim() && !dupId;
  const inUse = usage.people.length + usage.doors.length > 0;

  if (group.confirmDelete) {
    return (
      <Modal title={`Delete ${group.name || group.groupId}?`} onClose={onClose}>
        {inUse ? (
          <>
            <div class="consequence warn">
              Still in use, so this cannot be deleted yet. Removing a group out from
              under a person or a door revokes access with no obvious cause, so the
              server refuses it too.
            </div>
            {usage.people.length > 0 && (
              <p class="muted"><strong>People:</strong> {usage.people.map((p) => p.name).join(', ')}</p>
            )}
            {usage.doors.length > 0 && (
              <p class="muted"><strong>Doors:</strong> {usage.doors.map((d) => d.name).join(', ')}</p>
            )}
            <Actions><button onClick={onClose}>Close</button></Actions>
          </>
        ) : (
          <>
            <p class="muted">Nothing references this group, so removing it changes no one's access.</p>
            <Actions>
              <button onClick={onClose} disabled={busy}>Cancel</button>
              <button class="danger" disabled={busy} onClick={() => onDelete(rec)}>
                {busy ? 'Deleting…' : 'Delete group'}
              </button>
            </Actions>
          </>
        )}
      </Modal>
    );
  }

  return (
    <Modal title={isNew ? 'Add group' : `Rename ${group.name || group.groupId}`} onClose={onClose}>
      <Field label="Name" hint="What people will see, e.g. “Contractors”.">
        <Text value={rec.name} onInput={setName} placeholder="Contractors" autofocus />
      </Field>

      <Field
        label="Group ID"
        hint={isNew
          ? 'Permanent. People and doors reference this, so it cannot be changed later.'
          : 'Permanent — only the display name can change.'}
        error={dupId ? 'That ID is already taken.' : null}
      >
        <Text value={rec.groupId}
              onInput={(v) => { setIdTouched(true); set('groupId', v.toLowerCase().replace(/[^a-z0-9]/g, '')); }}
              disabled={!isNew} />
      </Field>

      {isNew && (
        <div class="consequence">
          A new group opens nothing until it is added to at least one door on the
          Doors page, and given to at least one person.
        </div>
      )}

      <Actions>
        <button onClick={onClose} disabled={busy}>Cancel</button>
        <button class="primary" disabled={!valid || busy} onClick={() => onSave(strip(rec))}>
          {busy ? 'Saving…' : 'Save'}
        </button>
      </Actions>
    </Modal>
  );
}

function strip(rec) {
  const out = { ...rec };
  delete out.isNew;
  delete out.confirmDelete;
  return out;
}
