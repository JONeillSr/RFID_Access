/**
 * Revoke door access when an Entra account is disabled or deleted.
 *
 * WHAT THIS CLOSES
 * The commonest access-control failure is not a forced door: it is someone
 * leaving, HR disabling their account, and the fob continuing to work because
 * nobody told the door system. This makes offboarding a single action in the
 * place it already happens.
 *
 * The delivery half already existed. effectiveRoster() skips inactive people, so
 * setting active = false reaches every door on its next sync with no device
 * change at all. This function only has to decide when to set it.
 *
 * IT ONLY EVER REVOKES
 * It never reactivates, even when Entra says the account is fine again.
 * Restoring building access stays a deliberate act with a human's name against
 * it, and a one-way job can never silently undo a manual revocation -- a fob
 * pulled for being lost, or for an incident, while the account itself is
 * untouched.
 *
 * IT FAILS OPEN, LOUDLY
 * If Graph is unreachable, the permission is missing, or a token cannot be had,
 * this changes NOTHING and records the failure. The alternative -- failing
 * closed -- turns a Graph outage into a building nobody can enter, which is a
 * far worse Tuesday than a revocation arriving late.
 *
 * But failing open silently would be worse than not having the feature: the
 * guarantee would stop holding while everything looked fine. So the last
 * SUCCESSFUL sweep is recorded and surfaced in the admin app, on the same
 * reasoning as the device's NTP line -- "has checked" and "is checking" are
 * different claims, and only one of them is a control.
 */

import { app, InvocationContext, Timer } from '@azure/functions';
import { TableClient } from '@azure/data-tables';
import { DefaultAzureCredential } from '@azure/identity';
import { bumpRosterRev } from '../storage';

const account = process.env.STORAGE_ACCOUNT_NAME!;
const endpoint = `https://${account}.table.core.windows.net`;
const credential = new DefaultAzureCredential();
const t = (name: string) => new TableClient(endpoint, name, credential);

const GRAPH = 'https://graph.microsoft.com/v1.0';
const SCOPE = 'https://graph.microsoft.com/.default';

export interface SweepResult {
  ranAt: string;
  ok: boolean;
  checked: number;
  revoked: string[];
  /** Marked 'entra' but with no object id: covered on paper, not in fact. */
  unlinked: string[];
  error?: string;
}

/** One person's account state, or a reason we could not establish it. */
type Verdict =
  | { kind: 'enabled' }
  | { kind: 'disabled' }
  | { kind: 'deleted' }
  | { kind: 'unknown'; why: string };

async function checkAccount(oid: string, token: string): Promise<Verdict> {
  let res: Response;
  try {
    res = await fetch(`${GRAPH}/users/${encodeURIComponent(oid)}?$select=id,accountEnabled`, {
      headers: { Authorization: `Bearer ${token}` },
    });
  } catch (e) {
    // Network-level failure. NOT evidence about the account.
    return { kind: 'unknown', why: `graph unreachable: ${(e as Error).message}` };
  }

  if (res.status === 404) {
    // A real answer from Graph: no such user. Deleted counts at least as
    // strongly as disabled. This is only reached because the request itself
    // succeeded -- a transport failure above never lands here, so a network
    // blip cannot be mistaken for a deleted account.
    return { kind: 'deleted' };
  }
  if (res.status === 401 || res.status === 403) {
    // Missing consent or an expired token. Says nothing about the person.
    return { kind: 'unknown', why: `graph refused (${res.status}) - check User.Read.All consent` };
  }
  if (!res.ok) {
    return { kind: 'unknown', why: `graph returned ${res.status}` };
  }

  const body = (await res.json()) as { accountEnabled?: boolean };
  // Absent is not false. If Graph did not tell us, we do not know, and not
  // knowing must never revoke.
  if (typeof body.accountEnabled !== 'boolean') {
    return { kind: 'unknown', why: 'graph omitted accountEnabled' };
  }
  return body.accountEnabled ? { kind: 'enabled' } : { kind: 'disabled' };
}

/** Bounds on the configurable interval, and the default when unset. */
export const SWEEP_MIN_MINUTES = 5;
export const SWEEP_MAX_MINUTES = 24 * 60;
export const SWEEP_DEFAULT_MINUTES = 15;

export interface SweepConfig {
  intervalMinutes: number;
  enabled: boolean;
}

export async function getSweepConfig(): Promise<SweepConfig> {
  try {
    const row: any = await t('Meta').getEntity('meta', 'entraSweepConfig');
    const raw = Number(row.intervalMinutes);
    const n = Number.isFinite(raw) ? raw : SWEEP_DEFAULT_MINUTES;
    return {
      // Clamp on read as well as on write: a value edited straight into the
      // table by hand should not be able to hammer Graph or disable the control
      // by being absurd.
      intervalMinutes: Math.min(SWEEP_MAX_MINUTES, Math.max(SWEEP_MIN_MINUTES, n)),
      enabled: row.enabled !== false,
    };
  } catch {
    return { intervalMinutes: SWEEP_DEFAULT_MINUTES, enabled: true };
  }
}

