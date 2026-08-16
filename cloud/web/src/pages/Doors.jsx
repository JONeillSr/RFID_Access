import { useEffect, useState } from 'preact/hooks';
import { api, atLeast } from '../auth';
import { Table, Pill } from '../components/Table';

export function Doors({ notify }) {
  const [doors, setDoors] = useState([]);
  const [name, setName] = useState('');
  const [code, setCode] = useState('');

  useEffect(() => {
    api('/v1/admin/doors').then((r) => setDoors(r?.doors ?? [])).catch(notify);
  }, []);

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
          d.groups.join(', '),
          d.silentMinutes === null ? 'never'
            : d.silentMinutes > 10
              ? <span class="bad">{d.silentMinutes}m ago — not checking in</span>
              : (d.silentMinutes < 1 ? 'just now' : `${d.silentMinutes}m ago`),
          <>
            {!d.paired && <Pill kind="warn">unpaired</Pill>}
            {d.fwHold && <Pill>held</Pill>}
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
    </>
  );
}
