/**
 * @file    EventLog.h
 * @brief   Durable spool of access events, awaiting upload to the backend.
 *
 * The existing tapLog is a 20-entry RAM ring that dies at every reboot and has
 * no notion of which door it belongs to. This is its persistent counterpart: an
 * append-only record of everything the door decided, kept on LittleFS until the
 * cloud confirms it has them.
 *
 * IT IS A BUFFER, NOT AN ARCHIVE. The cloud is the system of record; this exists
 * so a WAN outage, a backend deploy, or a flat-out Azure incident costs no audit
 * data. At ~40 B a record, the 256 KB filesystem holds on the order of 5,000
 * events — roughly a month of complete disconnection at a busy door.
 *
 * FILE SHAPE
 * ----------
 * Append-only file of fixed-size records, plus a separately persisted "acked"
 * high-water mark. That combination suits the real access pattern far better
 * than a ring buffer would: appends are frequent, acks happen at most once per
 * sync, and compaction is rare. A ring would rewrite its header on every single
 * event for no benefit.
 *
 *   append   -> one record appended, nothing else touched
 *   ack      -> one small cursor write, only when the backend confirms
 *   compact  -> drop the acked prefix, only once the file grows past a cap
 *
 * A record carries its own CRC, so a write torn by power loss is detected and
 * discarded on the next load rather than corrupting the spool behind it.
 *
 * IDENTITY AND IDEMPOTENCY
 * ------------------------
 * Every record is stamped (bootId, idx): bootId comes from a counter bumped once
 * per boot in NVS, idx counts within that boot. Together with the deviceId the
 * backend already knows, that tuple is a globally unique, monotonic key — so an
 * upload can be retried freely and the server can dedupe on primary key rather
 * than guessing. One NVS write per boot, none per event.
 *
 * TIME BEFORE NTP
 * ---------------
 * The clock reads 1970 until the first SNTP response lands, which is well after
 * the first events of a boot can occur. Every record therefore stores uptimeMs
 * unconditionally, and epoch only when the clock is already trustworthy. At
 * upload time, records with epoch == 0 are resolved as bootEpoch + uptimeMs and
 * flagged approximate, so a tap during a network-less boot still lands on the
 * timeline instead of in 1970 — honestly labelled rather than silently invented.
 */

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class EventLog {
public:
    // What happened. Values are persisted and shipped to the backend, so they
    // are explicit and must never be renumbered — only appended to.
    enum Type : uint8_t {
        EVT_TAP        = 1,   // credential presented
        EVT_EXIT       = 2,   // request-to-exit button
        EVT_SCHED_ON   = 3,   // unlock window opened
        EVT_SCHED_OFF  = 4,   // unlock window closed
        EVT_BOOT       = 5,   // device started
        EVT_CONFIG     = 6,   // settings or roster changed locally
        EVT_SYNC_FAIL  = 7,   // a sync attempt failed (why, in reason)
        // 8 and 9 are reserved for door position sensing (Phase 6) so the
        // numbering stays contiguous with the plan.
        EVT_FW_UPDATED = 10,  // OTA applied; cred holds "<old>><new>"
        EVT_FW_FAILED  = 11,  // OTA attempted and refused/failed; cred holds target
    };

    // Why the decision went the way it did. Far more useful than a bare
    // granted/denied when someone asks "why couldn't I get in on Tuesday".
    enum Reason : uint8_t {
        R_NONE          = 0,
        R_ENROLLED      = 1,  // granted: credential is in the roster
        R_NOT_ENROLLED  = 2,  // denied: unknown credential
        R_EXIT_BUTTON   = 3,  // granted: request-to-exit
        R_SCHEDULE      = 4,  // granted: inside an unlock window
        R_NO_TIME       = 5,  // schedule inactive: clock never synced
    };

    static const uint8_t F_GRANTED     = 0x01;
    static const uint8_t F_TIME_APPROX = 0x02;   // epoch derived, not observed

    struct Record {
        uint32_t bootId;
        uint32_t idx;
        uint32_t uptimeMs;
        uint32_t epoch;        // 0 = clock was not yet valid
        uint8_t  type;
        uint8_t  reason;
        uint8_t  flags;
        uint8_t  _pad;
        /// Raw credential for card events; empty for most others.
        ///
        /// Firmware events reuse this field for the version change
        /// ("2.4.4>2.5.0"), because the record is a fixed 40 bytes and adding a
        /// field would invalidate every spool file already on a device. Treat it
        /// as "16 bytes of event-specific detail" rather than strictly a
        /// credential; the backend only looks it up in the credential index for
        /// card events, so a version string here resolves to no person, which is
        /// correct.
        char     cred[16];
        uint32_t crc;
    };                         // exactly 40 bytes

    /// Call once in setup(), after LittleFS is mounted. Bumps the boot counter,
    /// scans the spool, and drops any torn trailing record.
    bool begin(const char* path = "/events.dat");

    /// Append one event. Cheap and non-blocking enough for the access task:
    /// one small append, no read-modify-write.
    bool append(Type type, Reason reason, bool granted, const char* credential);

    /// Records written but not yet confirmed by the backend.
    size_t pending() const;

    /// Copy out up to `max` un-acked records, oldest first, for upload.
    size_t peek(Record* out, size_t max) const;

    /// Confirm the backend durably holds everything up to and including this
    /// (bootId, idx). Only now may the records be discarded — an upload that is
    /// sent but never acknowledged is retried, never dropped.
    void ack(uint32_t bootId, uint32_t idx);

    /// True if the spool has hit its cap and is discarding the oldest events.
    /// Worth surfacing: it means the door has been offline long enough to start
    /// losing audit history.
    bool overflowed() const;

    uint32_t bootId() const { return _bootId; }
    size_t   bytesOnDisk() const;

private:
    // ~5,000 records at 40 B. Sized to leave the roster room on a 256 KB volume.
    static const size_t MAX_RECORDS = 5000;
    static const size_t COMPACT_AT  = 512;   // acked records before compaction

    String   _path;
    uint32_t _bootId   = 0;
    uint32_t _nextIdx  = 0;
    size_t   _total    = 0;      // records currently in the file
    size_t   _acked    = 0;      // leading records already confirmed
    bool     _overflow = false;
    bool     _ready    = false;

    mutable SemaphoreHandle_t _mutex = nullptr;
    void lock()   const { if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY); }
    void unlock() const { if (_mutex) xSemaphoreGive(_mutex); }

    bool     scan();             // caller holds the lock
    bool     compact();          // caller holds the lock
    static uint32_t crc32(const uint8_t* d, size_t n);
    static bool     valid(const Record& r);
};

extern EventLog eventLog;
