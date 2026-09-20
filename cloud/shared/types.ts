/**
 * Shared contract for the RFID Access backend.
 *
 * Imported by BOTH the Function App (cloud/api) and the admin web app
 * (cloud/web) so the two halves cannot drift.
 *
 * ⚠️ THE FIRMWARE IS A THIRD COPY THAT TypeScript CANNOT SEE.
 * EventType and EventReason below mirror `EventLog::Type` and
 * `EventLog::Reason` in lib/EventLog/EventLog.h. Those values are persisted in
 * the on-device spool and shipped over the wire, so they are fixed for all time:
 * append new values, never renumber. If you change one side, change the other —
 * nothing here will catch it for you.
 */

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------

/** A human. Reports are about people, not fobs: one person may carry several. */
/**
 * Who decides whether this person still works here.
 *
 * DELIBERATELY EXPLICIT, not inferred from whether an object id is present.
 * An absent id would conflate two states that need different treatment:
 * a contractor who correctly has no Entra account, and an employee somebody
 * forgot to link. The second is a hole in the guarantee this exists to provide
 * -- they look covered and are not -- so it has to be reportable, and it can
 * only be reported if "should be linked" was stated rather than guessed.
 *
 *   'entra'  -- an Entra account governs access; the sweep may revoke
 *   'manual' -- guest, contractor, one-off; only a human changes this
 */
export type ManagedBy = 'entra' | 'manual';

export interface Person {
  personId: string;
  name: string;
  email?: string;
  active: boolean;
  /** Group ids. Access is granted where a person's groups meet a door's. */
  groups: string[];
  notes?: string;

  /**
   * Defaults to 'manual' for any record that does not say otherwise, so
   * existing people are never swept by surprise on deploy. Opting a person in
   * is a deliberate act.
   */
  managedBy?: ManagedBy;

  /**
   * Entra object id (oid), not UPN or email. Those change with marriages and
   * rebrands; the oid never does, and a link that silently breaks is a
   * revocation that silently stops happening.
   */
  entraObjectId?: string;

  /** Why this person is inactive, so an admin is not left guessing. */
  deactivatedReason?: string;
  deactivatedAt?: string;
}

/** A physical credential. Belongs to at most one person. */
export interface Credential {
  credId: string;
  /** The number the reader emits. This IS the secret — treat it as one. */
  number: string;
  personId?: string;
  /** Human label, e.g. "blue fob", "backup card". */
  label?: string;
  active: boolean;
  validFrom?: string; // ISO 8601
  validTo?: string;   // ISO 8601
}

export interface Group {
  groupId: string;
  name: string;
}

/** Per-door configuration, authored centrally and pushed down on sync. */
export type ReaderMode = 'cnd' | 'wiegand';

export interface DoorConfig {
  relayHoldMs: number;
  resultHoldMs: number;
  /**
   * Reader line format. 'cnd' is Paxton's native Clock & Data and the default.
   * The device attaches its capture interrupts from this at boot, so a change
   * only takes effect when the door restarts -- compare with the door's own
   * `readerMode` (what it is running) to see a change still pending.
   */
  readerMode?: ReaderMode;
  /**
   * A reed contact is fitted to this door. Off by default: an unwired input
   * reads as an open door and would raise a forced-open alert immediately.
   * A board with no free input ignores this; see `hasDoorContact`.
   */
  doorContact?: boolean;
  /** Seconds a door may stay open after a release before it is reported held open; 0 = off. */
  doorHeldSec?: number;
  /**
   * Raise the setup AP at the door's NEXT boot, alongside its normal connection,
   * so someone on site can move it to a network this one cannot reach. The door
   * keeps working throughout and closes the AP again after 30 minutes.
   *
   * Acted on when it CHANGES. Leaving it true does not re-raise the AP on later
   * boots; set it false and true again to ask a second time.
   */
  openSetupPortal?: boolean;
  /** Minutes past midnight, local to the door's timezone. */
  schedule?: {
    enabled: boolean;
    startMin: number;
    endMin: number;
    /** Bit 0 = Sunday … bit 6 = Saturday. */
    daysMask: number;
  };
}

export interface Door {
  deviceId: string;
  name: string;
  site: string;
  /**
   * BOARD_NAME as reported by the device, e.g. "ESP32 DevKit V1".
   * Firmware is per board type: an image built for one variant bricks another,
   * so a rollout must never offer a build that does not match this.
   */
  board: string;
  groups: string[];
  config: DoorConfig;
  lastSeen?: string;
  firmware?: string;
  rosterRev?: number;
  /** Reader format the door reported running at its last sync. */
  readerMode?: ReaderMode;
  /** Whether the door's board can take a contact at all (reported by the door). */
  hasDoorContact?: boolean;
  /**
   * Hold this door back from firmware offers.
   *
   * Firmware is published per BOARD, so without this every door of the same
   * board updates at once -- which is precisely what you do not want when one of
   * them is somewhere awkward to reach. Set it on the doors you want to update
   * last, prove the image on an accessible one, then clear it.
   */
  fwHold?: boolean;
  /**
   * The boot the door was in at its last sync, and when that boot began (unix
   * seconds). Recorded ONCE per boot and reused, because a door recomputes its
   * boot start on every request and the value jitters by a second. Events with
   * no clock are dated from it, so a jittering start would move their storage
   * keys between retries and duplicate them.
   */
  bootId?: number;
  bootEpoch?: number;
}

// ---------------------------------------------------------------------------
// Events — values MUST match lib/EventLog/EventLog.h
// ---------------------------------------------------------------------------

