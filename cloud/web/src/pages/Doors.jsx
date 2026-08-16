import { useEffect, useState } from 'preact/hooks';
import { api, atLeast } from '../auth';
import { Table, Pill } from '../components/Table';
import { Modal, Field, Text, Check, GroupPicker, Actions, useRecord } from '../components/Form';

export function Doors({ notify, flash }) {
  const [doors, setDoors] = useState([]);
  const [groups, setGroups] = useState([]);
  const [name, setName] = useState('');
  const [code, setCode] = useState('');
  const [editing, setEditing] = useState(null);
  const [roster, setRoster] = useState(null);
  const [busy, setBusy] = useState(false);

  const load = async () => {
    try {
      const [d, g] = await Promise.all([api('/v1/admin/doors'), api('/v1/admin/groups')]);
      setDoors(d?.doors ?? []);
      setGroups(g?.groups ?? []);
    } catch (e) { notify(e); }
  };
  useEffect(() => { load(); }, []);

  const pair = async () => {
    if (!name.trim()) return notify(new Error('Enter a door name first'));
    try {
      const r = await api('/v1/admin/doors/pairing-code', {
        method: 'POST',
        body: JSON.stringify({ doorName: name.trim() }),
      });
      setCode(r.code);
    } catch (e) { notify(e); }
  };

  const save = async (rec) => {
    setBusy(true);
    try {
      const r = await api('/v1/admin/doors', { method: 'POST', body: JSON.stringify(rec) });
      await load();
      setEditing(null);
      flash(`Saved ${rec.name} — the door applies this on its next sync (roster rev ${r?.rosterRev ?? '?'}).`);
    } catch (e) { notify(e); } finally { setBusy(false); }
  };

  /** "Why can't X get in?" answered from the door's side. */
  const showRoster = async (d) => {
    try {
      setRoster({ door: d.name, loading: true });
      const r = await api(`/v1/admin/doors/roster?deviceId=${encodeURIComponent(d.deviceId)}`);
      setRoster({ door: d.name, roster: r?.roster ?? [] });
    } catch (e) { setRoster(null); notify(e); }
  };

  const canEdit = atLeast('Admin');

  return (
    <>
      <h2>Doors</h2>
      <Table
        headers={['Door', 'Site', 'Board', 'Firmware', 'Groups', 'Last seen', '']}
        rows={doors.map((d) => [
          <div><strong>{d.name}</strong><div class="muted">{d.deviceId}</div></div>,
          d.site,
          d.board,
          d.firmware,
          d.groups.length ? d.groups.join(', ') : <span class="bad">none — opens for nobody</span>,
          d.silentMinutes === null ? 'never'
            : d.silentMinutes > 10
              ? <span class="bad">{d.silentMinutes}m ago — not checking in</span>
              : (d.silentMinutes < 1 ? 'just now' : `${d.silentMinutes}m ago`),
          <>
            {!d.paired && <Pill kind="warn">unpaired</Pill>}
            {d.fwHold && <Pill>held</Pill>}
            <div class="rowacts">
              <button class="small" onClick={() => showRoster(d)}>Who gets in?</button>
              {canEdit && <button class="small" onClick={() => setEditing({ ...d })}>Edit</button>}
            </div>
          </>,
        ])}
      />

      {atLeast('Operator') && (
        <div class="card">
          <h3>Pair a device</h3>
          <p class="muted">
            Generates a code valid for 15 minutes, usable once. Enter it on the
            device's /setup page.
          </p>
          <input
            placeholder="Door name, e.g. Shop Door"
            value={name}
            onInput={(e) => setName(e.currentTarget.value)}
          />
          <button class="primary" onClick={pair}>Generate code</button>
          {code && <div class="code-out">{code}</div>}
        </div>
      )}

      {editing && (
        <DoorDialog door={editing} groups={groups} busy={busy}
                    onClose={() => setEditing(null)} onSave={save} />
      )}

      {roster && (
        <Modal title={`Who gets in at ${roster.door}?`} onClose={() => setRoster(null)}>
          {roster.loading ? <p class="muted">Loading…</p> : (
            <>
              <p class="muted">
                The roster this door actually receives — group membership resolved.
                If someone is missing here, they are missing at the door.
              </p>
              {roster.roster.length ? (
                <Table
                  headers={['Card', 'Person']}
                  rows={roster.roster.map((r) => [<code>{r.cred}</code>, r.name])}
                />
              ) : (
                <div class="consequence warn">
                  Nobody. This door opens for no card at all — usually it has no
                  groups, or no one shares its groups.
                </div>
              )}
            </>
          )}
          <Actions><button onClick={() => setRoster(null)}>Close</button></Actions>
        </Modal>
      )}
    </>
  );
}

