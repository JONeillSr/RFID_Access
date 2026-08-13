#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

enum EventType : uint8_t {
    EVT_CARD_TAP,
    EVT_EXIT_REQUEST,   // request-to-exit button; card fields unused
};

struct CardTapEvent {
    char uid[20];
    char cardType[32];
};

struct AppEvent {
    EventType    type;
    CardTapEvent card;
};

extern QueueHandle_t appEventQueue;
