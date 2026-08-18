/**
 * Enroll a card that tapped but was not recognized.
 *
 * WHY THIS EXISTS AS ITS OWN FLOW
 * The alternative is reading a ten-digit number off the dashboard and typing it
 * into a fob form. That transcription is the single most error-prone step in
 * administering this system, and getting it wrong produces a fob that does not
 * work and a stored number that matches nothing — with no error anywhere,
 * because a wrong-but-valid number is indistinguishable from a right one until
 * someone stands at a door and fails to get in.
 *
 * Here the number is carried through untouched and never retyped.
 *
 * It can also create the person in the same step. Requiring a person to exist
 * first would mean leaving this screen, remembering the number, and coming back
 * — which reintroduces exactly the transcription this is meant to remove.
 */
import { useState } from 'preact/hooks';
import { api, atLeast } from '../auth';
import {
  Modal, Field, Text, Check, GroupPicker, Actions,
  suggestPersonId, suggestCredId,
} from './Form';

export function EnrollDialog({ card, people, groups, creds, onClose, onDone, notify }) {
  const [mode, setMode] = useState(people.length ? 'existing' : 'new');
  const [personId, setPersonId] = useState('');
  const [newPerson, setNewPerson] = useState({ name: '', email: '', groups: [], active: true });
  const [newId, setNewId] = useState('');
  const [idTouched, setIdTouched] = useState(false);
  const [label, setLabel] = useState('');
  const [busy, setBusy] = useState(false);

  const takenPeople = people.map((p) => p.personId);
  const existingCred = creds.find((c) => c.number === card.cred);

  const setName = (v) => {
    setNewPerson((p) => ({ ...p, name: v }));
    if (!idTouched) setNewId(suggestPersonId(v, takenPeople));
  };

  const targetName = mode === 'new'
    ? newPerson.name
    : people.find((p) => p.personId === personId)?.name ?? '';

  const valid = mode === 'new'
    ? newPerson.name.trim() && newId.trim() && !takenPeople.includes(newId)
    : !!personId;

  const submit = async () => {
    setBusy(true);
    try {
      let owner = personId;

      // Person first: a credential pointing at a person who does not exist is
      // skipped by effectiveRoster, so the fob would appear enrolled and open
      // nothing. If this step fails we stop, rather than leaving that behind.
      if (mode === 'new') {
        await api('/v1/admin/people', {
          method: 'POST',
          body: JSON.stringify({
            personId: newId, name: newPerson.name, email: newPerson.email,
            active: true, groups: newPerson.groups,
          }),
        });
        owner = newId;
      }

      const r = await api('/v1/admin/credentials', {
        method: 'POST',
        body: JSON.stringify({
          credId: suggestCredId(targetName, 'fob', creds.map((c) => c.credId)),
          number: card.cred,
          label: label || 'enrolled from unknown tap',
          personId: owner,
          active: true,
        }),
      });

      onDone(`Enrolled ${card.cred} to ${targetName} — doors will pick this up on ` +
             `their next sync (roster rev ${r?.rosterRev ?? '?'}).`);
    } catch (e) {
      notify(e);
      setBusy(false);
    }
  };

  if (!atLeast('Operator')) {
    return (
      <Modal title="Enroll card" onClose={onClose}>
        <p class="muted">Enrolling a card requires the Operator role.</p>
        <Actions><button onClick={onClose}>Close</button></Actions>
      </Modal>
    );
  }

  return (
    <Modal title={`Enroll card ${card.cred}`} onClose={onClose}>
      <div class="consequence">
        Seen <strong>{card.taps}×</strong> at <strong>{card.door}</strong>, most
        recently {new Date(card.lastSeen).toLocaleString()}.
      </div>

      {existingCred && (
        <div class="consequence warn">
          This number is already registered as <code>{existingCred.credId}</code>
          {existingCred.active ? '' : ' (inactive)'}. It is being denied for another
          reason — an inactive fob or person, or a group that does not open that
          door. Enrolling it again will fail; check the fob on the People page instead.
        </div>
      )}

      <Field label="Give it to">
        <div class="picker">
          <label class="check">
            <input type="radio" name="who" checked={mode === 'existing'}
                   disabled={!people.length}
                   onChange={() => setMode('existing')} />
            <span>Someone already set up</span>
          </label>
          {mode === 'existing' && (
            <select value={personId} onChange={(e) => setPersonId(e.currentTarget.value)}
                    style="width:100%;margin:6px 0 10px">
              <option value="">— choose a person —</option>
              {people.map((p) => (
                <option key={p.personId} value={p.personId}>
                  {p.name}{p.active ? '' : ' (inactive)'}
                </option>
              ))}
            </select>
          )}
          <label class="check">
            <input type="radio" name="who" checked={mode === 'new'}
                   onChange={() => setMode('new')} />
            <span>A new person</span>
          </label>
        </div>
      </Field>

      {mode === 'new' && (
        <>
          <Field label="Full name">
            <Text value={newPerson.name} onInput={setName} placeholder="Avery O'Neill" />
          </Field>
          <Field label="Person ID" hint="Permanent — used in logs and on every event."
                 error={takenPeople.includes(newId) ? 'That ID is already taken.' : null}>
            <Text value={newId}
                  onInput={(v) => { setIdTouched(true); setNewId(v.toLowerCase().replace(/[^a-z0-9-]/g, '')); }} />
          </Field>
          <Field label="Email">
            <Text value={newPerson.email}
                  onInput={(v) => setNewPerson((p) => ({ ...p, email: v }))} />
          </Field>
          <Field label="Groups">
            <GroupPicker all={groups} selected={newPerson.groups}
                         onChange={(g) => setNewPerson((p) => ({ ...p, groups: g }))} />
          </Field>
          {!newPerson.groups.length && (
            <div class="consequence warn">
              With no group this person opens <strong>nothing</strong>. The fob will
              be enrolled and still be denied at every door.
            </div>
          )}
        </>
      )}

      <Field label="Label" hint="Optional — what this card physically is.">
        <Text value={label} onInput={setLabel} placeholder="keychain fob" />
      </Field>

      <Actions>
        <button onClick={onClose} disabled={busy}>Cancel</button>
        <button class="primary" disabled={!valid || busy || !!existingCred} onClick={submit}>
          {busy ? 'Enrolling…' : 'Enroll card'}
        </button>
      </Actions>
    </Modal>
  );
}
