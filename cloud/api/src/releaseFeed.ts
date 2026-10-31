/**
 * The fast path for a remote release.
 *
 * A door only hears from the backend when it syncs, every ~30 s. That is fine for
 * config and far too slow for someone standing at the door. Holding a request
 * open instead (long-poll) would keep a Flex Consumption instance executing
 * around the clock -- about $130 a month at 2 GB, for one button.
 *
 * So each door gets one small blob, `release/<deviceId>`, and polls it every few
 * seconds over a connection it keeps open, with If-None-Match so an unchanged
 * blob costs a 304 and a few bytes. Storage bills per transaction, not per
 * second held: cents a month. Pressing Release overwrites the blob; the door sees
 * it on its next poll.
 *
 * The door reads it with a SAS URL for that one blob, read-only, issued on every
 * sync. The storage account has shared-key access disabled, so the SAS is a USER
 * DELEGATION SAS, signed with a key the Function App's managed identity obtains.
 * There is still no account key anywhere.
 *
 * Nothing here is the only path: sync also carries an outstanding release, so a
 * door that cannot poll (low heap, blob unreachable) is released one sync later.
 */

import {
  BlobServiceClient,
  BlobSASPermissions,
  generateBlobSASQueryParameters,
  type UserDelegationKey,
} from '@azure/storage-blob';
import { DefaultAzureCredential } from '@azure/identity';

const account = process.env.STORAGE_ACCOUNT_NAME!;
const CONTAINER = 'release';

const blobs = new BlobServiceClient(
  `https://${account}.blob.core.windows.net`,
  new DefaultAzureCredential()
);

/** How long a door's feed URL is good for. Re-issued every sync, so generous. */
const SAS_LIFETIME_MS = 24 * 60 * 60 * 1000;

// A user delegation key may live up to 7 days. Fetching one is a network call,
// so it is cached per instance and replaced while it still covers a fresh SAS.
const KEY_LIFETIME_MS = 3 * 24 * 60 * 60 * 1000;
let cachedKey: { key: UserDelegationKey; expiresMs: number } | undefined;

async function delegationKey(): Promise<UserDelegationKey> {
  const now = Date.now();
  if (cachedKey && cachedKey.expiresMs - now > SAS_LIFETIME_MS + 60 * 60 * 1000) {
    return cachedKey.key;
  }
  // Start a few minutes back: a door's clock is not the only one that drifts.
  const startsOn = new Date(now - 5 * 60 * 1000);
  const expiresOn = new Date(now + KEY_LIFETIME_MS);
  const key = await blobs.getUserDelegationKey(startsOn, expiresOn);
  cachedKey = { key, expiresMs: expiresOn.getTime() };
  return key;
}

/** Read-only URL for this door's release blob. */
export async function releaseFeedUrl(deviceId: string): Promise<string> {
  const key = await delegationKey();
  const now = Date.now();
  const sas = generateBlobSASQueryParameters(
    {
      containerName: CONTAINER,
      blobName: deviceId,
      permissions: BlobSASPermissions.parse('r'),
      startsOn: new Date(now - 5 * 60 * 1000),
      expiresOn: new Date(now + SAS_LIFETIME_MS),
    },
    key,
    account
  ).toString();
  return `https://${account}.blob.core.windows.net/${CONTAINER}/${encodeURIComponent(deviceId)}?${sas}`;
}

/**
 * Publish a release to the door's feed. `exp` is unix seconds; the door judges
 * it against the server time it learned at its last sync, not its own clock,
 * which may never have synced.
 */
export async function publishRelease(deviceId: string, id: string, exp: number): Promise<void> {
  const body = JSON.stringify({ id, exp });
  await blobs
    .getContainerClient(CONTAINER)
    .getBlockBlobClient(deviceId)
    .upload(body, Buffer.byteLength(body), {
      blobHTTPHeaders: { blobContentType: 'application/json', blobCacheControl: 'no-store' },
    });
}
