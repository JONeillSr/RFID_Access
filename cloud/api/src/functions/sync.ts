/**
 * POST /api/v1/sync — the only endpoint a door calls in normal operation.
 *
 * One request carries events up and roster/config/firmware down. That is
 * deliberate: on a constrained device the TLS handshake, not the payload, is the
 * expensive part of a cycle, so a combined endpoint costs one handshake instead
 * of three.
 *
 * Nothing here is in the door's decision path. A failure, a timeout, or this
 * whole service being down must never affect whether a valid fob opens a door —
 * the device decides locally against its cached roster and spools events until
 * this succeeds. That is the governing constraint of the entire design.
 */

import { app, HttpRequest, HttpResponseInit, InvocationContext } from '@azure/functions';
import type { SyncRequest, SyncResponse } from '../../../shared/types';
import { RESTART_REQUEST_TTL_MS, RELEASE_REQUEST_TTL_MS } from '../../../shared/types';
import { releaseFeedUrl } from '../releaseFeed';
import { authenticateDevice } from '../auth';
import {
  effectiveRoster,
  getRosterRev,
  getDoor,
  touchDoor,
  ingestEvents,
  getFirmwareFor,
  wouldOffer,
} from '../storage';

export async function sync(
  req: HttpRequest,
  ctx: InvocationContext
): Promise<HttpResponseInit> {
  let body: SyncRequest;
  try {
    body = (await req.json()) as SyncRequest;
  } catch {
    return { status: 400, jsonBody: { error: 'malformed json' } };
  }

  const door = await authenticateDevice(body.deviceId, req.headers.get('x-device-key') ?? undefined);
  if (!door) {
    // No detail: a prober must not learn whether the device id exists.
    ctx.warn(`sync: auth failed for deviceId=${body.deviceId ?? '(none)'}`);
    return { status: 401, jsonBody: { error: 'unauthorized' } };
  }

  // ---- events up ---------------------------------------------------------
  let ackBootId = 0;
  let ackIdx = 0;

  // The start of the door's current boot, fixed once per boot.
  //
  // The door recomputes it on every request as (now - uptime), so it jitters by
  // a second between syncs. Events with no clock are dated from it, and their
  // storage keys include that date -- a jittering start would therefore move
  // them between a failed write and its retry, and duplicate them. So the first
  // value seen for a boot is kept and reused for the rest of that boot.
  const reportedBootId = Number.isFinite(body.bootId) ? Number(body.bootId) : 0;
  const reportedBootEpoch = body.bootEpoch > 0 ? body.bootEpoch : 0;
  // Same boot number AND a start within a couple of minutes of the stored one.
  // The number alone is not enough: a door whose filesystem is erased restarts
  // its boot counter, and could reuse a number whose stored start is months old.
  const sameBoot = door.bootId === reportedBootId && (door.bootEpoch ?? 0) > 0 &&
    (reportedBootEpoch === 0 || Math.abs(door.bootEpoch! - reportedBootEpoch) <= 120);
  const bootEpoch = sameBoot ? door.bootEpoch! : reportedBootEpoch;

  const events = Array.isArray(body.events) ? body.events : [];
  if (events.length > 0) {
    const { written, partial } = await ingestEvents(
      door.deviceId,
      door.name,
      events,
      {
        currentBootId: reportedBootId,
        currentBootStartMs: bootEpoch > 0 ? bootEpoch * 1000 : undefined,
      }
    );

    if (partial > 0) {
      // Some events reached only one of the two tables. Do NOT ack those: the
      // device keeps them and retries, and the upsert keys make the retry
      // converge rather than duplicate.
      ctx.error(
        `sync: ${partial} event(s) partially written for ${door.deviceId} - not acked, will retry`
      );
    }

    if (written > 0 && partial === 0) {
      // Ack only a fully successful batch, and only up to the last event —
      // events arrive in order, so the highest pair covers everything before it.
      const last = events[events.length - 1]!;
      ackBootId = last.bootId;
      ackIdx = last.idx;
    }
  }

  // ---- state down --------------------------------------------------------
  const rosterRev = await getRosterRev();
  const deviceRev = body.rosterRev ?? 0;

  const response: SyncResponse = {
    ackBootId,
    ackIdx,
    rosterRev,
    serverEpoch: Math.floor(Date.now() / 1000),
  };

  // Send the roster only when the device is behind. At 20 doors this is a few
  // KB, but sending it every 30s to every door for no reason is pure waste.
  if (deviceRev !== rosterRev) {
    response.roster = await effectiveRoster(door.deviceId);
  }

  const doorRow = await getDoor(door.deviceId);
  if (doorRow?.config) response.config = doorRow.config;

  // ---- remote restart ----------------------------------------------------
  // The door says which restart it last honoured. A match means the one asked
  // for has happened, so the request is cleared rather than offered again.
  const reportedRestart = typeof body.restartId === 'string' ? body.restartId : '';
  const restartDone = !!doorRow?.restartReqId && reportedRestart === doorRow.restartReqId;
  if (doorRow?.restartReqId && !restartDone) {
    // Only while fresh. A door that was offline when someone pressed Restart
    // must not reboot whenever it next reconnects, with nobody watching.
    const age = Date.now() - Date.parse(doorRow.restartReqAt ?? '');
    if (age >= 0 && age < RESTART_REQUEST_TTL_MS) {
      response.restart = { id: doorRow.restartReqId };
      ctx.log(`sync ${door.deviceId} asked to restart (request ${doorRow.restartReqId})`);
    }
  }

  // ---- remote release ----------------------------------------------------
  // Confirmed when the door reports the id. Kept on the row (releaseDoneId)
  // rather than cleared, so the admin app can show how the last one went.
  const reportedRelease = typeof body.releaseId === 'string' ? body.releaseId : '';
  const releaseId = doorRow?.releaseReqId;
  const releaseDone = !!releaseId &&
    (doorRow?.releaseDoneId === releaseId || reportedRelease === releaseId);
  const newlyReleased = !!releaseId && reportedRelease === releaseId &&
    doorRow?.releaseDoneId !== releaseId;
  if (releaseId && !releaseDone) {
    // The slow path, for a door that cannot poll its feed. Only while fresh: a
    // release that arrives after the visitor has left opens the door for
    // whoever is standing there instead.
    const reqMs = Date.parse(doorRow?.releaseReqAt ?? '');
    if (Date.now() - reqMs < RELEASE_REQUEST_TTL_MS) {
      response.release = { id: releaseId, exp: Math.floor((reqMs + RELEASE_REQUEST_TTL_MS) / 1000) };
    }
  }
  if (body.remoteRelease === true) {
    // Re-issued every sync, so a door never holds a URL near expiry. A failure
    // costs only the fast path -- never the sync.
    try {
      response.releaseFeed = { url: await releaseFeedUrl(door.deviceId) };
    } catch (e: any) {
      ctx.warn(`sync ${door.deviceId}: could not issue release feed URL: ${e?.message ?? e}`);
    }
  }

  // ---- firmware offer ----------------------------------------------------
  // Looked up BY THE DEVICE'S OWN BOARD. If nothing is published for that board,
  // nothing is offered — never a fallback to another variant's image, because a
  // mismatched image does not misbehave, it fails to boot and strands the door.
  //
  // The download is proxied through this API rather than handed out as a blob
  // SAS URL: the storage account has allowSharedKeyAccess disabled, and reusing
  // the device key the caller already presented is simpler and revocable per
  // door.
  if (body.board && doorRow?.fwHold) {
    ctx.log(`sync ${door.deviceId} is held back from firmware offers (fwHold)`);
  } else if (body.board) {
    const fw = await getFirmwareFor(body.board);
    // Newer only, unless the image was published as a rollback -- see wouldOffer.
    if (fw && wouldOffer(fw, body.firmware)) {
      response.firmware = {
        board: fw.board,          // echoed so the device can re-check before flashing
        version: fw.version,
        url: `https://${req.headers.get('host') ?? ''}/api/v1/firmware`,
        sha256: fw.sha256,
      };
      ctx.log(
        `sync ${door.deviceId} offered firmware ${fw.version} for ${fw.board} ` +
          `(currently ${body.firmware})`
      );
    }
  }

  await touchDoor(door.deviceId, {
    board: body.board,
    firmware: body.firmware,
    rosterRev: deviceRev,
    // What the door is actually running, which is not necessarily what it has
    // been configured with: the reader format is applied at boot.
    ...(body.readerMode === 'cnd' || body.readerMode === 'wiegand'
      ? { readerMode: body.readerMode }
      : {}),
    ...(typeof body.hasDoorContact === 'boolean'
      ? { hasDoorContact: body.hasDoorContact }
      : {}),
    // Only once the boot's start is actually known: storing 0 would pin a boot
    // to "no clock" for its whole life even after NTP arrives.
    ...(bootEpoch > 0 ? { bootId: reportedBootId, bootEpoch } : {}),
    ...(restartDone ? { restartReqId: '', restartDoneAt: new Date().toISOString() } : {}),
    ...(typeof body.remoteRelease === 'boolean' ? { hasRemoteRelease: body.remoteRelease } : {}),
    ...(newlyReleased ? { releaseDoneId: releaseId, releaseDoneAt: new Date().toISOString() } : {}),
  });

  ctx.log(
    `sync ${door.deviceId} fw=${body.firmware} events=${events.length} ` +
      `rev=${deviceRev}->${rosterRev}${response.roster ? ` roster=${response.roster.length}` : ''}`
  );

  return { status: 200, jsonBody: response };
}

app.http('sync', {
  methods: ['POST'],
  authLevel: 'anonymous',   // devices authenticate with x-device-key, not a function key
  route: 'v1/sync',
  handler: sync,
});
