/**
 * GET /api/v1/firmware — serve the image matching the calling device's board.
 *
 * Proxied through the API rather than handed out as a blob SAS URL, for three
 * reasons:
 *   - The storage account has allowSharedKeyAccess disabled, so a plain SAS is
 *     not available and a user-delegation SAS expires in at most 7 days.
 *   - The device already holds a per-door key, which is revocable per door.
 *   - The board is decided HERE, from the authenticated door's record, not from
 *     anything the caller can name in the request.
 *
 * That last point is the important one. There is no board parameter: a device
 * cannot ask for another variant's image, whether by accident or otherwise. A
 * mismatched image does not misbehave on an ESP32 — it fails to boot, and the
 * door is dead until someone reaches it physically.
 */

import { app, HttpRequest, HttpResponseInit, InvocationContext } from '@azure/functions';
import { BlobServiceClient } from '@azure/storage-blob';
import { DefaultAzureCredential } from '@azure/identity';
import { authenticateDevice } from '../auth';
import { getDoor, getFirmwareFor } from '../storage';

const account = process.env.STORAGE_ACCOUNT_NAME!;
const container = process.env.FIRMWARE_CONTAINER || 'firmware';
const credential = new DefaultAzureCredential();

export async function firmware(
  req: HttpRequest,
  ctx: InvocationContext
): Promise<HttpResponseInit> {
  const deviceId = req.headers.get('x-device-id') ?? undefined;
  const door = await authenticateDevice(deviceId, req.headers.get('x-device-key') ?? undefined);
  if (!door) {
    ctx.warn(`firmware: auth failed for ${deviceId ?? '(none)'}`);
    return { status: 401, jsonBody: { error: 'unauthorized' } };
  }

  // The board comes from the Doors row this device most recently checked in
  // with — never from the request.
  const doorRow = await getDoor(door.deviceId);
  if (!doorRow?.board) {
    return { status: 409, jsonBody: { error: 'device board unknown; sync first' } };
  }

  const fw = await getFirmwareFor(doorRow.board);
  if (!fw) {
    return { status: 404, jsonBody: { error: `no firmware published for ${doorRow.board}` } };
  }

  const blobs = new BlobServiceClient(`https://${account}.blob.core.windows.net`, credential);
  const blob = blobs.getContainerClient(container).getBlockBlobClient(fw.blobName);

  const exists = await blob.exists();
  if (!exists) {
    // Metadata says an image exists but the blob does not: publishing was
    // interrupted. Surface it loudly rather than letting devices retry forever.
    ctx.error(`firmware: metadata references missing blob ${fw.blobName}`);
    return { status: 500, jsonBody: { error: 'published image is missing' } };
  }

  const dl = await blob.download();
  ctx.log(
    `firmware: serving ${fw.blobName} (${fw.version}, ${fw.sizeBytes} B) ` +
      `to ${door.deviceId} [${doorRow.board}]`
  );

  return {
    status: 200,
    headers: {
      'Content-Type': 'application/octet-stream',
      'Content-Length': String(fw.sizeBytes),
      // Echoed so the device can cross-check what it is about to flash against
      // what it was offered, without trusting the filename.
      'x-fw-version': fw.version,
      'x-fw-sha256': fw.sha256,
      'x-fw-board': fw.board,
    },
    body: dl.readableStreamBody as any,
  };
}

app.http('firmware', {
  methods: ['GET'],
  authLevel: 'anonymous',   // the device key is the credential
  route: 'v1/firmware',
  handler: firmware,
});
