#include "AccessControl.h"
#include <Preferences.h>
#include <ArduinoJson.h>

Entry     allowList[MAX_ENTRIES];
int       allowCount = 0;
TapRecord tapLog[LOG_SIZE];
int       logCount = 0, logHead = 0;
String    lastUnknownUid = "";

SemaphoreHandle_t dataMutex;

static Preferences prefs;

static void saveAllowList() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < allowCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["uid"]  = allowList[i].uid;
        o["name"] = allowList[i].name;
    }
    String out;
    serializeJson(doc, out);
    prefs.begin("access", false);
    prefs.putString("allow", out);
    prefs.end();
}

static void loadAllowList() {
    prefs.begin("access", true);
    String in = prefs.getString("allow", "[]");
    prefs.end();
    JsonDocument doc;
    if (deserializeJson(doc, in) != DeserializationError::Ok) return;
    allowCount = 0;
    for (JsonObject o : doc.as<JsonArray>()) {
        if (allowCount >= MAX_ENTRIES) break;
        allowList[allowCount].uid  = o["uid"].as<String>();
        allowList[allowCount].name = o["name"].as<String>();
        allowCount++;
    }
}

void acInit() {
    dataMutex = xSemaphoreCreateMutex();
    loadAllowList();
}

int acFindEntry(const String& uid) {
    for (int i = 0; i < allowCount; i++)
        if (allowList[i].uid == uid) return i;
    return -1;
}

void acAddEntry(const String& uid, const String& name) {
    LOCK();
    if (uid.length() && acFindEntry(uid) < 0 && allowCount < MAX_ENTRIES) {
        allowList[allowCount++] = {uid, name};
        if (lastUnknownUid == uid) lastUnknownUid = "";
        saveAllowList();
    }
    UNLOCK();
}

void acRenameEntry(const String& uid, const String& name) {
    LOCK();
    int e = acFindEntry(uid);
    if (e >= 0 && name.length()) {
        allowList[e].name = name;
        saveAllowList();
    }
    UNLOCK();
}

void acRemoveEntry(const String& uid) {
    LOCK();
    int e = acFindEntry(uid);
    if (e >= 0) {
        for (int i = e; i < allowCount - 1; i++) allowList[i] = allowList[i + 1];
        allowCount--;
        saveAllowList();
    }
    UNLOCK();
}

bool acProcessEvent(const AppEvent& evt) {
    if (evt.type != EVT_CARD_TAP) return false;

    String uid  = evt.card.uid;
    String type = evt.card.cardType;

    LOCK();
    int  e       = acFindEntry(uid);
    bool granted = (e >= 0);
    tapLog[logHead] = {uid, type, granted, millis()};
    logHead = (logHead + 1) % LOG_SIZE;
    if (logCount < LOG_SIZE) logCount++;
    String nm = granted ? allowList[e].name : "";
    if (!granted) lastUnknownUid = uid;
    UNLOCK();

    Serial.print(granted ? ">>> GRANTED  " : ">>> DENIED   ");
    Serial.print("UID: ");
    Serial.print(uid);
    if (granted) { Serial.print("  ("); Serial.print(nm); Serial.print(")"); }
    Serial.println();
    return granted;
}
