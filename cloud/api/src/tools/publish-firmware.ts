/**
 * Publish a firmware image for one board type.
 *
 *   npm run publish-fw -- --board "ESP32 DevKit V1" --version 2.4.0 \
 *                         --file ../../.pio/build/esp32dev/firmware.bin
 *
 * Uploads the binary, records its SHA-256, and makes it the offered image for
 * that board. Devices on any other board are unaffected — firmware is published
 * per board type, never per fleet, because an image built for the wrong ESP32
 * variant does not boot and strands the door.
 *
 * Deliberate guards, because the failure here is unrecoverable without physical
 * access to every door:
 *
 *   - Refuses firmware.factory.bin. That is the merged image (bootloader +
 *     partition table + app) meant for flashing at offset 0 over USB. Written
 *     into an OTA slot it produces a device that will not boot.
 *   - Refuses an image larger than the OTA slot in partitions_rfid.csv.
 *   - Refuses a version string already published for that board unless --force,
 *     so an unchanged version number cannot silently ship different code.
 *   - Uploads the blob BEFORE writing the metadata, so a failure part-way leaves
 *     the old offer intact rather than pointing devices at a missing image.
 */

import { BlobServiceClient } from '@azure/storage-blob';
import { DefaultAzureCredential } from '@azure/identity';
import { createHash } from 'node:crypto';
import { readFileSync, existsSync, statSync } from 'node:fs';
import { basename, resolve } from 'node:path';
import { getFirmwareFor, putFirmware, listFirmware, boardKey } from '../storage';

// app0 size from partitions_rfid.csv (0x1D0000). An image beyond this cannot be
// written to the inactive slot at all.
const OTA_SLOT_BYTES = 0x1d0000;

const account = process.env.STORAGE_ACCOUNT_NAME;
if (!account) {
  console.error('STORAGE_ACCOUNT_NAME is not set.');
  process.exit(1);
}
const container = process.env.FIRMWARE_CONTAINER || 'firmware';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf('--' + name);
  return i >= 0 ? process.argv[i + 1] : undefined;
}
const force = process.argv.includes('--force');
const listOnly = process.argv.includes('--list');

