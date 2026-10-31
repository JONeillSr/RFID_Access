/**
 * `cards` lets a narrow screen show each row as a card instead of a table too
 * wide to read: the first cell becomes the card's title, the others are labelled
 * with their header, and an unheaded last cell (row actions) sits along the
 * bottom. On a wide screen it is an ordinary table; the switch is pure CSS.
 */
export function Table({ headers, rows, cards }) {
  const cellProps = (j) => {
    if (!cards) return {};
    if (j === 0) return { class: 'title' };
    if (!headers[j]) return { class: 'acts' };
    return { 'data-label': headers[j] };
  };
  return (
    <table class={cards ? 'cards' : undefined}>
      <thead><tr>{headers.map((h) => <th key={h}>{h}</th>)}</tr></thead>
      <tbody>
        {rows.map((cells, i) => (
          <tr key={i}>{cells.map((c, j) => <td key={j} {...cellProps(j)}>{c}</td>)}</tr>
        ))}
      </tbody>
    </table>
  );
}

export function Pill({ kind, children }) {
  return <span class={`pill ${kind ?? ''}`}>{children}</span>;
}

/**
 * When an event happened, stated only as precisely as it is actually known.
 *
 *   observed   the door's clock               a time
 *   derived    no clock, dated from its boot   a time marked ≈
 *   unknown    no clock, no way to date it    a WINDOW, never a time
 *
 * An unknown event's `at` is only where it was filed so it sorts in sequence.
 * Rendering it as a time is exactly the failure that put sixteen days of taps in
 * the future, so it is never shown as one. Every page renders event times
 * through here, so they cannot drift apart.
 */
export function EventTime({ e }) {
  const fmt = (iso) => new Date(iso).toLocaleString();
  if (e.timeUnknown) {
    return (
      <span title="The door had no clock when this happened, so only a window is known.">
        <span class="muted">time unknown</span> <Pill kind="warn">?</Pill>
        <div class="muted" style="font-size:12px">
          {e.timeNotBefore
            ? <>between {fmt(e.timeNotBefore)} and {fmt(e.timeNotAfter)}</>
            : <>before {fmt(e.timeNotAfter)}</>}
        </div>
      </span>
    );
  }
  return (
    <span>
      {fmt(e.at)}
      {e.timeApprox && (
        <span title="Recorded before the door's clock was set; dated from when it started.">
          <Pill>≈</Pill>
        </span>
      )}
    </span>
  );
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
