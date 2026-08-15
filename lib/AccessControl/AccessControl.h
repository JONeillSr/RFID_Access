/**
 * @file    AccessControl.h
 * @brief   Access decision + recent-tap log for the door.
 *
 * Storage of the allow-list lives in the Roster library (LittleFS-backed,
 * hash-keyed, atomically saved). This module owns the DECISION — what happens
 * when a credential is presented — and a short in-RAM log of recent taps that
 * backs /api/taps and the "Last tap" line on /status.
 *
 * The roster is reached only through the accessors below, so callers never hold
 * a pointer into it: Roster may swap its whole backing array during a cloud sync,
 * and an index or pointer captured beforehand would dangle. Ask by credential,
 * copy what you need, done.
 *
 * dataMutex / LOCK() / UNLOCK() guard the tap log ONLY. Roster guards itself, so
 * the accessors need no external locking (and nesting them inside LOCK() is safe
 * — Roster never calls back into this module).
 */

#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "Events.h"

const int LOG_SIZE = 20;

struct TapRecord { String uid; String type; bool granted; unsigned long ms; };

extern TapRecord tapLog[LOG_SIZE];
extern int       logCount;
extern int       logHead;
extern String    lastUnknownUid;

extern SemaphoreHandle_t dataMutex;

#define LOCK()   xSemaphoreTake(dataMutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(dataMutex)

/// Call once from setup(), AFTER LittleFS is mounted. Loads the roster and, on
/// a unit upgrading from the NVS-backed allow-list, migrates those entries over.
void acInit();

/// Handle a queued event from the access task. Returns true if access was
/// granted. Appends to the tap log and updates lastUnknownUid.
bool acProcessEvent(const AppEvent& evt);

// ---- roster queries ---------------------------------------------------------

/// Number of enrolled credentials.
size_t acCount();

/// Copy out the entry at `index` (0 .. acCount()-1). False if out of range.
/// `uid` comes back empty on a build that does not retain raw credentials.
bool acEntryAt(size_t index, String& uid, String& name);

/// Look up the holder's name for a credential. False if not enrolled.
bool acNameFor(const String& uid, String& name);

/// False when the roster could not be loaded from (or saved to) the filesystem:
/// the door still works, but edits will not survive a reboot. Shown on /status.
bool acPersistent();

/// Size of the roster file on disk, or 0 if it does not exist.
///
/// This is the check that distinguishes a healthy roster from one living only
/// in RAM. A failed save leaves the in-memory list populated, so acCount() looks
/// correct while every reboot silently re-migrates from NVS — which would also
/// resurrect any credential the operator had removed. A non-zero size here is
/// the proof that the list actually persisted.
size_t acRosterFileBytes();

// ---- roster mutation --------------------------------------------------------
// Each persists immediately and is safe to call from a web handler task.

void acAddEntry(const String& uid, const String& name);
void acRenameEntry(const String& uid, const String& name);
void acRemoveEntry(const String& uid);
