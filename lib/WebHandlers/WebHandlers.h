#pragma once
#include <WebServer.h>
#include <functional>

/**
 * Local web API for this door: roster, taps and unlock schedule.
 *
 * READ-ONLY ONCE CENTRALLY MANAGED
 * `isManaged` decides whether this door's roster has an owner elsewhere. When it
 * returns true the write endpoints (`/api/add`, `/api/rename`, `/api/remove`,
 * `POST /api/schedule`) answer 409 and change nothing; the read endpoints stay
 * exactly as they are, so a door can still explain itself locally even when the
 * network is gone.
 *
 * The reason is that a controller which can enrol its own fobs while offline is
 * an attack surface: anyone who reaches one door's page could add themselves,
 * and the central database would not know until it silently overwrote the entry
 * on the next sync. It also removes any possibility of local/central drift,
 * since the roster then has exactly one writer.
 *
 * The accepted cost is that you cannot enrol a fob during a WAN outage. Doors
 * keep working on their cached rosters; the fob list is frozen until the link
 * returns.
 *
 * Passed as a predicate rather than read from CloudSync directly so this module
 * stays ignorant of *why* it is locked -- and so an unpaired or bench unit keeps
 * full local CRUD by simply not supplying one.
 */
using IsManagedFn = std::function<bool()>;

void registerWebHandlers(WebServer& server, IsManagedFn isManaged = nullptr);
