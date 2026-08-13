/**
 * @file    WebStatus.h
 * @brief   Web status + scan-results page for the I2C scanner.
 *
 * Registers routes on an application-owned WebServer (STA mode only):
 *   GET /            human-readable HTML status + scan results
 *   GET /api/scan    same data as JSON
 *   GET /rescan      re-runs the I2C scan, then redirects back to /
 *
 * ElegantOTA is attached separately in main and serves /update.
 *
 * This is the dead-OLED backstop: if the panel never comes up, the same scan
 * results are still reachable over the network.
 */

#ifndef WEBSTATUS_H
#define WEBSTATUS_H

#include <WebServer.h>

/// Register all status routes on the supplied server.
void registerStatusHandlers(WebServer& server);

#endif  // WEBSTATUS_H
