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
import { authenticateDevice } from '../auth';
import {
  effectiveRoster,
  getRosterRev,
  getDoor,
  touchDoor,
  ingestEvents,
  getFirmwareFor,
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

  const events = Array.isArray(body.events) ? body.events : [];
  if (events.length > 0) {
    const { written, partial } = await ingestEvents(
      door.deviceId,
      door.name,
      events,
      body.bootEpoch ?? 0
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
    if (fw && fw.version !== body.firmware) {
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
