/**
 * Device authentication.
 *
 * Each paired door holds a long-lived key in NVS and sends it as `x-device-key`
 * over TLS. Only a hash of that key is ever stored server-side, so a dump of the
 * Doors table does not let anyone impersonate a door.
 *
 * Deliberately NOT a shared secret across the fleet: keys are per device, so one
 * compromised controller is revoked by clearing one row rather than reflashing
 * every door.
 *
 * A device key authenticates a DOOR, not a person. It permits exactly two
 * things — uploading that door's events and fetching that door's roster. It can
 * never enrol a credential or read another door's data. That boundary matters:
 * these controllers sit in physically reachable places, so the blast radius of
 * one being opened up should be small.
 */

import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';
import { TableClient } from '@azure/data-tables';
import { DefaultAzureCredential } from '@azure/identity';

const account = process.env.STORAGE_ACCOUNT_NAME!;
const credential = new DefaultAzureCredential();
const endpoint = `https://${account}.table.core.windows.net`;

function doors() {
  return new TableClient(endpoint, 'Doors', credential);
}
function meta() {
  return new TableClient(endpoint, 'Meta', credential);
}

/** Device keys are opaque high-entropy strings; 32 bytes is ample. */
export function newDeviceKey(): string {
  return randomBytes(32).toString('base64url');
}

export function hashKey(key: string): string {
  return createHash('sha256').update(key).digest('hex');
}

/**
 * Constant-time comparison. A plain === leaks, through timing, how many leading
 * characters matched, which is enough to recover a key given enough attempts.
 */
function safeEqual(a: string, b: string): boolean {
  const ab = Buffer.from(a, 'utf8');
  const bb = Buffer.from(b, 'utf8');
  if (ab.length !== bb.length) return false;
  return timingSafeEqual(ab, bb);
}

export interface AuthedDoor {
  deviceId: string;
  name: string;
  site: string;
}

/**
 * Verify the x-device-key header against the claimed deviceId.
 * Returns undefined on any failure — caller responds 401 without detail, so a
 * prober cannot distinguish "no such door" from "wrong key".
 */
export async function authenticateDevice(
  deviceId: string | undefined,
  presentedKey: string | undefined
): Promise<AuthedDoor | undefined> {
  if (!deviceId || !presentedKey) return undefined;

  try {
    const row = await doors().getEntity<any>('door', deviceId);
    if (!row.keyHash) return undefined;                 // door exists but unpaired
    if (!safeEqual(hashKey(presentedKey), row.keyHash)) return undefined;
    return {
      deviceId,
      name: row.name ?? deviceId,
      site: row.site ?? '',
    };
  } catch {
    return undefined;                                    // unknown device
  }
}

// ---------------------------------------------------------------------------
// Enrollment codes
// ---------------------------------------------------------------------------

const ENROLL_TTL_MS = 15 * 60 * 1000;

/**
 * Issue a short-lived, single-use pairing code (admin UI calls this).
 *
 * Short-lived and single-use because the code is typed into a device's /setup
 * page over plain HTTP on the local network — it is the weakest link in the
 * chain, so its useful lifetime is kept as small as practical.
 */
export async function issueEnrollCode(deviceName: string, site: string): Promise<string> {
  // Ambiguity-free alphabet: no O/0, I/1. These get read off a screen and typed
  // by hand, often on a ladder.
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  let code = '';
  const bytes = randomBytes(8);
  for (let i = 0; i < 8; i++) code += alphabet[bytes[i]! % alphabet.length];

  await meta().upsertEntity(
    {
      partitionKey: 'enroll',
      rowKey: code,
      deviceName,
      site,
      expiresAt: new Date(Date.now() + ENROLL_TTL_MS).toISOString(),
    },
    'Replace'
  );
  return code;
}

export interface RedeemedCode {
  deviceName: string;
  site: string;
}

/** Redeem a code exactly once. Deleted on use, whether or not it had expired. */
export async function redeemEnrollCode(code: string): Promise<RedeemedCode | undefined> {
  if (!code) return undefined;
  try {
    const row = await meta().getEntity<any>('enroll', code.toUpperCase().trim());

    // Delete first: a code must not survive a failure later in pairing, or a
    // retry would silently reuse it.
    await meta().deleteEntity('enroll', row.rowKey as string);

    if (!row.expiresAt || Date.parse(row.expiresAt) < Date.now()) return undefined;
    return { deviceName: row.deviceName ?? '', site: row.site ?? '' };
  } catch {
    return undefined;
  }
}
