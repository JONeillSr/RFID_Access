import { useEffect, useState } from 'preact/hooks';
import { api, atLeast } from '../auth';
import { Table, Pill } from '../components/Table';
import { Modal, Field, Text, Check, GroupPicker, Actions, useRecord } from '../components/Form';

/**
 * How long a restart request stays deliverable. Mirrors RESTART_REQUEST_TTL_MS
 * in cloud/shared/types.ts, which the web app does not import -- change both.
 */
const RESTART_TTL_MS = 15 * 60 * 1000;

/** 'pending' while the door may still collect it, 'lapsed' after, else null. */
function restartState(d) {
  if (!d.restartReqAt) return null;
  return Date.now() - Date.parse(d.restartReqAt) < RESTART_TTL_MS ? 'pending' : 'lapsed';
}

/** Mirrors RELEASE_REQUEST_TTL_MS in cloud/shared/types.ts -- change both. */
const RELEASE_TTL_MS = 60 * 1000;

/**
 * The latest remote release: 'done' (confirmed by the door), 'pending' (inside
 * its minute), 'lapsed' (the door never confirmed it), or null.
 */
function releaseState(d) {
  if (!d.releaseReqAt) return null;
  if (d.releaseDone) return 'done';
  return Date.now() - Date.parse(d.releaseReqAt) < RELEASE_TTL_MS ? 'pending' : 'lapsed';
}

