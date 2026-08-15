/**
 * @file    Roster.h
 * @brief   Persistent credential roster: the allow-list a door decides against.
 *
 * Replaces the previous fixed 30-entry array serialised as a JSON blob in NVS.
 * That shape had three problems for a multi-door fleet: a hard 30-credential
 * ceiling, a rewrite of the whole blob on every edit, and no way for a sync
 * client to swap the list wholesale without the decision path briefly seeing a
 * half-written roster.
 *
 * STORAGE
 * -------
 * One binary file on LittleFS (default /roster.dat):
 *
 *   header  magic 'RSTR', format version, flags, count, rev, salt, CRC32
 *   records count * Entry, sorted by hash
 *
 * Writes go to a temporary file which is then renamed over the live one. Rename
 * is atomic on LittleFS, so a power cut during a save leaves either the old
 * roster or the new one, never a truncated file. The CRC is checked on load; a
 * corrupt file is refused rather than half-trusted.
 *
 * WHY A HASH IS THE KEY
 * ---------------------
 * Each record is keyed by a salted, truncated SHA-256 of the credential, and the
 * records are kept sorted by that hash so lookup is a binary search rather than
 * a linear scan.
 *
 * Be clear about what this does and does not buy today. While `cred` is still
 * populated (see RETENTION below) the hash provides NO secrecy — the raw number
 * sits in the same record. Its value right now is structural: a fixed-width key,
 * an ordering, and a format that does not have to change later. Once the cloud
 * becomes the master record and raw retention is switched off, the same file
 * layout becomes genuinely privacy-preserving, with no migration.
 *
 * The salt lives in the file header, not in NVS. A per-device salt defeats
 * precomputation across a fleet, which is its real job; keeping it in NVS would
 * add nothing against physical theft (an attacker holding the flash holds both)
 * while creating a way to lose the roster by erasing NVS.
 *
 * RETENTION
 * ---------
 * `cred` holds the raw credential. Build with -D ROSTER_STORE_RAW=0 to store the
 * hash alone; the record layout is unchanged, the field is simply left empty.
 *
 * Keep it ON while the device is the master record — which is the case until the
 * Azure backend lands. A door that cannot report which cards it holds cannot be
 * audited, backed up, or administered from its own /config page.
 * Turn it OFF once the cloud is authoritative, so a stolen controller yields no
 * clonable card numbers.
 *
 * CONCURRENCY
 * -----------
 * Self-guarding: every public method takes an internal mutex, so the reader task
 * may call allows() while a web handler edits and a sync task replaces the whole
 * list. replaceAll() builds the new roster completely, then swaps it in under
 * that lock, so a tap arriving mid-sync sees either the old list or the new one
 * and never a partial state.
 *
 * This class never calls back into its callers, so nesting it inside another
 * lock (AccessControl's dataMutex, for instance) cannot deadlock.
 */

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef ROSTER_STORE_RAW
#define ROSTER_STORE_RAW 1      // see RETENTION above
#endif

class Roster {
public:
    static const size_t MAX_CRED = 16;   // credential text + NUL
    static const size_t MAX_NAME = 24;   // holder name + NUL

    struct Entry {
        uint64_t hash;                   // salted, truncated SHA-256 of cred
        char     cred[MAX_CRED];         // raw credential; "" if not retained
        char     name[MAX_NAME];         // display name
    };

    /// Load from LittleFS. Returns false if the filesystem is unavailable or the
    /// file is corrupt; in both cases the roster is left EMPTY, which denies
    /// every card rather than admitting an unknown one. Safe to call once in
    /// setup(), after LittleFS.begin().
    bool begin(const char* path = "/roster.dat");

    /// True once begin() has completed against a mounted filesystem. False means
    /// edits will not survive a reboot — surface it rather than hiding it.
    bool persistent() const { return _persistent; }

    // ---- decision path ----------------------------------------------------
    /// Is this credential enrolled? The only call on the hot path.
    bool allows(const char* credential) const;

    // ---- enumeration (local UI, backups) ----------------------------------
    size_t count() const;
    bool   entryAt(size_t index, Entry& out) const;
    bool   findByCredential(const char* credential, Entry& out) const;

    // ---- local mutation ---------------------------------------------------
    // Each persists immediately. Return false if the roster is full, the
    // credential is malformed, or the write failed.
    bool add(const char* credential, const char* name);
    bool rename(const char* credential, const char* name);
    bool remove(const char* credential);

    // ---- wholesale replace (Phase 3 sync) ---------------------------------
    /// Swap in an entirely new roster and record the revision it came from.
    /// Built fully before the swap, so the decision path never observes a
    /// partially applied update.
    bool replaceAll(const Entry* entries, size_t n, uint32_t rev);

    /// Revision of the roster currently held. 0 = never synced (locally edited).
    uint32_t rev() const;

    /// Hash a credential with this device's salt. Exposed so the sync client can
    /// compare a server-supplied list without the raw numbers.
    uint64_t hashOf(const char* credential) const;

    /// Bytes the roster occupies on disk, for /status.
    size_t   bytesOnDisk() const;

private:
    static const uint32_t MAGIC   = 0x52535452;   // 'RSTR'
    static const uint16_t VERSION = 1;
    static const size_t   MAX_ENTRIES = 512;      // ceiling; ~24 KB at 48 B each

    struct Header {
        uint32_t magic;
        uint16_t version;
        uint16_t flags;        // bit 0: raw credentials retained
        uint32_t count;
        uint32_t rev;
        uint64_t salt;
        uint32_t crc32;        // over the record array only
    };

    Entry*   _entries = nullptr;      // sorted by hash; owned
    size_t   _count   = 0;
    uint32_t _rev     = 0;
    uint64_t _salt    = 0;
    bool     _persistent = false;
    String   _path;

    mutable SemaphoreHandle_t _mutex = nullptr;
    void lock()   const { if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY); }
    void unlock() const { if (_mutex) xSemaphoreGive(_mutex); }

    bool   load();                                  // caller holds the lock
    bool   save();                                  // caller holds the lock
    int    indexOfHash(uint64_t h) const;           // binary search, -1 if absent
    static uint32_t crc32(const uint8_t* d, size_t n);
    static void     copyField(char* dst, size_t cap, const char* src);
};
