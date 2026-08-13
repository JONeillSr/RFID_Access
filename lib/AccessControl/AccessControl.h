#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "Events.h"

const int MAX_ENTRIES = 30;
const int LOG_SIZE    = 20;

struct Entry     { String uid; String name; };
struct TapRecord { String uid; String type; bool granted; unsigned long ms; };

extern Entry     allowList[MAX_ENTRIES];
extern int       allowCount;
extern TapRecord tapLog[LOG_SIZE];
extern int       logCount;
extern int       logHead;
extern String    lastUnknownUid;

extern SemaphoreHandle_t dataMutex;

#define LOCK()   xSemaphoreTake(dataMutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(dataMutex)

// Call once from setup() before tasks start
void acInit();

// Call from the access-control task loop to handle a queued event.
// Returns true if access was granted, false if denied.
bool acProcessEvent(const AppEvent& evt);

// Lookup — caller must hold the lock
int  acFindEntry(const String& uid);

// Mutations — acquire and release the lock internally
void acAddEntry(const String& uid, const String& name);
void acRenameEntry(const String& uid, const String& name);
void acRemoveEntry(const String& uid);
