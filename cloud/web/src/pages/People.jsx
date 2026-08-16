import { useEffect, useState } from 'preact/hooks';
import { api } from '../auth';
import { Table, Pill } from '../components/Table';

export function People({ notify }) {
  const [people, setPeople] = useState([]);
  useEffect(() => {
    api('/v1/admin/people').then((r) => setPeople(r?.people ?? [])).catch(notify);
  }, []);

  return (
    <>
      <h2>People</h2>
      <Table
        headers={['Person', 'Groups', 'Fobs', 'Status']}
        rows={people.map((p) => [
          <div>
            <strong>{p.name}</strong>
            {p.email && <div class="muted">{p.email}</div>}
          </div>,
          p.groups.join(', '),
          (p.credentials ?? []).length
            ? (p.credentials ?? []).map((c) => (
                <div key={c.credId}>
                  <code>{c.number}</code>
                  {c.label && <span class="muted"> {c.label}</span>}
                  {!c.active && <Pill kind="warn">inactive</Pill>}
                </div>
              ))
            : <span class="muted">no fobs</span>,
          p.active ? <Pill kind="ok">active</Pill> : <Pill kind="warn">inactive</Pill>,
        ])}
      />
    </>
  );
}
