/**
 * Form primitives for the admin write surface.
 *
 * ⚠️ THE API UPSERTS WITH `Replace`, NOT `Merge`.
 *
 * `POST /v1/admin/people` and `/v1/admin/credentials` overwrite the whole row.
 * A field the form does not send is not left alone — it is **erased**. Editing a
 * fob to change its label would silently clear `validFrom`/`validTo` if the form
 * only sent the fields it displays.
 *
 * So every edit form here starts from the complete record fetched from the API
 * and posts it back whole. `useRecord` exists to make that the path of least
 * resistance: seed it with the original object, edit fields on top, submit the
 * merged result. Do not hand-build a partial body.
 *
 * (Doors are the exception — that route uses `Merge` server-side, because
 * `keyHash` and `pairedAt` must survive an edit and are never sent by any UI.)
 */
import { useEffect, useRef, useState } from 'preact/hooks';

/**
 * Modal dialog. Uses <dialog> for focus trapping and Escape handling rather than
 * reimplementing either badly.
 */
export function Modal({ title, onClose, children }) {
  const ref = useRef(null);

  useEffect(() => {
    const d = ref.current;
    if (d && !d.open) d.showModal();
    const cancel = (e) => { e.preventDefault(); onClose(); };
    d?.addEventListener('cancel', cancel);
    return () => d?.removeEventListener('cancel', cancel);
  }, []);

  return (
    <dialog ref={ref} class="modal" onClick={(e) => { if (e.target === ref.current) onClose(); }}>
      <div class="modal-body">
        <div class="modal-head">
          <h3>{title}</h3>
          <button class="icon" onClick={onClose} aria-label="Close">×</button>
        </div>
        {children}
      </div>
    </dialog>
  );
}

/** Labelled input with optional hint and per-field error. */
export function Field({ label, hint, error, children }) {
  return (
    <label class="field">
      <span class="lbl">{label}</span>
      {children}
      {hint && !error && <span class="hint">{hint}</span>}
      {error && <span class="bad hint">{error}</span>}
    </label>
  );
}

export function Text({ value, onInput, ...rest }) {
  return <input type="text" value={value ?? ''} onInput={(e) => onInput(e.currentTarget.value)} {...rest} />;
}

export function Check({ label, checked, onChange, hint }) {
  return (
    <label class="check">
      <input type="checkbox" checked={!!checked} onChange={(e) => onChange(e.currentTarget.checked)} />
      <span>{label}</span>
      {hint && <span class="hint">{hint}</span>}
    </label>
  );
}

/**
 * Group multi-select as checkboxes.
 *
 * A <select multiple> is the compact choice but it is genuinely hard to use --
 * ctrl-click to deselect is not discoverable, and getting this wrong means
 * accidentally removing someone's access. Checkboxes make the current state
 * readable at a glance, which is what matters for a permissions control.
 */
export function GroupPicker({ all, selected, onChange }) {
  const toggle = (id, on) =>
    onChange(on ? [...selected, id] : selected.filter((g) => g !== id));

  if (!all.length) {
    return <span class="muted">No groups defined yet — an Admin must create one first.</span>;
  }
  return (
    <div class="picker">
      {all.map((g) => (
        <Check
          key={g.groupId}
          label={g.name || g.groupId}
          checked={selected.includes(g.groupId)}
          onChange={(on) => toggle(g.groupId, on)}
        />
      ))}
    </div>
  );
}

export function Actions({ children }) {
  return <div class="actions">{children}</div>;
}

/**
 * Editable copy of a record, preserving every field the form never shows.
 *
 * `original` is spread in first so unshown fields (validFrom, validTo, and
 * anything added to the API later) ride along untouched into the Replace.
 */
export function useRecord(original) {
  const [rec, setRec] = useState({ ...original });
  const set = (k, v) => setRec((r) => ({ ...r, [k]: v }));
  return [rec, set, setRec];
}

/**
 * Suggest an id in the house style, e.g. "Avery O'Neill" -> "averyo".
 *
 * Only ever a SUGGESTION for new records. Ids are permanent: the API keys rows
 * by them, so "renaming" one writes a new row and abandons the old, which keeps
 * granting access. The edit form therefore locks the field.
 */
export function suggestPersonId(name, taken = []) {
  const parts = String(name).toLowerCase().replace(/[^a-z\s]/g, '').split(/\s+/).filter(Boolean);
  if (!parts.length) return '';
  let base = parts.length > 1 ? parts[0] + parts[parts.length - 1][0] : parts[0];
  if (!taken.includes(base)) return base;
  for (let n = 2; n < 100; n++) if (!taken.includes(base + n)) return base + n;
  return base;
}

/** Fob ids look like "jtc-avery-keychain1". */
export function suggestCredId(personName, kind = 'fob', taken = []) {
  const first = String(personName).toLowerCase().replace(/[^a-z\s]/g, '').split(/\s+/)[0] || 'card';
  for (let n = 1; n < 100; n++) {
    const id = `jtc-${first}-${kind}${n}`;
    if (!taken.includes(id)) return id;
  }
  return `jtc-${first}-${kind}`;
}
