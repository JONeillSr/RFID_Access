/**
 * @file    Roster.cpp
 * @brief   Implementation of the persistent credential roster.
 */

#include "Roster.h"
#include <LittleFS.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#include <string.h>

// ---- small helpers ----------------------------------------------------------

void Roster::copyField(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

// Bitwise CRC-32 (IEEE). No table: this runs over at most ~24 KB, a handful of
// times per boot, so the ~1 KB a lookup table would cost is not worth it.
uint32_t Roster::crc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

uint64_t Roster::hashOf(const char* credential) const {
    if (!credential || !*credential) return 0;

    // salt || credential -> SHA-256, first 8 bytes big-endian.
    uint8_t buf[8 + MAX_CRED];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)(_salt >> (8 * (7 - i)));
    size_t len = strnlen(credential, MAX_CRED - 1);
    memcpy(buf + 8, credential, len);

    uint8_t digest[32];
    mbedtls_sha256(buf, 8 + len, digest, /*is224=*/0);

    uint64_t h = 0;
    for (int i = 0; i < 8; i++) h = (h << 8) | digest[i];
    // 0 is reserved as "no hash", so never return it for a real credential.
    return h ? h : 1;
}

int Roster::indexOfHash(uint64_t h) const {
    // Records are kept sorted by hash, so this is a binary search.
    size_t lo = 0, hi = _count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (_entries[mid].hash == h) return (int)mid;
        if (_entries[mid].hash < h)  lo = mid + 1;
        else                          hi = mid;
    }
    return -1;
}

// ---- lifecycle --------------------------------------------------------------

bool Roster::begin(const char* path) {
    if (!_mutex) _mutex = xSemaphoreCreateMutex();
    _path = path;

    lock();
    _entries = (Entry*)calloc(MAX_ENTRIES, sizeof(Entry));
    if (!_entries) {                      // out of heap: deny everything
        _count = 0;
        _persistent = false;
        unlock();
        return false;
    }
    bool ok = load();
    unlock();
    return ok;
}

bool Roster::load() {
    _count = 0;
    _rev   = 0;

    // The application mounts LittleFS in setup(); deliberately do NOT re-mount
    // here. Calling begin() on an already-mounted filesystem logs a warning on
    // every boot, and this module has no business owning the mount anyway.
    // totalBytes() reads 0 when nothing is mounted, which is the check we want.
    if (LittleFS.totalBytes() == 0) {
        _persistent = false;
        return false;
    }
    _persistent = true;

    File f = LittleFS.open(_path, "r");
    if (!f) {
        // First boot on a fresh filesystem. Not an error: an empty roster is a
        // valid state (it denies everything), and a salt is minted here so the
        // first save is self-describing.
        _salt = ((uint64_t)esp_random() << 32) | esp_random();
        return true;
    }

    Header h{};
    if (f.read((uint8_t*)&h, sizeof(h)) != (int)sizeof(h) ||
        h.magic != MAGIC || h.version != VERSION || h.count > MAX_ENTRIES) {
        f.close();
        _salt = ((uint64_t)esp_random() << 32) | esp_random();
        return false;                     // refuse a file we do not understand
    }

    size_t bytes = h.count * sizeof(Entry);
    if (bytes && f.read((uint8_t*)_entries, bytes) != (int)bytes) {
        f.close();
        _salt = h.salt;
        return false;
    }
    f.close();

    if (crc32((const uint8_t*)_entries, bytes) != h.crc32) {
        // Corrupt. Stay empty rather than trusting records that may have been
        // altered: for an access-control list, denying a valid card is a far
        // better failure than admitting an unknown one.
        _count = 0;
        _salt  = h.salt;
        return false;
    }

    _count = h.count;
    _rev   = h.rev;
    _salt  = h.salt;
    return true;
}