export enum EventType {
  Tap        = 1,
  Exit       = 2,
  ScheduleOn = 3,
  ScheduleOff= 4,
  Boot       = 5,
  Config     = 6,
  SyncFail   = 7,
  /** Door opened with no grant, exit press or unlock window in effect. */
  DoorForced = 8,
  /**
   * Door held open past its limit (reason HeldOpen), or closed again after that
   * (reason Closed, with `cred` holding how long it was open, e.g. "95s").
   */
  DoorHeld   = 9,
  /** OTA applied. `cred` carries the change, e.g. "2.4.4>2.5.0". */
  FirmwareUpdated = 10,
  /** OTA refused or failed. `cred` carries the target version. */
  FirmwareFailed  = 11,
}

export enum EventReason {
  None        = 0,
  Enrolled    = 1,
  NotEnrolled = 2,
  ExitButton  = 3,
  Schedule    = 4,
  NoTime      = 5,
  /** Door forced: nothing released it. */
  NoRelease   = 6,
  /** Door held: open past the limit since the last release ended. */
  HeldOpen    = 7,
  /** Door held: closed again. */
  Closed      = 8,
}

/** One event exactly as the device spooled it. */
export interface DeviceEvent {
  bootId: number;
  idx: number;
  uptimeMs: number;
  /** Unix seconds, or 0 when the device clock was not yet NTP-synced. */
  epoch: number;
  type: EventType;
  reason: EventReason;
  granted: boolean;
  /** True when epoch was derived from bootEpoch rather than observed. */
  timeApprox: boolean;
  /** Raw credential; empty for events with no card (exit, boot, schedule). */
  cred: string;
}

/**
 * An event after ingest: identity resolved and DENORMALIZED.
 *
 * personName and doorName are copied in at write time and never recomputed. If a
 * fob is reassigned later, history must still show who held it at the time — a
 * report that rewrites the past is worse than no report.
 */
export interface StoredEvent extends Omit<DeviceEvent, 'timeApprox'> {
  deviceId: string;
  doorName: string;
  /** Absent for exit presses, boots, schedule changes — those have no person. */
  personId?: string;
  personName?: string;
  credId?: string;
  /**
   * Resolved ISO timestamp -- or, when `timeUnknown` is true, only a PLACEMENT
   * used for partitioning and sorting. Read the bounds instead in that case.
   */
  at: string;
  timeApprox: boolean;
  /**
   * The door had no trusted clock and the time could not be derived. The event
   * happened, in sequence order, somewhere between the bounds; `at` is not a
   * claim about when. See api/src/eventTime.ts.
   */
  timeUnknown?: boolean;
  /** ISO. Absent when nothing earlier with a known time exists. */
  timeNotBefore?: string;
  /** ISO. */
  timeNotAfter?: string;
}

// ---------------------------------------------------------------------------
// Device protocol
// ---------------------------------------------------------------------------

/**
 * POST /api/v1/sync — the only endpoint a door calls in normal operation.
 * One request carries events up and roster/config/firmware down, so a cycle
 * costs a single TLS handshake. On a constrained device the handshake, not the
 * payload, is the expensive part.
 */
export interface SyncRequest {
  deviceId: string;
  board: string;
  firmware: string;
  bootId: number;
  /**
   * Unix seconds at which this boot began (now - uptime), or 0 if the device
   * still has no trusted clock. Used to resolve events whose epoch is 0.
   */
  bootEpoch: number;
  /** Roster revision the device currently holds; 0 = never synced. */
  rosterRev: number;
  /** Reader line format the device is RUNNING, not the one configured for it. */
  readerMode?: ReaderMode;
  /**
   * This board has an input a door contact can land on. The C6 and C3 have no
   * free pin, so the admin app disables the control rather than offering a
   * setting the door will ignore.
   */
  hasDoorContact?: boolean;
  events: DeviceEvent[];
}

/** A credential as pushed to a door: the flat list it checks taps against. */
export interface RosterEntry {
  /** Raw number. Omitted once devices store hashes only. */
  cred?: string;
  /** Salted truncated SHA-256, hex. */
  hash?: string;
  name: string;
}

export interface FirmwareOffer {
  /** Must equal the requesting device's board, or the device rejects it. */
  board: string;
  version: string;
  url: string;
  sha256: string;
}

export interface SyncResponse {
  /** Highest (bootId, idx) durably stored. The device discards up to here. */
  ackBootId: number;
  ackIdx: number;
  rosterRev: number;
  /** Present only when the device's rosterRev is stale. */
  roster?: RosterEntry[];
  config?: DoorConfig;
  /** Present only when an image matching this device's board is available. */
  firmware?: FirmwareOffer;
  /** Server time, so a door with no NTP can still bound its clock. */
  serverEpoch: number;
}

/** POST /api/v1/enroll — one-time device pairing. */
export interface EnrollRequest {
  deviceId: string;
  board: string;
  firmware: string;
  /** Short-lived code issued by the admin UI and typed into /setup. */
  code: string;
}

export interface EnrollResponse {
  /** Long-lived key, stored in NVS and sent as x-device-key thereafter. */
  deviceKey: string;
  doorName: string;
  site: string;
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

export interface ReportQuery {
  from: string; // ISO 8601
  to: string;   // ISO 8601
  personId?: string;
  deviceId?: string;
  grantedOnly?: boolean;
}

export interface ReportPage {
  events: StoredEvent[];
  /** Opaque cursor; date ranges spanning months fan out across partitions. */
  continuation?: string;
}

/**
 * An exit-button release with no grant at that door in the preceding window.
 * The exit button opens the door with no record of who, by design, so this is
 * the report that makes that blind spot reviewable rather than invisible.
 */
export interface UnattributedExit {
  deviceId: string;
  doorName: string;
  at: string;
  /** Seconds searched backwards for a preceding grant. */
  windowSec: number;
}