/** Only recent outcomes are worth a pill; last week's release is history. */
const recent = (iso, mins) => iso && Date.now() - Date.parse(iso) < mins * 60000;

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

  // While a restart is outstanding, refresh so the pill clears by itself when
  // the door confirms it -- otherwise "pending" sits there until someone reloads.
  const anyPending = doors.some((d) => restartState(d) === 'pending');
  // A release is someone waiting at a door, so it is watched closely: the door
  // confirms within seconds, and the person who pressed the button should see it.
  const releasing = doors.some((d) => releaseState(d) === 'pending');
  useEffect(() => {
    if (!anyPending && !releasing) return;
    const id = setInterval(load, releasing ? 2000 : 15000);
    return () => clearInterval(id);
  }, [anyPending, releasing]);

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

  const restart = async (d) => {
    const silent = d.silentMinutes !== null && d.silentMinutes > 10;
    const msg =
      `Restart ${d.name}?

` +
      'It restarts when it next checks in, usually within a minute, and waits ' +
      'for any release in progress to finish. Fobs will not open it until it ' +
      'is back up a few seconds later; an unlock schedule resumes on its own.' +
      (silent
        ? `

This door last checked in ${d.silentMinutes} minutes ago. If it ` +
          'does not reconnect within 15 minutes the request lapses rather than ' +
          'restarting it whenever it next comes back.'
        : '');
    if (!confirm(msg)) return;
    try {
      await api('/v1/admin/doors/restart', {
        method: 'POST',
        body: JSON.stringify({ deviceId: d.deviceId }),
      });
      await load();
      flash(`Restart requested for ${d.name} — it restarts on its next sync.`);
    } catch (e) { notify(e); }
  };

  const release = async (d) => {
    if (!confirm(`Release ${d.name} now?\n\nIt unlocks for the usual relay hold, as if a fob ` +
                 'had been tapped. This is logged under your name.')) return;
    try {
      await api('/v1/admin/doors/release', {
        method: 'POST',
        body: JSON.stringify({ deviceId: d.deviceId }),
      });
      await load();
      flash(`Releasing ${d.name}…`);
    } catch (e) { notify(e); }
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
        cards
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
            {releaseState(d) === 'pending' && <Pill kind="warn">releasing…</Pill>}
            {releaseState(d) === 'done' && recent(d.releaseDoneAt, 10) && (
              <span title={`Released by ${d.releaseReqBy || 'an operator'} at ${new Date(d.releaseDoneAt).toLocaleString()}`}>
                <Pill kind="ok">released</Pill>
              </span>
            )}
            {releaseState(d) === 'lapsed' && recent(d.releaseReqAt, 10) && (
              <span title="The door did not confirm the release within a minute, so it was not opened.">
                <Pill kind="warn">release not confirmed</Pill>
              </span>
            )}
            {restartState(d) === 'pending' && (
              <span title={`Requested by ${d.restartReqBy || 'an admin'} at ${new Date(d.restartReqAt).toLocaleString()}`}>
                <Pill kind="warn">restart pending</Pill>
              </span>
            )}
            {restartState(d) === 'lapsed' && (
              <span title="The door did not check in within 15 minutes of the request, so it was not restarted.">
                <Pill kind="warn">restart lapsed</Pill>
              </span>
            )}
            <div class="rowacts">
              {atLeast('Operator') && d.paired && d.hasRemoteRelease &&
               d.silentMinutes !== null && d.silentMinutes <= 2 && releaseState(d) !== 'pending' && (
                <button class="small" onClick={() => release(d)}>Release</button>
              )}
              <button class="small" onClick={() => showRoster(d)}>Who gets in?</button>
              {canEdit && <button class="small" onClick={() => setEditing({ ...d })}>Edit</button>}
              {canEdit && d.paired && restartState(d) !== 'pending' && (
                <button class="small" onClick={() => restart(d)}>Restart</button>
              )}
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
    readerMode: cfg.readerMode ?? '',
    doorContact: cfg.doorContact === true,
    doorHeldSec: cfg.doorHeldSec ?? '',
    openSetupPortal: cfg.openSetupPortal === true,
  });

  // Whether the board can take a contact at all. The C6 and C3 have no free
  // input, so offering the setting there would promise something the door
  // silently ignores. Doors on firmware before 2.8.2 do not report it.
  const canContact = door.hasDoorContact === true;

  // What the door reports running, which is not what it has been configured
  // with until it restarts: the format decides which interrupts it attaches.
  const running = door.readerMode || '';
  const readerName = (m) => (m === 'wiegand' ? 'Wiegand' : m === 'cnd' ? 'Clock & Data' : 'unknown');
  const readerPending = rec.readerMode && running && rec.readerMode !== running;

  const num = (v) => (v === '' || v === null ? null : Number(v));
  const relayOk = rec.relayHoldMs === '' || (Number.isFinite(num(rec.relayHoldMs)) && num(rec.relayHoldMs) > 0);
  const resultOk = rec.resultHoldMs === '' || (Number.isFinite(num(rec.resultHoldMs)) && num(rec.resultHoldMs) > 0);
  const heldOk = rec.doorHeldSec === '' ||
    (Number.isInteger(num(rec.doorHeldSec)) && num(rec.doorHeldSec) >= 0 && num(rec.doorHeldSec) <= 3600);
  const valid = rec.name.trim() && relayOk && resultOk && heldOk;

  const submit = () => {
    // Only send config keys that have a value, and preserve anything already in
    // the stored blob that this form does not know about -- the POST replaces
    // the whole thing.
    const config = { ...cfg };
    if (rec.relayHoldMs === '') delete config.relayHoldMs; else config.relayHoldMs = num(rec.relayHoldMs);
    if (rec.resultHoldMs === '') delete config.resultHoldMs; else config.resultHoldMs = num(rec.resultHoldMs);
    if (rec.readerMode === '') delete config.readerMode; else config.readerMode = rec.readerMode;
    config.doorContact = rec.doorContact;
    config.openSetupPortal = rec.openSetupPortal;
    if (rec.doorHeldSec === '') delete config.doorHeldSec; else config.doorHeldSec = num(rec.doorHeldSec);

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

      <Field label="Reader format"
             hint={`The line format this door's reader speaks. Applied when the door next restarts${
               running ? `; running now: ${readerName(running)}` : ''
             }. Leave as the device setting to manage it at the door instead.`}>
        <select value={rec.readerMode} onChange={(e) => set('readerMode', e.currentTarget.value)}>
          <option value="">— leave to the device —</option>
          <option value="cnd">Clock &amp; Data (Paxton default)</option>
          <option value="wiegand">Wiegand</option>
        </select>
      </Field>

      {readerPending && (
        <div class="consequence warn">
          This door is still reading <strong>{readerName(running)}</strong>. The
          change applies at its next restart; until then nothing changes at the
          door. Save, then use <strong>Restart</strong> in the doors list to
          apply it now.
        </div>
      )}

      {rec.readerMode === 'wiegand' && (
        <div class="consequence warn">
          A reader that is not actually sending Wiegand will stop working
          entirely — every fob denied, with the error count on the door's status
          page climbing. Change it back here if that happens.
        </div>
      )}

      {canContact ? (
        <>
          <Check label="Door contact fitted" checked={rec.doorContact}
                 onChange={(v) => set('doorContact', v)}
                 hint="A magnetic reed contact on the frame, closed when the door is shut. Reports a door forced open or held open." />

          {rec.doorContact && (
            <Field label="Held-open alert (seconds)"
                   hint="How long the door may stay open after a release ends before it is reported held open. 0 turns the held-open alert off; forced alerts still work."
                   error={heldOk ? null : 'Must be 0-3600.'}>
              <Text value={rec.doorHeldSec} onInput={(v) => set('doorHeldSec', v)} placeholder="60" />
            </Field>
          )}

          {rec.doorContact && !cfg.doorContact && (
            <div class="consequence warn">
              Turn this on only once a contact is actually wired. An unconnected
              input reads as an open door, so the first thing you will see is a
              forced-open alert that is not real.
            </div>
          )}
        </>
      ) : (
        <div class="consequence">
          This door's board has no free input for a door contact, so forced-open
          and held-open detection are unavailable on it.
        </div>
      )}

      <Check label="Open the setup network at the next restart" checked={rec.openSetupPortal}
             onChange={(v) => set('openSetupPortal', v)}
             hint="For moving this door to a WiFi network the current one cannot reach. It keeps working and stays online throughout." />

      {rec.openSetupPortal && !cfg.openSetupPortal && (
        <div class="consequence warn">
          Nothing happens until the door restarts (save, then use{' '}
          <strong>Restart</strong> in the doors list), and then someone has to be
          within WiFi range of it. The setup network is <strong>open</strong> —
          anyone in range can reach the door while it is up — so it closes itself
          after 30 minutes. To move a door to a network you can already reach,
          use the WiFi fields on the door's own setup page instead: it tries the
          new network and returns to this one if it cannot join.
        </div>
      )}

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
