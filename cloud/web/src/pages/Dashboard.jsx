import { useEffect, useState } from 'preact/hooks';
import { api, atLeast } from '../auth';
import { Table, Tile, EventTime } from '../components/Table';
import { EnrollDialog } from '../components/EnrollDialog';
import { describeEvent, TAP, DOOR_FORCED } from '../events';

/**
 * Fleet health at a glance.
 *
 * The tiles are chosen around one question: is anything wrong that nobody would
 * otherwise notice? A door that stopped syncing still grants access from its
 * cached roster, so it looks perfectly healthy at the door itself -- this is the
 * only place that shows.
 */
export function Dashboard({ notify, flash }) {
  const [doors, setDoors] = useState([]);
  const [recent, setRecent] = useState([]);
  const [forced, setForced] = useState([]);
  const [unknown, setUnknown] = useState([]);
  const [people, setPeople] = useState([]);
  const [groups, setGroups] = useState([]);
  const [creds, setCreds] = useState([]);
  const [enrolling, setEnrolling] = useState(null);
  const [err, setErr] = useState(null);

  const load = async () => {
      try {
        const d = await api('/v1/admin/doors');
        setDoors(d?.doors ?? []);
        setUnknown((await api('/v1/admin/reports/unknown'))?.cards ?? []);

        // Needed only to enroll a card; fetched up front so the dialog opens
        // populated rather than showing an empty person list while it loads.
        if (atLeast('Operator')) {
          const [p, g, c] = await Promise.all([
            api('/v1/admin/people'), api('/v1/admin/groups'), api('/v1/admin/credentials'),
          ]);
          setPeople(p?.people ?? []);
          setGroups(g?.groups ?? []);
          setCreds(c?.credentials ?? []);
        }
        // Newest activity across the fleet: one request per door, which is fine
        // at this scale and avoids a cross-partition scan server-side.
        const per = await Promise.all(
          (d?.doors ?? []).map((x) =>
            api(`/v1/admin/reports/door?deviceId=${encodeURIComponent(x.deviceId)}`)
              .then((r) => r?.events ?? [])
              .catch(() => [])
          )
        );
        const all = per.flat().sort((a, b) => (a.at < b.at ? 1 : -1));
        setRecent(all.slice(0, 12));
        // Taken from the whole window (the report's default, 7 days) before the
        // recent list is cut to 12. A forced door must not drop off the dashboard
        // just because the door has been busy since.
        setForced(all.filter((e) => e.type === DOOR_FORCED));
      } catch (e) { setErr(e); }
  };
  useEffect(() => { load(); }, []);

  if (err) return <p class="bad">{err.message}</p>;

  const silent = doors.filter((d) => d.silentMinutes !== null && d.silentMinutes > 10);
  const unpaired = doors.filter((d) => !d.paired);
  const denials = recent.filter((e) => e.type === TAP && !e.granted);

  return (
    <>
      <h2>Dashboard</h2>
      <div class="grid">
        <Tile n={doors.length} label="doors" />
        <Tile n={silent.length} label="not checking in" alert={silent.length > 0} />
        <Tile n={unpaired.length} label="unpaired" alert={unpaired.length > 0} />
        <Tile n={unknown.length} label="unknown cards seen" alert={unknown.length > 0} />
        <Tile n={denials.length} label="recent denials" />
        <Tile n={forced.length} label="forced openings (7 days)" alert={forced.length > 0} />
      </div>

      {silent.length > 0 && (
        <div class="card">
          <h3>Doors not checking in</h3>
          <p class="muted">
            These still grant access from their cached rosters, so nothing looks
            wrong at the door. But they are not receiving roster changes, and a
            revoked fob may still work.
          </p>
          {silent.map((d) => (
            <div key={d.deviceId}>
              <strong>{d.name}</strong>{' '}
              <span class="bad">last seen {d.silentMinutes} minutes ago</span>
            </div>
          ))}
        </div>
      )}

      {forced.length > 0 && (
        <div class="card">
          <h3>Doors forced open</h3>
          <p class="muted">
            Each of these opened with no fob, no exit-button press and no unlock
            window. On a door that can be opened from inside by its handle, a
            normal exit looks the same until a request-to-exit sensor is fitted.
          </p>
          <Table
            headers={['When', 'Door']}
            rows={forced.map((e) => [<EventTime e={e} />, e.doorName])}
          />
        </div>
      )}

      {unknown.length > 0 && (
        <div class="card">
          <h3>Unknown cards seen</h3>
          <p class="muted">
            Cards that tapped but are not enrolled.{' '}
            {atLeast('Operator')
              ? 'Click one to give it to someone — the number is carried across, so it never has to be retyped.'
              : 'Enrolling a card requires the Operator role.'}
          </p>
          <Table
            headers={['Card', 'Taps', 'Last seen', 'Door', '']}
            rows={unknown.map((c) => [
              <code>{c.cred}</code>,
              c.taps,
              <EventTime e={c} />,
              c.door,
              atLeast('Operator') ? (
                <div class="rowacts">
                  <button class="small" onClick={() => setEnrolling(c)}>Enroll</button>
                </div>
              ) : null,
            ])}
          />
        </div>
      )}

      {enrolling && (
        <EnrollDialog
          card={enrolling} people={people} groups={groups} creds={creds}
          notify={notify}
          onClose={() => setEnrolling(null)}
          onDone={(text) => { setEnrolling(null); flash(text); load(); }}
        />
      )}

      <h3>Recent activity</h3>
      <Table
        headers={['When', 'Door', 'Person', 'Result']}
        rows={recent.map((e) => [
          <EventTime e={e} />,
          e.doorName,
          // The detail field is a card number only for taps; other events use it
          // for a version, a duration or a setting, which describeEvent shows.
          e.personName ?? (e.type === TAP && e.cred ? <code>{e.cred}</code> : '—'),
          describeEvent(e),
        ])}
      />
    </>
  );
}