/**
 * Door settings.
 *
 * `deviceId`, `board` and pairing state are never editable: the first two are
 * reported by the device itself, and the API deliberately merges rather than
 * replaces so `keyHash` survives. Sending them here would at best be ignored and
 * at worst unpair a working door.
 */
function DoorDialog({ door, groups, busy, onClose, onSave }) {
  const cfg = door.config ?? {};
  const [rec, set] = useRecord({
    deviceId: door.deviceId, name: door.name ?? '', site: door.site ?? '',
    groups: door.groups ?? [], fwHold: door.fwHold === true,
    relayHoldMs: cfg.relayHoldMs ?? '', resultHoldMs: cfg.resultHoldMs ?? '',
  });

  const num = (v) => (v === '' || v === null ? null : Number(v));
  const relayOk = rec.relayHoldMs === '' || (Number.isFinite(num(rec.relayHoldMs)) && num(rec.relayHoldMs) > 0);
  const resultOk = rec.resultHoldMs === '' || (Number.isFinite(num(rec.resultHoldMs)) && num(rec.resultHoldMs) > 0);
  const valid = rec.name.trim() && relayOk && resultOk;

  const submit = () => {
    // Only send config keys that have a value, and preserve anything already in
    // the stored blob that this form does not know about -- the POST replaces
    // the whole thing.
    const config = { ...cfg };
    if (rec.relayHoldMs === '') delete config.relayHoldMs; else config.relayHoldMs = num(rec.relayHoldMs);
    if (rec.resultHoldMs === '') delete config.resultHoldMs; else config.resultHoldMs = num(rec.resultHoldMs);

    onSave({
      deviceId: rec.deviceId, name: rec.name, site: rec.site,
      groups: rec.groups, fwHold: rec.fwHold, config,
    });
  };

  return (
    <Modal title={`Edit ${door.name}`} onClose={onClose}>
      <Field label="Door name">
        <Text value={rec.name} onInput={(v) => set('name', v)} autofocus />
      </Field>

      <Field label="Site">
        <Text value={rec.site} onInput={(v) => set('site', v)} placeholder="JT Custom Trailers" />
      </Field>

      <Field label="Groups this door honours">
        <GroupPicker all={groups} selected={rec.groups} onChange={(g) => set('groups', g)} />
      </Field>

      {!rec.groups.length && (
        <div class="consequence warn">
          With no groups this door opens for <strong>nobody</strong>. The exit
          button still works.
        </div>
      )}

      <Field label="Relay hold (ms)" hint="How long the door stays unlocked after a granted tap. Blank uses the device default."
             error={relayOk ? null : 'Must be a positive number.'}>
        <Text value={rec.relayHoldMs} onInput={(v) => set('relayHoldMs', v)} placeholder="3000" />
      </Field>

      <Field label="Result screen hold (ms)" hint="How long the granted/denied screen stays up."
             error={resultOk ? null : 'Must be a positive number.'}>
        <Text value={rec.resultHoldMs} onInput={(v) => set('resultHoldMs', v)} placeholder="4000" />
      </Field>

      <Check label="Hold firmware updates" checked={rec.fwHold}
             onChange={(v) => set('fwHold', v)}
             hint="Keeps this door on its current version when a new image is published." />

      {rec.fwHold && (
        <div class="consequence">
          Useful for a door that is awkward to reach — prove an image on an
          accessible door first, then release the hold.
        </div>
      )}

      {door.silentMinutes !== null && door.silentMinutes > 10 && (
        <div class="consequence warn">
          This door last checked in {door.silentMinutes} minutes ago, so it will
          not apply these changes until it reconnects.
        </div>
      )}

      <Actions>
        <button onClick={onClose} disabled={busy}>Cancel</button>
        <button class="primary" disabled={!valid || busy} onClick={submit}>
          {busy ? 'Saving…' : 'Save'}
        </button>
      </Actions>
    </Modal>
  );
}