async function main() {
  const blobs = new BlobServiceClient(
    `https://${account}.blob.core.windows.net`,
    new DefaultAzureCredential()
  );

  if (listOnly) {
    const all = await listFirmware();
    if (!all.length) {
      console.log('\nNothing published yet.\n');
      return;
    }
    console.log('\nPublished firmware:\n');
    for (const f of all) {
      console.log(
        `  ${f.board.padEnd(20)} ${f.version.padEnd(10)} ` +
          `${String(f.sizeBytes).padStart(9)} B  ${f.publishedAt}`
      );
    }
    console.log('');
    return;
  }

  const board = arg('board');
  const version = arg('version');
  const file = arg('file');

  if (!board || !version || !file) {
    console.error('Usage: --board "<BOARD_NAME>" --version <x.y.z> --file <firmware.bin>');
    console.error('       --list                 show what is currently published');
    console.error('       --force                overwrite an existing version');
    console.error('\nBoard must match the device\'s BOARD_NAME exactly, e.g. "ESP32 DevKit V1".');
    process.exit(1);
  }

  const path = resolve(process.cwd(), file);
  if (!existsSync(path)) {
    console.error(`File not found: ${path}`);
    process.exit(1);
  }

  // Guard: the factory image is not an OTA image.
  if (basename(path).includes('factory')) {
    console.error(
      `\nRefusing ${basename(path)}.\n\n` +
        'That is the merged image (bootloader + partition table + app) for USB\n' +
        'flashing at offset 0. Written into an OTA slot it produces a device that\n' +
        'will not boot. Publish firmware.bin instead.\n'
    );
    process.exit(1);
  }

  const size = statSync(path).size;
  if (size > OTA_SLOT_BYTES) {
    console.error(
      `\nImage is ${size} B; the OTA slot is ${OTA_SLOT_BYTES} B ` +
        `(${((100 * size) / OTA_SLOT_BYTES).toFixed(1)}% over).\n` +
        'It cannot be written to the inactive slot.\n'
    );
    process.exit(1);
  }

  const buf = readFileSync(path);

  // GUARD: the version label must match the FW_VERSION compiled into the image.
  //
  // This is the most dangerous mistake available here, and it is silent. Publish
  // a 2.4.0 binary as "2.4.1" and every device on that board updates, reboots,
  // still reports 2.4.0, is offered 2.4.1 again, and updates forever -- a fleet
  // of doors rebooting every sync interval, indefinitely. The device cannot
  // detect it: from its side each offer looks like a legitimately newer version.
  //
  // main.cpp emits "fw=<FW_VERSION>" in its boot banner, so the string is
  // present verbatim in the image and can simply be read back.
  const ascii = buf.toString('latin1');
  const stamped = [...ascii.matchAll(/fw=(\d+\.\d+\.\d+)/g)].map((m) => m[1]!);
  const distinct = [...new Set(stamped)];

  if (distinct.length === 0) {
    console.error(
      `\nCannot find a "fw=<version>" marker in ${basename(path)}.\n\n` +
        'The publish tool verifies the declared version against the one compiled\n' +
        'into the image, and cannot do that here. Either this is not an RFID_Access\n' +
        'build, or the boot banner changed. Pass --force to publish unverified.\n'
    );
    if (!force) process.exit(1);
  } else if (!distinct.includes(version)) {
    console.error(
      `\nVERSION MISMATCH — refusing to publish.\n\n` +
        `  you asked to publish : ${version}\n` +
        `  the image reports    : ${distinct.join(', ')}\n\n` +
        'Publishing an image under a version it does not report puts every device\n' +
        'on this board into a permanent update loop: it flashes, reboots, still\n' +
        `reports ${distinct[0]}, is offered ${version} again, and repeats forever.\n\n` +
        `Fix: set FW_VERSION to ${version} in platformio.ini, rebuild, and publish\n` +
        `that image — or publish this one as ${distinct[0]}.\n`
    );
    process.exit(1);
  } else {
    console.log(`  image self-reports ${version} — matches`);
  }

  const sha256 = createHash('sha256').update(buf).digest('hex');

  const existing = await getFirmwareFor(board);
  if (existing && existing.version === version && !force) {
    if (existing.sha256 === sha256) {
      console.log(`\n${board} ${version} is already published, identical image. Nothing to do.\n`);
      return;
    }
    console.error(
      `\n${board} ${version} is already published with a DIFFERENT image.\n` +
        `  published sha256 ${existing.sha256}\n  this file  sha256 ${sha256}\n\n` +
        'Shipping different code under the same version makes a fleet impossible\n' +
        'to reason about. Bump FW_VERSION, or pass --force if you are certain.\n'
    );
    process.exit(1);
  }

  const blobName = `${boardKey(board)}/${version}/firmware.bin`;
  const client = blobs.getContainerClient(container).getBlockBlobClient(blobName);

  console.log(`\nUploading ${basename(path)} (${size} B) -> ${blobName}`);
  await client.uploadData(buf, {
    blobHTTPHeaders: { blobContentType: 'application/octet-stream' },
  });

  // Metadata last: until this is written, devices are still offered the previous
  // image, so an interrupted publish never points them at a missing blob.
  await putFirmware({
    board,
    version,
    blobName,
    sha256,
    sizeBytes: size,
    publishedAt: new Date().toISOString(),
  });

  console.log(`  sha256 ${sha256}`);
  console.log(`\nPublished. Devices reporting board "${board}" will be offered ${version}`);
  console.log('on their next sync. Other boards are unaffected.\n');
  console.log('Devices refuse any image whose board does not match their own, and');
  console.log('defer flashing while their door is unlocked.\n');
}

main().catch((err) => {
  console.error('\nPublish failed:', err?.message ?? err);
  process.exit(1);
});
