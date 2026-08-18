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
  // Firmware history asks a different question from the access reports -- about
  // the doors themselves rather than who went through them -- so it is a mode
  // rather than another filter on the same query.
  const view = q.get('view') ?? 'access';

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
    const from = new Date(Date.now() - Number(days) * 86400000).toISOString();
    const params = new URLSearchParams({ from });
    let path;

    if (view === 'firmware') {
      // Fleet-wide by default: "has every door taken this release?" is the
      // question, and it cannot be answered one door at a time.
      if (deviceId) params.set('deviceId', deviceId);
      path = `/v1/admin/reports/firmware?${params}`;
    } else if (personId) {
      params.set('personId', personId);
      if (deviceId) params.set('deviceId', deviceId);
      path = `/v1/admin/reports/person?${params}`;
    } else if (deviceId) {
      params.set('deviceId', deviceId);
      path = `/v1/admin/reports/door?${params}`;
    } else {
      setData(null);
      return;
    }

    setBusy(true);
    api(path).then(setData).catch(notify).finally(() => setBusy(false));
  }, [view, personId, deviceId, days]);

  return (
    <>
      <h2>Reports</h2>
      <div class="card">
        <select value={view} onChange={(e) => setParam('view', e.currentTarget.value)}>
          <option value="access">Access history</option>
          <option value="firmware">Firmware history</option>
        </select>
        {view === 'access' && (
          <select value={personId} onChange={(e) => setParam('personId', e.currentTarget.value)}>
            <option value="">— any person —</option>
            {people.map((p) => <option key={p.personId} value={p.personId}>{p.name}</option>)}
          </select>
        )}
        <select value={deviceId} onChange={(e) => setParam('deviceId', e.currentTarget.value)}>
          <option value="">{view === 'firmware' ? '— all doors —' : '— any door —'}</option>
          {doors.map((d) => <option key={d.deviceId} value={d.deviceId}>{d.name}</option>)}
        </select>
        <select value={days} onChange={(e) => setParam('days', e.currentTarget.value)}>
          {[1, 7, 30, 90].map((n) => (
            <option key={n} value={n}>last {n} day{n > 1 ? 's' : ''}</option>
          ))}
        </select>
      </div>

      {busy && <p class="muted">Loading…</p>}
      {!busy && view === 'firmware' && data && <Firmware data={data} />}
      {!busy && view === 'access' && !data && <p class="muted">Choose a person or a door.</p>}
      {!busy && view === 'access' && data && (
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

/**
 * Firmware history.
 *
 * Leads with what every door is running right now, because that is the question
 * being asked — the event list is how it got there. A door held back or stuck a
 * version behind is the finding; the timeline is the evidence.
 */
function Firmware({ data }) {
  const doors = data.doors ?? [];
  const versions = [...new Set(doors.map((d) => d.running).filter(Boolean))];

  return (
    <>
      <h3>What each door is running</h3>
      <Table
        headers={['Door', 'Running', 'Updates in window', 'State']}
        rows={doors.map((d) => [
          <div><strong>{d.doorName}</strong><div class="muted">{d.deviceId}</div></div>,
          <code>{d.running || '—'}</code>,
          d.events.length,
          <>
            {d.fwHold && <Pill kind="warn">held</Pill>}
            {versions.length > 1 && d.running !== versions[0] && !d.fwHold &&
              <Pill kind="warn">behind</Pill>}
            {d.events.some((e) => !e.succeeded) && <Pill kind="warn">had a failure</Pill>}
          </>,
        ])}
      />

      {versions.length > 1 && (
        <div class="card">
          <strong>The fleet is not on one version.</strong>{' '}
          <span class="muted">
            Running: {versions.map((v) => <code key={v}>{v}</code>).reduce((a, b) => [a, ', ', b])}.
            That is expected during a staged rollout and while a door carries a
            firmware hold — but a door left behind unintentionally stops getting
            fixes, and nothing else reports it.
          </span>
        </div>
      )}

      <h3>Update history</h3>
      {data.events.length === 0
        ? <p class="muted">No firmware activity in this window.</p>
        : <>
            <p class="muted">
              {data.events.length} event(s)
              {data.failures > 0 && <> · <span class="bad">{data.failures} failed</span></>}
              {data.unconfirmed > 0 && <> · <span class="bad">{data.unconfirmed} unconfirmed</span></>}
            </p>
            <Table
              headers={['When', 'Door', 'Change', 'Result', 'Came back']}
              rows={data.events.map((e) => [
                <span>
                  {new Date(e.at).toLocaleString()}
                  {e.timeApprox && <Pill>≈</Pill>}
                </span>,
                e.doorName,
                e.change
                  ? <span><code>{e.from}</code> → <code>{e.to}</code></span>
                  : <span class="muted">—</span>,
                e.succeeded
                  ? <Pill kind="ok">updated</Pill>
                  : <Pill kind="warn">FAILED</Pill>,
                // The evidence that matters. A door can report a successful
                // flash and never start again; only the boot afterwards proves
                // the image runs.
                !e.succeeded
                  ? <span class="muted">did not reboot</span>
                  : e.confirmed
                    ? <span class="muted">{new Date(e.rebootedAt).toLocaleTimeString()}</span>
                    : <span class="bad">no boot seen</span>,
              ])}
            />
            <p class="muted">{data.note}</p>
          </>}
    </>
  );
}
