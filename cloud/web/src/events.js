/**
 * Event type names.
 *
 * ⚠️ MIRRORS `EventLog::Type` in lib/EventLog/EventLog.h. Those values are
 * persisted in device spool files and shipped over the wire, so they are fixed
 * for all time: append, never renumber. Nothing here will catch a mismatch —
 * this is the third copy, alongside cloud/shared/types.ts and the firmware.
 */
export const TAP = 1, EXIT = 2, SCHED_ON = 3, SCHED_OFF = 4,
             BOOT = 5, CONFIG = 6, SYNC_FAIL = 7,
             DOOR_FORCED = 8, DOOR_HELD = 9,
             FW_UPDATED = 10, FW_FAILED = 11;

// EventLog::Reason values used by the door-position events.
const R_CLOSED = 8;

export function describeEvent(e) {
  switch (e.type) {
    case TAP: return e.granted ? 'granted' : 'DENIED';
    case EXIT: return 'exit button';
    case SCHED_ON: return 'unlock window opened';
    case SCHED_OFF: return 'unlock window closed';
    case BOOT: return 'device booted';
    case CONFIG: return e.cred ? `config changed (${e.cred})` : 'config changed';
    case SYNC_FAIL: return 'sync failed';
    case DOOR_FORCED: return 'DOOR FORCED OPEN';
    case DOOR_HELD:
      return e.reason === R_CLOSED
        ? `closed after being held open ${e.cred}`
        : 'DOOR HELD OPEN';
    case FW_UPDATED: return `firmware updated ${e.cred}`;
    case FW_FAILED: return `firmware FAILED ${e.cred}`;
    default: return `type ${e.type}`;
  }
}
