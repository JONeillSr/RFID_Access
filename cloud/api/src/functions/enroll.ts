/**
 * POST /api/v1/enroll — one-time device pairing.
 *
 * An operator generates a short-lived code in the admin UI, types it into the
 * device's /setup page, and the device exchanges it here for a long-lived key
 * that it stores in NVS. From then on it authenticates with that key.
 *
 * This avoids the two obvious alternatives, both worse: baking a secret into the
 * firmware image (which is published, and identical across every door), or
 * hand-provisioning each device with a key over a serial cable.
 */

import { app, HttpRequest, HttpResponseInit, InvocationContext } from '@azure/functions';
import { TableClient } from '@azure/data-tables';
import { DefaultAzureCredential } from '@azure/identity';
import type { EnrollRequest, EnrollResponse } from '../../../shared/types';
import { newDeviceKey, hashKey, redeemEnrollCode } from '../auth';

const account = process.env.STORAGE_ACCOUNT_NAME!;
const endpoint = `https://${account}.table.core.windows.net`;
const credential = new DefaultAzureCredential();

export async function enroll(
  req: HttpRequest,
  ctx: InvocationContext
): Promise<HttpResponseInit> {
  let body: EnrollRequest;
  try {
    body = (await req.json()) as EnrollRequest;
  } catch {
    return { status: 400, jsonBody: { error: 'malformed json' } };
  }

  if (!body.deviceId || !body.code) {
    return { status: 400, jsonBody: { error: 'deviceId and code are required' } };
  }

  // Single-use: the code is consumed here whether or not the rest succeeds.
  const redeemed = await redeemEnrollCode(body.code);
  if (!redeemed) {
    ctx.warn(`enroll: bad or expired code from ${body.deviceId}`);
    return { status: 403, jsonBody: { error: 'invalid or expired code' } };
  }

  const doors = new TableClient(endpoint, 'Doors', credential);

  // Re-pairing an existing door issues a NEW key and discards the old one, which
  // is also the recovery path for a device whose key was lost or compromised.
  const key = newDeviceKey();

  await doors.upsertEntity(
    {
      partitionKey: 'door',
      rowKey: body.deviceId,
      name: redeemed.deviceName || body.deviceId,
      site: redeemed.site,
      board: body.board ?? '',
      firmware: body.firmware ?? '',
      keyHash: hashKey(key),
      pairedAt: new Date().toISOString(),
      lastSeen: new Date().toISOString(),
    },
    'Merge'   // preserve groups/config an admin may already have set
  );

  ctx.log(`enroll: ${body.deviceId} paired as "${redeemed.deviceName}" (${body.board})`);

  const response: EnrollResponse = {
    deviceKey: key,          // the only time this is ever transmitted
    doorName: redeemed.deviceName || body.deviceId,
    site: redeemed.site,
  };

  return { status: 200, jsonBody: response };
}

app.http('enroll', {
  methods: ['POST'],
  authLevel: 'anonymous',   // the enrollment code IS the credential here
  route: 'v1/enroll',
  handler: enroll,
});