bool Roster::save() {
    if (!_persistent) return false;

    // Write to a temporary file, then rename over the live one. Rename is atomic
    // on LittleFS, so a power cut leaves either the old roster or the new one.
    String tmp = _path + ".tmp";
    LittleFS.remove(tmp);

    File f = LittleFS.open(tmp, "w");
    if (!f) return false;

    size_t bytes = _count * sizeof(Entry);
    Header h{};
    h.magic   = MAGIC;
    h.version = VERSION;
    h.flags   = ROSTER_STORE_RAW ? 1 : 0;
    h.count   = (uint32_t)_count;
    h.rev     = _rev;
    h.salt    = _salt;
    h.crc32   = crc32((const uint8_t*)_entries, bytes);

    bool ok = f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h);
    if (ok && bytes) ok = f.write((const uint8_t*)_entries, bytes) == bytes;
    f.close();

    if (!ok) { LittleFS.remove(tmp); return false; }

    LittleFS.remove(_path);
    return LittleFS.rename(tmp, _path);
}

// ---- decision path ----------------------------------------------------------

bool Roster::allows(const char* credential) const {
    if (!credential || !*credential) return false;
    lock();
    bool found = indexOfHash(hashOf(credential)) >= 0;
    unlock();
    return found;
}

// ---- enumeration ------------------------------------------------------------

size_t Roster::count() const {
    lock();
    size_t n = _count;
    unlock();
    return n;
}

bool Roster::entryAt(size_t index, Entry& out) const {
    lock();
    bool ok = index < _count;
    if (ok) out = _entries[index];
    unlock();
    return ok;
}

bool Roster::findByCredential(const char* credential, Entry& out) const {
    if (!credential || !*credential) return false;
    lock();
    int i = indexOfHash(hashOf(credential));
    if (i >= 0) out = _entries[i];
    unlock();
    return i >= 0;
}

// ---- mutation ---------------------------------------------------------------

bool Roster::add(const char* credential, const char* name) {
    if (!credential || !*credential) return false;
    lock();
    uint64_t h = hashOf(credential);
    if (_count >= MAX_ENTRIES || indexOfHash(h) >= 0) { unlock(); return false; }

    // Insert in hash order so the array stays binary-searchable.
    size_t pos = 0;
    while (pos < _count && _entries[pos].hash < h) pos++;
    memmove(&_entries[pos + 1], &_entries[pos], (_count - pos) * sizeof(Entry));

    Entry& e = _entries[pos];
    memset(&e, 0, sizeof(e));
    e.hash = h;
#if ROSTER_STORE_RAW
    copyField(e.cred, MAX_CRED, credential);
#endif
    copyField(e.name, MAX_NAME, name);
    _count++;

    bool ok = save();
    unlock();
    return ok;
}

bool Roster::rename(const char* credential, const char* name) {
    if (!credential || !*credential || !name || !*name) return false;
    lock();
    int i = indexOfHash(hashOf(credential));
    if (i < 0) { unlock(); return false; }
    copyField(_entries[i].name, MAX_NAME, name);
    bool ok = save();
    unlock();
    return ok;
}

bool Roster::remove(const char* credential) {
    if (!credential || !*credential) return false;
    lock();
    int i = indexOfHash(hashOf(credential));
    if (i < 0) { unlock(); return false; }
    memmove(&_entries[i], &_entries[i + 1], (_count - i - 1) * sizeof(Entry));
    _count--;
    bool ok = save();
    unlock();
    return ok;
}

bool Roster::replaceAll(const Entry* entries, size_t n, uint32_t rev) {
    if (n > MAX_ENTRIES) return false;

    // Build the replacement completely before touching the live array, so a
    // failure part-way leaves the existing roster intact.
    Entry* staged = (Entry*)calloc(MAX_ENTRIES, sizeof(Entry));
    if (!staged) return false;
    if (n) memcpy(staged, entries, n * sizeof(Entry));

    // Sort by hash (insertion sort: n is small and the input is usually close to
    // ordered already).
    for (size_t i = 1; i < n; i++) {
        Entry key = staged[i];
        size_t j = i;
        while (j > 0 && staged[j - 1].hash > key.hash) { staged[j] = staged[j - 1]; j--; }
        staged[j] = key;
    }

    lock();
    Entry* old = _entries;
    _entries = staged;                    // the swap the decision path races with
    _count   = n;
    _rev     = rev;
    bool ok  = save();
    unlock();

    free(old);
    return ok;
}

uint32_t Roster::rev() const {
    lock();
    uint32_t r = _rev;
    unlock();
    return r;
}

size_t Roster::bytesOnDisk() const {
    lock();
    size_t n = sizeof(Header) + _count * sizeof(Entry);
    unlock();
    return n;
}