export async function runSweep(ctx: InvocationContext): Promise<SweepResult> {
  const ranAt = new Date().toISOString();
  const result: SweepResult = { ranAt, ok: false, checked: 0, revoked: [], unlinked: [] };

  // People opted in to Entra management. Anything not saying 'entra' is a
  // guest, contractor or one-off and is left entirely alone.
  const managed: any[] = [];
  for await (const p of t('People').listEntities<any>()) {
    if (String(p.managedBy ?? 'manual') !== 'entra') continue;
    managed.push(p);
  }

  // Marked as Entra-governed but never linked. These are the dangerous ones:
  // they read as covered and are not. Reported, never revoked -- revoking on an
  // administrative omission would lock people out for a data-entry mistake.
  const linked = managed.filter((p) => {
    if (String(p.entraObjectId ?? '').trim()) return true;
    result.unlinked.push(String(p.name ?? p.rowKey));
    return false;
  });

  if (linked.length === 0) {
    result.ok = true;
    await recordSweep(result);
    ctx.log(`entra sweep: nothing linked to check (${result.unlinked.length} unlinked)`);
    return result;
  }

  let token: string;
  try {
    const t0 = await credential.getToken(SCOPE);
    if (!t0?.token) throw new Error('no token returned');
    token = t0.token;
  } catch (e) {
    // No token: change nothing, and let the staleness clock keep running.
    result.error = `could not obtain a Graph token: ${(e as Error).message}`;
    await recordSweep(result);
    ctx.error(`entra sweep: ${result.error}`);
    return result;
  }

  let changed = false;
  let unknowns = 0;

  for (const p of linked) {
    const verdict = await checkAccount(String(p.entraObjectId), token);
    result.checked++;

    if (verdict.kind === 'unknown') {
      unknowns++;
      ctx.warn(`entra sweep: ${p.rowKey}: ${verdict.why}`);
      continue;                       // never revoke on not-knowing
    }
    if (verdict.kind === 'enabled') continue;   // one-way: never reactivates

    if (p.active === false) continue;           // already revoked, nothing to do

    const why = verdict.kind === 'deleted'
      ? 'Entra account deleted'
      : 'Entra account disabled';

    await t('People').upsertEntity(
      {
        partitionKey: 'person',
        rowKey: String(p.rowKey),
        active: false,
        deactivatedReason: why,
        deactivatedAt: ranAt,
      },
      'Merge'                          // Merge, not Replace: leave groups and the rest intact
    );
    result.revoked.push(`${p.name ?? p.rowKey} (${why})`);
    changed = true;
    ctx.log(`entra sweep: revoked ${p.rowKey} - ${why}`);
  }

  // Bump ONCE, after the pass. Forgetting this is the failure where the database
  // agrees the person is gone and every door still lets them in.
  if (changed) {
    const rev = await bumpRosterRev();
    ctx.log(`entra sweep: roster rev -> ${rev}`);
  }

  // A sweep that could not establish some accounts is not a clean sweep. Saying
  // otherwise would let the staleness clock reset while the guarantee was only
  // partly enforced.
  result.ok = unknowns === 0;
  if (unknowns > 0) result.error = `${unknowns} account(s) could not be checked`;

  await recordSweep(result);
  ctx.log(`entra sweep: checked ${result.checked}, revoked ${result.revoked.length}, ` +
          `unlinked ${result.unlinked.length}, ok=${result.ok}`);
  return result;
}

/** Persist the outcome so the admin app can show how long since a clean run. */
async function recordSweep(r: SweepResult): Promise<void> {
  const row: any = {
    partitionKey: 'meta',
    rowKey: 'entraSweep',
    lastRunAt: r.ranAt,
    lastOk: r.ok,
    checked: r.checked,
    revoked: r.revoked.join('; '),
    unlinked: r.unlinked.join('; '),
    error: r.error ?? '',
  };
  // Only a clean run moves this. It is what "how long since this control was
  // actually enforced" is measured from, so a failed run must not reset it.
  if (r.ok) row.lastSuccessAt = r.ranAt;
  await t('Meta').upsertEntity(row, 'Merge');
}

/**
 * A fixed HEARTBEAT, not the sweep interval.
 *
 * A timer trigger's CRON is baked in at deploy time. It can be read from an app
 * setting, but app settings are the one thing a Bicep deployment overwrites
 * wholesale -- see the warning in cloud/infra/README.md -- so an interval
 * configured that way would silently revert to the template's value on the next
 * unrelated deployment. A security control that quietly changes its own cadence
 * is worse than one with a fixed cadence.
 *
 * So the timer ticks on a fixed short period and the POLICY lives in the table,
 * editable from the admin app, taking effect on the next tick with no redeploy.
 * The cost is that the interval cannot be finer than this heartbeat, which is
 * why the configurable minimum matches it.
 */
const HEARTBEAT_MINUTES = SWEEP_MIN_MINUTES;

app.timer('entraSweep', {
  schedule: `0 */${HEARTBEAT_MINUTES} * * * *`,
  handler: async (_t: Timer, ctx: InvocationContext) => {
    try {
      const cfg = await getSweepConfig();
      if (!cfg.enabled) {
        // Deliberately does NOT touch lastSuccessAt. A disabled sweep must go
        // stale in the admin app exactly like a broken one: the control is not
        // being enforced either way, and the reason is a detail.
        ctx.log('entra sweep: disabled by configuration, skipping');
        return;
      }

      // Due yet? Measured from the last ATTEMPT, so a run that fails does not
      // spin every heartbeat retrying a Graph outage.
      let lastRunAt = 0;
      try {
        const row: any = await t('Meta').getEntity('meta', 'entraSweep');
        lastRunAt = row?.lastRunAt ? Date.parse(String(row.lastRunAt)) : 0;
      } catch { /* never run */ }

      const dueInMs = lastRunAt + cfg.intervalMinutes * 60000 - Date.now();
      // Half a heartbeat of slack, or a schedule that lands a few seconds early
      // defers a whole cycle and the effective interval silently doubles.
      if (lastRunAt && dueInMs > HEARTBEAT_MINUTES * 30000) return;

      await runSweep(ctx);
    } catch (e) {
      // Never let a sweep failure become an unhandled rejection: the next tick
      // must still happen, and the staleness clock is what raises the alarm.
      ctx.error(`entra sweep failed: ${(e as Error).message}`);
    }
  },
});
