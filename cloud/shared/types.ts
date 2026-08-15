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
export interface Person {
  personId: string;
  name: string;
  email?: string;
  active: boolean;
  /** Group ids. Access is granted where a person's groups meet a door's. */
  groups: string[];
  notes?: string;
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
export interface DoorConfig {
  relayHoldMs: number;
  resultHoldMs: number;
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
  // Reserved for Phase 6 (door position sensing); keep numbering contiguous.
  // DoorForced = 8,
  // DoorHeld   = 9,
}

export enum EventReason {
  None        = 0,
  Enrolled    = 1,
  NotEnrolled = 2,
  ExitButton  = 3,
  Schedule    = 4,
  NoTime      = 5,
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
  /** Resolved ISO timestamp. */
  at: string;
  timeApprox: boolean;
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
