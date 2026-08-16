import { useEffect, useState } from 'preact/hooks';
import { useLocation, useSearch } from 'wouter-preact';
import { api } from '../auth';
import { Table, Pill } from '../components/Table';
import { describeEvent } from '../events';

/**
 * Filters live in the URL, so a report can be bookmarked or sent to someone —
 * the main thing the pre-build version could not do.
 */
export function Reports({ notify }) {
  const search = useSearch();
  const [, navigate] = useLocation();
  const q = new URLSearchParams(search);

  const [people, setPeople] = useState([]);
  const [doors, setDoors] = useState([]);
  const [data, setData] = useState(null);
  const [busy, setBusy] = useState(false);

  const personId = q.get('personId') ?? '';
  const deviceId = q.get('deviceId') ?? '';
  const days = q.get('days') ?? '7';

  useEffect(() => {
    Promise.all([api('/v1/admin/people'), api('/v1/admin/doors')])
      .then(([p, d]) => { setPeople(p?.people ?? []); setDoors(d?.doors ?? []); })
      .catch(notify);
  }, []);

  const setParam = (k, v) => {
    const next = new URLSearchParams(search);
    v ? next.set(k, v) : next.delete(k);
    navigate(`/reports?${next}`, { replace: true });
  };

  useEffect(() => {
    if (!personId && !deviceId) { setData(null); return; }
    setBusy(true);
    const from = new Date(Date.now() - Number(days) * 86400000).toISOString();
    const params = new URLSearchParams({ from });
    let path;
    if (personId) {
      params.set('personId', personId);
      if (deviceId) params.set('deviceId', deviceId);
      path = `/v1/admin/reports/person?${params}`;
    } else {
      params.set('deviceId', deviceId);
      path = `/v1/admin/reports/door?${params}`;
    }
    api(path).then(setData).catch(notify).finally(() => setBusy(false));
  }, [personId, deviceId, days]);

  return (
    <>
      <h2>Reports</h2>
      <div class="card">
        <select value={personId} onChange={(e) => setParam('personId', e.currentTarget.value)}>
          <option value="">— any person —</option>
          {people.map((p) => <option key={p.personId} value={p.personId}>{p.name}</option>)}
        </select>
        <select value={deviceId} onChange={(e) => setParam('deviceId', e.currentTarget.value)}>
          <option value="">— any door —</option>
          {doors.map((d) => <option key={d.deviceId} value={d.deviceId}>{d.name}</option>)}
        </select>
        <select value={days} onChange={(e) => setParam('days', e.currentTarget.value)}>
          {[1, 7, 30, 90].map((n) => (
            <option key={n} value={n}>last {n} day{n > 1 ? 's' : ''}</option>
          ))}
        </select>
      </div>

      {busy && <p class="muted">Loading…</p>}
      {!busy && !data && <p class="muted">Choose a person or a door.</p>}
      {!busy && data && (
        data.events.length === 0
          ? <p class="muted">No events in this window.</p>
          : <>
              <p class="muted">
                {data.count} event(s){data.truncated ? ' (truncated)' : ''}
              </p>
              <Table
                headers={['When', 'Door', 'Person', 'Result']}
                rows={data.events.map((e) => [
                  <span>
                    {new Date(e.at).toLocaleString()}
                    {/* Logged before the device clock was trusted: derived, not
                        observed. Saying so beats presenting a guess. */}
                    {e.timeApprox && <Pill>≈</Pill>}
                  </span>,
                  e.doorName,
                  e.personName ?? (e.cred ? <code>{e.cred}</code> : '—'),
                  describeEvent(e),
                ])}
              />
            </>
      )}
    </>
  );
}
