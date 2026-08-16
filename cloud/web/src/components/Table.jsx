export function Table({ headers, rows }) {
  return (
    <table>
      <thead><tr>{headers.map((h) => <th key={h}>{h}</th>)}</tr></thead>
      <tbody>
        {rows.map((cells, i) => (
          <tr key={i}>{cells.map((c, j) => <td key={j}>{c}</td>)}</tr>
        ))}
      </tbody>
    </table>
  );
}

export function Pill({ kind, children }) {
  return <span class={`pill ${kind ?? ''}`}>{children}</span>;
}

/** Big-number tile for the dashboard. `alert` draws attention without shouting. */
export function Tile({ n, label, alert }) {
  return (
    <div class={`tile ${alert ? 'alert' : ''}`}>
      <div class="n">{n}</div>
      <div class="l">{label}</div>
    </div>
  );
}
