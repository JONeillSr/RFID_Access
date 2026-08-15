#include "AccessControl.h"
#include "Roster.h"
#include "EventLog.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

TapRecord tapLog[LOG_SIZE];
int       logCount = 0, logHead = 0;
String    lastUnknownUid = "";

SemaphoreHandle_t dataMutex;

static const char* ROSTER_PATH = "/roster.dat";

static Roster gRoster;

// -----------------------------------------------------------------------------
//  Migration from the pre-2.0 NVS allow-list
// -----------------------------------------------------------------------------
//
// Units upgrading from the single-door firmware hold their enrolled cards as a
// JSON blob in NVS ("access"/"allow"). Import it once, the first time this build
// runs on a device that has no roster file yet.
//
// The gate is the EXISTENCE of the roster file, not whether it is empty. That
// distinction matters: an operator who deliberately removes every card leaves an
// empty-but-present roster, and re-importing the old NVS list on the next reboot
// would silently resurrect credentials they had revoked.
//
// The NVS blob is deliberately NOT deleted. It costs a few hundred bytes and is
// the only copy of the old list if the roster file is ever lost.

static void migrateFromNvs() {
    Preferences prefs;
    prefs.begin("access", true);
    String in = prefs.getString("allow", "[]");
    prefs.end();

    JsonDocument doc;
    if (deserializeJson(doc, in) != DeserializationError::Ok) return;

    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull() || arr.size() == 0) return;

    static Roster::Entry staged[64];
    size_t n = 0;
    for (JsonObject o : arr) {
        if (n >= sizeof(staged) / sizeof(staged[0])) break;
        String uid  = o["uid"].as<String>();
        String name = o["name"].as<String>();
        if (!uid.length()) continue;
        memset(&staged[n], 0, sizeof(staged[n]));
        staged[n].hash = gRoster.hashOf(uid.c_str());
        strncpy(staged[n].cred, uid.c_str(),  Roster::MAX_CRED - 1);
        strncpy(staged[n].name, name.c_str(), Roster::MAX_NAME - 1);
        n++;
    }
    if (n) gRoster.replaceAll(staged, n, /*rev=*/0);
}

void acInit() {
    dataMutex = xSemaphoreCreateMutex();

    // Check before begin(): begin() creates nothing, but being explicit about
    // the pre-existing state keeps the migration gate readable.
    bool hadRosterFile = LittleFS.exists(ROSTER_PATH);

    gRoster.begin(ROSTER_PATH);

    if (!hadRosterFile) migrateFromNvs();
}

// ---- roster queries ---------------------------------------------------------

size_t acCount() { return gRoster.count(); }

bool acEntryAt(size_t index, String& uid, String& name) {
    Roster::Entry e;
    if (!gRoster.entryAt(index, e)) return false;
    uid  = e.cred;
    name = e.name;
    return true;
}

bool acNameFor(const String& uid, String& name) {
    Roster::Entry e;
    if (!gRoster.findByCredential(uid.c_str(), e)) return false;
    name = e.name;
    return true;
}

bool acPersistent() { return gRoster.persistent(); }

size_t acRosterFileBytes() {
    File f = LittleFS.open(ROSTER_PATH, "r");
    if (!f) return 0;
    size_t n = f.size();
    f.close();
    return n;
}

// ---- roster mutation --------------------------------------------------------

void acAddEntry(const String& uid, const String& name) {
    if (!uid.length()) return;
    if (gRoster.add(uid.c_str(), name.c_str())) {
        LOCK();
        if (lastUnknownUid == uid) lastUnknownUid = "";
        UNLOCK();
    }
}

void acRenameEntry(const String& uid, const String& name) {
    gRoster.rename(uid.c_str(), name.c_str());
}

void acRemoveEntry(const String& uid) {
    gRoster.remove(uid.c_str());
}

bool acReplaceRoster(const RosterUpdate* entries, size_t n, uint32_t rev) {
    if (n > 0 && !entries) return false;

    // Build the Roster::Entry array here rather than in CloudSync so the hash
    // salt and record layout stay private to this module.
    Roster::Entry* staged = (Roster::Entry*)calloc(n ? n : 1, sizeof(Roster::Entry));
    if (!staged) return false;

    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char* cred = entries[i].cred;
        if (!cred || !*cred) continue;                  // skip malformed rows
        memset(&staged[k], 0, sizeof(staged[k]));
        staged[k].hash = gRoster.hashOf(cred);
        strncpy(staged[k].cred, cred, Roster::MAX_CRED - 1);
        if (entries[i].name) strncpy(staged[k].name, entries[i].name, Roster::MAX_NAME - 1);
        k++;
    }

    bool ok = gRoster.replaceAll(staged, k, rev);
    free(staged);

    if (ok) {
        // A roster change is worth an audit record: it is the moment a door's
        // idea of who may enter changed, and correlating that with a later
        // "why was I denied" is otherwise guesswork.
        eventLog.append(EventLog::EVT_CONFIG, EventLog::R_NONE, false, "");
    }
    return ok;
}

uint32_t acRosterRev() { return gRoster.rev(); }

// ---- decision ---------------------------------------------------------------

bool acProcessEvent(const AppEvent& evt) {
    if (evt.type != EVT_CARD_TAP) return false;

    String uid  = evt.card.uid;
    String type = evt.card.cardType;

    // Roster lookup first, outside the tap-log lock: it guards itself, and
    // holding two locks longer than necessary buys nothing here.
    Roster::Entry e;
    bool granted = gRoster.findByCredential(uid.c_str(), e);
    String nm    = granted ? String(e.name) : String();

    LOCK();
    tapLog[logHead] = {uid, type, granted, millis()};
    logHead = (logHead + 1) % LOG_SIZE;
    if (logCount < LOG_SIZE) logCount++;
    if (!granted) lastUnknownUid = uid;
    UNLOCK();

    // Durable record, outside the lock: this is a filesystem write, and holding
    // the tap-log mutex across it would stall any web handler reading /api/taps.
    // The raw credential is stored even on a denial -- that is what lets an
    // unknown card be enrolled from the central UI later.
    eventLog.append(EventLog::EVT_TAP,
                    granted ? EventLog::R_ENROLLED : EventLog::R_NOT_ENROLLED,
                    granted, uid.c_str());

    Serial.print(granted ? ">>> GRANTED  " : ">>> DENIED   ");
    Serial.print("UID: ");
    Serial.print(uid);
    if (granted) { Serial.print("  ("); Serial.print(nm); Serial.print(")"); }
    Serial.println();
    return granted;
}
