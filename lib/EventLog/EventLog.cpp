/**
 * @file    EventLog.cpp
 * @brief   Implementation of the durable event spool.
 */

#include "EventLog.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <string.h>

EventLog eventLog;

// Same bitwise CRC-32 as Roster: small inputs, called rarely, not worth a table.
uint32_t EventLog::crc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

bool EventLog::valid(const Record& r) {
    return r.crc == crc32((const uint8_t*)&r, sizeof(Record) - sizeof(uint32_t));
}

bool EventLog::begin(const char* path) {
    if (!_mutex) _mutex = xSemaphoreCreateMutex();
    _path = path;

    lock();

    // Boot counter: one NVS write per boot, which is what makes (bootId, idx)
    // unique across power cycles without a write per event.
    Preferences prefs;
    prefs.begin("evtlog", false);
    _bootId = prefs.getUInt("bootId", 0) + 1;
    prefs.putUInt("bootId", _bootId);
    prefs.end();

    _nextIdx = 0;
    _ready = scan();

    unlock();
    return _ready;
}

// Walk the file counting intact records. A record failing its CRC means a write
// was torn by power loss; everything from there on is suspect, so the file is
// truncated back to the last good record rather than carrying a hole forward.
bool EventLog::scan() {
    _total = 0;
    _acked = 0;
    _overflow = false;

    if (LittleFS.totalBytes() == 0) return false;

    File f = LittleFS.open(_path, "r");
    if (!f) return true;                 // no spool yet: a valid empty state

    size_t good = 0;
    Record r;
    while (f.read((uint8_t*)&r, sizeof(r)) == (int)sizeof(r)) {
        if (!valid(r)) break;            // torn or corrupt: stop here
        good++;
    }
    size_t fileRecords = f.size() / sizeof(Record);
    f.close();

    _total = good;

    // Trim a partial or corrupt tail so the next append starts clean.
    if (good < fileRecords) {
        File in = LittleFS.open(_path, "r");
        File out = LittleFS.open(_path + ".tmp", "w");
        if (in && out) {
            for (size_t i = 0; i < good; i++) {
                if (in.read((uint8_t*)&r, sizeof(r)) != (int)sizeof(r)) break;
                out.write((const uint8_t*)&r, sizeof(r));
            }
        }
        if (in) in.close();
        if (out) out.close();
        LittleFS.remove(_path);
        LittleFS.rename(_path + ".tmp", _path);
    }

    // Restore the ack cursor. Kept in NVS rather than the file so acking does
    // not rewrite spool data.
    Preferences prefs;
    prefs.begin("evtlog", true);
    _acked = prefs.getUInt("acked", 0);
    prefs.end();
    if (_acked > _total) _acked = _total;

    return true;
}

bool EventLog::append(Type type, Reason reason, bool granted,
                      const char* credential) {
    if (!_ready) return false;

    lock();

    if (_total - _acked >= MAX_RECORDS) {
        // Spool full of un-acked events: the door has been offline a long time.
        // Drop the oldest by treating one as acked, so recent history — the part
        // most likely to matter — survives rather than the device going deaf.
        _overflow = true;
        _acked++;
    }

    Record r;
    memset(&r, 0, sizeof(r));
    r.bootId   = _bootId;
    r.idx      = _nextIdx++;
    r.uptimeMs = millis();

    // Only stamp a real time if the clock is actually trustworthy. Anything
    // before 2023 means SNTP has not landed yet; leave epoch 0 and let the
    // uploader resolve it from the boot epoch, flagged approximate.
    time_t now = time(nullptr);
    r.epoch = (now > 1700000000) ? (uint32_t)now : 0;

    r.type   = (uint8_t)type;
    r.reason = (uint8_t)reason;
    r.flags  = (granted ? F_GRANTED : 0) | (r.epoch ? 0 : F_TIME_APPROX);
    if (credential) strncpy(r.cred, credential, sizeof(r.cred) - 1);
    r.crc = crc32((const uint8_t*)&r, sizeof(r) - sizeof(uint32_t));

    File f = LittleFS.open(_path, "a");
    bool ok = false;
    if (f) {
        ok = f.write((const uint8_t*)&r, sizeof(r)) == sizeof(r);
        f.close();
    }
    if (ok) _total++;

    unlock();
    return ok;
}

size_t EventLog::pending() const {
    lock();
    size_t n = _total - _acked;
    unlock();
    return n;
}

size_t EventLog::peek(Record* out, size_t max) const {
    lock();
    size_t n = 0;
    File f = LittleFS.open(_path, "r");
    if (f) {
        if (f.seek(_acked * sizeof(Record))) {
            Record r;
            while (n < max && f.read((uint8_t*)&r, sizeof(r)) == (int)sizeof(r)) {
                if (!valid(r)) break;
                out[n++] = r;
            }
        }
        f.close();
    }
    unlock();
    return n;
}

void EventLog::ack(uint32_t bootId, uint32_t idx) {
    lock();

    // Advance the cursor past every record at or before (bootId, idx). Compared
    // as a pair so an ack from a previous boot cannot retire this boot's events.
    File f = LittleFS.open(_path, "r");
    if (f) {
        if (f.seek(_acked * sizeof(Record))) {
            Record r;
            while (f.read((uint8_t*)&r, sizeof(r)) == (int)sizeof(r)) {
                if (!valid(r)) break;
                bool covered = (r.bootId < bootId) ||
                               (r.bootId == bootId && r.idx <= idx);
                if (!covered) break;
                _acked++;
            }
        }
        f.close();
    }

    Preferences prefs;
    prefs.begin("evtlog", false);
    prefs.putUInt("acked", _acked);
    prefs.end();

    if (_acked >= COMPACT_AT) compact();

    unlock();
}

// Rewrite the file without its acked prefix. Only worth doing occasionally --
// every compaction rewrites the whole spool, so it is gated on COMPACT_AT.
bool EventLog::compact() {
    File in = LittleFS.open(_path, "r");
    if (!in) return false;
    File out = LittleFS.open(_path + ".tmp", "w");
    if (!out) { in.close(); return false; }

    in.seek(_acked * sizeof(Record));
    Record r;
    size_t kept = 0;
    while (in.read((uint8_t*)&r, sizeof(r)) == (int)sizeof(r)) {
        if (!valid(r)) break;
        out.write((const uint8_t*)&r, sizeof(r));
        kept++;
    }
    in.close();
    out.close();

    LittleFS.remove(_path);
    if (!LittleFS.rename(_path + ".tmp", _path)) return false;

    _total = kept;
    _acked = 0;
    Preferences prefs;
    prefs.begin("evtlog", false);
    prefs.putUInt("acked", 0);
    prefs.end();
    return true;
}

bool EventLog::overflowed() const {
    lock();
    bool o = _overflow;
    unlock();
    return o;
}

size_t EventLog::bytesOnDisk() const {
    lock();
    size_t n = 0;
    File f = LittleFS.open(_path, "r");
    if (f) { n = f.size(); f.close(); }
    unlock();
    return n;
}
